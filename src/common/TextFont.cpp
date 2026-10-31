#include "System.h"

#include "TextFont.h"

bool textFontReady(const TextFont &font)
{
	return font.blob != NULL && font.height > 0;
}

bool textFontInit(TextFont &font, const void *blob, int bytes)
{
	font.blob = NULL;
	font.bytes = 0;
	font.height = 0;
	font.width = 0;
	font.firstChar = 0;
	font.charCount = 0;

	if (blob == NULL || bytes < 4)
		return false;

	const u8 *data = (const u8 *)blob;

	const int height = data[0];
	const int first  = data[2];
	// The count is a byte, and the built-in face has all 256 codes; a stored
	// zero therefore means 256.  None of the AGG fonts can hit that, they
	// carry 96.
	const int count  = data[3] != 0 ? data[3] : 256;

	if (height <= 0 || height > 64)
		return false;

	const int tableOffset = 4 + count * 2;
	if (tableOffset > bytes)
		return false;

	int width = 0;
	for (int i = 0; i < count; i++)
	{
		const int offset = data[4 + i * 2] | (data[5 + i * 2] << 8);
		const int pos = tableOffset + offset;
		if (pos >= bytes)
			return false;

		const int glyphWidth = data[pos];
		if (glyphWidth <= 0 || glyphWidth > 64)
			return false;

		if (pos + 1 + ((glyphWidth + 7) / 8) * height > bytes)
			return false;

		if (glyphWidth > width)
			width = glyphWidth;
	}

	if (width <= 0)
		return false;

	font.blob = data;
	font.bytes = bytes;
	font.height = height;
	font.width = width;
	font.firstChar = first;
	font.charCount = count;
	return true;
}

bool textFontGlyph(const TextFont &font, unsigned ch, const u8 **bits,
                   int *width, int *rows, int *advance)
{
	if (!textFontReady(font))
		return false;

	const int index = (int)ch - font.firstChar;
	if (index < 0 || index >= font.charCount)
		return false;

	const int offset = font.blob[4 + index * 2] |
	                   (font.blob[5 + index * 2] << 8);
	const int pos = 4 + font.charCount * 2 + offset;
	if (pos >= font.bytes)
		return false;

	const int w = font.blob[pos];
	const int stride = (w + 7) / 8;
	if (w <= 0 || pos + 1 + stride * font.height > font.bytes)
		return false;

	*bits    = font.blob + pos + 1;
	*width   = w;
	*rows    = font.height;
	*advance = w;
	return true;
}
