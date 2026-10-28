#include "stdafx.h"
#include "DDrawConfig.h"

#if VBA_ENABLE_DDRAW

#define DIRECTDRAW_VERSION 0x0700
#include "ddraw.h"

#include "resource.h"
#include "MainWnd.h"
#include "Reg.h"
#include "VBA.h"
#include "TextOverlay.h"

#include "../common/System.h"
#include "../common/SystemGlobals.h"
#include "../gba/GBAGlobals.h"
#include "../gb/gbGlobals.h"
#include "../common/Text.h"
#include "../version.h"

extern u32 RGB_LOW_BITS_MASK;
extern int systemSpeed;
extern int Init_2xSaI(u32);
extern void directXMessage(const char *);

class DirectDrawDisplay : public IDisplay
{
private:
	HINSTANCE ddrawDLL;
	LPDIRECTDRAW7        pDirectDraw;
	LPDIRECTDRAWSURFACE7 ddsPrimary;
	LPDIRECTDRAWSURFACE7 ddsOffscreen;
	LPDIRECTDRAWCLIPPER  ddsClipper;
	int  width;
	int  height;
	bool failed;

	bool initializeOffscreen(int w, int h);
public:
	DirectDrawDisplay();
	virtual ~DirectDrawDisplay();

	virtual bool initialize();
	virtual void cleanup();
	virtual void render();
	virtual void clear();
	virtual bool changeRenderSize(int w, int h);
	virtual DISPLAY_TYPE getType() { return DIRECT_DRAW; };
	virtual void setOption(const char *, int) {}
};

static int ffs(UINT mask)
{
	int m = 0;
	if (mask)
	{
		while (!(mask & (1 << m)))
			m++;

		return (m);
	}

	return (0);
}

DirectDrawDisplay::DirectDrawDisplay()
{
	pDirectDraw  = NULL;
	ddsPrimary   = NULL;
	ddsOffscreen = NULL;
	ddsClipper   = NULL;
	ddrawDLL     = NULL;
	width        = 0;
	height       = 0;
	failed       = false;
}

DirectDrawDisplay::~DirectDrawDisplay()
{
	cleanup();
}

void DirectDrawDisplay::cleanup()
{
	if (pDirectDraw != NULL)
	{
		if (ddsClipper != NULL)
		{
			ddsClipper->Release();
			ddsClipper = NULL;
		}

		if (ddsOffscreen != NULL)
		{
			ddsOffscreen->Release();
			ddsOffscreen = NULL;
		}

		if (ddsPrimary != NULL)
		{
			ddsPrimary->Release();
			ddsPrimary = NULL;
		}

		pDirectDraw->Release();
		pDirectDraw = NULL;
	}

	if (ddrawDLL != NULL)
	{
		/**/ ::FreeLibrary(ddrawDLL);
		ddrawDLL = NULL;
	}
	width  = 0;
	height = 0;
}

bool DirectDrawDisplay::initialize()
{
	// DirectDraw blits to the primary surface and lets a clipper decide where
	// the pixels are allowed to land, so the clipper follows the picture child
	// and the blit lands where the child is - which is also the rectangle the
	// window layout computed for the frame, see theApp.dest.
	CWnd *pWnd = theApp.winRenderWnd();

	// The cooperative level, unlike the clipper, has to be the application's
	// top level window: a child handle is refused there.
	CWnd *pTopWnd = theApp.m_pMainWnd;

	if (pWnd == NULL || pTopWnd == NULL || pTopWnd->GetSafeHwnd() == NULL)
	{
		winlog(_T("DirectDraw: no window to draw into\n"));
		return FALSE;
	}

	GUID *guid = NULL;
	if (theApp.ddrawEmulationOnly)
		guid = (GUID *)DDCREATE_EMULATIONONLY;

	if (theApp.pVideoDriverGUID)
		guid = theApp.pVideoDriverGUID;

	ddrawDLL = /**/ ::LoadLibrary(_T("DDRAW.DLL"));
	decltype(&DirectDrawCreateEx) DDrawCreateEx;
	if (ddrawDLL != NULL)
	{
		DDrawCreateEx = reinterpret_cast<decltype(&DirectDrawCreateEx)>(
			GetProcAddress(ddrawDLL, "DirectDrawCreateEx"));

		if (DDrawCreateEx == NULL)
		{
			directXMessage("DirectDrawCreateEx");
			return FALSE;
		}
	}
	else
	{
		directXMessage("DDRAW.DLL");
		return FALSE;
	}

	HRESULT hret = DDrawCreateEx(guid,
	                             (void * *)&pDirectDraw,
	                             IID_IDirectDraw7,
	                             NULL);

	if (hret != DD_OK)
	{
		winlog(_T("Error creating DirectDraw object %08x\n"), hret);
		if (theApp.ddrawEmulationOnly)
		{
			// disable emulation only setting in case of failure
			regSetDwordValue(_T("ddrawEmulationOnly"), 0);
		}
		//    errorMessage(myLoadString(IDS_ERROR_DISP_DRAWCREATE), hret);
		return FALSE;
	}

	if (theApp.ddrawDebug)
	{
		DDCAPS driver;
		DDCAPS hel;
		ZeroMemory(&driver, sizeof(driver));
		ZeroMemory(&hel, sizeof(hel));
		driver.dwSize = sizeof(driver);
		hel.dwSize    = sizeof(hel);
		pDirectDraw->GetCaps(&driver, &hel);
		int    i;
		DWORD *p = (DWORD *)&driver;
		for (i = 0; i < (int)driver.dwSize; i += 4)
			winlog(_T("Driver CAPS %2d: %08x\n"), i>>2, *p++);
		p = (DWORD *)&hel;
		for (i = 0; i < (int)hel.dwSize; i += 4)
			winlog(_T("HEL CAPS %2d: %08x\n"), i>>2, *p++);
	}

	// Both modes are windowed: full screen is the borderless window the other
	// renderers use, so the cooperative level never takes the display over and
	// the desktop keeps its mode.
	hret = pDirectDraw->SetCooperativeLevel(pTopWnd->GetSafeHwnd(), DDSCL_NORMAL);

	if (hret != DD_OK)
	{
		winlog(_T("Error SetCooperativeLevel %08x\n"), hret);
		//    errorMessage(myLoadString(IDS_ERROR_DISP_DRAWLEVEL), hret);
		return FALSE;
	}

	DDSURFACEDESC2 ddsd;
	ZeroMemory(&ddsd, sizeof(ddsd));
	ddsd.dwSize         = sizeof(ddsd);
	ddsd.dwFlags        = DDSD_CAPS;
	ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

	hret = pDirectDraw->CreateSurface(&ddsd, &ddsPrimary, NULL);
	if (hret != DD_OK)
	{
		winlog(_T("Error primary CreateSurface %08x\n"), hret);
		//    errorMessage(myLoadString(IDS_ERROR_DISP_DRAWSURFACE), hret);
		return FALSE;
	}

	if (theApp.ddrawDebug)
	{
		DDSCAPS2 caps;
		ZeroMemory(&caps, sizeof(caps));
		ddsPrimary->GetCaps(&caps);

		winlog(_T("Primary CAPS 1: %08x\n"), caps.dwCaps);
		winlog(_T("Primary CAPS 2: %08x\n"), caps.dwCaps2);
		winlog(_T("Primary CAPS 3: %08x\n"), caps.dwCaps3);
		winlog(_T("Primary CAPS 4: %08x\n"), caps.dwCaps4);
	}

	// The clipper is what confines the blit to the picture, so it is attached
	// to the primary surface in both modes.
	hret = pDirectDraw->CreateClipper(0, &ddsClipper, NULL);
	if (hret == DD_OK)
	{
		ddsClipper->SetHWnd(0, pWnd->GetSafeHwnd());
		ddsPrimary->SetClipper(ddsClipper);
	}
	else
		winlog(_T("Error CreateClipper %08x\n"), hret);

	DDPIXELFORMAT px;

	px.dwSize = sizeof(px);

	hret = ddsPrimary->GetPixelFormat(&px);

	switch (px.dwRGBBitCount)
	{
	case 15:
	case 16:
		systemColorDepth = 16;
		break;
	case 24:
		systemColorDepth      = 24;
		theApp.filterFunction = NULL;
		break;
	case 32:
		systemColorDepth = 32;
		break;
	default:
		systemMessage(
		    IDS_ERROR_DISP_COLOR,
		    "Unsupported display setting for color depth: %d bits. \nWindows desktop must be in either 16-bit, 24-bit or 32-bit mode for this program to work in window mode.",
		    px.dwRGBBitCount);
		return FALSE;
	}
	theApp.updateFilter();
	theApp.updateIFB();

	if (failed)
		return false;

	pTopWnd->DragAcceptFiles(TRUE);

	return true;
}

bool DirectDrawDisplay::changeRenderSize(int w, int h)
{
	if (w != width || h != height)
	{
		if (ddsOffscreen)
		{
			ddsOffscreen->Release();
			ddsOffscreen = NULL;
		}
		if (!initializeOffscreen(w, h))
		{
			failed = true;
			return false;
		}
	}
	return true;
}

bool DirectDrawDisplay::initializeOffscreen(int w, int h)
{
	DDSURFACEDESC2 ddsd;

	ZeroMemory(&ddsd, sizeof(ddsd));
	ddsd.dwSize         = sizeof(ddsd);
	ddsd.dwFlags        = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH;
	ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
	if (theApp.ddrawUseVideoMemory)
		ddsd.ddsCaps.dwCaps |= (DDSCAPS_LOCALVIDMEM|DDSCAPS_VIDEOMEMORY);
	ddsd.dwWidth  = w;
	ddsd.dwHeight = h;

	HRESULT hret = pDirectDraw->CreateSurface(&ddsd, &ddsOffscreen, NULL);

	if (hret != DD_OK)
	{
		winlog(_T("Error offscreen CreateSurface %08x\n"), hret);
		if (theApp.ddrawUseVideoMemory)
		{
			regSetDwordValue(_T("ddrawUseVideoMemory"), 0);
		}
		//    errorMessage(myLoadString(IDS_ERROR_DISP_DRAWSURFACE2), hret);
		return false;
	}

	if (theApp.ddrawDebug)
	{
		DDSCAPS2 caps;
		ZeroMemory(&caps, sizeof(caps));
		ddsOffscreen->GetCaps(&caps);

		winlog(_T("Offscreen CAPS 1: %08x\n"), caps.dwCaps);
		winlog(_T("Offscreen CAPS 2: %08x\n"), caps.dwCaps2);
		winlog(_T("Offscreen CAPS 3: %08x\n"), caps.dwCaps3);
		winlog(_T("Offscreen CAPS 4: %08x\n"), caps.dwCaps4);
	}

	DDPIXELFORMAT px;

	px.dwSize = sizeof(px);

	hret = ddsOffscreen->GetPixelFormat(&px);

	if (theApp.ddrawDebug)
	{
		DWORD *pdword = (DWORD *)&px;
		for (int ii = 0; ii < 8; ii++)
		{
			winlog(_T("Pixel format %d %08x\n"), ii, pdword[ii]);
		}
	}

	switch (px.dwRGBBitCount)
	{
	case 15:
	case 16:
		systemColorDepth = 16;
		break;
	case 24:
		systemColorDepth      = 24;
		theApp.filterFunction = NULL;
		break;
	case 32:
		systemColorDepth = 32;
		break;
	default:
		systemMessage(
		    IDS_ERROR_DISP_COLOR,
		    "Unsupported display setting for color depth: %d bits. \nWindows desktop must be in either 16-bit, 24-bit or 32-bit mode for this program to work in window mode.",
		    px.dwRGBBitCount);
		return FALSE;
	}
	if (theApp.ddrawDebug)
	{
		winlog(_T("R Mask: %08x\n"), px.dwRBitMask);
		winlog(_T("G Mask: %08x\n"), px.dwGBitMask);
		winlog(_T("B Mask: %08x\n"), px.dwBBitMask);
	}

	systemRedShift   = ffs(px.dwRBitMask);
	systemGreenShift = ffs(px.dwGBitMask);
	systemBlueShift  = ffs(px.dwBBitMask);

	if ((px.dwFlags&DDPF_RGB) != 0 &&
	    px.dwRBitMask == 0xF800 &&
	    px.dwGBitMask == 0x07E0 &&
	    px.dwBBitMask == 0x001F)
	{
		systemGreenShift++;
		Init_2xSaI(565);
		RGB_LOW_BITS_MASK = 0x821;
	}
	else if ((px.dwFlags&DDPF_RGB) != 0 &&
	         px.dwRBitMask == 0x7C00 &&
	         px.dwGBitMask == 0x03E0 &&
	         px.dwBBitMask == 0x001F)
	{
		Init_2xSaI(555);
		RGB_LOW_BITS_MASK = 0x421;
	}
	else if ((px.dwFlags&DDPF_RGB) != 0 &&
	         px.dwRBitMask == 0x001F &&
	         px.dwGBitMask == 0x07E0 &&
	         px.dwBBitMask == 0xF800)
	{
		systemGreenShift++;
		Init_2xSaI(565);
		RGB_LOW_BITS_MASK = 0x821;
	}
	else if ((px.dwFlags&DDPF_RGB) != 0 &&
	         px.dwRBitMask == 0x001F &&
	         px.dwGBitMask == 0x03E0 &&
	         px.dwBBitMask == 0x7C00)
	{
		Init_2xSaI(555);
		RGB_LOW_BITS_MASK = 0x421;
	}
	else
	{
		// 32-bit or 24-bit
		if (systemColorDepth == 32 || systemColorDepth == 24)
		{
			systemRedShift   += 3;
			systemGreenShift += 3;
			systemBlueShift  += 3;
			if (systemColorDepth == 32)
				Init_2xSaI(32);
		}
	}

	if (theApp.ddrawDebug)
	{
		winlog(_T("R shift: %d\n"), systemRedShift);
		winlog(_T("G shift: %d\n"), systemGreenShift);
		winlog(_T("B shift: %d\n"), systemBlueShift);
	}

	systemUpdateColorMaps();

	width  = w;
	height = h;
	return true;
}

// A windowed blit has no back buffer to clear, so the picture area on the
// primary surface is filled black instead.  The clipper keeps that inside the
// picture child, which is the same rectangle the frames are blitted to.
void DirectDrawDisplay::clear()
{
	if (ddsPrimary == NULL)
		return;

	DDBLTFX fx;
	ZeroMemory(&fx, sizeof(fx));
	fx.dwSize      = sizeof(fx);
	fx.dwFillColor = 0;

	ddsPrimary->Blt(&theApp.dest, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
}

void DirectDrawDisplay::render()
{
	HRESULT hret;

	if (pDirectDraw == NULL ||
	    ddsOffscreen == NULL ||
	    ddsPrimary == NULL)
		return;

	bool fastForward = speedup;
#if (defined(WIN32) && !defined(SDL))
	if (theApp.frameSearchSkipping)
	{
		if (theApp.frameSearchFirstStep)
			fastForward = true;
		else
			return; // don't render skipped frame search frames
	}
#endif

	DDSURFACEDESC2 ddsDesc;

	ZeroMemory(&ddsDesc, sizeof(ddsDesc));

	ddsDesc.dwSize = sizeof(ddsDesc);

	hret = ddsOffscreen->Lock(NULL,
	                          &ddsDesc,
#ifndef FINAL_VERSION
	                          DDLOCK_NOSYSLOCK |
#endif
	                          DDLOCK_WRITEONLY | DDLOCK_SURFACEMEMORYPTR | DDLOCK_WAIT,
	                          NULL);

	if (hret == DDERR_SURFACELOST)
	{
		hret = ddsPrimary->Restore();
		if (hret == DD_OK)
		{
			hret = ddsOffscreen->Restore();

			if (hret == DD_OK)
			{
				hret = ddsOffscreen->Lock(NULL,
				                          &ddsDesc,
#ifndef FINAL_VERSION
				                          DDLOCK_NOSYSLOCK |
#endif
				                          DDLOCK_WRITEONLY | DDLOCK_SURFACEMEMORYPTR | DDLOCK_WAIT,
				                          NULL);
			}
		}
	}

	u8 *data = pix;
	int dataPitch = theApp.filterWidth * (systemColorDepth / 8) + (systemColorDepth == 24 ? 0 : 4);

	if (textMethod != 0) // do not draw Lua HUD to a video dump
	{
		systemClonePixBuffer(osd);
		data = osd;
		systemRenderLua((u8 *)data, dataPitch);
	}

	if (hret == DD_OK)
	{
		if (theApp.filterFunction)
		{
			(*theApp.filterFunction)(data + dataPitch,
			                         dataPitch,
			                         (u8 *)theApp.delta,
			                         (u8 *)ddsDesc.lpSurface,
			                         ddsDesc.lPitch,
			                         theApp.filterWidth,
			                         theApp.filterHeight);
		}
		else
		{
			int copyX, copyY;
			systemGetLCDResolution(copyX, copyY);
			
			// Copy the frame straight into the locked surface.  The source buffer
			// carries a one row border and, except at 24 bpp, two extra pixels
			// per row, so skip the first row and walk the real stride.
			const int bytesPerPixel = systemColorDepth / 8;
			const int rowBytes = copyX * bytesPerPixel;
			const u8 *src = data + dataPitch;
			u8 *dst = (u8 *)ddsDesc.lpSurface;
			const int dstPitch = ddsDesc.lPitch;

			for (int y = 0; y < copyY; ++y)
			{
				memcpy(dst, src, rowBytes);
				src += dataPitch;
				dst += dstPitch;
			}
		}
		if (theApp.showSpeed && isFullscreenOption(theApp.videoOption))
		{
			char buffer[30];
			if (theApp.showSpeed == 1)
				sprintf(buffer, "%3d%%", systemSpeed);
			else
				sprintf(buffer, "%3d%%(%d, %d fps)", systemSpeed,
				        systemFrameSkip,
				        theApp.showRenderedFrames);
			if (theApp.showSpeedTransparent)
				drawTextTransp((u8 *)ddsDesc.lpSurface,
				               ddsDesc.lPitch,
				               theApp.rect.left+10,
				               theApp.rect.bottom-10,
				               buffer);
			else
				drawText((u8 *)ddsDesc.lpSurface,
				         ddsDesc.lPitch,
				         theApp.rect.left+10,
				         theApp.rect.bottom-10,
				         buffer);
		}

		if (textMethod == 1)
		{
			DrawTextMessages((u8 *)ddsDesc.lpSurface, ddsDesc.lPitch, theApp.rect.left, theApp.rect.bottom);
		}
	}
	else if (theApp.ddrawDebug)
		winlog(_T("Error during lock: %08x\n"), hret);

	hret = ddsOffscreen->Unlock(NULL);

	if (hret == DD_OK)
	{
		// the correct point where to wait
		if (theApp.vsync && !fastForward)
		{
			hret = pDirectDraw->WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, 0);
		}

		ddsOffscreen->PageLock(0);

		// The whole frame is stretched into the picture rectangle, so the
		// scaling mode only decides what that rectangle is.
		hret = ddsPrimary->Blt(&theApp.dest, ddsOffscreen, NULL, DDBLT_ASYNC, NULL);

		if (hret == DDERR_SURFACELOST)
		{
			hret = ddsPrimary->Restore();

			if (hret == DD_OK)
			{
				hret = ddsPrimary->Blt(&theApp.dest, ddsOffscreen, NULL, DDBLT_ASYNC, NULL);
			}
		}
		ddsOffscreen->PageUnlock(0);
	}
	else if (theApp.ddrawDebug)
		winlog(_T("Error during unlock: %08x\n"), hret);

	bool textMessageStarted = false;

	if (textMethod == 2 && !theApp.statusBarActive())
	{
		HDC hdc;
		HFONT font    = NULL;
		HFONT oldFont = NULL;
		const int lineHeight = screenTextLineHeight();
		const int textLeft   = theApp.dest.left + 10 + theApp.textMarginLeft;
		const int textBottom = theApp.dest.bottom - theApp.textMarginBottom;

		for (int slot = 0; slot < SCREEN_MESSAGE_SLOTS; slot++)
		{
			if (theApp.screenMessage[slot])
			{
				if ((theApp.screenMessageDuration[slot] < 0 || 
					(int)(GetTickCount() - theApp.screenMessageTime[slot]) < theApp.screenMessageDuration[slot]) &&
				    (!theApp.disableStatusMessage || slot == 1 || slot == 2))
				{
					if (!textMessageStarted)
					{
						textMessageStarted = true;
						ddsPrimary->SetClipper(ddsClipper);
						ddsPrimary->GetDC(&hdc);
						SetBkMode(hdc, TRANSPARENT);
						font    = createScreenTextFont();
						oldFont = (HFONT)SelectObject(hdc, font);
						SetTextColor(hdc, textColor != 7 ? RGB(0, 0, 0) : RGB(255, 255, 255));
					}

					if (outlinedText)
					{
						// draw black outline
						const static int xd [8] = {-1, 0, 1, 1, 1, 0, -1, -1};
						const static int yd [8] = {-1, -1, -1, 0, 1, 1, 1, 0};
						for (int i = 0; i < 8; i++)
						{
							TextOut(hdc,
							        textLeft+xd[i],
							        textBottom - lineHeight*(slot+1)+yd[i],
							        theApp.screenMessageBuffer[slot],
							        theApp.screenMessageBuffer[slot].GetLength());
						}
					}
				}
				else
				{
					theApp.screenMessage[slot] = false;
				}
			}
		}

		if (textMessageStarted)
		{
			COLORREF color;
			switch (textColor)
			{
			case 0:
				color = RGB(255, 255, 255); break;
			case 1:
				color = RGB(255, 0, 0); break;
			case 2:
				color = RGB(255, 255, 0); break;
			case 3:
				color = RGB(0, 255, 0); break;
			case 4:
				color = RGB(0, 255, 255); break;
			case 5:
				color = RGB(0, 0, 255); break;
			case 6:
				color = RGB(255, 0, 255); break;
			case 7:
				color = RGB(0, 0, 0); break;
			}
			SetTextColor(hdc, color);

			// draw center text
			for (int slot = 0; slot < SCREEN_MESSAGE_SLOTS; slot++)
			{
				if (theApp.screenMessage[slot])
				{
					if ((theApp.screenMessageDuration[slot] < 0 || 
						(int)(GetTickCount() - theApp.screenMessageTime[slot]) < theApp.screenMessageDuration[slot]) &&
					    (!theApp.disableStatusMessage || slot == 1 || slot == 2))
					{
						TextOut(hdc, textLeft, textBottom - lineHeight*(slot+1), theApp.screenMessageBuffer[slot],
						        theApp.screenMessageBuffer[slot].GetLength());
					}
				}
			}
		}

		if (textMessageStarted)
		{
			SelectObject(hdc, oldFont);
			DeleteObject(font);
			ddsPrimary->ReleaseDC(hdc);
		}
	}

	if (hret != DD_OK)
	{
		if (theApp.ddrawDebug)
			winlog(_T("Error on update screen: %08x\n"), hret);
	}
}

IDisplay *newDirectDrawDisplay()
{
	return new DirectDrawDisplay();
}

#else // ndef VBA_ENABLE_DDRAW

#include "Display.h"

// The renderer is not built, so there is nothing to create.  The menu does
// not offer it either, see the switch in DDrawConfig.h.
IDisplay *newDirectDrawDisplay()
{
	return NULL;
}

#endif // !VBA_ENABLE_DDRAW


