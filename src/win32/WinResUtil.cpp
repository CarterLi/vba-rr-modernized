#include "stdafx.h"
#include "resource.h"
#include "WinResUtil.h"
#include "Dialogs/FileDlg.h"
#include "../common/ImageWrite.h"
#include "../common/Util.h"

#include <string>

static HINSTANCE winResGetInstance(LPCTSTR resType, LPCTSTR resName)
{
	// TODO: make language DLL first
	return AfxFindResourceHandle(resName, resType);
}

// The version is a string table entry, not a version resource: it is written
// by the resource script that also carries the strings, and the translated
// script has its own copy of the value.  LoadString works on a module handle
// whether the module was loaded normally or as a data file.
CString winResLoadL10nVersion(HINSTANCE module)
{
	TCHAR buffer[96];

	const int length = /**/ ::LoadString(module, IDS_L10N_VERSION,
	                                     buffer, (int)_countof(buffer));
	if (length <= 0)
		return CString();

	return CString(buffer, length);
}

bool winResL10nVersionMatches(HINSTANCE module)
{
	// AfxGetResourceHandle() may already be a language module when the user
	// switches language, so the executable's own value has to be read from
	// AfxGetInstanceHandle().
	const CString expected = winResLoadL10nVersion(AfxGetInstanceHandle());
	const CString actual   = winResLoadL10nVersion(module);

	// An old module has no value and neither has an executable built before
	// this check existed.  Neither can be trusted to be in step, so both are
	// refused rather than falling back to a module that may be incomplete.
	return !expected.IsEmpty() && !actual.IsEmpty() &&
	       expected.CompareNoCase(actual) == 0;
}

UCHAR *winResGetResource(LPCTSTR resType, LPCTSTR resName)
{
	HINSTANCE winResInstance = winResGetInstance(resType, resName);

	HRSRC hRsrc = FindResourceEx(winResInstance, resType, resName, 0);

	if (hRsrc != NULL)
	{
		HGLOBAL hGlobal = LoadResource(winResInstance, hRsrc);

		if (hGlobal != NULL)
		{
			UCHAR *b = (UCHAR *)LockResource(hGlobal);

			return b;
		}
	}
	return NULL;
}

HMENU winResLoadMenu(LPCTSTR menuName)
{
	UCHAR *b = winResGetResource(RT_MENU, menuName);

	if (b != NULL)
	{
		HMENU menu = LoadMenuIndirect((CONST MENUTEMPLATE *)b);

		if (menu != NULL)
			return menu;
	}

	return LoadMenu(NULL, menuName);
}

int winResDialogBox(LPCTSTR boxName, HWND parent, DLGPROC dlgProc, LPARAM lParam)
{
	/*
	   UCHAR * b = winResGetResource(RT_DIALOG, boxName);

	   if(b != NULL) {

	   return DialogBoxIndirectParam(hInstance,
	   (LPCDLGTEMPLATE)b,
	   parent,
	   dlgProc,
	   lParam);
	   }

	   return DialogBoxParam(hInstance,
	   boxName,
	   parent,
	   dlgProc,
	   lParam);
	 */
	return 0;
}

int winResDialogBox(LPCTSTR boxName, HWND parent, DLGPROC dlgProc)
{
	return winResDialogBox(boxName,
	                       parent,
	                       dlgProc,
	                       0);
}

// Text from the string table.  CString::LoadString asks AfxFindResourceHandle
// for the module, so a language module that does not carry a string falls back
// to the one in the executable instead of coming back empty.
//
// The underscore is a real character here: a translation is shown as it was
// written.  Only the filter strings use '_' as their separator, and those are
// read through winResLoadFilter below.
CString winResLoadString(UINT id)
{
	CString res;

	if (!res.LoadString(id))
		res = _T("");

	return res;
}

// A filter string is "Description|pattern|Description|pattern|", but '|' in a
// string table entry is what the shell dialog builder reads as a separator, so
// the resources write '_' instead and it is turned back into '|' here.
CString winResLoadFilter(UINT id)
{
	CString res = winResLoadString(id);
	res.Replace('_', '|');

	return res;
}

// The string as the executable carries it, which is English.  Used where a
// translation must not be shown: the built-in 8x8 font of the "in game" and
// "on game" text modes is a table of ASCII bitmaps and nothing else, so a
// translated string would come out as the bytes of the code page.  The trace
// log reads it too, so a log stays readable whatever language the UI is in.
//
// The status bar, the message box and the "on screen" text mode have a Windows
// font that can draw anything, so they use winResLoadString and show the
// translation.  Both renderings of a screen message are prepared together by
// systemScreenMessageRes, which is what most callers want.
CString winResLoadOsdString(UINT id)
{
	CString res;

	// AfxGetInstanceHandle() is the executable; AfxGetResourceHandle() is the
	// language module, once one is loaded.
	if (!res.LoadString(AfxGetInstanceHandle(), id))
		res = _T("");

	return res;
}

CString winImageSaveDlg(CWnd *parent, const CString &initialName,
                        const CString &initialDir, const CString &title)
{
	// Without WIC nothing can be written, so do not open a dialog whose only
	// possible outcome is a failed save.
	if (!imageWriteAvailable())
	{
		utilReportImageSaveFailure(winAnsiFromWide(initialName));
		return CString();
	}

	ImageWriteFormatInfo formats[16];
	const int            count     = imageWriteFormats(formats, 16);
	const int            index     = imageCaptureFormatIndex();
	const CString        extension = imageCaptureExtension();

	// One entry per format WIC can write, so the user may save in any of them,
	// plus the extensions the dialog uses to keep a typed name in sync.  Fixed
	// arrays keep the extension pointers valid for the whole call.
	CString     extensionStorage[17];
	LPCTSTR     extensions[17];
	int         extensionCount = 0;

	CString filter;

	for (int i = 0; i < count && extensionCount < 16; ++i)
	{
		CString label = formats[i].name;
		label.Replace(_T(" Encoder"), _T(""));
		label.Trim();

		CString       pattern;
		const CString list = formats[i].extensions;
		for (int start = 0; start <= list.GetLength(); )
		{
			const int comma = list.Find(',', start);
			CString   entry = comma < 0 ? list.Mid(start)
			                           : list.Mid(start, comma - start);
			entry.Trim();
			if (!entry.IsEmpty())
			{
				if (!pattern.IsEmpty())
					pattern += ";";
				pattern += "*";
				pattern += entry;

				if (extensionStorage[i].IsEmpty())
					extensionStorage[i] = entry;
			}

			if (comma < 0)
				break;
			start = comma + 1;
		}

		extensions[extensionCount++] = extensionStorage[i];

		CString item;
		item.Format(_T("%s Image|%s|"), (LPCTSTR)label, (LPCTSTR)pattern);
		filter += item;
	}

	extensions[extensionCount] = NULL;
	filter += "All Files|*.*|";

	CString name = initialName;
	if (!name.IsEmpty() && !extension.IsEmpty() &&
	    name.Right(extension.GetLength()).CompareNoCase(extension) != 0)
		name += extension;

	FileDlg dlg(parent,
	            name,
	            filter,
	            index >= 0 ? index + 1 : 1,
	            extension,
	            extensionCount > 0 ? extensions : NULL,
	            initialDir,
	            title,
	            true);

	if (dlg.DoModal() == IDCANCEL)
		return CString();

	// The user may have picked another format; make it the capture format so the
	// extension of the file and what the writer puts in it stay in step.
	const int picked = dlg.getFilterIndex();
	if (picked >= 1 && picked <= count)
		imageCaptureSelectIndex(picked - 1);

	return dlg.GetPathName();
}
