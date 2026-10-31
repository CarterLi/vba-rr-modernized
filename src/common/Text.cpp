/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2002 Ben Parnell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* Code originally from fceu/drawing.h file, adapted by Forgotten
 */
#include "System.h"
#include "TextFont.h"

bool outlinedText = true, transparentText = false;
// textMethod:
// 0 - "in-game"; 1 - "on-game"; 2 - "on-screen"
int  textColor = 0, textMethod = 1;

extern u32 RGB_LOW_BITS_MASK;

// The 8x8 font the on screen display has always drawn with.  It used to be a
// table in this file; it is now a resource of the executable (vba.rc, RCDATA
// FONT_8X8) in the same container as every other font the emulator carries,
// so that there is one decoder and one renderer.  See resources/font.
static TextFont builtinFont;

// Called once, before the first window, with the bytes of that resource.
bool textSetBuiltinFont(const void *data, int bytes)
{
	return textFontInit(builtinFont, data, bytes);
}

static void calcColors(const int colorNum, int & lo, int & hi, int & out)
{
	int redLo, redHi, greenLo, greenHi, blueLo, blueHi;

	if (colorNum == 0 || colorNum == 1 || colorNum == 2 || colorNum == 6) // white, red, yellow, or magenta
		redLo = (0xf) << systemRedShift, redHi = (0x1f) << systemRedShift;
	else
		redLo = redHi = 0;

	if (colorNum == 0 || colorNum == 2 || colorNum == 3 || colorNum == 4) // white, yellow, green, or cyan
		greenLo = (0xf) << systemGreenShift, greenHi = (0x1f) << systemGreenShift;
	else
		greenLo = greenHi = 0;

	if (colorNum == 0 || colorNum == 4 || colorNum == 5 || colorNum == 6) // white, cyan, blue, or magenta
		blueLo = (0xf) << systemBlueShift, blueHi = (0x1f) << systemBlueShift;
	else
		blueLo = blueHi = 0;

	lo = redLo + greenLo + blueLo;
	hi = redHi + greenHi + blueHi;

	if (colorNum == 7) // black
		out = 0xffffffff; // white border
	else
		out = 0;  // black border
}

void drawTextGetColors(int colorNum, int &lo, int &hi, int &out)
{
	calcColors(colorNum, lo, hi, out);
}

int lastColID = 0;
static void progressColorList(const char *& colorList, int & lo, int & hi, int & out)
{
	if (*colorList)
	{
		if (*colorList != lastColID)
		{
			calcColors((int)(*colorList)-1, lo, hi, out);
			lastColID = *colorList;
		}
		colorList++;
	}
	else
	{
		colorList = NULL;
	}
}

// One pixel of a glyph cell.  `state` is what the cell holds: 1 for the
// glyph, 2 for its outline.  16bpp blends glyphs with the low colour and the
// outline with the border colour, which is what this renderer has always
// done; the wider depths use the high colour for both.
static inline void putTextPixel(u8 *p, int inc, int state,
                                int hiCol, int loCol, int outCol, bool trans)
{
	const int color = state == 1 ? hiCol : outCol;

	switch (inc)
	{
	case 2:
	{
		u16 *q = (u16 *)p;
		const u16 mask = u16(~RGB_LOW_BITS_MASK);
		const int c = (state == 1 && trans) ? loCol : color;
		*q = (u16)(trans ? c + ((*q & mask) >> 1) : c);
		break;
	}
	case 3:
	{
		u8 *q = p;
		if (trans)
		{
			q[0] = (u8)(((color & 255) >> 1) + (q[0] >> 1));
			q[1] = (u8)((((color >> 8) & 255) >> 1) + (q[1] >> 1));
			q[2] = (u8)((((color >> 16) & 255) >> 1) + (q[2] >> 1));
		}
		else
		{
			q[0] = (u8)(color & 255);
			q[1] = (u8)((color >> 8) & 255);
			q[2] = (u8)((color >> 16) & 255);
		}
		break;
	}
	default:
	{
		u32 *q = (u32 *)p;
		const u32 mask = 0xfefefe;
		*q = (u32)(trans ? (u32)color + ((*q & mask) >> 1) : (u32)color);
		break;
	}
	}
}

// Renders one glyph scaled to dw x dh pixels.  The result is a
// (dw + 2*t) by (dh + 2*t) cell of 0 = background, 1 = glyph, 2 = outline.
// The cell is cleared first, so the caller can reuse one buffer for every
// character of a string.  Nearest neighbour is what a pixel font should use:
// every destination pixel takes the bit of the glyph it covers.
static void glyphCell(const u8 *bits, int glyphWidth, int glyphHeight,
                      int dw, int dh, int t, bool outline, u8 *cell)
{
	const int cw = dw + 2 * t;
	const int rows = dh + 2 * t;

	for (int y = 0; y < rows; y++)
		for (int x = 0; x < cw; x++)
			cell[y * cw + x] = 0;

	const int stride = (glyphWidth + 7) / 8;
	for (int y = 0; y < dh; y++)
	{
		const u8 *row = bits + (size_t)(y * glyphHeight / dh) * stride;
		for (int x = 0; x < dw; x++)
		{
			const int sx = x * glyphWidth / dw;
			if (row[sx >> 3] & (0x80 >> (sx & 7)))
				cell[(y + t) * cw + (x + t)] = 1;
		}
	}

	if (!outline)
		return;

	// Grow every glyph pixel by t in all directions.  Text pixels are all
	// set before this runs, so marking only empty cells cannot eat into the
	// glyph, and a second pass over this loop cannot grow the border again.
	for (int y = 0; y < rows; y++)
		for (int x = 0; x < cw; x++)
		{
			if (cell[y * cw + x] != 1)
				continue;

			for (int oy = -t; oy <= t; oy++)
				for (int ox = -t; ox <= t; ox++)
				{
					int nx = x + ox;
					int ny = y + oy;
					if (nx >= 0 && nx < cw && ny >= 0 && ny < rows && cell[ny * cw + nx] == 0)
						cell[ny * cw + nx] = 2;
				}
		}
}

// Writes one cell into the frame.  (x, y) is its top left corner, which is
// the glyph's own corner less the outline, and only pixels inside the visible
// width are touched.
static void blitTextCell(u8 *screen, int pitch, int inc, int width,
                         int x, int y, const u8 *cell, int cw, int rows,
                         int hiCol, int loCol, int outCol, bool trans)
{
	for (int gy = 0; gy < rows; gy++)
	{
		const int py = y + gy;
		if (py < 0)
			continue;

		u8 *row = screen + (size_t)py * pitch;

		for (int gx = 0; gx < cw; gx++)
		{
			const int state = cell[gy * cw + gx];
			if (state == 0)
				continue;

			const int px = x + gx;
			if (px < 0 || px >= width)
				continue;

			putTextPixel(row + px * inc, inc, state, hiCol, loCol, outCol, trans);
		}
	}
}

// The largest cell the format allows: a glyph of 64 by 64 at 200 percent,
// plus two pixels of outline on every side.  One buffer serves every
// character of a string, so nothing is allocated while drawing.
enum { TEXT_CELL_SIZE = 132 };
static u8 textCell[TEXT_CELL_SIZE * TEXT_CELL_SIZE];

static void drawTextInternal(u8 *screen, int pitch, int x, int y,
                             const char *string, bool trans,
                             const char *colorList, int scalePercent)
{
	// Nothing can be drawn before the executable's font resource is in; the
	// display is created after that, so this only guards a broken build.
	if (!textFontReady(builtinFont))
		return;

	if (colorList && !*colorList)
		colorList = NULL;

	// In Game and On Game text is drawn into the emulated frame, which the
	// window then scales, so its size is expressed relative to the glyph and
	// not in points.
	if (scalePercent < 25)
		scalePercent = 25;
	if (scalePercent > 200)
		scalePercent = 200;

	// One pixel of outline at the native size, two from 150 percent up.
	int t = (scalePercent + 50) / 100;
	if (t < 1)
		t = 1;

	int loCol, hiCol, outCol;
	calcColors(textColor, loCol, hiCol, outCol);
	lastColID = 0;

	int inc = 2;
	switch (systemColorDepth)
	{
	case 24:
		inc = 3;
		break;
	case 32:
		inc = 4;
		break;
	}

	const int width = pitch / inc;

	while (*string)
	{
		// The font covers all 256 codes, so the character is taken as a
		// byte: a signed char would look the code up as a negative number.
		const unsigned c = (unsigned char)*string++;

		if (colorList)
			progressColorList(colorList, loCol, hiCol, outCol);

		const u8 *bits = NULL;
		int glyphWidth = 0, glyphHeight = 0, advance = 0;
		if (!textFontGlyph(builtinFont, c, &bits, &glyphWidth, &glyphHeight, &advance))
			continue;

		int dw = (glyphWidth * scalePercent + 50) / 100;
		int dh = (glyphHeight * scalePercent + 50) / 100;
		if (dw < 1)
			dw = 1;
		if (dh < 1)
			dh = 1;

		const int cw = dw + 2 * t;
		const int rows = dh + 2 * t;
		if (cw > TEXT_CELL_SIZE || rows > TEXT_CELL_SIZE)
			continue;

		glyphCell(bits, glyphWidth, glyphHeight, dw, dh, t, outlinedText, textCell);
		blitTextCell(screen, pitch, inc, width, x - t, y - t, textCell, cw, rows,
		             hiCol, loCol, outCol, trans);

		int step = (advance * scalePercent + 50) / 100;
		if (step < 1)
			step = 1;
		x += step;

		if (x >= width) // cut off text at right boundary
			break;
	}
}

int drawTextLineHeight(int scalePercent)
{
	if (scalePercent < 25)
		scalePercent = 25;
	if (scalePercent > 200)
		scalePercent = 200;

	// One row is the font's own height at this scale, plus the outline above
	// and below.  The built-in 8x8 gives the historical ten rows at 100
	// percent.
	const int fontHeight = textFontReady(builtinFont) ? builtinFont.height : 8;

	int glyph = (fontHeight * scalePercent + 50) / 100;
	if (glyph < 1)
		glyph = 1;
	int border = (scalePercent + 50) / 100;
	if (border < 1)
		border = 1;

	return glyph + 2 * border;
}

void drawText(u8 *screen, int pitch, int x, int y, const char *string, const char *colorList,
              int scalePercent)
{
	drawTextInternal(screen, pitch, x, y, string, transparentText, colorList, scalePercent);
}

void drawTextTransp(u8 *screen, int pitch, int x, int y, const char *string, const char *colorList,
                    int scalePercent)
{
	drawTextInternal(screen, pitch, x, y, string, true, colorList, scalePercent);
}

