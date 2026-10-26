#ifndef VBA_WIN32_WINRESUTIL_H
#define VBA_WIN32_WINRESUTIL_H

#pragma once

extern HMENU winResLoadMenu(LPCTSTR menuName);
extern int winResDialogBox(LPCTSTR boxName, HWND parent, DLGPROC dlgProc);
extern int winResDialogBox(LPCTSTR boxName, HWND parent, DLGPROC dlgProc, LPARAM lParam);
extern CString winResLoadString(UINT id);
extern CString winResLoadFilter(UINT id);

// The version of the English resource script a module carries: the SHA-256 of
// src/win32/vba.rc, or an empty string when the module has none.  A language
// module is only loaded when its value matches the executable's, because a
// module made from another revision can be missing strings, controls or whole
// dialogs this build expects.
extern CString winResLoadL10nVersion(HINSTANCE module);
extern bool winResL10nVersionMatches(HINSTANCE module);

// Text the on screen display shows: always the English the executable carries,
// whatever the UI language is, because the display's fonts cannot draw the
// characters of a translation.
extern CString winResLoadOsdString(UINT id);

// The save dialog for an image capture.  It offers every format WIC can write
// (see ImageWrite.h), with the capture format picked in the Options menu
// selected, and the user may save in any of them.  The pick becomes the capture
// format so the extension of the file and what the writer puts in it agree.
// `initialName` may or may not carry the extension yet.  Returns the picked
// path, or an empty string when the user cancelled.
extern CString winImageSaveDlg(CWnd *parent, const CString &initialName,
                               const CString &initialDir, const CString &title);

#endif // VBA_WIN32_WINRESUTIL_H
