// ModernFileDialog.cpp : the file and folder dialogs of Windows Vista and
// later, with the legacy common dialogs kept for Windows XP.
//
// The Vista+ shell dialogs (IFileOpenDialog / IFileSaveDialog) are only
// compiled into the build that may assume a Vista or later desktop.  A WINXP
// build leaves them out entirely and keeps using GetOpenFileName /
// GetSaveFileName / SHBrowseForFolder, the same way src/filters turns the
// SIMD paths off for that build.  At run time a non-WINXP build still falls
// back to the legacy dialogs whenever the shell ones cannot be created (a
// thread that already lives in a multithreaded apartment, for instance).
//
// shobjidl.h only declares IFileOpenDialog from _WIN32_WINNT 0x0600 on.  This
// translation unit includes windows.h directly instead of stdafx.h, so it
// sets the target level itself; the project targets Windows 10 throughout.

#if defined(WINXP)
#	define _WIN32_WINNT 0x0501
#	define WINVER       0x0501
#else
#	define _WIN32_WINNT 0x0A00
#	define WINVER       0x0A00
#endif

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commdlg.h>
#include <dlgs.h>
#include <shlobj.h>

#if !defined(WINXP)
#	include <shobjidl.h>
#endif

#include <string>
#include <vector>
#include <cstring>

#include "ModernFileDialog.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

#if !defined(WINXP)
#	pragma comment(lib, "uuid.lib")
#endif

namespace {

// ---------------------------------------------------------------------------
// legacy common dialogs: the Windows XP path, and the run time fall-back of
// every other build
// ---------------------------------------------------------------------------

const wchar_t *const *g_legacyExtensions = NULL;

void legacySyncExtension(HWND dialog, int selected)
{
	if (g_legacyExtensions == NULL)
		return;

	HWND edit = ::GetDlgItem(dialog, cmb13);
	if (edit == NULL)
		edit = ::GetDlgItem(dialog, edt1);
	if (edit == NULL)
		return;

	int count = 0;
	while (g_legacyExtensions[count] != NULL)
		count++;

	const wchar_t *extension = L"";
	if (selected >= 0 && selected < count && g_legacyExtensions[selected] != NULL)
		extension = g_legacyExtensions[selected];

	wchar_t text[MAX_PATH] = L"";
	::GetWindowTextW(edit, text, MAX_PATH);

	std::wstring name;
	if (text[0] == L'\0')
	{
		if (*extension == L'\0')
			return;
		name = std::wstring(L"*") + extension;
	}
	else
	{
		name = text;
		size_t dot = name.find(L'.');
		name = (dot == std::wstring::npos) ? name + extension : name.substr(0, dot) + extension;
	}

	::SetWindowTextW(edit, name.c_str());
}

UINT_PTR CALLBACK legacyHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM)
{
	if (g_legacyExtensions == NULL)
		return 0;

	if (msg == WM_COMMAND && HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == cmb1)
	{
		HWND combo = ::GetDlgItem(hwnd, cmb1);
		int selected = combo ? static_cast<int>(::SendMessage(combo, CB_GETCURSEL, 0, 0)) : -1;
		legacySyncExtension(hwnd, selected);
		return 1;
	}
	return 0;
}

int legacyFileDialog(const WinFileDialogParams &params, std::wstring &path, int *filterIndex)
{
	std::wstring filter;
	if (params.filter != NULL)
	{
		filter = params.filter;
		for (size_t i = 0; i < filter.size(); i++)
		{
			if (filter[i] == L'|')
				filter[i] = L'\0';
		}
	}

	std::vector<wchar_t> buffer(4 * MAX_PATH, L'\0');
	if (params.fileName != NULL)
	{
		size_t length = wcslen(params.fileName);
		if (length >= buffer.size())
			length = buffer.size() - 1;
		memcpy(&buffer[0], params.fileName, length * sizeof(wchar_t));
	}

	OPENFILENAMEW ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize     = sizeof(ofn);
	ofn.hwndOwner       = params.owner;
	ofn.lpstrFile       = &buffer[0];
	ofn.nMaxFile        = static_cast<DWORD>(buffer.size());
	ofn.lpstrFilter     = filter.empty() ? NULL : filter.c_str();
	ofn.nFilterIndex    = params.filterIndex < 1 ? 1 : static_cast<DWORD>(params.filterIndex);
	ofn.lpstrInitialDir = params.initialDir;
	ofn.lpstrTitle      = params.title;
	ofn.lpstrDefExt     = params.defaultExt;
	ofn.lpfnHook        = legacyHook;
	ofn.Flags           = OFN_ENABLESIZING | OFN_EXPLORER | OFN_ENABLEHOOK | OFN_PATHMUSTEXIST;

	if (params.save)
		ofn.Flags |= OFN_OVERWRITEPROMPT;
	else if (params.mustExist)
		ofn.Flags |= OFN_FILEMUSTEXIST;

	if (params.hideReadOnly)
		ofn.Flags |= OFN_HIDEREADONLY;

	const wchar_t *const *previous = g_legacyExtensions;
	g_legacyExtensions = params.extensions;

	BOOL picked = params.save ? ::GetSaveFileNameW(&ofn) : ::GetOpenFileNameW(&ofn);

	g_legacyExtensions = previous;

	if (!picked)
		return IDCANCEL;

	path = &buffer[0];
	if (filterIndex != NULL)
		*filterIndex = static_cast<int>(ofn.nFilterIndex);

	return IDOK;
}

int CALLBACK browseCallback(HWND hwnd, UINT msg, LPARAM, LPARAM data)
{
	if (msg == BFFM_INITIALIZED)
	{
		const wchar_t *dir = reinterpret_cast<const wchar_t *>(data);
		if (dir != NULL && *dir != L'\0')
			::SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
	}
	return 0;
}

bool legacyBrowseForFolder(HWND owner, const wchar_t *title, const wchar_t *initialDir, std::wstring &path)
{
	BROWSEINFOW info;
	ZeroMemory(&info, sizeof(info));
	info.hwndOwner = owner;
	info.lpszTitle = title;
	info.pidlRoot  = NULL;
	info.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
	info.lpfn      = browseCallback;
	info.lParam    = reinterpret_cast<LPARAM>(initialDir);

	LPITEMIDLIST idList = ::SHBrowseForFolderW(&info);
	if (idList == NULL)
		return false;

	wchar_t buffer[MAX_PATH] = L"";
	bool picked = ::SHGetPathFromIDListW(idList, buffer) != FALSE;
	::CoTaskMemFree(idList);

	if (!picked)
		return false;

	path = buffer;
	return true;
}

#if !defined(WINXP)

// ---------------------------------------------------------------------------
// the shell dialogs of Windows Vista and later
// ---------------------------------------------------------------------------

// Both kinds of folder picker hand the picked path back as wchar_t, and take
// the title and start directory the same way: the folder picker is shell UI,
// so it does not narrow anything the way the emulation core's file names are
// narrowed at their own boundary.

// "Description|pattern|Description|pattern|" -> COMDLG_FILTERSPEC pairs,
// leaving out the empty pairs the trailing separators of the resource
// strings produce.
void buildFilterSpecs(const wchar_t *filter,
                      std::vector<std::wstring> &storage,
                      std::vector<COMDLG_FILTERSPEC> &specs)
{
	if (filter == NULL || *filter == L'\0')
		return;

	for (const wchar_t *start = filter; ; )
	{
		const wchar_t *end = start;
		while (*end != L'\0' && *end != L'|')
			++end;
		storage.push_back(std::wstring(start, end - start));
		if (*end == L'\0')
		{
			break;
		}
		start = end + 1;
	}

	// storage is complete now, so the pointers stay valid while the dialog
	// is up.
	for (size_t i = 0; i + 1 < storage.size(); i += 2)
	{
		if (storage[i].empty() && storage[i + 1].empty())
			continue;

		COMDLG_FILTERSPEC spec;
		spec.pszName = storage[i].c_str();
		spec.pszSpec = storage[i + 1].c_str();
		specs.push_back(spec);
	}
}

void splitPath(const std::wstring &path, std::wstring &dir, std::wstring &leaf)
{
	size_t slash = path.find_last_of(L"\\/");
	if (slash == std::wstring::npos)
	{
		dir.clear();
		leaf = path;
		return;
	}

	dir  = path.substr(0, slash);
	leaf = path.substr(slash + 1);
}

// Keeps the extension in the file name box in sync with the selected file
// type, which is what the old subclassed common dialog did by hand.
class FileDialogEvents : public IFileDialogEvents
{
public:
	FileDialogEvents(bool save, const wchar_t *const *extensions, int count)
		: m_ref(1), m_save(save), m_updating(false)
	{
		for (int i = 0; i < count; i++)
			m_extensions.push_back(extensions[i] ? extensions[i] : L"");
	}

	STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
	{
		if (ppv == NULL)
			return E_POINTER;

		*ppv = NULL;
		if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IFileDialogEvents))
			*ppv = static_cast<IFileDialogEvents *>(this);

		if (*ppv == NULL)
			return E_NOINTERFACE;

		AddRef();
		return S_OK;
	}

	STDMETHODIMP_(ULONG) AddRef()
	{
		return static_cast<ULONG>(InterlockedIncrement(&m_ref));
	}

	STDMETHODIMP_(ULONG) Release()
	{
		LONG ref = InterlockedDecrement(&m_ref);
		if (ref == 0)
			delete this;
		return static_cast<ULONG>(ref);
	}

	STDMETHODIMP OnFileOk(IFileDialog *) { return S_OK; }
	STDMETHODIMP OnFolderChanging(IFileDialog *, IShellItem *) { return S_OK; }
	STDMETHODIMP OnFolderChange(IFileDialog *) { return S_OK; }
	STDMETHODIMP OnSelectionChange(IFileDialog *) { return S_OK; }
	STDMETHODIMP OnShareViolation(IFileDialog *, IShellItem *, FDE_SHAREVIOLATION_RESPONSE *)
	{
		return E_NOTIMPL;
	}
	STDMETHODIMP OnOverwrite(IFileDialog *, IShellItem *, FDE_OVERWRITE_RESPONSE *)
	{
		return E_NOTIMPL;
	}

	STDMETHODIMP OnTypeChange(IFileDialog *dialog)
	{
		if (m_updating)
			return S_OK;

		UINT index = 0;
		if (FAILED(dialog->GetFileTypeIndex(&index)))
			return S_OK;

		std::wstring extension;
		if (index >= 1 && index <= static_cast<UINT>(m_extensions.size()))
			extension = m_extensions[index - 1];

		if (extension.empty())
			return S_OK;

		std::wstring bare = extension;
		if (bare[0] == L'.')
			bare = bare.substr(1);
		if (!bare.empty())
			dialog->SetDefaultExtension(bare.c_str());

		// Only a save dialog has a name to keep in sync; an open dialog
		// filters by type on its own.
		if (!m_save)
			return S_OK;

		LPWSTR name = NULL;
		if (FAILED(dialog->GetFileName(&name)) || name == NULL)
			return S_OK;

		std::wstring file(name);
		::CoTaskMemFree(name);

		if (file.empty())
			return S_OK;

		size_t dot   = file.find_last_of(L'.');
		size_t slash = file.find_last_of(L"\\/");
		bool hasExtension = dot != std::wstring::npos && dot > 0 &&
		                    (slash == std::wstring::npos || dot > slash);

		std::wstring updated = (hasExtension ? file.substr(0, dot) : file) + extension;
		if (updated != file)
		{
			m_updating = true;
			dialog->SetFileName(updated.c_str());
			m_updating = false;
		}

		return S_OK;
	}

private:
	LONG m_ref;
	bool m_save;
	bool m_updating;
	std::vector<std::wstring> m_extensions;
};

// IDOK / IDCANCEL, or -1 when the shell dialog could not be used at all.
int vistaFileDialog(const WinFileDialogParams &params, std::wstring &path, int *filterIndex)
{
	// The shell dialogs need a single threaded apartment of their own.
	HRESULT hr = ::CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (hr == RPC_E_CHANGED_MODE)
		return -1;

	const bool uninitialize = SUCCEEDED(hr);

	IFileDialog *dialog = NULL;
	hr = ::CoCreateInstance(params.save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
	                        NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
	if (FAILED(hr) || dialog == NULL)
	{
		if (uninitialize)
			::CoUninitialize();
		return -1;
	}

	FILEOPENDIALOGOPTIONS options = 0;
	dialog->GetOptions(&options);
	options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
	if (params.save)
	{
		options |= FOS_OVERWRITEPROMPT;
	}
	else
	{
		options |= FOS_PATHMUSTEXIST;
		if (params.mustExist)
			options |= FOS_FILEMUSTEXIST;
	}
	dialog->SetOptions(options);

	if (params.title != NULL && *params.title != L'\0')
		dialog->SetTitle(params.title);

	std::wstring initialDir = params.initialDir ? params.initialDir : L"";
	std::wstring fileName   = params.fileName ? params.fileName : L"";
	if (!fileName.empty())
	{
		std::wstring dir;
		std::wstring leaf;
		splitPath(fileName, dir, leaf);
		if (!leaf.empty())
			dialog->SetFileName(leaf.c_str());
		if (initialDir.empty())
			initialDir = dir;
	}

	if (!initialDir.empty())
	{
		IShellItem *folder = NULL;
		if (SUCCEEDED(::SHCreateItemFromParsingName(initialDir.c_str(), NULL, IID_PPV_ARGS(&folder))) &&
		    folder != NULL)
		{
			dialog->SetFolder(folder);
			folder->Release();
		}
	}

	if (params.defaultExt != NULL && *params.defaultExt != L'\0')
	{
		std::wstring extension = params.defaultExt;
		while (!extension.empty() && extension[0] == L'.')
			extension = extension.substr(1);
		if (!extension.empty())
			dialog->SetDefaultExtension(extension.c_str());
	}

	std::vector<std::wstring> filterStorage;
	std::vector<COMDLG_FILTERSPEC> filterSpecs;
	buildFilterSpecs(params.filter, filterStorage, filterSpecs);
	if (!filterSpecs.empty() && SUCCEEDED(dialog->SetFileTypes(static_cast<UINT>(filterSpecs.size()),
	                                                          &filterSpecs[0])))
	{
		UINT index = params.filterIndex < 1 ? 1 : static_cast<UINT>(params.filterIndex);
		if (index > static_cast<UINT>(filterSpecs.size()))
			index = static_cast<UINT>(filterSpecs.size());
		dialog->SetFileTypeIndex(index);
	}

	int extensionCount = 0;
	if (params.extensions != NULL)
	{
		while (params.extensions[extensionCount] != NULL)
			extensionCount++;
	}

	FileDialogEvents *events = new FileDialogEvents(params.save, params.extensions, extensionCount);
	DWORD cookie = 0;
	if (FAILED(dialog->Advise(events, &cookie)))
		cookie = 0;

	hr = dialog->Show(params.owner);

	if (cookie != 0)
		dialog->Unadvise(cookie);
	events->Release();

	if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
	{
		dialog->Release();
		if (uninitialize)
			::CoUninitialize();
		return IDCANCEL;
	}

	if (FAILED(hr))
	{
		dialog->Release();
		if (uninitialize)
			::CoUninitialize();
		return -1;
	}

	std::wstring picked;
	IShellItem *item = NULL;
	if (SUCCEEDED(dialog->GetResult(&item)) && item != NULL)
	{
		LPWSTR display = NULL;
		if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &display)) && display != NULL)
		{
			picked = display;
			::CoTaskMemFree(display);
		}
		item->Release();
	}

	UINT typeIndex = 0;
	if (SUCCEEDED(dialog->GetFileTypeIndex(&typeIndex)) && filterIndex != NULL)
		*filterIndex = static_cast<int>(typeIndex);

	dialog->Release();
	if (uninitialize)
		::CoUninitialize();

	if (picked.empty())
		return IDCANCEL;

	path = picked;
	return IDOK;
}

// 1 when a folder was picked, 0 when the user cancelled, -1 when the shell
// dialog could not be used at all.
int vistaBrowseForFolder(HWND owner, const wchar_t *title, const wchar_t *initialDir, std::wstring &path)
{
	HRESULT hr = ::CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (hr == RPC_E_CHANGED_MODE)
		return -1;

	const bool uninitialize = SUCCEEDED(hr);

	IFileOpenDialog *dialog = NULL;
	hr = ::CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
	if (FAILED(hr) || dialog == NULL)
	{
		if (uninitialize)
			::CoUninitialize();
		return -1;
	}

	FILEOPENDIALOGOPTIONS options = 0;
	dialog->GetOptions(&options);
	options |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
	dialog->SetOptions(options);

	if (title != NULL && *title != L'\0')
		dialog->SetTitle(title);

	if (initialDir != NULL && *initialDir != L'\0')
	{
		IShellItem *folder = NULL;
		if (SUCCEEDED(::SHCreateItemFromParsingName(initialDir, NULL, IID_PPV_ARGS(&folder))) &&
		    folder != NULL)
		{
			dialog->SetFolder(folder);
			folder->Release();
		}
	}

	hr = dialog->Show(owner);

	if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
	{
		dialog->Release();
		if (uninitialize)
			::CoUninitialize();
		return 0;
	}

	std::wstring picked;
	if (SUCCEEDED(hr))
	{
		IShellItem *item = NULL;
		if (SUCCEEDED(dialog->GetResult(&item)) && item != NULL)
		{
			LPWSTR display = NULL;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &display)) && display != NULL)
			{
				picked = display;
				::CoTaskMemFree(display);
			}
			item->Release();
		}
	}

	dialog->Release();
	if (uninitialize)
		::CoUninitialize();

	if (picked.empty())
		return FAILED(hr) ? -1 : 0;

	path = picked;
	return 1;
}

#endif // !defined(WINXP)

} // namespace

int winFileDialog(const WinFileDialogParams &params, std::wstring &path, int *filterIndex)
{
	path.clear();
	if (filterIndex != NULL)
		*filterIndex = params.filterIndex;

#if !defined(WINXP)
	int result = vistaFileDialog(params, path, filterIndex);
	if (result >= 0)
		return result;
#endif

	return legacyFileDialog(params, path, filterIndex);
}

bool winBrowseForFolder(HWND owner, const wchar_t *title, const wchar_t *initialDir,
                        std::wstring &path)
{
	path.clear();

#if !defined(WINXP)
	int result = vistaBrowseForFolder(owner, title, initialDir, path);
	if (result >= 0)
		return result != 0;
#endif

	return legacyBrowseForFolder(owner, title, initialDir, path);
}
