// TextFont.h: the bitmap font container the display draws with.
//
// Every font the emulator carries uses this one container, on screen display
// and in-frame text alike, so there is one decoder and one set of rules:
//
//   [0] height in rows
//   [1] baseline (unused; 0 in every bundled font)
//   [2] first character code
//   [3] number of characters, or 0 for 256
//   [4 .. 4 + count * 2)  uint16 offsets, little endian, per character
//   then per glyph, at 4 + count * 2 + offset:
//       width byte, then height rows of ceil(width / 8) bytes
//
// Rows are packed with the leftmost pixel in the most significant bit, and
// the pen advances by the width byte.  That is the format of the embedded
// raster fonts of AGG 2.5, which is where the gse* faces come from; the
// built-in 8x8 face was converted to it (see resources/font/README.txt), so
// it carries all 256 codes and needs the "0 means 256" rule for its count.
//
// Glyphs are variable width.  A font of fixed-width glyphs simply stores the
// same width for every one of them, which is what the built-in 8x8 does.

#ifndef VBA_TEXTFONT_H
#define VBA_TEXTFONT_H

#pragma once

#include "System.h"

struct TextFont
{
	const u8 *blob;  // the bytes, as they came out of the resource
	int bytes;
	int height;      // rows per glyph
	int width;       // widest glyph, in pixels
	int firstChar;   // lowest character code the font has
	int charCount;   // number of glyphs
};

// Reads the header and checks every glyph against the size of the blob, so a
// font that is damaged or of another format is refused instead of drawn as
// garbage.  Returns false and leaves `font` unusable when that happens.
bool textFontInit(TextFont &font, const void *blob, int bytes);

// One glyph, by character code.  `bits` points at its rows, `width` pixels
// across, `rows` down, `advance` how far the pen moves; the rows are
// ceil(width / 8) bytes each.  False when the font has no such character.
bool textFontGlyph(const TextFont &font, unsigned ch, const u8 **bits,
                   int *width, int *rows, int *advance);

// True when the header has been read and the font can be drawn with.
bool textFontReady(const TextFont &font);

#endif // VBA_TEXTFONT_H
