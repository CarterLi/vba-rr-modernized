#ifndef VBA_TEXT_H
#define VBA_TEXT_H

#pragma once

extern void drawText(u8 *, int, int, int, const char *, const char*cl = NULL, int scalePercent = 100);
extern void drawTextTransp(u8 *, int, int, int, const char *, const char*cl = NULL, int scalePercent = 100);

// Height of one row of the built-in bitmap font at the given scale, outline
// included.  100 percent is the historical 8x8 glyph plus its one pixel
// outline, i.e. 10 rows.
extern int drawTextLineHeight(int scalePercent = 100);

// The palette entry used by drawText()/drawTextTransp(), exposed for the
// win32 system-font renderer so both paths pick exactly the same colours.
extern void drawTextGetColors(int colorNum, int &lo, int &hi, int &out);

extern bool outlinedText, transparentText;
extern int  textColor, textMethod;

// Installs the built-in 8x8 font.  The glyphs live in the executable's
// resources (vba.rc, RCDATA FONT_8X8) rather than in this library, so the
// win32 side hands them over before the first frame is drawn; the bytes are
// in the container described in TextFont.h.  Returns false when they are not
// a font this renderer can draw.
extern bool textSetBuiltinFont(const void *data, int bytes);

#endif // VBA_TEXT_H
