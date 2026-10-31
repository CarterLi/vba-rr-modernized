#include "stdafx.h"

#include "BitmapFont.h"
#include "WinText.h"

#include "../common/Text.h"

namespace {

struct FontEntry
{
	const TCHAR *name;     // the full name, which is what the settings file stores
	const TCHAR *family;   // the group the dialog lists it under
	const TCHAR *size;     // the label it has inside that group
	const TCHAR *resource; // the RCDATA resource holding it
};

// DeSmuME's HUD font list (aggdraw.cpp, fonts_list), in its order and under
// its names, so a user who knows one of them finds the same thing here.  The
// resources are in vba.rc and the bytes in resources/font.
//
// A face and its sizes are one family here: "gse" offers 4x6 up to 8x16, and
// the bold cuts sit next to the plain ones.  That is how DeSmuME's own font
// menu is laid out, and it keeps 34 entries out of the font combo.
const FontEntry fontEntries[] =
{
	{ _T("gse4x6"),               _T("gse"), _T("4x6"),               _T("FONT_GSE4X6") },
	{ _T("gse4x8"),               _T("gse"), _T("4x8"),               _T("FONT_GSE4X8") },
	{ _T("gse5x7"),               _T("gse"), _T("5x7"),               _T("FONT_GSE5X7") },
	{ _T("gse5x9"),               _T("gse"), _T("5x9"),               _T("FONT_GSE5X9") },
	{ _T("gse6x9"),               _T("gse"), _T("6x9"),               _T("FONT_GSE6X9") },
	{ _T("gse6x12"),              _T("gse"), _T("6x12"),              _T("FONT_GSE6X12") },
	{ _T("gse7x11"),              _T("gse"), _T("7x11"),              _T("FONT_GSE7X11") },
	{ _T("gse7x11_bold"),         _T("gse"), _T("7x11_bold"),         _T("FONT_GSE7X11_BOLD") },
	{ _T("gse7x15"),              _T("gse"), _T("7x15"),              _T("FONT_GSE7X15") },
	{ _T("gse7x15_bold"),         _T("gse"), _T("7x15_bold"),         _T("FONT_GSE7X15_BOLD") },
	{ _T("gse8x16"),              _T("gse"), _T("8x16"),              _T("FONT_GSE8X16") },
	{ _T("gse8x16_bold"),         _T("gse"), _T("8x16_bold"),         _T("FONT_GSE8X16_BOLD") },
	{ _T("mcs11_prop"),           _T("mcs"), _T("11_prop"),           _T("FONT_MCS11_PROP") },
	{ _T("mcs11_prop_condensed"), _T("mcs"), _T("11_prop_condensed"), _T("FONT_MCS11_PROP_CONDENSED") },
	{ _T("mcs12_prop"),           _T("mcs"), _T("12_prop"),           _T("FONT_MCS12_PROP") },
	{ _T("mcs13_prop"),           _T("mcs"), _T("13_prop"),           _T("FONT_MCS13_PROP") },
	{ _T("mcs5x10_mono"),         _T("mcs"), _T("5x10_mono"),         _T("FONT_MCS5X10_MONO") },
	{ _T("mcs5x11_mono"),         _T("mcs"), _T("5x11_mono"),         _T("FONT_MCS5X11_MONO") },
	{ _T("mcs6x10_mono"),         _T("mcs"), _T("6x10_mono"),         _T("FONT_MCS6X10_MONO") },
	{ _T("mcs6x11_mono"),         _T("mcs"), _T("6x11_mono"),         _T("FONT_MCS6X11_MONO") },
	{ _T("mcs7x12_mono_high"),    _T("mcs"), _T("7x12_mono_high"),    _T("FONT_MCS7X12_MONO_HIGH") },
	{ _T("mcs7x12_mono_low"),     _T("mcs"), _T("7x12_mono_low"),     _T("FONT_MCS7X12_MONO_LOW") },
	{ _T("verdana12"),            _T("verdana"), _T("12"),            _T("FONT_VERDANA12") },
	{ _T("verdana12_bold"),       _T("verdana"), _T("12_bold"),       _T("FONT_VERDANA12_BOLD") },
	{ _T("verdana13"),            _T("verdana"), _T("13"),            _T("FONT_VERDANA13") },
	{ _T("verdana13_bold"),       _T("verdana"), _T("13_bold"),       _T("FONT_VERDANA13_BOLD") },
	{ _T("verdana14"),            _T("verdana"), _T("14"),            _T("FONT_VERDANA14") },
	{ _T("verdana14_bold"),       _T("verdana"), _T("14_bold"),       _T("FONT_VERDANA14_BOLD") },
	{ _T("verdana16"),            _T("verdana"), _T("16"),            _T("FONT_VERDANA16") },
	{ _T("verdana16_bold"),       _T("verdana"), _T("16_bold"),       _T("FONT_VERDANA16_BOLD") },
	{ _T("verdana17"),            _T("verdana"), _T("17"),            _T("FONT_VERDANA17") },
	{ _T("verdana17_bold"),       _T("verdana"), _T("17_bold"),       _T("FONT_VERDANA17_BOLD") },
	{ _T("verdana18"),            _T("verdana"), _T("18"),            _T("FONT_VERDANA18") },
	{ _T("verdana18_bold"),       _T("verdana"), _T("18_bold"),       _T("FONT_VERDANA18_BOLD") },
};

const int fontEntryCount = (int)(sizeof(fontEntries) / sizeof(fontEntries[0]));

TextFont fonts[fontEntryCount];
bool fontsLoaded = false;

// Fonts live in the executable, not in the language module, so the resource
// handle is the process one.  A translation only carries strings and dialogs,
// and looking there would fail on every translated build.
bool loadResource(const TCHAR *name, const void **data, int *bytes)
{
	HRSRC resource = FindResource(NULL, name, RT_RCDATA);
	if (resource == NULL)
		return false;

	HGLOBAL handle = LoadResource(NULL, resource);
	if (handle == NULL)
		return false;

	const void *blob = LockResource(handle);
	if (blob == NULL)
		return false;

	const DWORD size = SizeofResource(NULL, resource);
	if (size == 0 || size > 0x7fffffff)
		return false;

	*data  = blob;
	*bytes = (int)size;
	return true;
}

} // namespace

void winBitmapFontInit()
{
	if (fontsLoaded)
		return;

	fontsLoaded = true;

	// The 8x8 face is not in the list below: it is what the display falls
	// back to, and it is installed straight into the core renderer.
	const void *data = NULL;
	int bytes = 0;
	if (loadResource(_T("FONT_8X8"), &data, &bytes) && textSetBuiltinFont(data, bytes))
	{
	}
	else
	{
		winlog(_T("font: FONT_8X8 is missing from the executable\n"));
	}

	for (int i = 0; i < fontEntryCount; i++)
	{
		TextFont &font = fonts[i];

		if (!loadResource(fontEntries[i].resource, &data, &bytes) ||
		    !textFontInit(font, data, bytes))
		{
			winlog(_T("font: %s is missing from the executable\n"), fontEntries[i].resource);
			ZeroMemory(&font, sizeof(font));
		}
	}
}

const TextFont *winBitmapFont(const CString &name)
{
	if (name.IsEmpty())
		return NULL;

	for (int i = 0; i < fontEntryCount; i++)
	{
		if (name.Compare(fontEntries[i].name) != 0)
			continue;

		// A font whose resource is missing stays out of the list, so the
		// dialog never offers something that cannot be drawn.
		return textFontReady(fonts[i]) ? &fonts[i] : NULL;
	}

	return NULL;
}

int winBitmapFontCount()
{
	return fontEntryCount;
}

const TextFont *winBitmapFontBySize(const CString &family, const CString &size,
                                    CString *name)
{
	for (int i = 0; i < fontEntryCount; i++)
	{
		if (family.Compare(fontEntries[i].family) != 0 ||
		    size.Compare(fontEntries[i].size) != 0)
			continue;

		if (name != NULL)
			*name = fontEntries[i].name;

		return textFontReady(fonts[i]) ? &fonts[i] : NULL;
	}

	return NULL;
}

bool winBitmapFontSplit(const CString &name, CString *family, CString *size)
{
	for (int i = 0; i < fontEntryCount; i++)
	{
		if (name.Compare(fontEntries[i].name) != 0)
			continue;

		if (family != NULL)
			*family = fontEntries[i].family;
		if (size != NULL)
			*size = fontEntries[i].size;

		return true;
	}

	return false;
}

const TextFont *winBitmapFontAt(int index, CString *name, CString *family,
                                CString *size)
{
	if (index < 0 || index >= fontEntryCount)
		return NULL;

	if (name != NULL)
		*name = fontEntries[index].name;
	if (family != NULL)
		*family = fontEntries[index].family;
	if (size != NULL)
		*size = fontEntries[index].size;

	return textFontReady(fonts[index]) ? &fonts[index] : NULL;
}
