#if !defined(VBA_WIN32_MODERNFILEDIALOG_H_INCLUDED)
#define VBA_WIN32_MODERNFILEDIALOG_H_INCLUDED

#pragma once

#include <windows.h>
#include <string>

// The file and folder dialogs of Windows Vista and later.
//
// The shell dialogs (IFileOpenDialog / IFileSaveDialog / FOS_PICKFOLDERS) are
// the ones every other application shows since Windows Vista; the legacy
// GetOpenFileName family is only kept as a fall-back for hosts where the
// shell dialogs cannot be created at all.

struct WinFileDialogParams
{
	WinFileDialogParams()
		: owner(NULL), save(false), mustExist(true), hideReadOnly(true),
		  title(NULL), initialDir(NULL), fileName(NULL), filter(NULL),
		  filterIndex(1), defaultExt(NULL), extensions(NULL)
	{
	}

	HWND        owner;         // window the dialog is centered on, may be NULL
	bool        save;          // save dialog instead of an open dialog
	bool        mustExist;     // open dialogs: the picked file has to exist
	bool        hideReadOnly;  // legacy dialogs only: no read-only check box
	const wchar_t *title;         // may be NULL
	const wchar_t *initialDir;    // may be NULL
	const wchar_t *fileName;      // initial name, may be NULL or empty
	const wchar_t *filter;        // "Description|pattern|Description|pattern|"
	int         filterIndex;   // 1 based, as OPENFILENAME.nFilterIndex
	const wchar_t *defaultExt;    // without the leading dot, may be NULL
	const wchar_t *const *extensions; // see below
};

// Shows the dialog and waits for the user.  Returns IDOK when a file was
// picked (path then holds it, filterIndex the selected filter) and IDCANCEL
// when the user cancelled.
//
// filterIndex may be NULL.  extensions, when given, is a NULL terminated
// array of the extensions matching the entries of filter (".sgm" style); the
// save dialog keeps the extension of an already typed name in sync with the
// selected file type, the way the legacy common dialog did.
int winFileDialog(const WinFileDialogParams &params, std::wstring &path,
                  int *filterIndex = NULL);

// Shows the folder picker.  Returns false when the user cancelled.
//
// This stays wide like winFileDialog above: it is shell UI, so a folder whose
// name the code page cannot hold must not be squeezed into an ANSI string.
bool winBrowseForFolder(HWND owner, const wchar_t *title, const wchar_t *initialDir,
                        std::wstring &path);

#endif // !defined(VBA_WIN32_MODERNFILEDIALOG_H_INCLUDED)
