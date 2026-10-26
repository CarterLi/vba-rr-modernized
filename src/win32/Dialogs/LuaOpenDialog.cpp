// LuaOpenDialog.cpp : implementation file
//
// One console window per script.  Every window owns a VbaLuaState (see
// common/lua-engine.cpp) and is completely independent: its own path box, its
// own Run/Stop button, its own console text, its own window layout and its own
// font.  Everything that used to be a file-scope global in this file is now a
// field of LuaConsoleWindow, which is what makes several windows possible.

//#include "../stdafx.h"
//#include "../resource.h"
#include "LuaOpenDialog.h"
#include "ModernFileDialog.h"
#include "../MainWnd.h"
#include "../WinResUtil.h"
#include "../WinMiscUtil.h"
#include "../WinText.h"
#include "../VBA.h"
#include "../Sound.h"

#include "../../common/vbalua.h"
#include "../../common/LuaHost.h"
#include "../../version.h"

#include <vector>

namespace
{
	struct ControlLayoutInfo
	{
		int controlID;

		enum LayoutType // what to do when the containing window resizes
		{
			NONE, // leave the control where it was
			RESIZE_END, // resize the control
			MOVE_START, // move the control
		};
		LayoutType horizontalLayout;
		LayoutType verticalLayout;
	};

	struct ControlLayoutState
	{
		int x, y, width, height;
		bool valid;
		ControlLayoutState() : x(0), y(0), width(0), height(0), valid(false) {}
	};

	const ControlLayoutInfo controlLayoutInfos[] = {
		{IDC_LUACONSOLE, ControlLayoutInfo::RESIZE_END, ControlLayoutInfo::RESIZE_END},
		{IDC_EDIT_LUAPATH, ControlLayoutInfo::RESIZE_END, ControlLayoutInfo::NONE},
		{IDC_BUTTON_LUARUN, ControlLayoutInfo::MOVE_START, ControlLayoutInfo::NONE},
		{IDC_BUTTON_LUASTOP, ControlLayoutInfo::MOVE_START, ControlLayoutInfo::NONE},
	};
	const int numControlLayoutInfos = sizeof(controlLayoutInfos) / sizeof(*controlLayoutInfos);

	// One per open console window.
	struct LuaConsoleWindow
	{
		HWND		 hDlg;
		VbaLuaState *script;		// NULL until something has been run
		HFONT		 font;			// console font, deleted with the window
		int			 width, height;	// client size, for the resize deltas
		ControlLayoutState layout[numControlLayoutInfos];
	};

	std::vector<LuaConsoleWindow *> s_consoles;

	// The last script any window opened, offered as the default for the next
	// window (the engine itself remembers nothing across windows).
	std::wstring s_lastScriptPath;

	LuaConsoleWindow *consoleFor(HWND hDlg)
	{
		if (!hDlg)
			return NULL;

		return (LuaConsoleWindow *)GetWindowLongPtr(hDlg, GWLP_USERDATA);
	}

	// The console a stopped script should update: the one it was started from.
	HWND hwndOf(std::intptr_t handle)
	{
		return (HWND)handle;
	}
}

void PrintToWindowConsole(std::intptr_t hDlgAsInt, lua_host::TextEncoding encoding, const char* str)
{
	HWND hDlg = hwndOf(hDlgAsInt);
	HWND hConsole = hDlg ? GetDlgItem(hDlg, IDC_LUACONSOLE) : NULL;
	if (!hConsole)
		return;

	int length = GetWindowTextLength(hConsole);
	if(length >= 250000)
	{
		// discard first half of text if it's getting too long
		SendMessage(hConsole, EM_SETSEL, 0, length/2);
		SendMessage(hConsole, EM_REPLACESEL, false, (LPARAM)_T(""));
		length = GetWindowTextLength(hConsole);
	}
	SendMessage(hConsole, EM_SETSEL, length, length);

	{
		// The console is a Unicode control while the script's output is bytes.
		// A script whose .lua file starts with a BOM declared itself UTF-8;
		// everything else is in the process code page, like the rest of the
		// emulator.  Handing those bytes to EM_REPLACESEL directly made the
		// control read them as UTF-16, which turned every line into garbage.
		//
		// The UTF-8 path still falls back per string: one line can mix the
		// script's own text with a file name or an English diagnostic that the
		// engine appended, and that part is ANSI however the file was saved.
		CString text = (encoding == lua_host::TextEncoding::Utf8)
			? winWideFromUtf8OrAnsi(str)
			: winWideFromAnsi(str);
		SendMessage(hConsole, EM_REPLACESEL, false, (LPARAM)(LPCTSTR)text);
	}
}

void WinLuaOnStart(std::intptr_t hDlgAsInt)
{
	HWND hDlg = hwndOf(hDlgAsInt);
	if (!hDlg)
		return;

	// disable browse while running because it misbehaves if clicked in a frameadvance loop
	if (HWND browse = GetDlgItem(hDlg, IDC_BUTTON_LUABROWSE))
		EnableWindow(browse, false);
	if (HWND stop = GetDlgItem(hDlg, IDC_BUTTON_LUASTOP))
		EnableWindow(stop, true);
	if (HWND run = GetDlgItem(hDlg, IDC_BUTTON_LUARUN))
		SetWindowText(run, _T("Restart"));
	if (HWND console = GetDlgItem(hDlg, IDC_LUACONSOLE))
		SetWindowText(console, _T("")); // clear the console
}

void WinLuaOnStop(std::intptr_t hDlgAsInt)
{
	HWND hDlg = hwndOf(hDlgAsInt);
	if (!hDlg)
		return;

	HWND prevWindow = GetActiveWindow();
	// bring to front among the other script windows, since a stopped script will
	// have some message for the user that would be easier to miss otherwise --
	// but never for a window that is on its way out
	if (::IsWindow(hDlg) && ::IsWindowVisible(hDlg))
		SetActiveWindow(hDlg);
	if (CWnd *main = AfxGetMainWnd())
	{
		if (prevWindow == main->GetSafeHwnd())
			SetActiveWindow(prevWindow);
	}

	if (HWND browse = GetDlgItem(hDlg, IDC_BUTTON_LUABROWSE))
		EnableWindow(browse, true);
	if (HWND stop = GetDlgItem(hDlg, IDC_BUTTON_LUASTOP))
		EnableWindow(stop, false);
	if (HWND run = GetDlgItem(hDlg, IDC_BUTTON_LUARUN))
		SetWindowText(run, _T("Run"));
}

HWND LuaWindowCreate(CWnd *parent)
{
	int index = (int)s_consoles.size();

	// The index travels through the WM_INITDIALOG lParam; it is only used to
	// stagger the windows so a new one does not land exactly on the last one.
	HWND hDlg = ::CreateDialogParam(AfxGetResourceHandle(), MAKEINTRESOURCE(IDD_LUA),
		parent ? parent->GetSafeHwnd() : NULL, (DLGPROC)DlgLuaScriptDialog, (LPARAM)index);

	if (!hDlg)
		return NULL;

	if (::IsIconic(hDlg))
		::ShowWindow(hDlg, SW_RESTORE);
	::SetForegroundWindow(hDlg);

	return hDlg;
}

void LuaWindowRunScript(CWnd *parent, const TCHAR *path)
{
	HWND hDlg = LuaWindowCreate(parent);
	if (!hDlg)
		return;

	SetDlgItemText(hDlg, IDC_EDIT_LUAPATH, path);

	// Press Run for the user, so a dropped script behaves exactly like one that
	// was picked in the window (same "no ROM" message, same error reporting).
	SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDC_BUTTON_LUARUN, BN_CLICKED),
		(LPARAM)GetDlgItem(hDlg, IDC_BUTTON_LUARUN));
}

bool LuaWindowsAny()
{
	return !s_consoles.empty();
}

void LuaWindowsCloseAll()
{
	// Copy: WM_DESTROY removes entries from the vector.
	std::vector<LuaConsoleWindow *> consoles = s_consoles;
	for (size_t i = 0; i < consoles.size(); ++i)
		::PostMessage(consoles[i]->hDlg, WM_CLOSE, 0, 0);
}

bool LuaWindowStepMessage(MSG *msg)
{
	for (size_t i = 0; i < s_consoles.size(); ++i)
	{
		if (::IsDialogMessage(s_consoles[i]->hDlg, msg))
			return true;
	}

	return false;
}

INT_PTR CALLBACK DlgLuaScriptDialog(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {

	case WM_INITDIALOG:
	{
		LuaConsoleWindow *info = new LuaConsoleWindow();
		info->hDlg	 = hDlg;
		info->script = NULL;
		info->font	 = NULL;
		SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)info);
		s_consoles.push_back(info);

		// remove the 30000 character limit from the console control
		SendMessage(GetDlgItem(hDlg, IDC_LUACONSOLE),EM_LIMITTEXT,0,0);

		RECT r;
		RECT r2;
		GetWindowRect(AfxGetMainWnd()->GetSafeHwnd(), &r);

		int windowIndex = (int)lParam;
		int staggerOffset = windowIndex * 24;
		r.left += staggerOffset;
		r.right += staggerOffset;
		r.top += staggerOffset;
		r.bottom += staggerOffset;

		GetWindowRect(hDlg, &r2);

		// push it away from the main window if we can
		const int width = (r.right-r.left);
		const int width2 = (r2.right-r2.left);
		if(r.left+width2 + width < GetSystemMetrics(SM_CXSCREEN))
		{
			r.right += width;
			r.left += width;
		}
		else if((int)r.left - (int)width2 > 0)
		{
			r.right -= width2;
			r.left -= width2;
		}

		SetWindowPos(hDlg, NULL, r.left, r.top, NULL, NULL, SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);

		RECT r3;
		GetClientRect(hDlg, &r3);
		info->width	 = r3.right - r3.left;
		info->height = r3.bottom - r3.top;

		DragAcceptFiles(hDlg, TRUE);

		// Offer the script the last window opened; a fresh window starts empty.
		if (!s_lastScriptPath.empty())
			SetDlgItemText(hDlg, IDC_EDIT_LUAPATH, s_lastScriptPath.c_str());

		EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_LUASTOP), false);
		return TRUE;
	}	break;

	case WM_SIZE:
	{
		LuaConsoleWindow *info = consoleFor(hDlg);
		if (!info)
			break;

		int prevDlgWidth  = info->width;
		int prevDlgHeight = info->height;

		int dlgWidth  = LOWORD(lParam);
		int dlgHeight = HIWORD(lParam);

		int deltaWidth	= dlgWidth - prevDlgWidth;
		int deltaHeight = dlgHeight - prevDlgHeight;

		for(int i = 0; i < numControlLayoutInfos; i++)
		{
			const ControlLayoutInfo &layoutInfo = controlLayoutInfos[i];
			ControlLayoutState &layoutState = info->layout[i];

			HWND hCtrl = GetDlgItem(hDlg,layoutInfo.controlID);
			if (!hCtrl)
				continue;

			int x,y,width,height;
			if(layoutState.valid)
			{
				x = layoutState.x;
				y = layoutState.y;
				width = layoutState.width;
				height = layoutState.height;
			}
			else
			{
				RECT r;
				GetWindowRect(hCtrl, &r);
				POINT p = {r.left, r.top};
				ScreenToClient(hDlg, &p);
				x = p.x;
				y = p.y;
				width = r.right - r.left;
				height = r.bottom - r.top;
			}

			switch(layoutInfo.horizontalLayout)
			{
				case ControlLayoutInfo::RESIZE_END: width += deltaWidth; break;
				case ControlLayoutInfo::MOVE_START: x += deltaWidth; break;
				default: break;
			}
			switch(layoutInfo.verticalLayout)
			{
				case ControlLayoutInfo::RESIZE_END: height += deltaHeight; break;
				case ControlLayoutInfo::MOVE_START: y += deltaHeight; break;
				default: break;
			}

			SetWindowPos(hCtrl, 0, x,y, width,height, 0);

			layoutState.x = x;
			layoutState.y = y;
			layoutState.width = width;
			layoutState.height = height;
			layoutState.valid = true;
		}

		info->width	 = dlgWidth;
		info->height = dlgHeight;

		RedrawWindow(hDlg, NULL, NULL, RDW_INVALIDATE);
	}	break;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
			case IDOK:
			case IDCANCEL: {
				DestroyWindow(hDlg);
			}	break;

			case IDC_BUTTON_LUARUN:
			{
				if (systemIsEmulating())
				{
					TCHAR filename[MAX_PATH];
					GetDlgItemText(hDlg, IDC_EDIT_LUAPATH, filename, MAX_PATH);

					LuaConsoleWindow *info = consoleFor(hDlg);
					if (!info)
						break;

					// Run doubles as Restart: drop the old engine first, so the
					// new load starts from a clean state (and the window keeps
					// exactly one script).
					if (info->script)
					{
						VBALuaCloseScript(info->script);
						info->script = NULL;
					}

					s_lastScriptPath = filename;
					info->script = VBALuaOpenScript(winAnsiFromWide(filename), (std::intptr_t)hDlg);
				}
				else
				{
					// The engine needs a game to run against, and refusing
					// here used to be the other silent click: nothing ran and
					// nothing said why.
					PrintToWindowConsole((std::intptr_t)hDlg, lua_host::TextEncoding::Ansi,
					                     "No game is running - load a ROM first.\r\n");
				}
			}	break;

			case IDC_BUTTON_LUASTOP:
			{
				LuaConsoleWindow *info = consoleFor(hDlg);
				if (info && info->script)
				{
					// Keep the object: the window still owns it, and Run has to
					// be able to start it again.
					VBALuaStopScript(info->script);
				}
			}	break;

			case IDC_BUTTON_LUAEDIT:
			{
				TCHAR Str_Tmp[1024];
				SendDlgItemMessage(hDlg,IDC_EDIT_LUAPATH,WM_GETTEXT,(WPARAM)512,(LPARAM)Str_Tmp);
				// tell the OS to open the file with its associated editor,
				// without blocking on it or leaving a command window open.
				if((INT_PTR)ShellExecute(NULL, _T("edit"), Str_Tmp, NULL, NULL, SW_SHOWNORMAL) == SE_ERR_NOASSOC)
					if((INT_PTR)ShellExecute(NULL, _T("open"), Str_Tmp, NULL, NULL, SW_SHOWNORMAL) == SE_ERR_NOASSOC)
						ShellExecute(NULL, NULL, _T("notepad"), Str_Tmp, NULL, SW_SHOWNORMAL);
			}	break;

			case IDC_BUTTON_LUABROWSE:
			{
				systemSoundClearBuffer();

				CString filter = winResLoadFilter(IDS_FILTER_LUA);
				CString title  = winResLoadString(IDS_SELECT_LUA_NAME);

				CString luaName = winGetDestFilename(theApp.gameFilename, IDS_LUA_DIR, ".lua");
				CString luaDir  = winGetDestDir(IDS_LUA_DIR);

				WinFileDialogParams params;
				params.owner       = hDlg;
				params.save        = false;
				params.mustExist   = true;
				params.title       = title;
				params.initialDir  = luaDir;
				params.fileName    = luaName;
				params.filter      = filter;
				params.filterIndex = 1;
				params.defaultExt  = L"lua";

				std::wstring path;
				if (winFileDialog(params, path) == IDOK)
				{
					SetWindowText(GetDlgItem(hDlg, IDC_EDIT_LUAPATH), path.c_str());
				}
				return TRUE;
			}	break;

			case IDC_EDIT_LUAPATH:
			{
				TCHAR filename[MAX_PATH];
				GetDlgItemText(hDlg, IDC_EDIT_LUAPATH, filename, MAX_PATH);
				FILE* file = _wfopen(filename, L"rb");
				EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_LUAEDIT), file != NULL);
				if(file)
					fclose(file);
			}	break;

			case IDC_LUACONSOLE_CHOOSEFONT:
			{
				LuaConsoleWindow *info = consoleFor(hDlg);
				if (!info)
					break;

				LOGFONT logFont;
				SystemParametersInfo(SPI_GETICONTITLELOGFONT, sizeof(LOGFONT), &logFont, 0);

				CHOOSEFONT cf;
				ZeroMemory(&cf, sizeof(cf));
				cf.lStructSize = sizeof(CHOOSEFONT);
				cf.hwndOwner = hDlg;
				cf.lpLogFont = &logFont;
				cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;
				if (ChooseFont(&cf)) {
					if (info->font) {
						DeleteObject(info->font);
						info->font = NULL;
					}
					info->font = CreateFontIndirect(&logFont);
					if (info->font)
						SendDlgItemMessage(hDlg, IDC_LUACONSOLE, WM_SETFONT, (WPARAM)info->font, 0);
				}
			}	break;

			case IDC_LUACONSOLE_CLEAR:
			{
				SetWindowText(GetDlgItem(hDlg, IDC_LUACONSOLE), _T(""));
			}	break;

			case IDC_LUACONSOLE_ABOUT:
			{
				CString text;

				// The engine says what it is: see VBALuaEngineVersion, which
				// reads the version out of the lua51.dll that is loaded, and
				// VBALuaEngineFileVersion, which adds that DLL's own file
				// version when it carries one, which is where a stock Lua 5.1
				// build states its patch level.
				text.Format(_T(VBA_NAME_AND_VERSION) _T(" Lua scripting\r\n\r\n")
				            "Engine  : %hs\r\n"
				            "Lua DLL : %hs",
				            VBALuaEngineVersion(), VBALuaEnginePath());

				CString fileVersion = VBALuaEngineFileVersion();
				if (!fileVersion.IsEmpty())
					text.AppendFormat(_T("\r\nFile ver: %s"), (LPCTSTR)fileVersion);

				CString title = winResLoadString(IDS_LUA_ABOUT_TITLE);
				MessageBox(hDlg, text, title, MB_OK | MB_ICONINFORMATION);
			}	break;
		}
		break;

	case WM_CLOSE: {
		DestroyWindow(hDlg);
		return TRUE;
	}	break;

	case WM_DESTROY: {
		LuaConsoleWindow *info = consoleFor(hDlg);
		if (info)
		{
			// Closing the window closes the script: the engine must not keep a
			// state alive whose console is gone.  WinLuaOnStop() copes with the
			// controls already being destroyed.
			if (info->script)
			{
				VBALuaCloseScript(info->script);
				info->script = NULL;
			}

			SetWindowLongPtr(hDlg, GWLP_USERDATA, 0);

			for (size_t i = 0; i < s_consoles.size(); ++i)
			{
				if (s_consoles[i] == info)
				{
					s_consoles.erase(s_consoles.begin() + i);
					break;
				}
			}

			if (info->font)
				DeleteObject(info->font);
			delete info;
		}

		DragAcceptFiles(hDlg, FALSE);
	}	break;

	case WM_DROPFILES: {
		HDROP hDrop;
		UINT fileCount;
		TCHAR filename[_MAX_PATH];

		hDrop = (HDROP)wParam;
		fileCount = DragQueryFile(hDrop, 0xFFFFFFFF, NULL, 0);
		if (fileCount > 0) {
			DragQueryFile(hDrop, 0, filename, _MAX_PATH);
			SetWindowText(GetDlgItem(hDlg, IDC_EDIT_LUAPATH), filename);
		}
		DragFinish(hDrop);
		return TRUE;
	}	break;

	}

	return FALSE;

}
