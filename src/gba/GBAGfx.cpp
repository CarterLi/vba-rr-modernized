#include "../Port.h"

#include <cstring>

#include "../common/System.h"
#include "../common/SystemGlobals.h"
#include "GBAGfx.h"

int coeff[32] = {
	0,   1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16
};

int32 layerEnable = 0xff00;

u32  line0[240];
u32  line1[240];
u32  line2[240];
u32  line3[240];
u32  lineOBJ[240];
u32  lineOBJWin[240];
u32  lineMix[240];
bool gfxInWin0[240];
bool gfxInWin1[240];
int lineOBJpixleft[128];

int gfxBG2Changed = 0;
int gfxBG3Changed = 0;

int gfxBG2X       = 0;
int gfxBG2Y       = 0;
int gfxBG3X       = 0;
int gfxBG3Y       = 0;
int gfxLastVCOUNT = 0;

// --- frame interpolation ----------------------------------------------------
//
// See documentation/frame-interpolation.md.  In short: a frame is drawn one
// scanline at a time, in the middle of emulation, so the only way to draw a
// frame between two of them is to remember what each line saw and replay the
// line renderer later.

int gfxInterpolationMode = 0;

// One ring is being filled, the other holds the frame before it.  OAM contents
// are kept once per frame rather than once per line: the sprite pass reads OAM
// live, and the older frame's copy is gone by the time the replay runs.
static GfxLineState gfxLineRing[2][GFX_INTERP_LINES];
static u8           gfxOamRing[2][0x400];
static int          gfxRingCur    = 0;
static int          gfxRingLines  = 0;
static int          gfxRingFrames = 0;

// A scroll or reference point that jumps further than this is a cut, a warp or
// a respawn, not motion: the field is taken from the newer frame instead.
#define GFX_INTERP_SCROLL_LIMIT 64            // BGxHOFS / BGxVOFS, pixels
#define GFX_INTERP_REF_LIMIT    (128 * 256)   // affine reference, 1/256 pixel
#define GFX_INTERP_OBJ_LIMIT    64            // OAM X and Y, pixels

void gfxResetLineStates()
{
	gfxRingCur    = 0;
	gfxRingLines  = 0;
	gfxRingFrames = 0;
}

void gfxCaptureLineState()
{
	if (gfxInterpolationMode == 0 || gfxRingLines >= GFX_INTERP_LINES || VCOUNT >= GFX_INTERP_LINES)
		return;

	u8 *oamSnap = gfxOamRing[gfxRingCur];
	if (VCOUNT == 0)
		memcpy(oamSnap, oam, 0x400);

	GfxLineState *s = &gfxLineRing[gfxRingCur][VCOUNT];

	s->dispcnt = DISPCNT;
	s->bldmod  = BLDMOD;
	s->colev   = COLEV;
	s->coly    = COLY;
	s->mosaic  = MOSAIC;

	s->win0h = WIN0H;
	s->win1h = WIN1H;
	s->win0v = WIN0V;
	s->win1v = WIN1V;
	s->winin = WININ;
	s->winout = WINOUT;

	s->bgcnt[0]  = BG0CNT;
	s->bgcnt[1]  = BG1CNT;
	s->bgcnt[2]  = BG2CNT;
	s->bgcnt[3]  = BG3CNT;

	s->bghofs[0] = BG0HOFS;
	s->bghofs[1] = BG1HOFS;
	s->bghofs[2] = BG2HOFS;
	s->bghofs[3] = BG3HOFS;

	s->bgvofs[0] = BG0VOFS;
	s->bgvofs[1] = BG1VOFS;
	s->bgvofs[2] = BG2VOFS;
	s->bgvofs[3] = BG3VOFS;

	s->bgpa[0][0] = BG2PA;
	s->bgpa[0][1] = BG2PB;
	s->bgpa[0][2] = BG2PC;
	s->bgpa[0][3] = BG2PD;
	s->bgpa[1][0] = BG3PA;
	s->bgpa[1][1] = BG3PB;
	s->bgpa[1][2] = BG3PC;
	s->bgpa[1][3] = BG3PD;

	s->bgx[0] = gfxBG2X;
	s->bgy[0] = gfxBG2Y;
	s->bgx[1] = gfxBG3X;
	s->bgy[1] = gfxBG3Y;

	s->vcount  = VCOUNT;
	s->changed = (u8)((gfxBG2Changed ? 1 : 0) | (gfxBG3Changed ? 2 : 0));

	if (++gfxRingLines >= GFX_INTERP_LINES)
	{
		gfxRingLines = 0;
		gfxRingCur ^= 1;
		if (gfxRingFrames < 2)
			gfxRingFrames++;
	}
}

// Shortest signed path from `from` to `to` around a counter of `bits` bits.
static inline s32 gfxWrapDelta(s32 from, s32 to, int bits)
{
	const s32 period = 1 << bits;
	s32       d      = (to - from) & (period - 1);
	if (d > period / 2)
		d -= period;
	return d;
}

static inline u16 gfxLerpWrap(u16 from, u16 to, int bits, int t, int limit)
{
	const s32 d = gfxWrapDelta(from, to, bits);
	if (d > limit || d < -limit)
		return to;
	return (u16)((from + ((d * t) >> 8)) & ((1 << bits) - 1));
}

static inline s32 gfxLerpLinear(s32 from, s32 to, int t, s32 limit)
{
	const s32 d = to - from;
	if (d > limit || d < -limit)
		return to;
	return from + ((d * t) >> 8);
}

// Copies the newer frame's sprites into `dst` with X and Y moved towards the
// older frame's values.  An object whose attributes changed between the two
// frames is left exactly as the newer frame has it: a different tile, size,
// flip or mode means the position in the other frame describes another
// picture, and moving it would show that other picture sliding.
static void gfxBuildInterpOam(u8 *dst, const u8 *older, const u8 *newer, int t)
{
	memcpy(dst, newer, 0x400);

	const u16 *a = (const u16 *)older;
	const u16 *b = (const u16 *)newer;
	u16 *      d = (u16 *)dst;

	for (int i = 0; i < 128; i++)
	{
		const u16 a0 = a[i * 4 + 0], a1 = a[i * 4 + 1], a2 = a[i * 4 + 2];
		const u16 b0 = b[i * 4 + 0], b1 = b[i * 4 + 1], b2 = b[i * 4 + 2];

		// Disabled in either frame: nothing to follow.
		if ((a0 & 0x0300) == 0x0200 || (b0 & 0x0300) == 0x0200)
			continue;

		// Everything but the coordinates has to match, or this is not the
		// same picture in both frames.
		if ((a0 & 0xFF00) != (b0 & 0xFF00) || (a1 & 0xFE00) != (b1 & 0xFE00) || a2 != b2)
			continue;

		const s32 dx = gfxWrapDelta(a1 & 0x1FF, b1 & 0x1FF, 9);
		const s32 dy = gfxWrapDelta(a0 & 0xFF, b0 & 0xFF, 8);
		if (dx > GFX_INTERP_OBJ_LIMIT || dx < -GFX_INTERP_OBJ_LIMIT)
			continue;
		if (dy > GFX_INTERP_OBJ_LIMIT || dy < -GFX_INTERP_OBJ_LIMIT)
			continue;

		const u16 x = (u16)(((b1 & 0x1FF) + ((dx * t) >> 8)) & 0x1FF);
		const u16 y = (u16)(((b0 & 0xFF) + ((dy * t) >> 8)) & 0xFF);

		d[i * 4 + 0] = (u16)((b0 & 0xFF00) | y);
		d[i * 4 + 1] = (u16)((b1 & 0xFE00) | x);
	}
}

// Replays the display for every line with the two rings interpolated by `t`
// (0 = older frame, 256 = newer frame) and writes the result into `dst`, which
// must have the same layout as `pix`: 241 pixels per row, the picture starting
// one row in, so the filters can read past the edges.
bool gfxReplayInterpolatedFrame(u8 *dst, int t)
{
	if (dst == NULL || gfxInterpolationMode == 0 || gfxRingFrames < 2)
		return false;

	const int bytesPerPixel = systemColorDepth / 8;
	if (bytesPerPixel != 2 && bytesPerPixel != 4)
		return false; // 24 bpp has a different row layout, see CPUDrawPixLine()

	const int newerIdx = gfxRingCur ^ 1;
	const int olderIdx = gfxRingCur;

	const GfxLineState *older = gfxLineRing[olderIdx];
	const GfxLineState *newer = gfxLineRing[newerIdx];

	// A mode change means the layers are something else on the other side of
	// it; a forced blank has no motion.  Both hand the frame back untouched.
	if (((older[0].dispcnt ^ newer[0].dispcnt) & 0x0007) != 0 || (newer[0].dispcnt & 0x0080) != 0)
		return false;

	if (t < 0)
		t = 0;
	if (t > 256)
		t = 256;

	u16 *const bgcntReg[4]  = { &BG0CNT, &BG1CNT, &BG2CNT, &BG3CNT };
	u16 *const bghofsReg[4] = { &BG0HOFS, &BG1HOFS, &BG2HOFS, &BG3HOFS };
	u16 *const bgvofsReg[4] = { &BG0VOFS, &BG1VOFS, &BG2VOFS, &BG3VOFS };
	u16 *const bgpaReg[2][4] = {
	    { &BG2PA, &BG2PB, &BG2PC, &BG2PD },
	    { &BG3PA, &BG3PB, &BG3PC, &BG3PD } };

	// Everything the replay writes is put back before returning: these are the
	// registers the game and the rest of the core read, and ioMem is only
	// written by UPDATE_REG, so changing the variables alone is not visible to
	// the emulated program.
	const u16 savedDispcnt = DISPCNT, savedBldmod = BLDMOD, savedColev = COLEV, savedColy = COLY;
	const u16 savedMosaic = MOSAIC;
	const u16 savedWin0h = WIN0H, savedWin1h = WIN1H, savedWin0v = WIN0V, savedWin1v = WIN1V;
	const u16 savedWinin = WININ, savedWinout = WINOUT;
	const u16 savedBgcnt[4]  = { BG0CNT, BG1CNT, BG2CNT, BG3CNT };
	const u16 savedBghofs[4] = { BG0HOFS, BG1HOFS, BG2HOFS, BG3HOFS };
	const u16 savedBgvofs[4] = { BG0VOFS, BG1VOFS, BG2VOFS, BG3VOFS };
	const u16 savedBgpa[2][4] = { { BG2PA, BG2PB, BG2PC, BG2PD }, { BG3PA, BG3PB, BG3PC, BG3PD } };
	const s32 savedBgx[2] = { gfxBG2X, gfxBG3X };
	const s32 savedBgy[2] = { gfxBG2Y, gfxBG3Y };
	const int savedChanged2 = gfxBG2Changed, savedChanged3 = gfxBG3Changed;
	const int savedLastVCOUNT = gfxLastVCOUNT;
	const int32 savedLayerEnable = layerEnable;
	const bool8 savedWindowOn = windowOn, savedFxOn = fxOn;
	const u16   savedVCOUNT = VCOUNT;
	u8 *const   savedOam = oam;

	if (gfxInterpolationMode >= 2)
	{
		static u8 interpOam[0x400];
		gfxBuildInterpOam(interpOam, gfxOamRing[olderIdx], gfxOamRing[newerIdx], t);
		oam = interpOam;
	}

	// The affine reference is preset per line, so the renderer must not reset
	// it from the registers: that is what `changed` asks it to do.  The wrap
	// test in mode1RenderLine() would do the same on the first line.
	gfxBG2Changed = 0;
	gfxBG3Changed = 0;
	gfxLastVCOUNT = 0;

	for (int y = 0; y < GFX_INTERP_LINES; y++)
	{
		const GfxLineState &sa = older[y];
		const GfxLineState &sb = newer[y];

		DISPCNT = sb.dispcnt;
		BLDMOD = sb.bldmod;
		COLEV  = sb.colev;
		COLY   = sb.coly;
		MOSAIC = sb.mosaic;

		WIN0H  = sb.win0h;
		WIN1H  = sb.win1h;
		WIN0V  = sb.win0v;
		WIN1V  = sb.win1v;
		WININ  = sb.winin;
		WINOUT = sb.winout;

		for (int i = 0; i < 4; i++)
		{
			*bgcntReg[i]  = sb.bgcnt[i];
			*bghofsReg[i] = gfxLerpWrap(sa.bghofs[i], sb.bghofs[i], 16, t, GFX_INTERP_SCROLL_LIMIT);
			*bgvofsReg[i] = gfxLerpWrap(sa.bgvofs[i], sb.bgvofs[i], 16, t, GFX_INTERP_SCROLL_LIMIT);
		}

		for (int i = 0; i < 2; i++)
		{
			for (int j = 0; j < 4; j++)
				*bgpaReg[i][j] = sb.bgpa[i][j];
		}

		gfxBG2X = gfxLerpLinear(sa.bgx[0], sb.bgx[0], t, GFX_INTERP_REF_LIMIT);
		gfxBG2Y = gfxLerpLinear(sa.bgy[0], sb.bgy[0], t, GFX_INTERP_REF_LIMIT);
		gfxBG3X = gfxLerpLinear(sa.bgx[1], sb.bgx[1], t, GFX_INTERP_REF_LIMIT);
		gfxBG3Y = gfxLerpLinear(sa.bgy[1], sb.bgy[1], t, GFX_INTERP_REF_LIMIT);

		// The same derivation CPUWriteMemory() does when DISPCNT changes.
		layerEnable = layerSettings & DISPCNT;
		windowOn    = (layerEnable & 0x6000) ? true : false;
		fxOn        = ((BLDMOD >> 6) & 3) != 0;

		VCOUNT = (u16)y;
		(*renderLine)();

		if (bytesPerPixel == 2)
		{
			u16 *d = (u16 *)dst + 241 * (y + 1);
			for (int x = 0; x < 240; x++)
				d[x] = systemColorMap16[lineMix[x] & 0xFFFF];
			d[240] = 0; // filters read one pixel past the edge
		}
		else
		{
			u32 *d = (u32 *)dst + 241 * (y + 1);
			for (int x = 0; x < 240; x++)
				d[x] = systemColorMap32[lineMix[x] & 0xFFFF];
		}
	}

	DISPCNT = savedDispcnt;
	BLDMOD = savedBldmod;
	COLEV  = savedColev;
	COLY   = savedColy;
	MOSAIC = savedMosaic;
	WIN0H  = savedWin0h;
	WIN1H  = savedWin1h;
	WIN0V  = savedWin0v;
	WIN1V  = savedWin1v;
	WININ  = savedWinin;
	WINOUT = savedWinout;

	for (int i = 0; i < 4; i++)
	{
		*bgcntReg[i]  = savedBgcnt[i];
		*bghofsReg[i] = savedBghofs[i];
		*bgvofsReg[i] = savedBgvofs[i];
	}

	for (int i = 0; i < 2; i++)
	{
		for (int j = 0; j < 4; j++)
			*bgpaReg[i][j] = savedBgpa[i][j];
	}

	gfxBG2X       = savedBgx[0];
	gfxBG3X       = savedBgx[1];
	gfxBG2Y       = savedBgy[0];
	gfxBG3Y       = savedBgy[1];
	gfxBG2Changed = savedChanged2;
	gfxBG3Changed = savedChanged3;
	gfxLastVCOUNT = savedLastVCOUNT;
	layerEnable   = savedLayerEnable;
	windowOn      = savedWindowOn;
	fxOn          = savedFxOn;
	VCOUNT        = savedVCOUNT;
	oam           = savedOam;

	return true;
}
