// System.cpp : Defines the system behaviors for the emulator.
//
#include "stdafx.h"
#include "Sound.h"
#include "Input.h"
#include "IUpdate.h"
#include "AVIWrite.h"
#include "WinMiscUtil.h"
#include "WinResUtil.h"
#include "resource.h"
#include "VBA.h"
#include "../gba/GBA.h"
#include "../gba/GBAGlobals.h"
#include "../gba/GBAinline.h"
#include "../gb/GB.h"
#include "../gb/gbGlobals.h"
//#include "../common/System.h"
#include "../common/SystemGlobals.h"
#include "../common/Text.h"
#include "../common/Util.h"
#include "../common/movie.h"
#include "../common/nesvideos-piece.h"
#include "../common/vbalua.h"
#include "../filters/lcdfilter.h"
#include "../version.h"
#include "Dialogs/ram_search.h"
#include <cassert>

u32	 RGB_LOW_BITS_MASK		 = 0;
int	 systemCartridgeType	 = IMAGE_GBA;
int	 systemSpeed			 = 0;
bool systemSoundOn			 = false;
u32	 systemColorMap32[0x10000];
u16	 systemColorMap16[0x10000];
u16	 systemGbPalette[24];
int	 systemRedShift			 = 0;
int	 systemBlueShift		 = 0;
int	 systemGreenShift		 = 0;
int	 systemColorDepth		 = 16;
int	 systemDebug			 = 0;
int	 systemVerbose			 = 0;
int	 systemFrameSkip		 = 0;
int	 systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;

// The buffer the display backends upload; NULL means `pix`.  The interpolation
// code points it at its own buffer for the extra frame it presents.
u8 *pixToPresent = NULL;

// static_assertion that BUTTON_REGULAR_RECORDING_MASK should be an u16 constant
namespace { const void * const s_STATIC_ASSERTION_(static_cast<void *>(BUTTON_REGULAR_RECORDING_MASK & 0xFFFF0000)); }

#define BMP_BUFFER_MAX_WIDTH (256)
#define BMP_BUFFER_MAX_HEIGHT (224)
#define BMP_BUFFER_MAX_DEPTH (4)
static u8 bmpBuffer[BMP_BUFFER_MAX_WIDTH * BMP_BUFFER_MAX_HEIGHT * BMP_BUFFER_MAX_DEPTH];

static int s_stockThrottleValues[] = {
	6, 15, 25, 25, 37, 50, 75, 87, 100, 112, 125, 150, 200, 300, 400, 600, 800, 1000
};

extern bool vbaShuttingDown;

void winSignal(int, int);
void winOutput(const char *, u32);

void (*dbgSignal)(int, int)	= &winSignal;
void (*dbgOutput)(const char *, u32) = &winOutput;

// Win32 stuff

//////////////////////////////////////////////
// ultility

void winSignal(int, int)
{}

void winOutput(const char *s, u32 addr)
{
	if (s)
	{
		log(s);
	}
	else
	{
		// Bytes the emulated CPU left in its debug area: they are a string in
		// the code page the core works in, so they go straight into the narrow
		// buffer instead of being widened and narrowed again on the way.
		CStringA str;
		while (char c = CPUReadByteQuick(addr++))
		{
			str += c;
		}
		log(str);
	}
}

#ifdef SDL
void log(const char *defaultMsg, ...)
{
	char	buffer[2048];
	va_list valist;

	va_start(valist, defaultMsg);
	vsprintf(buffer, defaultMsg, valist);

	if (out == NULL)
	{
		out = fopen("trace.log", "w");
	}

	fputs(buffer, out);

	va_end(valist);
}
#else
void log(const char *msg, ...)
{
	va_list valist;
	va_start(valist, msg);

#if 1
	// The log viewer takes the ANSI text these debug calls produce.
	CStringA buffer;
	buffer.FormatV(msg, valist);
	extern void toolsLog(const char *);
	toolsLog(buffer);
#else
	extern void winlog(LPCTSTR msg, ...);
	winlog(msg, valist);
#endif

	va_end(valist);
}
#endif

////////////////////////////////////

// input
int systemGetDefaultJoypad()
{
	return theApp.joypadDefault;
}

void systemSetDefaultJoypad(int which)
{
	theApp.joypadDefault = which;
}

bool systemReadJoypads()
{
	// this function is called at every frame, even if vba is fast-forwarded.
	// so we try to limit the input frequency here just in case.
	static u32 lastTime = systemGetClock();
	if ((u32)(systemGetClock() - lastTime) < 10)
		return false;
	lastTime = systemGetClock();

	if (theApp.input)
		return theApp.input->readDevices();
	return false;
}

u32 systemGetOriginalJoypad(int i, bool sensor)
{
	if (i < 0 || i > 3)
		i = 0;

	u32 res = 0;
	if (theApp.input)
		res = theApp.input->readDevice(i, sensor);

	// +auto input, XOR
	// maybe these should be moved into DirectInput.cpp
	if (theApp.autoFire || theApp.autoFire2)
	{
		res ^= (theApp.autoFireToggle ? theApp.autoFire : theApp.autoFire2);
		// systemUpdateJoypads() calls this once for each of the four joypads,
		// but the phase may only advance once per frame.  Flipping it on every
		// call made it flip an even number of times per frame, i.e. never, so
		// the same half was XORed in on every frame and the button was held
		// down instead of being pressed every other frame.
		if (i == 0 && (!theApp.autofireAccountForLag || !systemCounters.laggedLast))
		{
			theApp.autoFireToggle = !theApp.autoFireToggle;
		}
	}
	if (theApp.autoHold)
	{
		res ^= theApp.autoHold;
	}

	// filter buttons
	// maybe better elsewhere?
	if (!theApp.allowLeftRight)
	{
		// disallow L+R or U+D to being pressed at the same time
		if ((res & (BUTTON_MASK_RIGHT | BUTTON_MASK_LEFT)) == (BUTTON_MASK_RIGHT | BUTTON_MASK_LEFT))
			res &= ~BUTTON_MASK_RIGHT;  // leave only LEFT on
		if ((res & (BUTTON_MASK_DOWN | BUTTON_MASK_UP)) == (BUTTON_MASK_DOWN | BUTTON_MASK_UP))
			res &= ~BUTTON_MASK_DOWN;  // leave only UP on
	}

	if (!sensor)
	{
		if (res & BUTTON_MOTION_MASK)
			res &= ~BUTTON_MOTION_MASK;
	}

	if (systemCartridgeType != IMAGE_GBA && !gbSgbMode) // regular GB has no L/R buttons
	{
		if (res & (BUTTON_GBA_ONLY))
			res &= ~BUTTON_GBA_ONLY;
	}

	return res;
}

// screen

void systemRenderLua(u8 *data, int pitch)
{
	int copyW, copyH;
	int screenW, screenH;
	int copyOffsetX, copyOffsetY;
	systemGetLCDResolution(copyW, copyH);
	systemGetLCDBaseSize(screenW, screenH);
	systemGetLCDBaseOffset(copyOffsetX, copyOffsetY);

	//int displayW = right - left;
	//int displayH = bottom - top;
	//screenW = screenW * displayW / copyW;
	//screenH = screenH * displayH / copyH;
	//copyOffsetX = left + copyOffsetX * displayW / copyW;
	//copyOffsetY = top  + copyOffsetY * displayH / copyH;

	//int pitch = (((ppl * systemColorDepth + 7)>>3)+3)&~3;
	//int pitch = ppl * (systemColorDepth / 8) + (systemColorDepth == 24 ? 0 : 4);

	VBALuaGui(&data[(copyOffsetY + 1) * pitch + copyOffsetX * (systemColorDepth / 8)], pitch, screenW, screenH);
	VBALuaClearGui();
}

// delayed repaint
void systemRefreshScreen()
{
	if (theApp.m_pMainWnd)
	{
		theApp.m_pMainWnd->PostMessage(WM_PAINT, NULL, NULL);
	}
}

// Nearest-neighbour source index for every destination index along one axis.
// The map is built incrementally, so the per-pixel work in the scaling loop is
// a table lookup instead of a 64-bit multiply/divide (which compiles to a
// helper call in this 32-bit build and dominated the frame time).
struct ScaleAxisMap
{
	int  srcSize;
	int  dstSize;
	int *map;

	ScaleAxisMap() : srcSize(0), dstSize(0), map(NULL) {}
	~ScaleAxisMap() { delete[] map; }

	void ensure(int src, int dst)
	{
		if (map && src == srcSize && dst == dstSize)
			return;

		delete[] map;
		map		= new int[dst > 0 ? dst : 1];
		srcSize = src;
		dstSize = dst;

		// map[i] = floor(i * src / dst)
		int acc = 0;
		int si	= 0;
		for (int i = 0; i < dst; i++)
		{
			map[i] = si;
			acc += src;
			while (acc >= dst)
			{
				acc -= dst;
				si++;
			}
		}
	}
};

// Renders a frame at outWidth x outHeight with the active pixel filter applied
// and scaled up to the window magnification.  The raw frame carries the
// emulator's border padding and is 16 or 32 bpp, so it is converted to 24-bit
// BGR in a single pass (nearest neighbour, matching what the GDI renderer
// shows).  AVI frames are bottom-up DIB rows, screenshots are top-down.  The
// buffer is cached and stays valid until the next call, or NULL when there is
// nothing to render.
//
// `source` has the layout of `pix` - 241 pixels per row with one blank row on
// top - which is what lets the interpolated frame go through the same path.
static u8 *systemRenderScaledFrame(int outWidth, int outHeight, bool bottomUp, const u8 *source)
{
	const int depth = systemColorDepth / 8;

	if (depth != 2 && depth != 4)
		return NULL;

	const int rectWidth  = theApp.rect.right  - theApp.rect.left;
	const int rectHeight = theApp.rect.bottom - theApp.rect.top;

	if (outWidth <= 0 || outHeight <= 0 || rectWidth <= 0 || rectHeight <= 0)
		return NULL;

	const int srcPitch  = theApp.filterWidth * depth + 4;
	const int filtPitch = rectWidth * depth;

	// Filtered frame, only produced when a pixel filter is active.
	static u8 *filtered		= NULL;
	static int filteredSize = 0;
	const int filteredBytes = filtPitch * rectHeight;
	if (filteredSize < filteredBytes)
	{
		delete[] filtered;
		filtered	 = new u8[filteredBytes];
		filteredSize = filteredBytes;
	}

	// Row 0 of the raw buffer is the filter's over-read border; the LCD image
	// starts one row in (this matches how the renderers call the filters).
	const u8 *frame		= source + srcPitch;
	int		  framePitch = srcPitch;
	if (theApp.filterFunction && filtered)
	{
		// The filters only read their source, but the port's signature is not
		// const: the same one is used by the renderers, where the buffer is
		// writable.
		(*theApp.filterFunction)((u8 *)(source + srcPitch), srcPitch, (u8 *)theApp.delta,
		                         filtered, filtPitch, theApp.filterWidth, theApp.filterHeight);
		frame	   = filtered;
		framePitch = filtPitch;
	}

	static u8 *scaled	 = NULL;
	static int scaledSize = 0;
	const int outPitch = outWidth * 3;
	const int outBytes = outPitch * outHeight;
	if (scaledSize < outBytes)
	{
		delete[] scaled;
		scaled	  = new u8[outBytes];
		scaledSize = outBytes;
	}

	if (!scaled)
		return NULL;

	static ScaleAxisMap xMap;
	static ScaleAxisMap yMap;
	xMap.ensure(rectWidth, outWidth);
	yMap.ensure(rectHeight, outHeight);

	const int	*rx		= xMap.map;
	const int	*ry		= yMap.map;
	const int	 rShift = systemRedShift;
	const int	 gShift = systemGreenShift;
	const int	 bShift = systemBlueShift;
	const size_t rowStep = (size_t)framePitch;

	u8 *dst		 = bottomUp ? scaled + outPitch * (outHeight - 1) : scaled;
	const int step = bottomUp ? -outPitch : outPitch;

	if (depth == 2)
	{
		for (int y = 0; y < outHeight; y++)
		{
			const u16 *srcRow = (const u16 *)(frame + ry[y] * rowStep);
			u8 *d			  = dst;

			for (int x = 0; x < outWidth; x++)
			{
				const u32 v = srcRow[rx[x]];
				*d++ = (u8)(((v >> bShift) & 0x1f) << 3);
				*d++ = (u8)(((v >> gShift) & 0x1f) << 3);
				*d++ = (u8)(((v >> rShift) & 0x1f) << 3);
			}

			dst += step;
		}
	}
	else
	{
		for (int y = 0; y < outHeight; y++)
		{
			const u32 *srcRow = (const u32 *)(frame + ry[y] * rowStep);
			u8 *d			  = dst;

			for (int x = 0; x < outWidth; x++)
			{
				const u32 v = srcRow[rx[x]];
				*d++ = (u8)(((v >> bShift) & 0x1f) << 3);
				*d++ = (u8)(((v >> gShift) & 0x1f) << 3);
				*d++ = (u8)(((v >> rShift) & 0x1f) << 3);
			}

			dst += step;
		}
	}

	return scaled;
}

// Hands one frame to the recorder.  `frame` has the layout of `pix`, which is
// what lets the interpolated picture go through the same path.
static void systemWriteAviFrame(const u8 *frame, int width, int height)
{
	if (theApp.captureScaled && (systemColorDepth == 16 || systemColorDepth == 32))
	{
		u8 *scaled = systemRenderScaledFrame(theApp.aviRecorder->videoWidth(),
		                                     theApp.aviRecorder->videoHeight(),
		                                     true, frame);
		if (scaled != NULL)
			theApp.aviRecorder->AddFrame(scaled);
	}
	else
	{
		assert(
			width <= BMP_BUFFER_MAX_WIDTH && height <= BMP_BUFFER_MAX_HEIGHT
			&& systemColorDepth <= BMP_BUFFER_MAX_DEPTH * 8);
		utilWriteBMP(bmpBuffer, width, height, systemColorDepth, frame);
		theApp.aviRecorder->AddFrame(bmpBuffer);
	}
}

// Writes a screenshot rendered the way an AVI frame is: with the active pixel
// filter and at the size the emulator is drawn, matching the "window size and
// filter" capture option.  Returns false when no such frame could be produced.
bool winWriteFilteredScreenshot(const CString &fileName)
{
	int width  = theApp.surfaceSizeX;
	int height = theApp.surfaceSizeY;

	if (width <= 0 || height <= 0)
	{
		width  = theApp.rect.right - theApp.rect.left;
		height = theApp.rect.bottom - theApp.rect.top;
	}

	const u8 *frame = systemRenderScaledFrame(width, height, false, pix);
	if (frame == NULL)
		return false;

	return utilWriteImageBGR(winAnsiFromWide(fileName), width, height, frame, width * 3);
}

// `extraFrame` is the interpolated picture to write in front of the real one,
// or NULL when the file is written at the emulator's own rate.
static void systemRecordAviFrame(int width, int height, const u8 *extraFrame)
{
	extern u32 linearSoundFrameCount;
	extern u32 linearFrameCount;

	++linearFrameCount;
	if (!theApp.sound)
	{
		u32 wrap = systemGetFrameRateDividend();
		if (linearFrameCount > wrap)
			linearFrameCount -= wrap;
		linearSoundFrameCount = linearFrameCount;
	}

	// record avi
	bool firstFrameLogged = false;
	--linearFrameCount;
	do
	{
		++linearFrameCount;

		if (theApp.aviRecording && (!theApp.altAviRecordMethod || !firstFrameLogged))
		{
			// usually aviRecorder is created when vba starts avi recording, though
			if (theApp.aviRecorder == NULL)
			{
				theApp.aviRecorder = new AVIWrite();

				// When recording the filtered window contents the frame is the
				// size the emulator is displayed at, not the raw LCD resolution.
				if (theApp.captureScaled && theApp.surfaceSizeX > 0 && theApp.surfaceSizeY > 0)
				{
					width  = theApp.surfaceSizeX;
					height = theApp.surfaceSizeY;
				}

				BITMAPINFOHEADER bi;
				memset(&bi, 0, sizeof(bi));
				bi.biSize	   = 0x28;
				bi.biPlanes	   = 1;
				bi.biBitCount  = 24;
				bi.biWidth	   = width;
				bi.biHeight	   = height;
				bi.biSizeImage = 3 * width * height;
				theApp.aviRecorder->SetVideoFormat(&bi);
				if (!theApp.aviRecorder->Open(theApp.aviRecordName))
				{
					delete theApp.aviRecorder;
					theApp.aviRecorder	= NULL;
					theApp.aviRecording = false;
				}
			}

			// An audio-only recording has no picture to build, so skip the
			// conversion instead of handing AddFrame() a frame it drops.
			if (theApp.aviRecorder != NULL && !theApp.aviRecorder->IsPaused() &&
			    theApp.aviRecorder->videoWidth() > 0)
			{
				// A file written at twice the emulator's rate takes the
				// interpolated picture first, so the pair covers the half
				// frame before this one and then this one.
				if (extraFrame != NULL)
					systemWriteAviFrame(extraFrame, width, height);

				systemWriteAviFrame(pix, width, height);
			}
		}

		if (theApp.nvVideoLog)
		{
			// convert from whatever bit depth to 16-bit, while stripping away extra pixels
			assert(width <= BMP_BUFFER_MAX_WIDTH && height <= BMP_BUFFER_MAX_HEIGHT && 16 <= BMP_BUFFER_MAX_DEPTH * 8);
			utilWriteBMP(bmpBuffer, width, -height, 16, pix);
			NESVideoLoggingVideo((u8 *)bmpBuffer, width, height, 0x1000000 * 60);
		}

		firstFrameLogged = true;
	}
	while (linearFrameCount < linearSoundFrameCount); // compensate for frames lost due to frame skip being nonzero, etc.
}

// --- frame interpolation ----------------------------------------------------
//
// The game shows 59.7275 pictures a second.  An extra one is drawn between two
// of them by replaying the display over an interpolation of the state each
// scanline saw - see GBAGfx.cpp for that part and
// documentation/frame-interpolation.md for the reasoning.  The result goes
// into its own buffer and is presented before the real frame, so nothing that
// reads `pix` (recording, screenshots, `-hashtrace`) ever sees it.

static u8 *interpolationPix = NULL;

// From src/gba/GBAGfx.cpp: the ring of per-scanline display state, and the
// switch that makes the core fill it at all.
extern int	gfxInterpolationMode;
extern void gfxResetLineStates();
extern bool gfxReplayInterpolatedFrame(u8 *dst, int t);

// Whether this frame gets an interpolated one in front of it.  Everything is
// computed per frame on purpose: a menu toggle, a frame skip setting or a
// single frame step has to take effect on the next frame, and the ring in
// GBAGfx.cpp is only meaningful while every frame is drawn.
bool systemInterpolationSupported()
{
	if (theApp.frameInterpolation == 0 || theApp.display == NULL)
		return false;
	return theApp.display->supportsInterpolation();
}

bool systemInterpolationActive()
{
	if (!systemInterpolationSupported())
		return false;
	if (systemCartridgeType != IMAGE_GBA)
		return false;
	if (speedup || theApp.frameSearchSkipping || theApp.paused)
		return false;

	// Skipping frames and interpolating them are opposites: a skipped frame is
	// never drawn, so there is nothing to interpolate towards.
	if (systemFramesToSkip() != 0)
		return false;

	// Frame advance: the user is inspecting single frames, not motion.
	if (theApp.winPauseNextFrame)
		return false;

	return true;
}

// Keeps GBAGfx.cpp's ring on the same frames this file interpolates.  Leaving
// the ring alone when the feature is off keeps the capture cost at zero, and
// dropping it whenever the answer changes means the frames on either side of a
// pause, a frame step or a renderer switch are never interpolated together.
static void systemInterpolationSync()
{
	static int lastMode = -1;

	const int mode = systemInterpolationActive() ? theApp.frameInterpolation : 0;
	if (mode != lastMode)
	{
		gfxInterpolationMode = mode;
		gfxResetLineStates();
		// Says why the picture suddenly has no extra frame in it: 0 is "off or
		// not possible right now", 1 scroll only, 2 scroll and objects.
		winlog(_T("frame interpolation: mode %d -> %d\n"), lastMode, mode);
		lastMode = mode;
	}
}

// Builds the frame between the last two drawn ones.  False when the core could
// not produce one, in which case the caller presents the real frame alone.
static bool systemBuildInterpolatedFrame()
{
	if (!systemInterpolationActive())
		return false;

	if (interpolationPix == NULL)
		interpolationPix = (u8 *)PIX_CALLOC(4 * 241 * 162);

	if (!gfxReplayInterpolatedFrame(interpolationPix, 128))
		return false;

	// The on screen display is painted into the frame buffer in the "in game"
	// text mode, so the extra frame needs it too - otherwise every message
	// blinks at the doubled rate.  The message timing runs on the wall clock,
	// so drawing it a second time costs nothing and changes no state.
	const int pitch = theApp.filterWidth * (systemColorDepth / 8) + (systemColorDepth == 24 ? 0 : 4);
	if (textMethod == 0)
	{
		int copyX, copyY;
		systemGetLCDResolution(copyX, copyY);
		DrawTextMessages(&interpolationPix[pitch], pitch, 0, copyY);
	}

	// One-shot diagnostic, about two seconds in: says whether the replay
	// produced a frame of its own or handed back a copy of the real one, which
	// is the first thing to know when the feature seems to do nothing.  It
	// costs a single 240x160 compare per run.
	static int built = 0;
	const bool sample = (built == 120 || built == 300 || built == 600 || built == 900);
	if (sample && systemColorDepth == 32)
	{
		int diff = 0;
		const u32 *r = (const u32 *)(pix + pitch);
		const u32 *i = (const u32 *)(interpolationPix + pitch);
		for (int y = 0; y < 160; y++)
			for (int x = 0; x < 240; x++)
				if (r[y * 241 + x] != i[y * 241 + x])
					diff++;
		winlog(_T("frame interpolation: %d of %d pixels differ from the real frame\n"), diff, 240 * 160);
	}
	built++;

	return true;
}

// Waits out the second half of the frame period: systemFrame() stops at the
// half way mark, the interpolated frame goes out, and this puts the real frame
// on the other half - back to back the second one would overwrite the first
// before the monitor ever scanned it out.
static void systemInterpolationPace()
{
	if (!synchronize || !systemInterpolationActive())
		return;

	const int target = (int)(100000 / (systemGetFrameRate() * theApp.throttle));
	const int half   = target - target / 2;
	if (half <= 0)
		return;

	const int elapsed = (int)(systemGetClock() - theApp.throttleLastTime);
	if (half - elapsed > 0)
		Sleep(half - elapsed);

	theApp.throttleLastTime = systemGetClock();
}

// Presents one buffer without touching the per-frame listeners, which the real
// frame's present runs once.
static void systemPresentBuffer(u8 *buffer)
{
	if (vbaShuttingDown)
		return;

	pixToPresent = buffer;
	if (theApp.display)
		theApp.display->render();
	pixToPresent = NULL;
}

void systemRenderFrame()
{
	++theApp.renderedFrames;

	// "in-game" text rendering
	if (textMethod == 0) // transparent text can only be painted once, so timed messages will not be updated
	{
		int pitch = theApp.filterWidth * (systemColorDepth / 8) + (systemColorDepth == 24 ? 0 : 4);
		int copyW, copyH;
		systemGetLCDResolution(copyW, copyH);
		systemRenderLua(&pix[pitch], pitch);
		DrawTextMessages(&pix[pitch], pitch, 0, copyH);
	}

	VBAUpdateFrameCountDisplay();
	VBAUpdateButtonPressDisplay();

	// The status bar is the only place the counters and messages show while it
	// is enabled, and this is its refresh clock.
	theApp.updateStatusBar();

	// Built before the recorder runs: in the file the interpolated picture
	// comes first and the real one after it, the same order they are shown.
	const bool interpolated = systemBuildInterpolatedFrame();

	// avi dump
	int copyX, copyY;
	systemGetLCDResolution(copyX, copyY);

	// A file written at twice the emulator's rate has to take two pictures
	// per frame even when the replay refused to build one, or the picture
	// would run ahead of the sound.
	const bool doubled =
	    theApp.aviRecording && theApp.aviRecorder != NULL &&
	    theApp.aviRecorder->wantsInterpolatedFrames();
	systemRecordAviFrame(copyX, copyY, doubled ? (interpolated ? interpolationPix : pix) : NULL);

	// Interframe blending is a temporal filter of its own, so running both would
	// only smear the picture.  Interpolation wins when it is on.
	if (!interpolated && theApp.ifbFunction)
	{
		if (systemColorDepth == 16)
			theApp.ifbFunction(pix + theApp.filterWidth * 2 + 4, theApp.filterWidth * 2 + 4,
			                   theApp.filterWidth, theApp.filterHeight);
		else
			theApp.ifbFunction(pix + theApp.filterWidth * 4 + 4, theApp.filterWidth * 4 + 4,
			                   theApp.filterWidth, theApp.filterHeight);
	}

	if (interpolated)
	{
		systemPresentBuffer(interpolationPix);
		systemInterpolationPace();

		// The speed display counts pictures, not emulated frames, so it shows
		// the doubled rate while interpolation is on.  Nothing else reads
		// this counter.
		++theApp.renderedFrames;
	}

	systemRedrawScreen();

	Signal_RAM_Search_New_Frame(); // updates RAM search regions
}

void systemRedrawScreen()
{
	if (vbaShuttingDown)
		return;

	// The real frame; systemPresentBuffer() already handed its buffer back.
	pixToPresent = NULL;

	if (theApp.display)
		theApp.display->render();

	systemUpdateListeners();
}

void systemUpdateListeners()
{
	if (vbaShuttingDown)
		return;

	Update_RAM_Search(); // updates RAM search and RAM watch

	// update viewers etc.
	if (theApp.updateCount)
	{
		POSITION pos = theApp.updateList.GetHeadPosition();
		while (pos)
		{
			IUpdateListener *up = theApp.updateList.GetNext(pos);
			if (up)
				up->update();
		}
	}
}

int systemScreenCapture(int captureNumber)
{
	return winScreenCapture(captureNumber);
}

static void winShowSystemMessage(const CString &text)
{
	systemSoundClearBuffer();

	// A language module is loaded from loadSettings, before the main window
	// exists, so a message about it can arrive while m_pMainWnd is still
	// null.  Use a null owner in that case instead of dereferencing it.
	HWND owner = AfxGetApp()->m_pMainWnd != NULL
	             ? AfxGetApp()->m_pMainWnd->GetSafeHwnd() : NULL;
	/**/ ::MessageBox(owner, text, winResLoadString(IDS_ERROR),
	                  MB_OK | MB_ICONERROR);
}

// Wide form, used by the Win32 layer.  The format and the arguments are both
// TCHAR, so a translated string from a language module and its arguments agree
// in width whatever the translation contains.
void systemMessage(int number, const wchar_t *defaultMsg, ...)
{
	const CString format = number ? winResLoadString(number) : CString(defaultMsg);

	va_list valist;
	va_start(valist, defaultMsg);
	CString text;
	text.FormatV(format, valist);
	va_end(valist);

	winShowSystemMessage(text);
}

// Narrow form, kept for the emulation core, which has no notion of TCHAR.  Its
// format and arguments are ANSI, so the text is formatted there and then
// widened.  A translated string replaces it only when the format takes no
// arguments: a resource string is TCHAR and would not match ANSI arguments.
void systemMessage(int number, const char *defaultMsg, ...)
{
	va_list valist;
	va_start(valist, defaultMsg);
	CStringA text;
	text.FormatV(defaultMsg, valist);
	va_end(valist);

	CString message;
	if (number)
	{
		const CString translated = winResLoadString(number);
		if (translated.Find('%') < 0)
			message = translated;
	}
	if (message.IsEmpty())
		message = winWideFromAnsi(text);

	winShowSystemMessage(message);
}

// A colour list is one id per character of the message, not text: the core
// fills a fixed-size buffer (VBAUpdateFrameCountDisplay() fills every byte of
// it) and the narrow buffer this layer used to keep was copied with strncpy(),
// which is where the terminator came from.  Cut the list at the 40 characters
// the message keeps and terminate the copy here, so that winWideFromAnsi()
// has an end to read instead of running off the end of the caller's array.
static CString wideFromColorList(const char *colorList)
{
	char colors[41];
	int  count = 0;

	if (colorList != NULL)
	{
		while (count < 40 && colorList[count] != '\0')
		{
			colors[count] = colorList[count];
			++count;
		}
	}
	colors[count] = '\0';

	return winWideFromAnsi(colors);
}

// The single place a message is stored, in both of its renderings.  See the
// comment on the buffers in VBA.h for why there are two.
static void storeScreenMessage(int slot, int duration, const char *colorList,
                               const CString &asciiText, const CString &localizedText)
{
	if (slot < 0 || slot >= SCREEN_MESSAGE_SLOTS)
		return;

	theApp.screenMessage[slot] = true;
	theApp.screenMessageTime[slot]		  = GetTickCount();
	theApp.screenMessageDuration[slot]	  = duration;
	theApp.screenMessageBuffer[slot]	  = localizedText;
	theApp.screenMessageAsciiBuffer[slot] = asciiText;
	theApp.screenMessageColorBuffer[slot] = wideFromColorList(colorList);

	// 40 characters is what a row of the picture has room for.  The status
	// bar ellipsises on its own, but it reads the same buffer.
	if (theApp.screenMessageBuffer[slot].GetLength() > 40)
		theApp.screenMessageBuffer[slot] = theApp.screenMessageBuffer[slot].Left(40);
	if (theApp.screenMessageAsciiBuffer[slot].GetLength() > 40)
		theApp.screenMessageAsciiBuffer[slot] = theApp.screenMessageAsciiBuffer[slot].Left(40);

	// update the display when a main slot message appears while the game is paused
	if (slot == 0 && (theApp.paused || (theApp.frameSearching)))
		systemRefreshScreen();

	// A paused status bar has no frame to wait for either.
	theApp.updateStatusBar();
}

void systemScreenMessage(const char *msg, int slot, int duration, const char *colorList)
{
	// Text the emulation core assembled itself: there is no translation for
	// it, so both renderings are the same string.
	const CString text = winWideFromAnsi(msg);

	storeScreenMessage(slot, duration, colorList, text, text);
}

// A message whose text lives in the string table.  It is resolved twice: the
// executable's copy is English and goes to the two modes that draw the frame
// with the built-in ASCII font, the language module's copy goes to "on screen"
// and to the status bar.
//
// The arguments are formatted twice, once per string.  They have to match the
// specifiers of both, which is the same requirement a translation of anything
// else in the tree has to meet.
void systemScreenMessageRes(UINT id, ...)
{
	const CString ascii	  = winResLoadOsdString(id);
	const CString localized = winResLoadString(id);

	// A string the translation left out falls back to the English one, and a
	// missing executable string - which should not happen - to the other.
	const CString &asciiFormat	  = ascii.IsEmpty() ? localized : ascii;
	const CString &localizedFormat = localized.IsEmpty() ? ascii : localized;

	va_list args;
	va_start(args, id);
	va_list copy;
	va_copy(copy, args);

	CString asciiText, localizedText;
	asciiText.FormatV(asciiFormat, args);
	va_end(args);

	localizedText.FormatV(localizedFormat, copy);
	va_end(copy);

	storeScreenMessage(0, 3000, NULL, asciiText, localizedText);
}

void systemShowSpeed(int speed)
{
	systemSpeed = speed;
	theApp.showRenderedFrames = theApp.renderedFrames;
	theApp.renderedFrames	  = 0;
	if (!isFullscreenOption(theApp.videoOption) && theApp.showSpeed)
	{
		CString buffer;
		if (theApp.showSpeed == 1)
			buffer.Format(_T(VBA_NAME_AND_VERSION) _T(" %3d%%"), systemSpeed);
		else
			buffer.Format(_T(VBA_NAME_AND_VERSION) _T(" %3d%% (%d fps | %d skipped)"),
			              systemSpeed,
			              theApp.showRenderedFrames,
			              systemFrameSkip);

		systemSetTitle(buffer);
	}
}

void systemSetTitle(const char *title)
{
	if (theApp.m_pMainWnd != NULL)
	{
		AfxGetApp()->m_pMainWnd->SetWindowText(winWideFromAnsi(title));
	}
}

void systemSetTitle(const wchar_t *title)
{
	if (theApp.m_pMainWnd != NULL)
	{
		AfxGetApp()->m_pMainWnd->SetWindowText(title);
	}
}

// timing/speed

u32 systemGetClock()
{
	return timeGetTime();
}

void systemIncreaseThrottle()
{
	int throttle = theApp.throttle;

	if (throttle < 6)
		++throttle;
	else if (throttle < s_stockThrottleValues[_countof(s_stockThrottleValues) - 1])
	{
		int i = 0;
		while (throttle >= s_stockThrottleValues[i])
		{
			++i;
		}
		throttle = s_stockThrottleValues[i];
	}

	systemSetThrottle(throttle);
}

void systemDecreaseThrottle()
{
	int throttle = theApp.throttle;

	if (throttle > 6)
	{
		int i = _countof(s_stockThrottleValues) - 1;
		while (throttle <= s_stockThrottleValues[i])
		{
			--i;
		}
		throttle = s_stockThrottleValues[i];
	}
	else if (throttle > 1)
		--throttle;

	systemSetThrottle(throttle);
}

void systemSetThrottle(int throttle)
{
	theApp.throttle = throttle;
	char str[256];
	sprintf(str, "%d%% throttle speed", theApp.throttle);
	systemScreenMessage(str);
}

int systemGetThrottle()
{
	return theApp.throttle;
}

void systemFrame()
{
	// Decides, for the frame that is about to be emulated, whether the display
	// layer wants an interpolated frame in front of it.  It has to be asked
	// here because GBAGfx.cpp captures the scanline state while the frame is
	// drawn, before the frame boundary below is reached.
	systemInterpolationSync();

	if (theApp.altAviRecordMethod && theApp.aviRecording)
	{
		if (theApp.aviRecorder)
		{
			if (!theApp.aviRecorder->IsSoundAdded())
			{
				WAVEFORMATEX wfx;
				memset(&wfx, 0, sizeof(wfx));
				wfx.wFormatTag		= WAVE_FORMAT_PCM;
				wfx.nChannels		= 2;
				wfx.nSamplesPerSec	= 44100 / soundQuality;
				wfx.wBitsPerSample	= 16;
				wfx.nBlockAlign		= (wfx.wBitsPerSample / 8) * wfx.nChannels;
				wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
				wfx.cbSize = 0;
				theApp.aviRecorder->SetSoundFormat(&wfx);
			}
			theApp.aviRecorder->AddSound((u8 *)soundFrameSound, soundFrameSoundWritten * 2);
		}
	}

	soundFrameSoundWritten = 0;

	// no more stupid updates :)

	extern int quitAfterTime;                   // from VBA.cpp
	void	   VBAMovieStop(bool8 suppress_message); // from ../movie.cpp
	if (quitAfterTime >= 0 && systemCounters.frameCount == quitAfterTime)
	{
		VBAMovieStop(true);
		AfxPostQuitMessage(0);
	}

	// While a script holds the speed, nothing down here may slow the frame
	// down: neither the sound rate nor the sleep below.
	const bool luaSpeed = VBALuaSpeed() >= 0;

	// change the sound speed, or set it to normal - must always do this or it won't get reset after a change, but that's OK
	// because it's inexpensive
	if (theApp.sound)
	{
		theApp.sound->setSpeed(
		    speedup || luaSpeed || theApp.winPauseNextFrame || !synchronize || theApp.accuratePitchThrottle || theApp.useOldSync
			? 1.0f : (float)theApp.throttle / 100.0f);
	}

	// if a throttle speed is set and we're not allowed to change the sound frequency to achieve it,
	// sleep for a certain amount each time we get here to approximate the necessary slowdown
	if (synchronize && (theApp.accuratePitchThrottle || !theApp.sound || theApp.throttle < 6) /*&& !theApp.winPauseNextFrame*/)
	{
		/// FIXME: this is still a horrible way of achieving a certain frame time
		///        (look at what Snes9x does - it's complicated but much much better)

		static float sleepAmt = 0.0f; // variable to smooth out the sleeping amount so it doesn't oscillate so fast
//		if(!theApp.wasPaused) {
		if (!speedup && !luaSpeed)
		{
			u32 time = systemGetClock();
			u32 diff = time - theApp.throttleLastTime;
			if (theApp.wasPaused)
				diff = 0;

			int target = (int)(100000 / (systemGetFrameRate() * theApp.throttle));
			// With an interpolated frame in front of every real one the period
			// is split in two: this wait covers the first half, and
			// systemInterpolationPace() covers the second one after presenting
			// the interpolated frame.
			if (systemInterpolationActive())
				target /= 2;
			int d	   = (target - diff);

			if (d > 1000) // added to avoid 500-day waits for vba to start emulating.
				d = 1000;  // I suspect most users aren't that patient, and would find 1 second to be a more reasonable delay.

			sleepAmt = 0.8f * sleepAmt + 0.2f * (float)d;
			if (d - sleepAmt <= 1.5f && d - sleepAmt >= -1.5f)
				d = (int)(sleepAmt);

			if (d > 0)
			{
				Sleep(d);
			}
		}
		theApp.throttleLastTime = systemGetClock();
		//}
		//else
		//{
		// Sleep(100);
		//}
	}

	if (systemCounters.frameCount % 10 == 0)
	{
		if (theApp.rewindMemory)
		{
			if (++theApp.rewindCounter >= (theApp.rewindTimer))
			{
				theApp.rewindSaveNeeded = true;
				theApp.rewindCounter	= 0;
			}
		}
		if (systemSaveUpdateCounter)
		{
			if (--systemSaveUpdateCounter <= SYSTEM_SAVE_NOT_UPDATED)
			{
				winWriteBatteryFile();
				systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;
			}
		}
	}

	theApp.wasPaused = false;
///  theApp.autoFrameSkipLastTime = time;
}

int systemFramesToSkip()
{
	int framesToSkip = systemFrameSkip;

	bool fastForward = speedup;

	fastForward = (fastForward || theApp.frameSearchSkipping);
	int throttle = theApp.throttle;
	if (theApp.frameSearching && throttle < 100)
		throttle = 100;

	if (theApp.aviRecording || theApp.nvVideoLog || systemPausesNextFrame())
	{
		framesToSkip = 0; // render all frames
	}
	else
	{
		if (framesToSkip >= FRAMESKIP_UNLIMITED)
		{
			// "Skip All Frames": never reach frameSkipCount, so no frame is
			// ever drawn.  Not scaled by the throttle and not capped by it -
			// this is the setting that makes a benchmark measure the CPU core
			// alone.  The screen capture path still forces a frame above.
			framesToSkip = 0x7FFFFFFF;
		}
		else if (fastForward)
			framesToSkip = 9;  // try 6 FPS during speedup
		else if (throttle != 100)
			framesToSkip = (framesToSkip * throttle) / 100;

		// A script that took the speed over outranks the settings above, the
		// same way the fast-forward key does; the fastest of the two wins, so
		// holding the key down still does what it says.  Recording and a frame
		// that has to be drawn are excluded by the branch above on purpose.
		int luaSkip = VBALuaSpeed();
		if (luaSkip > framesToSkip)
			framesToSkip = luaSkip;
	}

	return framesToSkip;
}

// sound

bool systemSoundInit()
{
	if (theApp.sound)
	{
		delete theApp.sound;
		theApp.sound = NULL;
	}

	extern ISound *newDirectSound();
	extern ISound *newXAudio2();

	// XAudio2 is optional.  XAudio2_9.dll only ships with Windows 10 and later
	// and the audio device may be unusable, so any failure degrades to
	// DirectSound instead of leaving the emulator without sound.
	if (theApp.soundApi == SOUND_API_XAUDIO2)
	{
		ISound *xaudio2 = newXAudio2();
		if (xaudio2 != NULL && xaudio2->init())
		{
			theApp.sound = xaudio2;
			return true;
		}
		delete xaudio2;

		// Keep the menu in step with what is actually in use.
		theApp.soundApi = SOUND_API_DIRECTSOUND;
	}

	theApp.sound = newDirectSound();
	return theApp.sound != NULL && theApp.sound->init();
}

void systemSoundShutdown()
{
	if (theApp.sound)
		delete theApp.sound;
	theApp.sound = NULL;
}

void systemSoundPause()
{
	if (theApp.sound)
		theApp.sound->pause();
	soundPaused = 1;
}

void systemSoundResume()
{
	if (theApp.sound)
		theApp.sound->resume();
	soundPaused = 0;
}

bool systemSoundIsPaused()
{
//	return soundPaused;
	return !(theApp.sound && theApp.sound->isPlaying());
}

void systemSoundClearBuffer()
{
	if (theApp.sound)
		theApp.sound->clearAudioBuffer();
}

void systemSoundResetBuffer()
{
	if (theApp.sound)
		theApp.sound->reset();
}

void systemSoundWriteToBuffer()
{
	if (theApp.sound)
		theApp.sound->write();
}

bool systemSoundAppliesDSP()
{
	return !(theApp.soundRecording || theApp.aviRecording || theApp.nvAudioLog);
}

// emulation

bool systemIsEmulating()
{
	return emulating != 0;
}

bool systemIsGbBorderOn()
{
	return gbBorderOn != 0;
}

void systemGbBorderOn()
{
	if (vbaShuttingDown)
		return;

	if (emulating && systemCartridgeType == 1)
	{
		theApp.updateWindowSize(theApp.videoOption);
	}
}

bool systemIsRunningGBA()
{
	return (systemCartridgeType == IMAGE_GBA);
}

void systemUpdateColorMaps()
{
	lcdFilterUpdateSystemColorMaps(theApp.lcdFilterGba, theApp.lcdFilterGbc,
	                               theApp.lcdFilterColorMode,
	                               systemIsRunningGBA());
}

bool systemIsSpedUp()
{
	return theApp.speedupToggle;
}

bool systemIsPaused()
{
	return theApp.paused;
}

void systemSetPause(bool pause)
{
	if (pause)
	{
		capturePrevious	 = false;
		theApp.wasPaused = true;
		theApp.paused	 = true;
		theApp.speedupToggle = false;
		theApp.winPauseNextFrame = false;
		systemSoundPause();
		systemRefreshScreen();;
	}
	else
	{
		theApp.paused = false;
		systemSoundResume();
	}
}

__forceinline bool systemPausesNextFrame()
{
	return theApp.winPauseNextFrame && (!theApp.nextframeAccountForLag || !systemCounters.laggedLast);
}
