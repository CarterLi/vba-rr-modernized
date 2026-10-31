// BitmapFont.h: the fonts that ship inside the executable.
//
// Two kinds live here:
//
//  * the 8x8 face the on screen display has always drawn with, which used to
//    be a table in common/Text.cpp;
//  * the embedded raster fonts of AGG 2.5 that DeSmuME offers for its HUD:
//    the gse*, mcs* and verdana* faces, so the In Game and On Game modes can
//    use one of them instead of a system bitmap face.
//
// Both are stored in the same container and decoded by common/TextFont.cpp,
// which is also what the display renders with: there is one font format and
// one renderer.  Nothing here goes through GDI.  See
// src/win32/resources/font/README.txt for where the bytes come from.

#ifndef VBA_WIN32_BITMAPFONT_H
#define VBA_WIN32_BITMAPFONT_H

#pragma once

#include "../common/TextFont.h"

// Loads the built-in 8x8 face into common/Text.cpp and reads the headers of
// the bundled fonts.  Called once, from VBA::InitInstance(), before the first
// window is made; a resource that is missing is written to the trace and
// leaves that one font unusable rather than taking the program down.
void winBitmapFontInit();

// A bundled font by the name the settings file stores, e.g. "gse7x11".
// NULL when the name is not one of them, or when its resource is missing.
const TextFont *winBitmapFont(const CString &name);

// The other way round: a size of a family, ("gse", "7x11"), is what the
// settings file holds, and the renderer wants the full name.  `name` receives
// it.  NULL when that family has no such size.
const TextFont *winBitmapFontBySize(const CString &family, const CString &size,
                                    CString *name);

// Splits a bundled name into the family and the size label ("gse", "7x11").
// False for anything that is not a bundled font.
bool winBitmapFontSplit(const CString &name, CString *family, CString *size);

// The bundled fonts, in the order DeSmuME lists them.
//
// Each one belongs to a family ("gse") and is one size of it ("7x11"), which
// is how DeSmuME's own list reads and how the text options dialog offers
// them: one entry for the family, its sizes underneath.  The full name
// ("gse7x11") is what winBitmapFont() takes and what the settings file holds.
int winBitmapFontCount();
const TextFont *winBitmapFontAt(int index, CString *name, CString *family,
                                CString *size);

#endif // VBA_WIN32_BITMAPFONT_H
