Bitmap fonts that ship inside the executable
=============================================

These files are the fonts the whole program draws text with.  vba.rc carries
each one as an RCDATA resource, so they live in the .exe and nothing is loaded
from disk at run time.  The container they share is described in
src/common/TextFont.h; src/common/TextFont.cpp is the only decoder.

font8x8.bin
-----------

The 8x8 face the on screen display has always used.  It used to be a table in
src/common/Text.cpp, 256 glyphs in character code order, eight bytes each, one
byte per row, bit 0 the leftmost pixel.  That table was converted to the
container above, which needed two changes:

  * the bits of every row were reversed, because the container stores the
    leftmost pixel in the most significant bit;
  * a header and an offset table were written for it.

Nothing else about it changed: every glyph is still 8 pixels wide, so the pen
still advances by 8, and the sizes the display offers are the same.

The count byte is 0 for this font, which means 256 (see TextFont.h).  A byte
cannot hold 256, and this face is the only one that needs all of them.

Everything else
---------------

The gse*, mcs* and verdana* files are the embedded raster fonts of AGG 2.5
(Anti-Grain Geometry, Copyright (C) 2002-2006 Maxim Shemanarev), taken byte
for byte from an original copy of agg_embedded_raster_fonts.cpp.  They are the
same set DeSmuME offers for its HUD, under the same names and in the same
order; that is where this list is from.  AGG is distributed under the GNU
General Public License, version 2 or later, which is the licence this program
is under as well, so the fonts can travel with it.

Each of them is one fixed size, 96 glyphs covering ASCII 32 to 127, and they
are drawn at that size -- they are not scaled.  The width byte of a glyph is
also its advance, which is what AGG does.

Adding one
----------

Drop the bytes in here, add a resource line to vba.rc, and add a row to the
table in src/win32/BitmapFont.cpp.  The size the options dialog shows is read
from the font itself (its widest glyph by its height), so nothing else needs
to be told about it.
