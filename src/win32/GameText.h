// GameText.h: in-frame text rendered with a system bitmap font.
//
// The In Game and On Game modes write into the emulated frame, so they cannot
// use GDI directly.  This renderer rasterises each character into a small
// monochrome mask with GDI and blits that mask into the frame at the selected
// bit depth.  Only raster/bitmap faces are offered for these modes: they are
// already pixel fonts, so no antialiasing or alpha blending is needed.  The
// per-character colour list, outline and transparency rules of the built-in
// font are preserved.

#ifndef VBA_WIN32_GAMETEXT_H
#define VBA_WIN32_GAMETEXT_H

#pragma once

// The name of the built-in 8x8 face.  It is stored in the settings file as a
// font name, next to the bundled families ("gse"), so it is not a label a
// translation may change.
#define GAME_TEXT_FONT_BUILTIN      _T("Built-in")

// The built-in face has this one size, and the size combo labels it so.
#define GAME_TEXT_FONT_BUILTIN_SIZE _T("8x8")

// True when the In Game / On Game modes keep the built-in 8x8 bitmap font.
bool gameTextFontIsBuiltin();

// Height of one message row, outline included.  pixelHeight is the requested
// font height in pixels.  Returns 0 when the selected font cannot be used, in
// which case the caller falls back to the built-in bitmap font.
int gameTextLineHeight(int pixelHeight);

// Draws one message.  (x, y) is the top left of the glyph row and maxY is the
// first row below the frame (exclusive), so a large font cannot write past the
// end of the emulated frame.
void drawGameText(u8 *dest, int pitch, int x, int y, int maxY,
                  const wchar_t *text, const char *colorList, int pixelHeight);

#endif // VBA_WIN32_GAMETEXT_H
