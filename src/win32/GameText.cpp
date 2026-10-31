#include "stdafx.h"

#include "VBA.h"
#include "BitmapFont.h"
#include "GameText.h"

#include "../common/Text.h"

#include <map>
#include <vector>

extern u32 RGB_LOW_BITS_MASK;

bool gameTextFontIsBuiltin()
{
	// An empty value is what an install that predates the setting has, and
	// that install used the built-in font, so it keeps it.
	return theApp.gameTextFont.IsEmpty() ||
	       theApp.gameTextFont.Compare(GAME_TEXT_FONT_BUILTIN) == 0;
}

namespace {

struct Glyph
{
	int width;
	int height;
	int advance;
	std::vector<unsigned char> pixels; // 0 background, 1 glyph, 2 outline
};

class GlyphCache
{
public:
	GlyphCache()
		: dc(NULL), font(NULL), oldFont(NULL), bitmapFont(NULL), fontHeight(0),
		  outlineWidth(0), textHeight(0), outlineEnabled(false), configured(false)
	{}

	~GlyphCache()
	{
		reset();
	}

	void reset()
	{
		if (dc != NULL)
		{
			if (oldFont != NULL)
				SelectObject(dc, oldFont);
			DeleteDC(dc);
			dc = NULL;
		}
		if (font != NULL)
		{
			DeleteObject(font);
			font = NULL;
		}

		oldFont = NULL;
		bitmapFont = NULL;
		face.Empty();
		glyphs.clear();
		fontHeight = 0;
		outlineWidth = 0;
		textHeight = 0;
		outlineEnabled = false;
		configured = false;
	}

	bool configure(const CString &fontFace, int height, int outline, bool outlined)
	{
		if (configured && face == fontFace && fontHeight == height &&
		    outlineWidth == outline && outlineEnabled == outlined)
			return textHeight > 0;

		reset();

		face = fontFace;
		fontHeight = height;
		outlineWidth = outline;
		outlineEnabled = outlined;

		dc = CreateCompatibleDC(NULL);
		if (dc == NULL)
			return false;

		// OUT_RASTER_PRECIS keeps this path on a bitmap face even if the
		// family also has a scalable variant.  The game font list only offers
		// raster faces, but a stored setting from elsewhere could name both.
		font = CreateFontW(-height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
		                   DEFAULT_CHARSET, OUT_RASTER_PRECIS, CLIP_DEFAULT_PRECIS,
		                   NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
		                   fontFace.GetString());
		if (font == NULL)
		{
			reset();
			return false;
		}

		oldFont = (HGDIOBJ)SelectObject(dc, font);
		TEXTMETRICW tm;
		if (!GetTextMetricsW(dc, &tm) || tm.tmHeight <= 0)
		{
			reset();
			return false;
		}

		textHeight = tm.tmHeight;
		configured = true;
		return true;
	}

	int lineHeight() const
	{
		// One pixel of top and bottom margin makes the bottom outline of the
		// lowest message land inside the frame instead of being clipped.
		return textHeight + 2 * outlineWidth + 1;
	}

	// How far a glyph sits from the pen position on both sides: the one
	// pixel that keeps the outline of one character off the next, plus the
	// outline itself.
	int margin() const
	{
		return 1 + outlineWidth;
	}

	// The bundled bitmap fonts carry their glyphs in the executable, so there
	// is nothing to rasterise: the cache only has to know which face it is
	// and how the outline is to be drawn.  These fonts come in one fixed
	// height, so the cache is configured once per font.
	bool configureBitmap(const TextFont &font, int outline, bool outlined)
	{
		if (configured && bitmapFont == &font &&
		    outlineWidth == outline && outlineEnabled == outlined)
			return true;

		reset();

		bitmapFont = &font;
		outlineWidth = outline;
		outlineEnabled = outlined;
		textHeight = font.height;
		configured = true;
		return true;
	}

	const Glyph *get(wchar_t ch)
	{
		std::map<wchar_t, Glyph>::iterator it = glyphs.find(ch);
		if (it != glyphs.end())
			return &it->second;

		if (!configured)
			return NULL;

		Glyph glyph;
		if (!renderGlyph(ch, glyph))
			return NULL;

		// Messages are short, but a long session can show many different
		// characters.  Keeping the cache bounded is cheaper than tracking
		// every glyph forever.
		if (glyphs.size() >= 4096)
			glyphs.clear();

		Glyph &stored = glyphs[ch];
		stored = glyph;
		return &stored;
	}

private:
	bool renderGlyph(wchar_t ch, Glyph &glyph)
	{
		if (bitmapFont != NULL)
			return renderBitmapGlyph(ch, glyph);

		SIZE extent;
		if (!GetTextExtentPoint32W(dc, &ch, 1, &extent))
			return false;

		int advance = extent.cx;
		if (advance < 0)
			advance = 0;

		const int margin = 1 + outlineWidth;
		const int cw = advance + 2 * margin;
		const int chHeight = textHeight + 2 * margin;
		if (cw <= 0 || chHeight <= 0 || cw > 1024 || chHeight > 1024)
			return false;

		BITMAPINFO bmi;
		ZeroMemory(&bmi, sizeof(bmi));
		bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth       = cw;
		bmi.bmiHeader.biHeight      = -chHeight; // top-down
		bmi.bmiHeader.biPlanes      = 1;
		bmi.bmiHeader.biBitCount    = 1;
		bmi.bmiHeader.biCompression = BI_RGB;
		bmi.bmiHeader.biClrUsed     = 2;
		bmi.bmiColors[0].rgbRed     = 0;
		bmi.bmiColors[0].rgbGreen   = 0;
		bmi.bmiColors[0].rgbBlue    = 0;
		bmi.bmiColors[0].rgbReserved = 0;
		bmi.bmiColors[1].rgbRed     = 255;
		bmi.bmiColors[1].rgbGreen   = 255;
		bmi.bmiColors[1].rgbBlue    = 255;
		bmi.bmiColors[1].rgbReserved = 0;

		void *bits = NULL;
		HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
		if (bitmap == NULL || bits == NULL)
		{
			if (bitmap != NULL)
				DeleteObject(bitmap);
			return false;
		}

		HDC glyphDC = CreateCompatibleDC(dc);
		if (glyphDC == NULL)
		{
			DeleteObject(bitmap);
			return false;
		}

		HGDIOBJ oldBitmap = SelectObject(glyphDC, bitmap);
		HGDIOBJ oldGlyphFont = SelectObject(glyphDC, font);

		PatBlt(glyphDC, 0, 0, cw, chHeight, BLACKNESS);
		SetBkMode(glyphDC, OPAQUE);
		SetBkColor(glyphDC, RGB(0, 0, 0));
		SetTextColor(glyphDC, RGB(255, 255, 255));
		SetTextAlign(glyphDC, TA_LEFT | TA_TOP);
		TextOutW(glyphDC, margin, margin, &ch, 1);

		glyph.width   = cw;
		glyph.height  = chHeight;
		glyph.advance = advance;
		glyph.pixels.assign((size_t)cw * chHeight, 0);

		const int stride = ((cw + 31) / 32) * 4;
		const unsigned char *src = (const unsigned char *)bits;
		for (int y = 0; y < chHeight; y++)
		{
			const unsigned char *row = src + (size_t)y * stride;
			for (int x = 0; x < cw; x++)
			{
				if (row[x >> 3] & (0x80 >> (x & 7)))
					glyph.pixels[(size_t)y * cw + x] = 1;
			}
		}

		SelectObject(glyphDC, oldGlyphFont);
		SelectObject(glyphDC, oldBitmap);
		DeleteDC(glyphDC);
		DeleteObject(bitmap);

		growOutline(glyph);
		return true;
	}

	// The same mask, built from a bundled font: the glyph bits are already
	// cut for the pixel grid, so they are copied as they are, and only the
	// margin and outline are added.  Everything downstream blits one mask
	// whatever its source was.
	bool renderBitmapGlyph(wchar_t ch, Glyph &glyph)
	{
		const unsigned char *bits = NULL;
		int width = 0, rows = 0, advance = 0;
		if (!textFontGlyph(*bitmapFont, (unsigned)ch, &bits, &width, &rows, &advance))
			return false;

		const int margin = 1 + outlineWidth;
		const int cw = width + 2 * margin;
		const int chHeight = rows + 2 * margin;

		glyph.width   = cw;
		glyph.height  = chHeight;
		glyph.advance = advance;
		glyph.pixels.assign((size_t)cw * chHeight, 0);

		const int stride = (width + 7) / 8;
		for (int y = 0; y < rows; y++)
		{
			const unsigned char *row = bits + (size_t)y * stride;
			for (int x = 0; x < width; x++)
			{
				if (row[x >> 3] & (0x80 >> (x & 7)))
					glyph.pixels[(size_t)(y + margin) * cw + (x + margin)] = 1;
			}
		}

		growOutline(glyph);
		return true;
	}

	void growOutline(Glyph &glyph)
	{
		if (!outlineEnabled)
			return;

		const int cw = glyph.width;
		const int chHeight = glyph.height;

		for (int y = 0; y < chHeight; y++)
		{
			for (int x = 0; x < cw; x++)
			{
				if (glyph.pixels[(size_t)y * cw + x] != 1)
					continue;

				for (int oy = -outlineWidth; oy <= outlineWidth; oy++)
				{
					for (int ox = -outlineWidth; ox <= outlineWidth; ox++)
					{
						const int nx = x + ox;
						const int ny = y + oy;
						if (nx >= 0 && nx < cw && ny >= 0 && ny < chHeight &&
						    glyph.pixels[(size_t)ny * cw + nx] == 0)
							glyph.pixels[(size_t)ny * cw + nx] = 2;
					}
				}
			}
		}
	}

	HDC dc;
	HFONT font;
	HGDIOBJ oldFont;
	const TextFont *bitmapFont;
	CString face;
	std::map<wchar_t, Glyph> glyphs;
	int fontHeight;
	int outlineWidth;
	int textHeight;
	bool outlineEnabled;
	bool configured;
};

GlyphCache &gameGlyphs()
{
	static GlyphCache cache;
	return cache;
}

int clampTextHeight(int pixelHeight)
{
	if (pixelHeight < 1)
		pixelHeight = 1;
	if (pixelHeight > 128)
		pixelHeight = 128;
	return pixelHeight;
}

int scaledOutline(int pixelHeight)
{
	int outline = (pixelHeight + 4) / 8;
	if (outline < 1)
		outline = 1;
	return outline;
}

bool configureGlyphs(int pixelHeight)
{
	if (gameTextFontIsBuiltin())
		return false;

	CString face = theApp.gameTextFont;
	if (face.IsEmpty())
		return false;

	// A font that travels with the executable has one fixed height, so the
	// requested size only decides how thick the outline is: the glyphs are
	// drawn at the size they were cut for, which is also the size the options
	// dialog offers for it.
	const TextFont *font = winBitmapFont(face);
	if (font != NULL)
		return gameGlyphs().configureBitmap(*font, scaledOutline(font->height),
		                                    outlinedText);

	pixelHeight = clampTextHeight(pixelHeight);
	return gameGlyphs().configure(face, pixelHeight,
	                              scaledOutline(pixelHeight), outlinedText);
}

void blitGlyph(u8 *dest, int pitch, int x, int y, int maxY, const Glyph &glyph,
               int loCol, int hiCol, int outCol, bool trans)
{
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

	for (int gy = 0; gy < glyph.height; gy++)
	{
		const int py = y + gy;
		if (py < 0 || py >= maxY)
			continue;

		u8 *row = dest + (size_t)py * pitch;
		for (int gx = 0; gx < glyph.width; gx++)
		{
			const int state = glyph.pixels[(size_t)gy * glyph.width + gx];
			if (state == 0)
				continue;

			const int px = x + gx;
			if (px < 0 || px >= width)
				continue;

			if (systemColorDepth == 16)
			{
				u16 *p = (u16 *)(row + px * 2);
				u16 mask = u16(~RGB_LOW_BITS_MASK);
				if (state == 1)
				{
					if (trans)
						*p = (u16)(loCol + ((*p & mask) >> 1));
					else
						*p = (u16)hiCol;
				}
				else if (trans)
					*p = (u16)(outCol + ((*p & mask) >> 1));
				else
					*p = (u16)outCol;
			}
			else if (systemColorDepth == 24)
			{
				u8 *p = row + px * 3;
				const u32 color = state == 1 ? (u32)hiCol : (u32)outCol;
				if (trans)
				{
					*p       = (u8)(((color & 255) >> 1) + (*p >> 1));
					*(p + 1) = (u8)((((color >> 8) & 255) >> 1) + (*(p + 1) >> 1));
					*(p + 2) = (u8)((((color >> 16) & 255) >> 1) + (*(p + 2) >> 1));
				}
				else
				{
					*p       = (u8)(color & 255);
					*(p + 1) = (u8)((color >> 8) & 255);
					*(p + 2) = (u8)((color >> 16) & 255);
				}
			}
			else
			{
				u32 *p = (u32 *)(row + px * 4);
				u32 mask = 0xfefefe;
				if (state == 1)
				{
					if (trans)
						*p = (u32)(hiCol + ((*p & mask) >> 1));
					else
						*p = (u32)hiCol;
				}
				else if (trans)
					*p = (u32)(outCol + ((*p & mask) >> 1));
				else
					*p = (u32)outCol;
			}
		}
	}
}

} // namespace

int gameTextLineHeight(int pixelHeight)
{
	if (!configureGlyphs(pixelHeight))
		return 0;

	return gameGlyphs().lineHeight();
}

void drawGameText(u8 *dest, int pitch, int x, int y, int maxY,
                  const wchar_t *text, const char *colorList, int pixelHeight)
{
	if (dest == NULL || text == NULL || *text == 0 || maxY <= 0)
		return;

	if (!configureGlyphs(pixelHeight))
		return;

	GlyphCache &cache = gameGlyphs();

	// The margin comes from the cache, not from pixelHeight: a bundled font
	// keeps the outline its own height asks for, whatever size was stored
	// with it, and the glyph was built with that same margin.
	const int margin = cache.margin();
	const char *colors = (colorList != NULL && *colorList != 0) ? colorList : NULL;

	int loCol, hiCol, outCol;
	drawTextGetColors(textColor, loCol, hiCol, outCol);

	int penX = x;
	for (const wchar_t *p = text; *p != 0; p++)
	{
		if (colors != NULL)
		{
			drawTextGetColors((int)(unsigned char)*colors - 1, loCol, hiCol, outCol);
			colors++;
		}

		const Glyph *glyph = cache.get(*p);
		if (glyph == NULL)
			continue;

		blitGlyph(dest, pitch, penX - margin, y, maxY, *glyph,
		          loCol, hiCol, outCol, transparentText);

		penX += glyph->advance;

		const int inc = systemColorDepth == 32 ? 4 : (systemColorDepth == 24 ? 3 : 2);
		if (penX >= pitch / inc)
			break;
	}
}
