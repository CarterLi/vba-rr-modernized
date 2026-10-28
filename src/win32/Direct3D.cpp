#include "stdafx.h"

#pragma comment( lib, "d3d9" )

#include <d3d9.h>

#include "resource.h"
#include "MainWnd.h"
#include "Reg.h"
#include "VBA.h"
#include "TextOverlay.h"

//#include "../common/System.h"
#include "../common/SystemGlobals.h"
#include "../common/Text.h"
#include "../version.h"

extern int Init_2xSaI(u32);
extern void directXMessage(const char *);

typedef struct _D3DTLVERTEX
{
	float    sx; /* Screen coordinates */
	float    sy;
	float    sz;
	float    rhw; /* Reciprocal of homogeneous w */
	D3DCOLOR color; /* Vertex color */
	float    tu; /* Texture coordinates */
	float    tv;
	_D3DTLVERTEX() { }
	_D3DTLVERTEX(float _x, float _y, float _z, float _rhw,
	             D3DCOLOR _color,
	             float _tu, float _tv)
	{
		sx    = _x; sy = _y; sz = _z; rhw = _rhw;
		color = _color;
		tu    = _tu; tv = _tv;
	}
} D3DTLVERTEX, *LPD3DTLVERTEX;

#define D3DFVF_TLVERTEX D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1

// The swap effect the running display presents with, -1 while there is none.
static int currentSwapEffect = -1;

// Whether the running device is a Direct3D 9Ex one.  Direct3DCreate9Ex only
// arrived with Vista; on anything older the renderer gets a plain 9.0c device
// from Direct3DCreate9 and the render method menu says so rather than claiming
// 9Ex was used.
static bool currentDeviceIs9Ex = false;

// Names the Direct3D 9 flavor that is really in use, or returns NULL while the
// renderer is not running.  The render method menu asks for this, see
// direct3D9PresentationModel() below and MainWndOptions.cpp.
const char *direct3D9DeviceName()
{
	if (currentSwapEffect < 0)
		return NULL;

	return currentDeviceIs9Ex ? "9Ex" : "9.0c";
}

// Names the presentation model that is in use, or returns NULL while the
// renderer is not running.  The render method menu asks for this so that the
// model is visible without digging through the trace log, see
// MainWndOptions.cpp.
const char *direct3D9PresentationModel()
{
	switch (currentSwapEffect)
	{
	case D3DSWAPEFFECT_FLIPEX:  return "Flip";
	case D3DSWAPEFFECT_DISCARD: return "Bitblt";
	}

	return NULL;
}

class Direct3DDisplay : public IDisplay
{
public:
	Direct3DDisplay();
	virtual ~Direct3DDisplay();

	virtual bool initialize();
	virtual void cleanup();
	virtual void render();
	virtual void clear();
	virtual bool changeRenderSize(int w, int h);
	virtual void resize(int w, int h);
	virtual DISPLAY_TYPE getType() { return DIRECT_3D; };
	virtual void setOption(const char *, int);
	// Trilinear is the one that needs something of its own, the mip chain the
	// device makes for pMipTexture, so it stays unselectable where that
	// texture could not be made.
	virtual bool filterAvailable(int filter)
	{
		return filter >= 0 && filter <= 2 && (filter != 2 || pMipTexture != NULL);
	};

private:
	HINSTANCE             d3dDLL;
	LPDIRECT3D9           pD3D;
	LPDIRECT3D9EX         pD3DEx;
	LPDIRECT3DDEVICE9     pDevice;
	LPDIRECT3DDEVICE9EX   pDeviceEx;
	LPDIRECT3DTEXTURE9    pTexture;
	// The trilinear filter reads a mip chain, which the trilinear path keeps
	// in a texture of its own: the frame is written into a staging texture and
	// UpdateTexture copies it over, which is what makes the device generate
	// the chain again.  See initializeOffscreen().
	LPDIRECT3DTEXTURE9    pMipTexture;
	LPDIRECT3DTEXTURE9    pMipStaging;
	LPDIRECT3DTEXTURE9    pOverlayTexture;
	D3DSURFACE_DESC       dsdBackBuffer;
	D3DPRESENT_PARAMETERS dpp;
	D3DFORMAT             screenFormat;
	int  width;
	int  height;
	int  filterMode;
	bool filterDisabled;
	bool       failed;
	unsigned int textureSize;
	D3DTLVERTEX verts[4];
	TextOverlay overlay;

	void restoreDeviceObjects();
	void invalidateDeviceObjects();
	void releaseTextures();
	bool initializeOffscreen(int w, int h);
	void updateFiltering(int);
	void fillFrame(u8 *bits, int pitch, u8 *data, int dataPitch);
	void extendFrame(u8 *bits, int pitch, int frameWidth, int frameHeight);
	void calculateVertices();
	void present();
	void drawTextOverlay();
};

Direct3DDisplay::Direct3DDisplay()
{
	d3dDLL         = NULL;
	pD3D           = NULL;
	pD3DEx         = NULL;
	pDevice        = NULL;
	pDeviceEx      = NULL;
	pTexture       = NULL;
	pMipTexture    = NULL;
	pMipStaging    = NULL;
	pOverlayTexture = NULL;
	screenFormat   = D3DFMT_R5G6B5;
	width          = 0;
	height         = 0;
	filterMode     = 0;
	filterDisabled = false;
	failed         = false;
	textureSize    = 0;
}

Direct3DDisplay::~Direct3DDisplay()
{
	cleanup();
}

void Direct3DDisplay::cleanup()
{
	overlay.destroy();

	currentSwapEffect  = -1;
	currentDeviceIs9Ex = false;

	if (pOverlayTexture)
	{
		pOverlayTexture->Release();
		pOverlayTexture = NULL;
	}

	if (pTexture)
	{
		pTexture->Release();
		pTexture = NULL;
	}

	if (pMipStaging)
	{
		pMipStaging->Release();
		pMipStaging = NULL;
	}

	if (pMipTexture)
	{
		pMipTexture->Release();
		pMipTexture = NULL;
	}

	// pDevice / pDeviceEx and pD3D / pD3DEx may point at the same object, so
	// only one Release() per object is issued here.
	if (pDeviceEx)
	{
		pDeviceEx->Release();
		pDeviceEx = NULL;
		pDevice   = NULL;
	}
	else if (pDevice)
	{
		pDevice->Release();
		pDevice = NULL;
	}

	if (pD3DEx)
	{
		pD3DEx->Release();
		pD3DEx = NULL;
		pD3D   = NULL;
	}
	else if (pD3D)
	{
		pD3D->Release();
		pD3D = NULL;
	}

	if (d3dDLL != NULL)
	{
		FreeLibrary(d3dDLL);
		d3dDLL = NULL;
	}
}

bool Direct3DDisplay::initialize()
{
	CWnd *pWnd = theApp.winRenderWnd();

	d3dDLL = LoadLibrary(_T("D3D9.DLL"));
	if (d3dDLL == NULL)
	{
		directXMessage("D3D9.DLL");
		return FALSE;
	}

	// Prefer Direct3D 9Ex (Vista and later); fall back to plain 9.0c.
	decltype(&Direct3DCreate9Ex) create9Ex = reinterpret_cast<decltype(&Direct3DCreate9Ex)>(
	    GetProcAddress(d3dDLL, "Direct3DCreate9Ex"));
	if (create9Ex != NULL)
		create9Ex(D3D_SDK_VERSION, &pD3DEx);

	if (pD3DEx != NULL)
	{
		pD3D = pD3DEx;
		winlog(_T("Direct3D 9Ex device selected\n"));
	}
	else
	{
		decltype(&Direct3DCreate9) create9 = reinterpret_cast<decltype(&Direct3DCreate9)>(
		    GetProcAddress(d3dDLL, "Direct3DCreate9"));
		if (create9 == NULL)
		{
			winlog(_T("Error creating Direct3D object\n"));
			return FALSE;
		}
		pD3D = create9(D3D_SDK_VERSION);
		if (pD3D == NULL)
		{
			winlog(_T("Error creating Direct3D object\n"));
			return FALSE;
		}
	}


	D3DDISPLAYMODE mode;
	pD3D->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode);

	switch (mode.Format)
	{
	case D3DFMT_R8G8B8:
		systemColorDepth = 24;
		systemRedShift   = 19;
		systemGreenShift = 11;
		systemBlueShift  = 3;
		break;
	case D3DFMT_X8R8G8B8:
		systemColorDepth = 32;
		systemRedShift   = 19;
		systemGreenShift = 11;
		systemBlueShift  = 3;
		Init_2xSaI(32);
		break;
	case D3DFMT_R5G6B5:
		systemColorDepth = 16;
		systemRedShift   = 11;
		systemGreenShift = 6;
		systemBlueShift  = 0;
		Init_2xSaI(565);
		break;
	case D3DFMT_X1R5G5B5:
		systemColorDepth = 16;
		systemRedShift   = 10;
		systemGreenShift = 5;
		systemBlueShift  = 0;
		Init_2xSaI(555);
		break;
	default:
		systemMessage(0, "Unsupport D3D format %d", mode.Format);
		return false;
	}
	theApp.fsColorDepth = systemColorDepth;

	screenFormat = mode.Format;

	// check for available fullscreen modes
	ZeroMemory(&dpp, sizeof(dpp));
	dpp.Windowed         = TRUE;
	dpp.BackBufferFormat = mode.Format;
	dpp.BackBufferCount  = 1;
	dpp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
	dpp.BackBufferWidth  = theApp.surfaceSizeX;
	dpp.BackBufferHeight = theApp.surfaceSizeY;

	HRESULT hret;
	HRESULT flipRefused = S_OK;
	if (pD3DEx != NULL)
	{
		// Direct3D 9Ex has a flip model of its own (D3DSWAPEFFECT_FLIPEX,
		// Windows 7 and later), where the frame is handed to the desktop window
		// manager instead of being copied into it.  It wants at least two back
		// buffers, so that one can be drawn into while the window manager holds
		// the other, and systems without it refuse the swap effect outright, so
		// the bitblt model the renderer used before is what the second level
		// falls back to - the same idea as the three levels of the Direct3D 11
		// renderer.
		D3DSWAPEFFECT effects[2] = { D3DSWAPEFFECT_FLIPEX, D3DSWAPEFFECT_DISCARD };
		UINT         buffers[2]  = { 2, 1 };

		for (int level = 0; level < 2; level++)
		{
			dpp.SwapEffect      = effects[level];
			dpp.BackBufferCount = buffers[level];

			hret = pD3DEx->CreateDeviceEx(D3DADAPTER_DEFAULT,
			                              D3DDEVTYPE_HAL,
			                              pWnd->GetSafeHwnd(),
			                              D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
			                              &dpp,
			                              NULL,
			                              &pDeviceEx);
			if (SUCCEEDED(hret))
				break;

			if (level == 0)
				flipRefused = hret;
		}

		if (SUCCEEDED(hret))
			pDevice = pDeviceEx;
	}
	else
	{
		hret = pD3D->CreateDevice(D3DADAPTER_DEFAULT,
		                          D3DDEVTYPE_HAL,
		                          pWnd->GetSafeHwnd(),
		                          D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
		                          &dpp,
		                          &pDevice);
	}
	if (!SUCCEEDED(hret) || pDevice == NULL)
	{
		winlog(_T("Error creating Direct3DDevice %08x\n"), hret);
		return false;
	}

	// Which model it ended up with, for the trace log and for the render method
	// menu (see MainWndOptions.cpp).  Only the last line written to the trace log
	// survives, so the reason the flip model was turned down is carried into this
	// line when there was one.
	currentSwapEffect = (int)dpp.SwapEffect;
	currentDeviceIs9Ex = (pDeviceEx != NULL);
	if (flipRefused != S_OK)
		winlog(_T("Direct3D 9Ex: no flip model swap chain (%08x), using swap effect %d (bitblt), %d back buffer(s)\n"),
		       (unsigned int)flipRefused,
		       (int)dpp.SwapEffect,
		       (int)dpp.BackBufferCount);
	else
		winlog(_T("Direct3D 9%hs: swap effect %d (%hs), %d back buffer(s)\n"),
		       pDeviceEx != NULL ? "Ex" : ".0c",
		       (int)dpp.SwapEffect,
		       dpp.SwapEffect == D3DSWAPEFFECT_FLIPEX ? "flip" : "bitblt",
		       (int)dpp.BackBufferCount);

	restoreDeviceObjects();

	systemUpdateColorMaps();

	theApp.updateFilter();
	theApp.updateIFB();

	if (failed)
		return false;

	pWnd->DragAcceptFiles(TRUE);

	return TRUE;
}

void Direct3DDisplay::releaseTextures()
{
	if (pOverlayTexture)
	{
		pOverlayTexture->Release();
		pOverlayTexture = NULL;
	}
	if (pMipStaging)
	{
		pMipStaging->Release();
		pMipStaging = NULL;
	}
	if (pMipTexture)
	{
		pMipTexture->Release();
		pMipTexture = NULL;
	}
	if (pTexture)
	{
		pTexture->Release();
		pTexture = NULL;
	}
}

bool Direct3DDisplay::initializeOffscreen(int w, int h)
{
	int size = 256;
	if (w > 512 || h > 512)
		size = 1024;
	else if (w > 256 || h > 256)
		size = 512;
	textureSize = size;

	D3DCAPS9 caps;
	ZeroMemory(&caps, sizeof(caps));
	pDevice->GetDeviceCaps(&caps);

	int maxSize = (int)caps.MaxTextureWidth;
	if ((int)caps.MaxTextureHeight < maxSize)
		maxSize = (int)caps.MaxTextureHeight;
	if (maxSize > 0 && size > maxSize)
		size = maxSize;
	textureSize = size;

	if (size < w || size < h)
	{
		if (theApp.filterFunction)
		{
			filterDisabled        = true;
			theApp.filterFunction = NULL;
			systemMessage(0, "3D card cannot support needed texture size for filter function. Disabling it");
		}
		return false;
	}

	// Direct3D 9Ex does not support the managed pool, so a dynamic default
	// pool texture is used and fully rewritten every frame.
	if (SUCCEEDED(pDevice->CreateTexture(size,
	                                     size,
	                                     1,
	                                     D3DUSAGE_DYNAMIC,
	                                     screenFormat,
	                                     D3DPOOL_DEFAULT,
	                                     &pTexture,
	                                     NULL)))
	{
		// The trilinear filter reads a mip chain, and Direct3D 9 makes the
		// chain itself only for a texture in the default pool with the
		// automatic mipmap generation.  Such a texture cannot be locked, so
		// the frame is written into a system memory staging texture of the
		// same size and UpdateTexture copies it over, which is what has the
		// device rebuild the chain from the new frame.  The chain is made of
		// the whole square, hence extendFrame().
		if (SUCCEEDED(pDevice->CreateTexture(size,
		                                     size,
		                                     1,
		                                     D3DUSAGE_AUTOGENMIPMAP,
		                                     screenFormat,
		                                     D3DPOOL_DEFAULT,
		                                     &pMipTexture,
		                                     NULL)) &&
		    SUCCEEDED(pDevice->CreateTexture(size,
		                                     size,
		                                     1,
		                                     0,
		                                     screenFormat,
		                                     D3DPOOL_SYSTEMMEM,
		                                     &pMipStaging,
		                                     NULL)))
		{
			winlog(_T("Direct3D 9: trilinear filtering through %d levels of automatic mipmaps\n"),
			       (int)pMipTexture->GetLevelCount());
		}
		else
		{
			if (pMipStaging)
			{
				pMipStaging->Release();
				pMipStaging = NULL;
			}
			if (pMipTexture)
			{
				pMipTexture->Release();
				pMipTexture = NULL;
			}
			winlog(_T("Direct3D 9: no automatic mipmaps, trilinear filtering is not available\n"));
		}

		width  = w;
		height = h;
		return true;
	}
	return false;
}

void Direct3DDisplay::updateFiltering(int filter)
{
	filterMode = filter;

	switch (filter)
	{
	default:
	case 0:
		// point filtering
		pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
		break;
	case 1:
		// bilinear
		pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
		break;
	case 2:
		// trilinear
		pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
		break;
	}
}

void Direct3DDisplay::restoreDeviceObjects()
{
	// Store render target surface desc
	LPDIRECT3DSURFACE9 pBackBuffer;
	HRESULT hr;
	if (SUCCEEDED(hr = pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)))
	{
		pBackBuffer->GetDesc(&dsdBackBuffer);
		pBackBuffer->Release();
	}
	else
		systemMessage(0, "Failed GetBackBuffer %08x", hr);

	// Set up the texture
	pDevice->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
	pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);

	int filter = regQueryDwordValue(_T("d3dFilter"), 0);
	if (filter < 0 || filter > 2)
		filter = 0;
	updateFiltering(filter);

	// Set miscellaneous render states
	pDevice->SetRenderState(D3DRS_DITHERENABLE,   TRUE);
	pDevice->SetRenderState(D3DRS_ZENABLE,        FALSE);

	// turn off lighting
	pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

	// Text overlay texture, sized 1:1 with the back buffer.
	if (pOverlayTexture == NULL && dpp.BackBufferWidth > 0 && dpp.BackBufferHeight > 0)
	{
		pDevice->CreateTexture(dpp.BackBufferWidth,
		                       dpp.BackBufferHeight,
		                       1,
		                       D3DUSAGE_DYNAMIC,
		                       D3DFMT_A8R8G8B8,
		                       D3DPOOL_DEFAULT,
		                       &pOverlayTexture,
		                       NULL);
		overlay.create(dpp.BackBufferWidth, dpp.BackBufferHeight);
	}
}

void Direct3DDisplay::clear()
{}

void Direct3DDisplay::calculateVertices()
{
	// color
	D3DCOLOR col = 0xFFFFFFFF;

	// calculate rhw
	FLOAT z	  = 1.0f;
	FLOAT rhw = 1.0f / (z * 990.0f + 10.0f);

	// -0.5f is necessary in order to match texture alignment to display pixels
	FLOAT left     = -0.5f;
	FLOAT right    = dpp.BackBufferWidth - 0.5f;
	FLOAT top      = -0.5f;
	FLOAT bottom   = dpp.BackBufferHeight - 0.5f;

	FLOAT textureX = (FLOAT)theApp.rect.right / (FLOAT)textureSize;
	FLOAT textureY = (FLOAT)theApp.rect.bottom / (FLOAT)textureSize;

//#define D3D_DRAW_SINGLE_RECTANGLE
#ifdef D3D_DRAW_SINGLE_RECTANGLE
	// set up a rectangle
	verts[0] = D3DTLVERTEX(left,  top,    z, rhw, col, 0.0f,     0.0f);
	verts[1] = D3DTLVERTEX(right, top,    z, rhw, col, textureX, 0.0f);
	verts[2] = D3DTLVERTEX(right, bottom, z, rhw, col, textureX, textureY);
	verts[3] = D3DTLVERTEX(left,  bottom, z, rhw, col, 0.0f,     textureY);
#else
	// set up triangles
	verts[0] = D3DTLVERTEX(left,  bottom, z, rhw, col, 0.0f,     textureY);
	verts[1] = D3DTLVERTEX(left,  top,    z, rhw, col, 0.0f,     0.0f);
	verts[2] = D3DTLVERTEX(right, bottom, z, rhw, col, textureX, textureY);
	verts[3] = D3DTLVERTEX(right, top,    z, rhw, col, textureX, 0.0f);
#endif
}

void Direct3DDisplay::drawTextOverlay()
{
	if (textMethod != 2)
		return;
	if (pOverlayTexture == NULL || overlay.empty())
		return;

	if (!overlay.update())
		return;

	// The overlay is the size of the back buffer and only changes when a
	// message does, so it is uploaded again only then; the texture still holds
	// the last picture.
	if (overlay.changed())
	{
		D3DLOCKED_RECT locked;
		if (FAILED(pOverlayTexture->LockRect(0, &locked, NULL, D3DLOCK_DISCARD)))
			return;

		const unsigned char *src = overlay.pixels();
		int srcPitch = overlay.pitch();
		int copyW    = overlay.width() * 4;
		if (copyW > (int)locked.Pitch)
			copyW = (int)locked.Pitch;
		for (int y = 0; y < overlay.height(); y++)
			memcpy((unsigned char *)locked.pBits + y * locked.Pitch, src + y * srcPitch, copyW);

		pOverlayTexture->UnlockRect(0);
	}

	pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	pDevice->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
	pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

	pDevice->SetTexture(0, pOverlayTexture);

	D3DTLVERTEX quad[4];
	D3DCOLOR col = 0xFFFFFFFF;
	FLOAT z   = 1.0f;
	FLOAT rhw = 1.0f / (z * 990.0f + 10.0f);
	quad[0] = D3DTLVERTEX(-0.5f,                              dpp.BackBufferHeight - 0.5f, z, rhw, col, 0.0f, 1.0f);
	quad[1] = D3DTLVERTEX(-0.5f,                              -0.5f,                       z, rhw, col, 0.0f, 0.0f);
	quad[2] = D3DTLVERTEX(dpp.BackBufferWidth - 0.5f,         dpp.BackBufferHeight - 0.5f, z, rhw, col, 1.0f, 1.0f);
	quad[3] = D3DTLVERTEX(dpp.BackBufferWidth - 0.5f,         -0.5f,                       z, rhw, col, 1.0f, 0.0f);

	pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(D3DTLVERTEX));

	pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

	int filter = regQueryDwordValue(_T("d3dFilter"), 0);
	if (filter < 0 || filter > 2)
		filter = 0;
	updateFiltering(filter);
}

// Copies the frame into the texture: the source buffer carries a one row
// border and, except at 24 bpp, two extra pixels per row, so the copy starts
// one row in and walks the real stride.
void Direct3DDisplay::fillFrame(u8 *bits, int pitch, u8 *data, int dataPitch)
{
	int copyX, copyY;
	systemGetLCDResolution(copyX, copyY);

	const int bytesPerPixel = systemColorDepth / 8;
	const int rowBytes = copyX * bytesPerPixel;
	const u8 *src = data + dataPitch;
	u8 *dst = bits;

	for (int y = 0; y < copyY; ++y)
	{
		memcpy(dst, src, rowBytes);
		src += dataPitch;
		dst += pitch;
	}
}

// The mip chain covers the whole square the texture is, not just the frame in
// its first rows and columns, and a level is built from the four texels above
// it: whatever the rest of the texture holds would be pulled into the edges of
// the picture.  Repeating the last row and column of the frame across the rest
// keeps the edges of the frame what the picture put there.
void Direct3DDisplay::extendFrame(u8 *bits, int pitch, int frameWidth, int frameHeight)
{
	const int bytesPerPixel = systemColorDepth / 8;
	const int size = (int)textureSize;

	if (frameWidth <= 0 || frameHeight <= 0 || frameWidth > size || frameHeight > size)
		return;

	const int rowBytes = size * bytesPerPixel;

	for (int y = 0; y < frameHeight; ++y)
	{
		u8 *row  = bits + y * pitch;
		u8 *last = row + (frameWidth - 1) * bytesPerPixel;

		for (int x = frameWidth; x < size; ++x)
			memcpy(row + x * bytesPerPixel, last, bytesPerPixel);
	}

	u8 *lastRow = bits + (frameHeight - 1) * pitch;

	for (int y = frameHeight; y < size; ++y)
		memcpy(bits + y * pitch, lastRow, rowBytes);
}

void Direct3DDisplay::render()
{
	if (!pDevice)
		return;

	// Test the cooperative level to see if it's okay to render
	if (FAILED(pDevice->TestCooperativeLevel()))
	{
		return;
	}
	pDevice->Clear(0L, NULL, D3DCLEAR_TARGET, 0x000000ff, 1.0f, 0L);

	u8 *data = pix;
	int dataPitch = theApp.filterWidth * (systemColorDepth / 8) + (systemColorDepth == 24 ? 0 : 4);

	if (textMethod != 0) // do not draw Lua HUD to a video dump
	{
		systemClonePixBuffer(osd);
		data = osd;
		systemRenderLua((u8 *)data, dataPitch);
	}

	if (SUCCEEDED(pDevice->BeginScene()))
	{
		// The trilinear filter reads a mip chain, and the chain is only made
		// for the texture that has one: the frame goes into the staging copy
		// of it, and UpdateTexture is what has the device rebuild the chain
		// from the new frame.  Every other filter draws the frame texture
		// itself.
		const bool trilinear = filterMode == 2 && pMipTexture != NULL && pMipStaging != NULL;
		LPDIRECT3DTEXTURE9 target = trilinear ? pMipStaging : pTexture;

		D3DLOCKED_RECT locked;
		if (target && SUCCEEDED(target->LockRect(0, &locked, NULL,
		                                         trilinear ? 0 : D3DLOCK_DISCARD)))
		{
			if (theApp.filterFunction)
			{
					theApp.filterFunction(data + dataPitch,
					                      dataPitch,
					                      (u8 *)theApp.delta,
					                      (u8 *)locked.pBits,
					                      locked.Pitch,
					                      theApp.filterWidth,
					                      theApp.filterHeight);
			}
			else
				fillFrame((u8 *)locked.pBits, locked.Pitch, data, dataPitch);

			if (isFullscreenOption(theApp.videoOption) && theApp.showSpeed)
			{
				char buffer[30];
				if (theApp.showSpeed == 1)
					sprintf(buffer, "%3d%%", systemSpeed);
				else
					sprintf(buffer, "%3d%%(%d, %d fps)", systemSpeed,
					        systemFrameSkip,
					        theApp.showRenderedFrames);
				if (theApp.showSpeedTransparent)
					drawTextTransp((u8 *)locked.pBits,
					               locked.Pitch,
					               theApp.rect.left+10,
					               theApp.rect.bottom-10,
					               buffer);
				else
					drawText((u8 *)locked.pBits,
					         locked.Pitch,
					         theApp.rect.left+10,
					         theApp.rect.bottom-10,
					         buffer);
			}

			if (textMethod == 1)
			{
				DrawTextMessages((u8 *)locked.pBits, locked.Pitch, theApp.rect.left, theApp.rect.bottom);
			}

			if (trilinear)
			{
				// The CPU filter and the text only write the frame, so the rest
				// of the square is filled in before the chain is made.
				int frameWidth, frameHeight;
				if (theApp.filterFunction)
				{
					frameWidth  = theApp.filterWidth;
					frameHeight = theApp.filterHeight;
				}
				else
					systemGetLCDResolution(frameWidth, frameHeight);

				extendFrame((u8 *)locked.pBits, locked.Pitch, frameWidth, frameHeight);
			}

			target->UnlockRect(0);

			// set the texture
			if (trilinear)
			{
				pDevice->UpdateTexture(pMipStaging, pMipTexture);
				pDevice->SetTexture(0, pMipTexture);
			}
			else
				pDevice->SetTexture(0, pTexture);

			// configure shader for vertex type
			pDevice->SetVertexShader(NULL);
			pDevice->SetFVF(D3DFVF_TLVERTEX);

#ifdef D3D_DRAW_SINGLE_RECTANGLE
			// draw the rectangle
				pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, verts, sizeof(D3DTLVERTEX));
			//#undef D3D_DRAW_RECT
#else
			// draw the triangles
			pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(D3DTLVERTEX));
#endif
		}

		drawTextOverlay();

		pDevice->EndScene();
	}

	present();
}

void Direct3DDisplay::present()
{
	if (pDeviceEx != NULL)
		pDeviceEx->PresentEx(NULL, NULL, NULL, NULL, 0);
	else
		pDevice->Present(NULL, NULL, NULL, NULL);
}

void Direct3DDisplay::invalidateDeviceObjects()
{
	overlay.destroy();
}

void Direct3DDisplay::resize(int w, int h)
{
	if (pDevice && w > 0 && h > 0)
	{
		// The window layout calls this whenever the picture area changes,
		// and a reset to the size the swap chain already has would cost a
		// frame and flash the picture black.
		if ((int)dpp.BackBufferWidth == w && (int)dpp.BackBufferHeight == h)
			return;

		dpp.BackBufferWidth  = w;
		dpp.BackBufferHeight = h;
		HRESULT hr;
		invalidateDeviceObjects();
		releaseTextures();
		if (pDeviceEx != NULL)
		{
			hr = pDeviceEx->ResetEx(&dpp, NULL);

			// A reset does not have to keep the flip model, in which case the
			// bitblt model takes over for the rest of the session.
			if (FAILED(hr) && dpp.SwapEffect == D3DSWAPEFFECT_FLIPEX)
			{
				winlog(_T("Direct3D 9Ex: reset refused the flip model (%08x), using the bitblt model\n"), hr);
				dpp.SwapEffect      = D3DSWAPEFFECT_DISCARD;
				dpp.BackBufferCount = 1;

				hr = pDeviceEx->ResetEx(&dpp, NULL);
			}
		}
		else
			hr = pDevice->Reset(&dpp);
		if (SUCCEEDED(hr))
		{
			currentSwapEffect = (int)dpp.SwapEffect;
			// A reset keeps the device it was created as, so the flavor does
			// not change here; this is only for the sake of keeping the two
			// together.
			currentDeviceIs9Ex = (pDeviceEx != NULL);
			restoreDeviceObjects();
			if (width > 0 && height > 0)
				initializeOffscreen(width, height);
		}
		else
			systemMessage(0, "Failed device reset %08x", hr);
	}
	calculateVertices();
}

bool Direct3DDisplay::changeRenderSize(int w, int h)
{
	if (w != width || h != height)
	{
		// The trilinear textures are the same size as the frame, so they go
		// with it.
		if (pMipStaging)
		{
			pMipStaging->Release();
			pMipStaging = NULL;
		}
		if (pMipTexture)
		{
			pMipTexture->Release();
			pMipTexture = NULL;
		}
		if (pTexture)
		{
			pTexture->Release();
			pTexture = NULL;
		}
		if (!initializeOffscreen(w, h))
		{
			failed = true;
			return false;
		}
	}
	calculateVertices();

	return true;
}

void Direct3DDisplay::setOption(const char *option, int value)
{
	if (!strcmp(option, "d3dFilter"))
		updateFiltering(value);
}

IDisplay *newDirect3DDisplay()
{
	return new Direct3DDisplay();
}
