// LuaOpenDialog.cpp : implementation file
//

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
#include "../../version.h"

HWND LuaConsoleHWnd = NULL;
HFONT hFont = NULL;
LOGFONT LuaConsoleLogFont;

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
	int x,y,width,height;
	bool valid;
	ControlLayoutState() : valid(false) {}
};

static ControlLayoutInfo controlLayoutInfos [] = {
	{IDC_LUACONSOLE, ControlLayoutInfo::RESIZE_END, ControlLayoutInfo::RESIZE_END},
	{IDC_EDIT_LUAPATH, ControlLayoutInfo::RESIZE_END, ControlLayoutInfo::NONE},
	{IDC_BUTTON_LUARUN, ControlLayoutInfo::MOVE_START, ControlLayoutInfo::NONE},
	{IDC_BUTTON_LUASTOP, ControlLayoutInfo::MOVE_START, ControlLayoutInfo::NONE},
};
static const int numControlLayoutInfos = sizeof(controlLayoutInfos)/sizeof(*controlLayoutInfos);

struct {
	int width; int height;
	ControlLayoutState layoutState [numControlLayoutInfos];
} windowInfo;

void PrintToWindowConsole(std::intptr_t hDlgAsInt, const char* str)
{
	HWND hDlg = (HWND)(std::intptr_t)hDlgAsInt;
	HWND hConsole = GetDlgItem(hDlg, IDC_LUACONSOLE);

	int length = GetWindowTextLength(hConsole);
	if(length >= 250000)
	{
		// discard first half of text if it's getting too long
		SendMessage(hConsole, EM_SETSEL, 0, length/2);
		SendMessage(hConsole, EM_REPLACESEL, false, (LPARAM)_T(""));
		length = GetWindowTextLength(hConsole);
	}
	SendMessage(hConsole, EM_SETSEL, length, length);

	//LuaPerWindowInfo& info = LuaWindowInfo[hDlg];

	{
		// The console is a Unicode control while the Lua core, like the rest of
		// the emulation code, works with char strings in the ANSI code page.
		// Handing those bytes to EM_REPLACESEL directly made the control read
		// them as UTF-16, which turned every line into garbage.
		CString text = winWideFromAnsi(str);
		SendMessage(hConsole, EM_REPLACESEL, false, (LPARAM)(LPCTSTR)text);
	}
}

void WinLuaOnStart(std::intptr_t hDlgAsInt)
{
	HWND hDlg = (HWND)(std::intptr_t)hDlgAsInt;
	//LuaPerWindowInfo& info = LuaWindowInfo[hDlg];
	//info.started = true;
	EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_LUABROWSE), false); // disable browse while running because it misbehaves if clicked in a frameadvance loop
	EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_LUASTOP), true);
	SetWindowText(GetDlgItem(hDlg, IDC_BUTTON_LUARUN), _T("Restart"));
	SetWindowText(GetDlgItem(hDlg, IDC_LUACONSOLE), _T("")); // clear the console
//	Show_Genesis_Screen(HWnd); // otherwise we might never show the first thing the script draws
}

void WinLuaOnStop(std::intptr_t hDlgAsInt)
{
	HWND hDlg = (HWND)(std::intptr_t)hDlgAsInt;
	//LuaPerWindowInfo& info = LuaWindowInfo[hDlg];

	HWND prevWindow = GetActiveWindow();
	SetActiveWindow(hDlg); // bring to front among other script/secondary windows, since a stopped script will have some message for the user that would be easier to miss otherwise
	if(prevWindow == AfxGetMainWnd()->GetSafeHwnd()) SetActiveWindow(prevWindow);

	//info.started = false;
	EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_LUABROWSE), true);
	EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_LUASTOP), false);
	SetWindowText(GetDlgItem(hDlg, IDC_BUTTON_LUARUN), _T("Run"));
//	if(statusOK)
//		Show_Genesis_Screen(MainWindow->getHWnd()); // otherwise we might never show the last thing the script draws
	//if(info.closeOnStop)
	//	PostMessage(hDlg, WM_CLOSE, 0, 0);
}

INT_PTR CALLBACK DlgLuaScriptDialog(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	RECT r;
	RECT r2;
	int dx1, dy1, dx2, dy2;

	switch (msg) {

	case WM_INITDIALOG:
	{
		// remove the 30000 character limit from the console control
		SendMessage(GetDlgItem(hDlg, IDC_LUACONSOLE),EM_LIMITTEXT,0,0);

		GetWindowRect(AfxGetMainWnd()->GetSafeHwnd(), &r);
		dx1 = (r.right - r.left) / 2;
		dy1 = (r.bottom - r.top) / 2;

		GetWindowRect(hDlg, &r2);
		dx2 = (r2.right - r2.left) / 2;
		dy2 = (r2.bottom - r2.top) / 2;

		int windowIndex = 0;//std::find(LuaScriptHWnds.begin(), LuaScriptHWnds.end(), hDlg) - LuaScriptHWnds.begin();
		int staggerOffset = windowIndex * 24;
		r.left += staggerOffset;
		r.right += staggerOffset;
		r.top += staggerOffset;
		r.bottom += staggerOffset;

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
		windowInfo.width = r3.right - r3.left;
		windowInfo.height = r3.bottom - r3.top;
		for(int i = 0; i < numControlLayoutInfos; i++) {
			ControlLayoutState& layoutState = windowInfo.layoutState[i];
			layoutState.valid = false;
		}

		DragAcceptFiles(hDlg, TRUE);
		SetDlgItemText(hDlg, IDC_EDIT_LUAPATH, winWideFromAnsi(VBAGetLuaScriptName()));

		SystemParametersInfo(SPI_GETICONTITLELOGFONT, sizeof(LOGFONT), &LuaConsoleLogFont, 0); // reset with an acceptable font
		return TRUE;
	}	break;

	case WM_SIZE:
	{
		// resize or move controls in the window as necessary when the window is resized

		//LuaPerWindowInfo& windowInfo = LuaWindowInfo[hDlg];
		int prevDlgWidth = windowInfo.width;
		int prevDlgHeight = windowInfo.height;

		int dlgWidth = LOWORD(lParam);
		int dlgHeight = HIWORD(lParam);

		int deltaWidth = dlgWidth - prevDlgWidth;
		int deltaHeight = dlgHeight - prevDlgHeight;

		for(int i = 0; i < numControlLayoutInfos; i++)
		{
			ControlLayoutInfo layoutInfo = controlLayoutInfos[i];
			ControlLayoutState& layoutState = windowInfo.layoutState[i];

			HWND hCtrl = GetDlgItem(hDlg,layoutInfo.controlID);

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

		windowInfo.width = dlgWidth;
		windowInfo.height = dlgHeight;

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
					VBALoadLuaCode(winAnsiFromWide(filename));
				}
				else
				{
					// The engine needs a game to run against, and refusing
					// here used to be the other silent click: nothing ran and
					// nothing said why.
					PrintToWindowConsole((std::intptr_t)hDlg,
					                     "No game is running - load a ROM first.\r\n");
				}
			}	break;

			case IDC_BUTTON_LUASTOP:
			{
				VBALuaStop();
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
				CHOOSEFONT cf;

				ZeroMemory(&cf, sizeof(cf));
				cf.lStructSize = sizeof(CHOOSEFONT);
				cf.hwndOwner = hDlg;
				cf.lpLogFont = &LuaConsoleLogFont;
				cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;
				if (ChooseFont(&cf)) {
					if (hFont) {
						DeleteObject(hFont);
						hFont = NULL;
					}
					hFont = CreateFontIndirect(&LuaConsoleLogFont);
					if (hFont)
						SendDlgItemMessage(hDlg, IDC_LUACONSOLE, WM_SETFONT, (WPARAM)hFont, 0);
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
		PostMessage(hDlg, WM_DESTROY, 0, 0);
	}	break;

	case WM_DESTROY: {
		//VBALuaStop();
		DragAcceptFiles(hDlg, FALSE);
		if (hFont) {
			DeleteObject(hFont);
			hFont = NULL;
		}
		LuaConsoleHWnd = NULL;
	}	break;

	case WM_DROPFILES: {
		HDROP hDrop;
		//UINT fileNo;
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
