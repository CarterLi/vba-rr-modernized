// Direct3D11.cpp: Direct3D 11 display backend.
//
// The emulator frame is uploaded into a dynamic BGRA/BGR565 texture and drawn
// as a single textured quad.  OSD text is rendered by TextOverlay into a
// second texture and blended on top, so it stays 1:1 with the window.
//
// Optionally the frame is scaled up by an upscaler instead of the sampler:
// NVIDIA Image Scaling (a compute shader) or AMD FidelityFX Super Resolution
// 1.0 (a reconstruction and a sharpening pixel shader) runs between the frame
// upload and the quad draw, see the upscaling sections below.
//
// The swap chain presents in the flip model where the system supports it and in
// the bitblt model everywhere else, see createSwapChain().

#include "stdafx.h"

// Neither d3d11.dll nor d3dcompiler_47.dll is linked, they are loaded by hand,
// see direct3D11Available() below.  Nothing from dxgi.dll is called either: the
// factory comes from the device and the swap chain from that.

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h> // IDXGIFactory2, the flip model swap chain (Windows 8)
#include <d3dcompiler.h>

#include "resource.h"
#include "MainWnd.h"
#include "Reg.h"
#include "VBA.h"
#include "TextOverlay.h"

// NVIDIA Image Scaling SDK, see src/win32/upscalers for the shader sources.
#include "upscalers/NIS_Config.h"
// AMD FidelityFX Super Resolution 1.0 host constants, shared with the OpenGL
// 3.3 backend.
#include "upscalers/UpscalerFsr.h"

#include "../common/SystemGlobals.h"
#include "../common/Text.h"
#include "../version.h"

extern int Init_2xSaI(u32);
extern void directXMessage(const char *);

// d3d11.dll and d3dcompiler_47.dll are not part of Windows 7, and an executable
// that imports them cannot start there at all, even when it never touches the
// Direct3D 11 renderer.  They are therefore resolved on demand, and
// direct3D11Available() lets the render method menu leave the renderer out when
// either of them is missing.  The DLLs stay loaded for the lifetime of the
// process so the entry points stay valid across display recreation.
static HINSTANCE d3d11DLL       = NULL;
static HINSTANCE d3dcompilerDLL = NULL;
static decltype(&D3D11CreateDevice) d3d11CreateDevice = NULL;
static decltype(&D3DCompile)        d3dCompile        = NULL;
static bool      d3d11Probed    = false;

// Resolves the DLLs once.  Silent, because the render method menu uses the
// outcome and a message box per menu would be absurd.
static void loadD3D11Libraries()
{
	if (d3d11Probed)
		return;

	d3d11Probed = true;

	d3d11DLL = LoadLibrary(_T("D3D11.DLL"));
	if (d3d11DLL != NULL)
		d3d11CreateDevice = reinterpret_cast<decltype(&D3D11CreateDevice)>(
		    GetProcAddress(d3d11DLL, "D3D11CreateDevice"));

	d3dcompilerDLL = LoadLibrary(_T("D3DCOMPILER_47.DLL"));
	if (d3dcompilerDLL != NULL)
		d3dCompile = reinterpret_cast<decltype(&D3DCompile)>(
		    GetProcAddress(d3dcompilerDLL, "D3DCompile"));

	if (d3d11CreateDevice == NULL)
		winlog(_T("D3D11: D3D11.DLL is not available\n"));
	if (d3dCompile == NULL)
		winlog(_T("D3D11: D3DCOMPILER_47.DLL is not available\n"));
}

bool direct3D11Available()
{
	loadD3D11Libraries();

	return d3d11CreateDevice != NULL && d3dCompile != NULL;
}

// The swap effect the running display presents with, -1 while there is none.
static int currentSwapEffect = -1;

// Names the presentation model that is in use, or returns NULL while the
// renderer is not running.  The render method menu asks for this so that the
// model is visible without digging through the trace log, see
// MainWndOptions.cpp.
const char *direct3D11PresentationModel()
{
	switch (currentSwapEffect)
	{
	case DXGI_SWAP_EFFECT_FLIP_DISCARD:    return "Flip";
	case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "Flip sequential";
	case DXGI_SWAP_EFFECT_DISCARD:         return "Bitblt";
	}

	return NULL;
}

struct D3D11VERTEX
{
	float x, y;
	float u, v;
};

static const char *g_quadShader =
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VSOut VSMain(float4 pos : POSITION, float2 uv : TEXCOORD0)\n"
    "{\n"
    "    VSOut o;\n"
    "    o.pos = pos;\n"
    "    o.uv = uv;\n"
    "    return o;\n"
    "}\n"
    "Texture2D frameTex : register(t0);\n"
    "SamplerState frameSampler : register(s0);\n"
    "float4 PSMain(VSOut i) : SV_TARGET\n"
    "{\n"
    "    return frameTex.Sample(frameSampler, i.uv);\n"
    "}\n";

// --- NVIDIA Image Scaling (NIS) ------------------------------------------
//
// The block layout is baked into the compute shader and has to match the
// dispatch; 32x24 with 128 threads per group is what the SDK's NISOptimizer
// picks for upscaling on generic hardware.
#define NIS_BLOCK_WIDTH        32
#define NIS_BLOCK_HEIGHT       24
#define NIS_THREAD_GROUP_SIZE  128

#define NIS_STR_INNER(x) #x
#define NIS_STR(x)       NIS_STR_INNER(x)

// A single NIS pass scales by at most 2x, so larger windows need several
// passes.  Four of them reach 16x, well past any window size.
#define NIS_MAX_PASSES 4

// FidelityFX Super Resolution 1.0 reconstructs the frame (EASU) and sharpens it
// (RCAS) in one full screen pass each; FSR_PASS_COUNT, FSR_CONSTANT_FLOATS and
// the constants themselves are in upscalers/UpscalerFsr.h, which the OpenGL 3.3
// backend fills the same way.

// --- Upscaling -----------------------------------------------------------
//
// The upscaler shaders, and the SDK headers they include, are compiled while
// the renderer starts up.  Their sources are stored in the executable as
// RCDATA resources rather than in string literals, because the FidelityFX
// headers are far past the 64 kB a single C++ literal may hold; see
// src/win32/upscalers.

static const char *upscalerShaderSource(LPCSTR name, DWORD *size)
{
	HRSRC resource = FindResource(NULL, winWideFromAnsi(name), RT_RCDATA);
	if (resource == NULL)
	{
		winlog(_T("upscaler: %hs is missing from the executable\n"), name);
		return NULL;
	}

	HGLOBAL handle = LoadResource(NULL, resource);
	if (handle == NULL)
	{
		winlog(_T("upscaler: cannot load %hs\n"), name);
		return NULL;
	}

	if (size)
		*size = SizeofResource(NULL, resource);
	return (const char *)LockResource(handle);
}

// The SDK headers the shaders include, as they are stored in the executable.
// A shader that includes another one needs a row here and the resource in
// vba.rc, nothing else.
struct UpscalerInclude
{
	const char *file;     // the name as the shader includes it
	const char *resource; // the RCDATA resource holding it
};

static const UpscalerInclude g_upscalerIncludes[] =
{
	{ "NIS_Scaler.h", "NIS_SHADER_SCALER" },
	{ "ffx_a.h",      "FSR_SHADER_A" },
	{ "ffx_fsr1.h",   "FSR_SHADER_FSR1" },
};

#define UPSCALER_INCLUDE_COUNT (sizeof(g_upscalerIncludes) / sizeof(g_upscalerIncludes[0]))

// Serves the SDK headers to D3DCompile from those resources, so the shaders
// stay inside the executable instead of sitting next to it.
class UpscalerIncludeHandler : public ID3DInclude
{
public:
	STDMETHOD(Open)(D3D_INCLUDE_TYPE type, LPCSTR name, LPCVOID parent, LPCVOID *data, UINT *bytes)
	{
		if (name == NULL)
			return E_FAIL;

		const char *resource = NULL;

		for (UINT i = 0; i < UPSCALER_INCLUDE_COUNT; i++)
			if (!_stricmp(name, g_upscalerIncludes[i].file))
			{
				resource = g_upscalerIncludes[i].resource;
				break;
			}

		if (resource == NULL)
			return E_FAIL;

		DWORD size = 0;
		const char *source = upscalerShaderSource(resource, &size);
		if (source == NULL)
			return E_FAIL;

		*data  = source;
		*bytes = (UINT)size;
		return S_OK;
	}

	STDMETHOD(Close)(LPCVOID data)
	{
		return S_OK;
	}
};

// The upscaled frame fills the back buffer, so it is drawn with a quad that
// maps the whole window onto the whole texture.
static void setFullscreenQuad(D3D11VERTEX *vertices)
{
	vertices[0].x = -1.0f; vertices[0].y = -1.0f; vertices[0].u = 0.0f; vertices[0].v = 1.0f;
	vertices[1].x = -1.0f; vertices[1].y =  1.0f; vertices[1].u = 0.0f; vertices[1].v = 0.0f;
	vertices[2].x =  1.0f; vertices[2].y = -1.0f; vertices[2].u = 1.0f; vertices[2].v = 1.0f;
	vertices[3].x =  1.0f; vertices[3].y =  1.0f; vertices[3].u = 1.0f; vertices[3].v = 0.0f;
}

// --- Upscaler parts ------------------------------------------------------
//
// The pieces every upscaler is made of.  They are here so that a new one is
// mostly data: the shaders are loaded through compileUpscalerShader(), the
// constants through createUpscalerConstants(), and the targets they draw into
// through updateUpscaleTarget().

// Compiles one of the shaders stored in the executable.  Returns NULL and logs
// what the compiler said when it does not build.
static ID3DBlob *compileUpscalerShader(LPCSTR resource, LPCSTR name, LPCSTR entry,
                                       LPCSTR profile, const D3D_SHADER_MACRO *defines = NULL,
                                       ID3DInclude *include = NULL)
{
	DWORD sourceSize = 0;
	const char *source = upscalerShaderSource(resource, &sourceSize);
	if (source == NULL)
		return NULL;

	ID3DBlob *blob  = NULL;
	ID3DBlob *error = NULL;

	if (FAILED(d3dCompile(source, sourceSize, name, defines, include, entry, profile, 0, 0,
	                      &blob, &error)))
	{
		winlog(_T("%hs: %hs does not compile: %hs\n"), name, entry,
		       error ? (const char *)error->GetBufferPointer() : "?");
		if (error)
			error->Release();
		return NULL;
	}

	if (error)
		error->Release();

	return blob;
}

// Compiles one of the shaders stored in the executable and creates the pixel
// shader out of it.
static ID3D11PixelShader *createUpscalerPixelShader(ID3D11Device *device, LPCSTR resource,
                                                    LPCSTR name, LPCSTR entry, LPCSTR profile,
                                                    ID3DInclude *include = NULL)
{
	ID3DBlob *blob = compileUpscalerShader(resource, name, entry, profile, NULL, include);
	if (blob == NULL)
		return NULL;

	ID3D11PixelShader *shader = NULL;
	HRESULT hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL,
	                                       &shader);
	blob->Release();

	if (FAILED(hr))
	{
		winlog(_T("%hs: CreatePixelShader failed %08x\n"), name, hr);
		return NULL;
	}

	return shader;
}

// The dynamic constant buffer a pass writes its constants into, and the upload
// that fills it.
static bool createUpscalerConstants(ID3D11Device *device, UINT bytes, ID3D11Buffer **buffer)
{
	D3D11_BUFFER_DESC cbd;
	ZeroMemory(&cbd, sizeof(cbd));
	cbd.ByteWidth      = bytes;
	cbd.Usage          = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	return SUCCEEDED(device->CreateBuffer(&cbd, NULL, buffer));
}

static bool writeUpscalerConstants(ID3D11DeviceContext *context, ID3D11Buffer *buffer,
                                   const void *constants, UINT bytes)
{
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;

	memcpy(mapped.pData, constants, bytes);
	context->Unmap(buffer, 0);
	return true;
}

// The texture one pass of an upscaler draws into, with the view the next pass,
// or the quad, reads it back through.
struct UpscaleTarget
{
	ID3D11Texture2D          *texture;
	ID3D11ShaderResourceView *srv;
	ID3D11RenderTargetView   *rtv;
	int                       width;
	int                       height;
};

static void releaseUpscaleTarget(UpscaleTarget &target)
{
	if (target.rtv)     { target.rtv->Release();     target.rtv = NULL; }
	if (target.srv)     { target.srv->Release();     target.srv = NULL; }
	if (target.texture) { target.texture->Release(); target.texture = NULL; }
	target.width  = 0;
	target.height = 0;
}

// Creates the target, or keeps the one that already has the size the pass
// needs.  Returns false, and leaves the target empty, when the device will not
// give us one.
static bool updateUpscaleTarget(ID3D11Device *device, UpscaleTarget &target,
                                int width, int height, const char *name)
{
	if (width <= 0 || height <= 0)
		return false;

	if (target.texture != NULL && target.width == width && target.height == height)
		return true;

	releaseUpscaleTarget(target);

	D3D11_TEXTURE2D_DESC td;
	ZeroMemory(&td, sizeof(td));
	td.Width            = width;
	td.Height           = height;
	td.MipLevels        = 1;
	td.ArraySize        = 1;
	td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage            = D3D11_USAGE_DEFAULT;
	td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	if (FAILED(device->CreateTexture2D(&td, NULL, &target.texture)) ||
	    FAILED(device->CreateShaderResourceView(target.texture, NULL, &target.srv)) ||
	    FAILED(device->CreateRenderTargetView(target.texture, NULL, &target.rtv)))
	{
		winlog(_T("%hs: cannot create the %dx%d target\n"), name, width, height);
		releaseUpscaleTarget(target);
		return false;
	}

	target.width  = width;
	target.height = height;
	return true;
}

// False for an upscaler this backend cannot run, see upscalerAvailable().  It is
// declared here because the IDisplay method answers with it, defined with the
// upscaling code further down.
bool direct3D11UpscalerAvailable(int upscaler);

// The name of an upscaler the way the table spells it, or NULL for one that is
// not in it.  Declared here for the trace the renderer writes while it starts.
const char *direct3D11UpscalerName(int upscaler);

class Direct3D11Display : public IDisplay
{
public:
	Direct3D11Display();
	virtual ~Direct3D11Display();

	virtual bool initialize();
	virtual void cleanup();
	virtual void render();
	virtual void clear();
	virtual bool changeRenderSize(int w, int h);
	virtual void resize(int w, int h);
	virtual DISPLAY_TYPE getType() { return DIRECT_3D11; };
	virtual void setOption(const char *, int);
	virtual bool upscalerAvailable(int upscaler) { return direct3D11UpscalerAvailable(upscaler); };
	// Trilinear reads a mip chain, which is the texture pMipTexture carries,
	// so it stays unselectable where that one could not be made.
	virtual bool filterAvailable(int filter)
	{
		return filter >= 0 && filter <= 2 && (filter != 2 || pMipTexture != NULL);
	};

private:
	bool createDevice();
	bool createSwapChain();
	bool createShaders();
	bool createSamplers();
	bool createBackBufferViews();
	void releaseFrameTexture();
	void releaseAll();
	bool initializeOffscreen(int w, int h);
	void createOverlayTarget(int w, int h);
	void updateFiltering(int filter);
	void calculateVertices();
	void drawQuad(ID3D11ShaderResourceView *srv, D3D11VERTEX *quad, bool blend,
	              ID3D11PixelShader *pixelShader = NULL);

	// NVIDIA Image Scaling
	bool initNis();
	void releaseNis();
	void releaseNisTargets();
	bool updateNisChain(int srcWidth, int srcHeight, int dstWidth, int dstHeight);
	ID3D11ShaderResourceView *scaleFrameWithNis();

	// AMD FidelityFX Super Resolution 1.0
	bool initFsr();
	void releaseFsr();
	void releaseFsrTargets();
	bool updateFsrTargets(int dstWidth, int dstHeight);
	ID3D11ShaderResourceView *scaleFrameWithFsr();

	// The pixel filter shaders of the table below: one pixel shader per pass,
	// the block of constants each of them reads and the target each draws into.
	// The chain the user selected is the one that gets built.
	void initUpscaler(int upscaler);
	void releaseUpscalers();
	ID3D11ShaderResourceView *scaleFrameWithUpscaler(int upscaler);

	HWND                  hwnd;
	ID3D11Device         *pDevice;
	ID3D11DeviceContext  *pContext;
	IDXGISwapChain       *pSwapChain;
	// Set when the swap chain presents in the flip model, where every Present
	// hands a different back buffer over and the render target view has to be
	// rebuilt for the buffer that is current (see createSwapChain()).
	bool                  flipModel;
	ID3D11RenderTargetView *pRTV;
	ID3D11Texture2D      *pFrameTexture;
	ID3D11ShaderResourceView *pFrameSRV;
	// The trilinear filter reads a mip chain.  The frame texture is dynamic so
	// the frame can be mapped into it, and a dynamic texture has no chain, so
	// the frame is copied into this one and GenerateMips() rebuilds the chain
	// from it; see render().
	ID3D11Texture2D      *pMipTexture;
	ID3D11ShaderResourceView *pMipSRV;
	ID3D11Texture2D      *pOverlayTexture;
	ID3D11ShaderResourceView *pOverlaySRV;
	ID3D11VertexShader   *pVS;
	ID3D11PixelShader    *pPS;
	ID3D11InputLayout    *pLayout;
	ID3D11SamplerState   *pSamplerPoint;
	ID3D11SamplerState   *pSamplerLinear;
	ID3D11SamplerState   *pSamplerTrilinear;
	ID3D11BlendState     *pBlendAlpha;
	ID3D11Buffer         *pVertexBuffer;

	// NVIDIA Image Scaling: one pass target per 2x step between the frame and
	// the back buffer.
	struct NisTarget
	{
		ID3D11Texture2D           *texture;
		ID3D11ShaderResourceView  *srv;
		ID3D11UnorderedAccessView *uav;
		int                        width;
		int                        height;
	};

	D3D_FEATURE_LEVEL          featureLevel;
	bool                       nisAvailable;
	ID3D11ComputeShader       *pNisCS;
	ID3D11Buffer              *pNisCB;
	ID3D11Texture2D           *pNisCoefScale;
	ID3D11ShaderResourceView  *pNisCoefScaleSRV;
	ID3D11Texture2D           *pNisCoefUsm;
	ID3D11ShaderResourceView  *pNisCoefUsmSRV;
	NisTarget                  nisTargets[NIS_MAX_PASSES];
	int                        nisPassCount;
	int                        nisSrcWidth;
	int                        nisSrcHeight;
	int                        nisDstWidth;
	int                        nisDstHeight;

	// FidelityFX Super Resolution: EASU writes the first target, RCAS the
	// second one, which is the one that gets drawn.
	bool                       fsrAvailable;
	ID3D11PixelShader         *pFsrEasuPS;
	ID3D11PixelShader         *pFsrRcasPS;
	ID3D11Buffer              *pFsrCB;
	UpscaleTarget              fsrTargets[FSR_PASS_COUNT];

	DXGI_FORMAT frameFormat;
	int  width;
	int  height;
	int  backWidth;
	int  backHeight;
	int  filterMode;
	bool failed;
	TextOverlay overlay;
	D3D11VERTEX quad[4];
};

Direct3D11Display::Direct3D11Display()
{
	hwnd          = NULL;
	pDevice       = NULL;
	pContext      = NULL;
	pSwapChain    = NULL;
	flipModel     = false;
	pRTV          = NULL;
	pFrameTexture = NULL;
	pFrameSRV     = NULL;
	pMipTexture   = NULL;
	pMipSRV       = NULL;
	pOverlayTexture = NULL;
	pOverlaySRV   = NULL;
	pVS           = NULL;
	pPS           = NULL;
	pLayout       = NULL;
	pSamplerPoint = NULL;
	pSamplerLinear = NULL;
	pSamplerTrilinear = NULL;
	pBlendAlpha   = NULL;
	pVertexBuffer = NULL;
	featureLevel  = D3D_FEATURE_LEVEL_10_0;
	nisAvailable  = false;
	pNisCS        = NULL;
	pNisCB        = NULL;
	pNisCoefScale = NULL;
	pNisCoefScaleSRV = NULL;
	pNisCoefUsm   = NULL;
	pNisCoefUsmSRV = NULL;
	ZeroMemory(nisTargets, sizeof(nisTargets));
	nisPassCount  = 0;
	nisSrcWidth   = 0;
	nisSrcHeight  = 0;
	nisDstWidth   = 0;
	nisDstHeight  = 0;
	fsrAvailable  = false;
	pFsrEasuPS    = NULL;
	pFsrRcasPS    = NULL;
	pFsrCB        = NULL;
	ZeroMemory(fsrTargets, sizeof(fsrTargets));
	frameFormat   = DXGI_FORMAT_B8G8R8A8_UNORM;
	width         = 0;
	height        = 0;
	backWidth     = 0;
	backHeight    = 0;
	filterMode    = 0;
	failed        = false;
}

Direct3D11Display::~Direct3D11Display()
{
	cleanup();
}

void Direct3D11Display::cleanup()
{
        overlay.destroy();

        // Drop every binding and wait for the GPU before the views and the
        // swap chain are released.  Releasing them while the context still
        // points at them is what leaves a device behind that the next
        // creation trips over, and releaseAll() clears pContext, so this has
        // to run first.
        if (pContext)
        {
                pContext->ClearState();
                pContext->Flush();
        }

        releaseAll();
}

void Direct3D11Display::releaseAll()
{
	releaseFrameTexture();
	releaseNis();
	releaseFsr();
	releaseUpscalers();

	if (pOverlaySRV)   { pOverlaySRV->Release();   pOverlaySRV = NULL; }
	if (pOverlayTexture) { pOverlayTexture->Release(); pOverlayTexture = NULL; }
	if (pRTV)          { pRTV->Release();          pRTV = NULL; }
	if (pVertexBuffer) { pVertexBuffer->Release(); pVertexBuffer = NULL; }
	if (pBlendAlpha)   { pBlendAlpha->Release();   pBlendAlpha = NULL; }
	if (pSamplerTrilinear) { pSamplerTrilinear->Release(); pSamplerTrilinear = NULL; }
	if (pSamplerLinear) { pSamplerLinear->Release(); pSamplerLinear = NULL; }
	if (pSamplerPoint) { pSamplerPoint->Release(); pSamplerPoint = NULL; }
	if (pLayout)       { pLayout->Release();       pLayout = NULL; }
	if (pPS)           { pPS->Release();           pPS = NULL; }
	if (pVS)           { pVS->Release();           pVS = NULL; }
	if (pSwapChain)
	{
		// A swap chain that something else still holds keeps the window it was
		// made for: the next renderer then either paints behind the last frame
		// or cannot take the window at all.  The count left over says whether
		// that is what happened.
		ULONG left = pSwapChain->Release();
		pSwapChain = NULL;

		if (left != 0)
			winlog(_T("D3D11: the swap chain is still held by %u reference(s)\n"), left);
	}
	flipModel = false;
	currentSwapEffect = -1;
	if (pContext)      { ULONG left = pContext->Release(); pContext = NULL;
		if (left != 0) winlog(_T("D3D11: the context is still held by %u reference(s)\n"), left); }
	if (pDevice)       { ULONG left = pDevice->Release();  pDevice = NULL;
		if (left != 0) winlog(_T("D3D11: the device is still held by %u reference(s)\n"), left); }
}

void Direct3D11Display::releaseFrameTexture()
{
	if (pMipSRV)       { pMipSRV->Release();       pMipSRV = NULL; }
	if (pMipTexture)   { pMipTexture->Release();   pMipTexture = NULL; }
	if (pFrameSRV)     { pFrameSRV->Release();     pFrameSRV = NULL; }
	if (pFrameTexture) { pFrameTexture->Release(); pFrameTexture = NULL; }
}

bool Direct3D11Display::createDevice()
{
	if (!direct3D11Available())
		return false;

	UINT flags = 0;
#ifdef _DEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL obtained;

	// The device is created on its own: a flip model swap chain can only be
	// created from the factory that belongs to the device, see
	// createSwapChain().
	HRESULT hr = d3d11CreateDevice(NULL,
	                               D3D_DRIVER_TYPE_HARDWARE,
	                               NULL,
	                               flags,
	                               levels,
	                               sizeof(levels) / sizeof(levels[0]),
	                               D3D11_SDK_VERSION,
	                               &pDevice,
	                               &obtained,
	                               &pContext);
	if (FAILED(hr))
	{
		winlog(_T("D3D11CreateDevice failed %08x\n"), hr);
		return false;
	}

	featureLevel = obtained;

	return createSwapChain() && createBackBufferViews();
}

// A flip model swap chain does not copy the back buffer into a redirection
// surface the way the bitblt model does, the desktop window manager composes
// straight from it, which costs one full frame copy less per present and is what
// lets the window manager send the frames to the display directly.  It needs
// DXGI 1.2 (Windows 8) to exist at all and DXGI 1.4 (Windows 10) for
// FLIP_DISCARD, and it cannot be created through the legacy
// IDXGIFactory::CreateSwapChain, so each level is asked for and the bitblt swap
// chain below is what is left for systems that support neither.
bool Direct3D11Display::createSwapChain()
{
	IDXGIDevice   *dxgiDevice = NULL;
	IDXGIAdapter  *adapter    = NULL;
	IDXGIFactory  *factory    = NULL;
	IDXGIFactory2 *factory2   = NULL;
	HRESULT        flipRefused = S_OK;

	// The factory that owns the device, so no DXGI entry point has to be
	// imported (see loadD3D11Libraries()).
	HRESULT hr = pDevice->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice);
	if (SUCCEEDED(hr))
		hr = dxgiDevice->GetAdapter(&adapter);
	if (SUCCEEDED(hr))
		hr = adapter->GetParent(__uuidof(IDXGIFactory), (void **)&factory);
	if (SUCCEEDED(hr) && factory != NULL)
		factory->QueryInterface(__uuidof(IDXGIFactory2), (void **)&factory2);

	if (factory2 != NULL)
	{
		DXGI_SWAP_CHAIN_DESC1 sd;
		ZeroMemory(&sd, sizeof(sd));
		sd.Width              = backWidth > 0 ? backWidth : 1;
		sd.Height             = backHeight > 0 ? backHeight : 1;
		sd.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.SampleDesc.Count   = 1; // the flip model does not allow MSAA
		sd.SampleDesc.Quality = 0;
		sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount        = 2; // the minimum, and the lowest latency
		sd.Scaling            = DXGI_SCALING_STRETCH;
		sd.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd.AlphaMode          = DXGI_ALPHA_MODE_IGNORE;

		// Windows 10 knows FLIP_DISCARD and Windows 8 only FLIP_SEQUENTIAL,
		// each refuses the other one, so this is what settles the second level.
		IDXGISwapChain1 *swapChain1 = NULL;
		hr = factory2->CreateSwapChainForHwnd(pDevice, hwnd, &sd, NULL, NULL, &swapChain1);
		if (FAILED(hr))
		{
			sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
			hr = factory2->CreateSwapChainForHwnd(pDevice, hwnd, &sd, NULL, NULL, &swapChain1);
		}

		if (SUCCEEDED(hr))
		{
			pSwapChain = swapChain1;
			flipModel  = true;

			// What the swap chain ended up with, so the model that is in use
			// can be read back out of the trace file (and named by the render
			// method menu) instead of assumed.
			DXGI_SWAP_CHAIN_DESC1 actual;
			ZeroMemory(&actual, sizeof(actual));
			if (FAILED(swapChain1->GetDesc1(&actual)))
			{
				actual.SwapEffect  = sd.SwapEffect;
				actual.BufferCount = sd.BufferCount;
			}

			currentSwapEffect = (int)actual.SwapEffect;
			winlog(_T("D3D11: swap effect %d (%hs), %d buffers\n"),
			       (int)actual.SwapEffect,
			       actual.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "flip discard" : "flip sequential",
			       (int)actual.BufferCount);
		}
		else
		{
			// Only the last line written to the trace log survives, so the
			// reason the flip model was turned down is carried into the line
			// that reports the model that is in use.
			flipRefused = hr;
		}
	}

	if (pSwapChain == NULL && factory != NULL)
	{
		DXGI_SWAP_CHAIN_DESC sd;
		ZeroMemory(&sd, sizeof(sd));
		sd.BufferCount                        = 1;
		sd.BufferDesc.Width                   = backWidth > 0 ? backWidth : 1;
		sd.BufferDesc.Height                  = backHeight > 0 ? backHeight : 1;
		sd.BufferDesc.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.BufferDesc.RefreshRate.Numerator   = 60;
		sd.BufferDesc.RefreshRate.Denominator = 1;
		sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow                       = hwnd;
		sd.SampleDesc.Count                   = 1;
		sd.SampleDesc.Quality                 = 0;
		sd.Windowed                           = TRUE;
		sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

		if (FAILED(factory->CreateSwapChain(pDevice, &sd, &pSwapChain)))
		{
			winlog(_T("D3D11: CreateSwapChain failed\n"));
			pSwapChain = NULL;
		}
		else
		{
			currentSwapEffect = (int)sd.SwapEffect;
			if (flipRefused != S_OK)
				winlog(_T("D3D11: no flip model swap chain (%08x), using swap effect %d (bitblt), 1 buffer\n"),
				       (unsigned int)flipRefused, (int)sd.SwapEffect);
			else
				winlog(_T("D3D11: swap effect %d (bitblt), 1 buffer\n"), (int)sd.SwapEffect);
		}
	}

	if (factory2)   factory2->Release();
	if (factory)    factory->Release();
	if (adapter)    adapter->Release();
	if (dxgiDevice) dxgiDevice->Release();

	if (pSwapChain == NULL)
	{
		winlog(_T("D3D11: no swap chain\n"));
		return false;
	}

	return true;
}

bool Direct3D11Display::createBackBufferViews()
{
	if (pRTV)
	{
		pRTV->Release();
		pRTV = NULL;
	}

	ID3D11Texture2D *backBuffer = NULL;
	if (FAILED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer)))
		return false;

	HRESULT hr = pDevice->CreateRenderTargetView(backBuffer, NULL, &pRTV);
	backBuffer->Release();
	return SUCCEEDED(hr);
}

bool Direct3D11Display::createShaders()
{
	ID3DBlob *vsBlob = NULL;
	ID3DBlob *psBlob = NULL;
	ID3DBlob *error = NULL;

	if (FAILED(d3dCompile(g_quadShader, strlen(g_quadShader), "quadVS", NULL, NULL,
	                      "VSMain", "vs_4_0", 0, 0, &vsBlob, &error)))
	{
		winlog(_T("D3D11 vertex shader compilation failed: %s\n"), error ? winWideFromAnsi((const char *)error->GetBufferPointer()) : CString(_T("?")));
		if (error) error->Release();
		return false;
	}
	if (FAILED(d3dCompile(g_quadShader, strlen(g_quadShader), "quadPS", NULL, NULL,
	                      "PSMain", "ps_4_0", 0, 0, &psBlob, &error)))
	{
		winlog(_T("D3D11 pixel shader compilation failed: %s\n"), error ? winWideFromAnsi((const char *)error->GetBufferPointer()) : CString(_T("?")));
		if (error) error->Release();
		vsBlob->Release();
		return false;
	}

	HRESULT hr = pDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &pVS);
	if (SUCCEEDED(hr))
		hr = pDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), NULL, &pPS);

	if (SUCCEEDED(hr))
	{
		D3D11_INPUT_ELEMENT_DESC elems[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		hr = pDevice->CreateInputLayout(elems, 2, vsBlob->GetBufferPointer(),
		                                vsBlob->GetBufferSize(), &pLayout);
	}

	vsBlob->Release();
	psBlob->Release();
	return SUCCEEDED(hr);
}

bool Direct3D11Display::createSamplers()
{
	D3D11_SAMPLER_DESC sd;
	ZeroMemory(&sd, sizeof(sd));
	sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = D3D11_FLOAT32_MAX;

	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	if (FAILED(pDevice->CreateSamplerState(&sd, &pSamplerPoint)))
		return false;

	sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	if (FAILED(pDevice->CreateSamplerState(&sd, &pSamplerLinear)))
		return false;

	// Trilinear is the one that reads between two mip levels; without a chain
	// to read it is bilinear with extra work, which is what the frame texture
	// that is sampled without one gets.
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	if (FAILED(pDevice->CreateSamplerState(&sd, &pSamplerTrilinear)))
		return false;

	D3D11_BLEND_DESC bd;
	ZeroMemory(&bd, sizeof(bd));
	bd.RenderTarget[0].BlendEnable           = TRUE;
	bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
	bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (FAILED(pDevice->CreateBlendState(&bd, &pBlendAlpha)))
		return false;

	D3D11_BUFFER_DESC vb;
	ZeroMemory(&vb, sizeof(vb));
	vb.Usage          = D3D11_USAGE_DYNAMIC;
	vb.ByteWidth      = sizeof(D3D11VERTEX) * 4;
	vb.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
	vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(pDevice->CreateBuffer(&vb, NULL, &pVertexBuffer)))
		return false;

	return true;
}

bool Direct3D11Display::initialize()
{
        CWnd *pWnd = theApp.winRenderWnd();

        if (pWnd == NULL || pWnd->GetSafeHwnd() == NULL)
        {
                winlog(_T("D3D11: no window to draw into\n"));
                return false;
        }

        if (!direct3D11Available())
	{
		// Same treatment as the other renderers: report what could not be
		// loaded and let the caller fall back to something else.
		if (d3d11DLL == NULL)
			directXMessage("D3D11.DLL");
		else if (d3d11CreateDevice == NULL)
			directXMessage("D3D11CreateDevice");
		else if (d3dcompilerDLL == NULL)
			directXMessage("D3DCOMPILER_47.DLL");
		else
			directXMessage("D3DCompile");
		return false;
	}

	hwnd      = pWnd->GetSafeHwnd();
	backWidth  = theApp.surfaceSizeX;
	backHeight = theApp.surfaceSizeY;


	// Pick the emulator pixel format from the desktop mode, like the D3D9 backend.
	HDC dc = GetDC(hwnd);
	int bits = GetDeviceCaps(dc, BITSPIXEL);
	ReleaseDC(hwnd, dc);

	if (bits <= 16)
	{
		systemColorDepth = 16;
		systemRedShift   = 11;
		systemGreenShift = 6;
		systemBlueShift  = 0;
		frameFormat      = DXGI_FORMAT_B5G6R5_UNORM;
		Init_2xSaI(565);
	}
	else
	{
		systemColorDepth = 32;
		// Same byte-aligned layout the D3D9 backend uses: a 5-bit component is
		// shifted into the top bits of its byte, so 0x1F becomes 0xF8 instead of
		// 0x1F.  Using 16/8/0 here renders the whole frame almost black.
		systemRedShift   = 19;
		systemGreenShift = 11;
		systemBlueShift  = 3;
		frameFormat      = DXGI_FORMAT_B8G8R8A8_UNORM;
		Init_2xSaI(32);
	}
	theApp.fsColorDepth = systemColorDepth;

	if (!createDevice())
	{
		systemMessage(0, "Failed to create Direct3D 11 device");
		return false;
	}
	if (!createShaders() || !createSamplers())
	{
		systemMessage(0, "Failed to initialise Direct3D 11 shaders");
		return false;
	}

	if (theApp.upscaler == UPSCALER_NIS)
		initNis();
	else if (theApp.upscaler == UPSCALER_FSR)
		initFsr();

	// Only the chain the user selected is built here.  Compiling every one of
	// them while the renderer starts is what used to hold up the start of the
	// program; the others are built when the menu selects them, see
	// setOption().  Failing is not fatal, the entry is then left out.
	initUpscaler(theApp.upscaler);

	// The setting the renderer starts with, once, and whether it can run it.
	{
		const char *picked = direct3D11UpscalerName(theApp.upscaler);

		if (picked == NULL)
			picked = theApp.upscaler == UPSCALER_NIS ? "NIS"
			       : theApp.upscaler == UPSCALER_FSR ? "FSR"
			       : theApp.upscaler == UPSCALER_OFF ? "Off"
			       : "not one of this renderer's";

		winlog(_T("D3D11: upscaler %hs (%d), sharpening %d, max scale %d%hs\n"),
		       picked, theApp.upscaler, theApp.upscalerSharpness, theApp.upscalerMaxScale,
		       direct3D11UpscalerAvailable(theApp.upscaler) ? "" : " - this renderer does not run it");
	}

	filterMode = regQueryDwordValue(_T("d3dFilter"), 0);
	if (filterMode < 0 || filterMode > 2)
		filterMode = 0;

	systemUpdateColorMaps();

	theApp.updateFilter();
	theApp.updateIFB();

	if (failed)
		return false;

	pWnd->DragAcceptFiles(TRUE);
	return true;
}

bool Direct3D11Display::initializeOffscreen(int w, int h)
{
	releaseFrameTexture();

	D3D11_TEXTURE2D_DESC td;
	ZeroMemory(&td, sizeof(td));
	td.Width              = w;
	td.Height             = h;
	td.MipLevels          = 1;
	td.ArraySize          = 1;
	td.Format             = frameFormat;
	td.SampleDesc.Count   = 1;
	td.Usage              = D3D11_USAGE_DYNAMIC;
	td.BindFlags          = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags     = D3D11_CPU_ACCESS_WRITE;

	HRESULT hr = pDevice->CreateTexture2D(&td, NULL, &pFrameTexture);
	if (FAILED(hr))
	{
		winlog(_T("D3D11 CreateTexture2D failed %08x\n"), hr);
		return false;
	}

	hr = pDevice->CreateShaderResourceView(pFrameTexture, NULL, &pFrameSRV);
	if (FAILED(hr))
	{
		releaseFrameTexture();
		return false;
	}

	// The trilinear filter reads a mip chain, and a dynamic texture - which is
	// what the frame has to be mapped into - has none.  This one is a copy of
	// it with the whole chain, which GenerateMips() rebuilds every frame; a
	// format the device cannot render to or build a chain for simply leaves
	// the filter unavailable.
	D3D11_TEXTURE2D_DESC md;
	ZeroMemory(&md, sizeof(md));
	md.Width            = w;
	md.Height           = h;
	md.MipLevels        = 0;
	md.ArraySize        = 1;
	md.Format           = frameFormat;
	md.SampleDesc.Count = 1;
	md.Usage            = D3D11_USAGE_DEFAULT;
	md.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	md.MiscFlags        = D3D11_RESOURCE_MISC_GENERATE_MIPS;

	hr = pDevice->CreateTexture2D(&md, NULL, &pMipTexture);
	if (SUCCEEDED(hr))
		hr = pDevice->CreateShaderResourceView(pMipTexture, NULL, &pMipSRV);

	if (FAILED(hr))
	{
		if (pMipSRV)     { pMipSRV->Release();     pMipSRV = NULL; }
		if (pMipTexture) { pMipTexture->Release(); pMipTexture = NULL; }
		winlog(_T("D3D11: no mip chain for %dx%d, trilinear filtering is not available\n"), w, h);
	}

	if (pOverlayTexture == NULL && backWidth > 0 && backHeight > 0)
		createOverlayTarget(backWidth, backHeight);

	width  = w;
	height = h;
	return true;
}

// The on screen display is the size of the back buffer, so it is made when the
// swap chain is and remade whenever the window changes size; resize() throws
// the old one away and nothing else would bring it back.
void Direct3D11Display::createOverlayTarget(int w, int h)
{
	if (pOverlaySRV)     { pOverlaySRV->Release();     pOverlaySRV = NULL; }
	if (pOverlayTexture) { pOverlayTexture->Release(); pOverlayTexture = NULL; }
	overlay.destroy();

	if (pDevice == NULL || w <= 0 || h <= 0)
		return;

	D3D11_TEXTURE2D_DESC od;
	ZeroMemory(&od, sizeof(od));
	od.Width            = w;
	od.Height           = h;
	od.MipLevels        = 1;
	od.ArraySize        = 1;
	od.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
	od.SampleDesc.Count = 1;
	od.Usage            = D3D11_USAGE_DYNAMIC;
	od.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
	od.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;

	if (SUCCEEDED(pDevice->CreateTexture2D(&od, NULL, &pOverlayTexture)))
		pDevice->CreateShaderResourceView(pOverlayTexture, NULL, &pOverlaySRV);

	overlay.create(w, h);

	winlog(_T("D3D11: on screen display %dx%d%s\n"), w, h, pOverlaySRV != NULL ? _T("") : _T(", no view"));
}

void Direct3D11Display::updateFiltering(int filter)
{
	filterMode = filter;
}

// --- NVIDIA Image Scaling -------------------------------------------------

bool Direct3D11Display::initNis()
{
	if (nisAvailable)
		return true;

	if (pDevice == NULL)
		return false;

	if (featureLevel < D3D_FEATURE_LEVEL_11_0)
	{
		winlog(_T("NIS: needs Direct3D feature level 11.0\n"));
		return false;
	}

	D3D_SHADER_MACRO defines[] =
	{
		{ "NIS_SCALER",            "1" },
		{ "NIS_HDR_MODE",          "0" },
		{ "NIS_BLOCK_WIDTH",       NIS_STR(NIS_BLOCK_WIDTH) },
		{ "NIS_BLOCK_HEIGHT",      NIS_STR(NIS_BLOCK_HEIGHT) },
		{ "NIS_THREAD_GROUP_SIZE", NIS_STR(NIS_THREAD_GROUP_SIZE) },
		{ NULL, NULL }
	};

	UpscalerIncludeHandler includeHandler;
	ID3DBlob *blob = compileUpscalerShader("NIS_SHADER_MAIN", "NIS_Main.hlsl", "main", "cs_5_0",
	                                       defines, &includeHandler);
	if (blob == NULL)
		return false;

	HRESULT hr = pDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &pNisCS);
	blob->Release();
	if (FAILED(hr))
	{
		winlog(_T("NIS: CreateComputeShader failed %08x\n"), hr);
		return false;
	}

	// The filter bank is 64 phases of 8 taps; the SDK exposes it as two 2x64
	// RGBA32F textures (kFilterSize / 4 columns).
	D3D11_TEXTURE2D_DESC td;
	ZeroMemory(&td, sizeof(td));
	td.Width            = (UINT)(kFilterSize / 4);
	td.Height           = (UINT)kPhaseCount;
	td.MipLevels        = 1;
	td.ArraySize        = 1;
	td.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
	td.SampleDesc.Count = 1;
	td.Usage            = D3D11_USAGE_IMMUTABLE;
	td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA data;
	ZeroMemory(&data, sizeof(data));
	data.SysMemPitch = (UINT)kFilterSize * sizeof(float);

	data.pSysMem = coef_scale;
	if (FAILED(pDevice->CreateTexture2D(&td, &data, &pNisCoefScale)))
	{
		winlog(_T("NIS: cannot create the scaler coefficient texture\n"));
		return false;
	}

	data.pSysMem = coef_usm;
	if (FAILED(pDevice->CreateTexture2D(&td, &data, &pNisCoefUsm)))
	{
		winlog(_T("NIS: cannot create the sharpening coefficient texture\n"));
		return false;
	}

	if (FAILED(pDevice->CreateShaderResourceView(pNisCoefScale, NULL, &pNisCoefScaleSRV)) ||
	    FAILED(pDevice->CreateShaderResourceView(pNisCoefUsm, NULL, &pNisCoefUsmSRV)))
	{
		winlog(_T("NIS: cannot create the coefficient views\n"));
		return false;
	}

	if (!createUpscalerConstants(pDevice, sizeof(NISConfig), &pNisCB))
	{
		winlog(_T("NIS: cannot create the config buffer\n"));
		return false;
	}

	nisAvailable = true;
	return true;
}

void Direct3D11Display::releaseNisTargets()
{
	for (int i = 0; i < NIS_MAX_PASSES; i++)
	{
		if (nisTargets[i].uav)     { nisTargets[i].uav->Release();     nisTargets[i].uav = NULL; }
		if (nisTargets[i].srv)     { nisTargets[i].srv->Release();     nisTargets[i].srv = NULL; }
		if (nisTargets[i].texture) { nisTargets[i].texture->Release(); nisTargets[i].texture = NULL; }
		nisTargets[i].width  = 0;
		nisTargets[i].height = 0;
	}
	nisPassCount = 0;
}

void Direct3D11Display::releaseNis()
{
	releaseNisTargets();

	if (pNisCoefScaleSRV) { pNisCoefScaleSRV->Release(); pNisCoefScaleSRV = NULL; }
	if (pNisCoefUsmSRV)   { pNisCoefUsmSRV->Release();   pNisCoefUsmSRV   = NULL; }
	if (pNisCoefScale)    { pNisCoefScale->Release();    pNisCoefScale    = NULL; }
	if (pNisCoefUsm)      { pNisCoefUsm->Release();      pNisCoefUsm      = NULL; }
	if (pNisCB)           { pNisCB->Release();           pNisCB           = NULL; }
	if (pNisCS)           { pNisCS->Release();           pNisCS           = NULL; }
	nisAvailable = false;
}

bool Direct3D11Display::updateNisChain(int srcWidth, int srcHeight, int dstWidth, int dstHeight)
{
	if (nisPassCount > 0 && nisSrcWidth == srcWidth && nisSrcHeight == srcHeight &&
	    nisDstWidth == dstWidth && nisDstHeight == dstHeight)
		return true;

	releaseNisTargets();

	nisSrcWidth  = srcWidth;
	nisSrcHeight = srcHeight;
	nisDstWidth  = dstWidth;
	nisDstHeight = dstHeight;

	if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0)
		return false;

	// A pass may not shrink the image and may not scale by more than 2x, so a
	// window bigger than that is reached in steps of at most 2x.
	int w    = srcWidth;
	int h    = srcHeight;
	int pass = 0;

	while ((w < dstWidth || h < dstHeight) && pass < NIS_MAX_PASSES)
	{
		if (w < dstWidth)
			w = (w * 2 < dstWidth) ? w * 2 : dstWidth;
		if (h < dstHeight)
			h = (h * 2 < dstHeight) ? h * 2 : dstHeight;

		D3D11_TEXTURE2D_DESC td;
		ZeroMemory(&td, sizeof(td));
		td.Width            = w;
		td.Height           = h;
		td.MipLevels        = 1;
		td.ArraySize        = 1;
		td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage            = D3D11_USAGE_DEFAULT;
		td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		if (FAILED(pDevice->CreateTexture2D(&td, NULL, &nisTargets[pass].texture)) ||
		    FAILED(pDevice->CreateShaderResourceView(nisTargets[pass].texture, NULL, &nisTargets[pass].srv)) ||
		    FAILED(pDevice->CreateUnorderedAccessView(nisTargets[pass].texture, NULL, &nisTargets[pass].uav)))
		{
			winlog(_T("NIS: cannot create the %dx%d pass target\n"), w, h);
			releaseNisTargets();
			return false;
		}

		nisTargets[pass].width  = w;
		nisTargets[pass].height = h;
		pass++;
	}

	if (w != dstWidth || h != dstHeight)
	{
		// The back buffer is not reachable within NIS_MAX_PASSES, or it is
		// smaller than the frame, which NIS cannot scale down to.
		releaseNisTargets();
		return false;
	}

	nisPassCount = pass;
	return pass > 0;
}

ID3D11ShaderResourceView *Direct3D11Display::scaleFrameWithNis()
{
	if (!nisAvailable || pNisCS == NULL || pNisCB == NULL || pFrameSRV == NULL)
		return NULL;

	if (theApp.upscaler != UPSCALER_NIS)
		return NULL;

	// The chain maps the whole frame texture onto the whole back buffer, the
	// same mapping the textured quad would use.
	if (theApp.rect.right != width || theApp.rect.bottom != height)
		return NULL;

	int targetWidth, targetHeight;
	upscalerTargetSize(width, height, backWidth, backHeight,
	                   theApp.upscalerMaxScale, &targetWidth, &targetHeight);

	if (!updateNisChain(width, height, targetWidth, targetHeight))
		return NULL;

	const float sharpness = theApp.upscalerSharpness / 100.0f;

	ID3D11ShaderResourceView *source = pFrameSRV;
	int srcWidth  = width;
	int srcHeight = height;

	for (int i = 0; i < nisPassCount; i++)
	{
		NisTarget& target = nisTargets[i];

		NISConfig config;
		ZeroMemory(&config, sizeof(config));
		if (!NVScalerUpdateConfig(config, sharpness,
		                          0, 0, srcWidth, srcHeight, srcWidth, srcHeight,
		                          0, 0, target.width, target.height,
		                          target.width, target.height, NISHDRMode::None))
		{
			return NULL;
		}

		if (!writeUpscalerConstants(pContext, pNisCB, &config, sizeof(config)))
			return NULL;

		pContext->CSSetShader(pNisCS, NULL, 0);
		pContext->CSSetConstantBuffers(0, 1, &pNisCB);
		pContext->CSSetSamplers(0, 1, &pSamplerLinear);
		pContext->CSSetShaderResources(0, 1, &source);
		pContext->CSSetShaderResources(1, 1, &pNisCoefScaleSRV);
		pContext->CSSetShaderResources(2, 1, &pNisCoefUsmSRV);
		pContext->CSSetUnorderedAccessViews(0, 1, &target.uav, NULL);

		pContext->Dispatch((target.width + NIS_BLOCK_WIDTH - 1) / NIS_BLOCK_WIDTH,
		                   (target.height + NIS_BLOCK_HEIGHT - 1) / NIS_BLOCK_HEIGHT, 1);

		// Unbind so the next pass, and the quad draw, can read the result.
		ID3D11UnorderedAccessView *nullUav    = NULL;
		ID3D11ShaderResourceView  *nullSrv[3] = { NULL, NULL, NULL };
		pContext->CSSetUnorderedAccessViews(0, 1, &nullUav, NULL);
		pContext->CSSetShaderResources(0, 3, nullSrv);

		source    = target.srv;
		srcWidth  = target.width;
		srcHeight = target.height;
	}

	return source;
}

// --- AMD FidelityFX Super Resolution 1.0 ----------------------------------
//
// EASU reconstructs the frame at an arbitrary ratio and RCAS sharpens the
// result; both are pixel shaders, so the frame is drawn twice through the
// full screen quad with one round trip through a window sized target in
// between.  The constants they are given are in upscalers/UpscalerFsr.h, which
// the OpenGL 3.3 backend fills with the same function.

bool Direct3D11Display::initFsr()
{
	if (fsrAvailable)
		return true;

	if (pDevice == NULL)
		return false;

	if (featureLevel < D3D_FEATURE_LEVEL_11_0)
	{
		winlog(_T("FSR: needs Direct3D feature level 11.0\n"));
		return false;
	}

	UpscalerIncludeHandler includeHandler;

	pFsrEasuPS = createUpscalerPixelShader(pDevice, "FSR_SHADER_MAIN", "FSR_Main.hlsl",
	                                       "FSR_Easu", "ps_5_0", &includeHandler);
	pFsrRcasPS = createUpscalerPixelShader(pDevice, "FSR_SHADER_MAIN", "FSR_Main.hlsl",
	                                       "FSR_Rcas", "ps_5_0", &includeHandler);
	if (pFsrEasuPS == NULL || pFsrRcasPS == NULL)
	{
		releaseFsr();
		return false;
	}

	if (!createUpscalerConstants(pDevice, FSR_CONSTANT_FLOATS * sizeof(float), &pFsrCB))
	{
		winlog(_T("FSR: cannot create the constant buffer\n"));
		releaseFsr();
		return false;
	}

	fsrAvailable = true;
	return true;
}

void Direct3D11Display::releaseFsrTargets()
{
	for (int i = 0; i < FSR_PASS_COUNT; i++)
		releaseUpscaleTarget(fsrTargets[i]);
}

void Direct3D11Display::releaseFsr()
{
	releaseFsrTargets();

	if (pFsrCB)     { pFsrCB->Release();     pFsrCB     = NULL; }
	if (pFsrRcasPS) { pFsrRcasPS->Release(); pFsrRcasPS = NULL; }
	if (pFsrEasuPS) { pFsrEasuPS->Release(); pFsrEasuPS = NULL; }
	fsrAvailable = false;
}

bool Direct3D11Display::updateFsrTargets(int dstWidth, int dstHeight)
{
	for (int i = 0; i < FSR_PASS_COUNT; i++)
		if (!updateUpscaleTarget(pDevice, fsrTargets[i], dstWidth, dstHeight, "FSR"))
		{
			releaseFsrTargets();
			return false;
		}

	return true;
}

ID3D11ShaderResourceView *Direct3D11Display::scaleFrameWithFsr()
{
	if (!fsrAvailable || pFsrEasuPS == NULL || pFsrRcasPS == NULL || pFsrCB == NULL ||
	    pFrameSRV == NULL)
		return NULL;

	if (theApp.upscaler != UPSCALER_FSR)
		return NULL;

	// The passes map the whole frame texture onto the whole back buffer, the
	// same mapping the textured quad would use.
	if (theApp.rect.right != width || theApp.rect.bottom != height)
		return NULL;

	int targetWidth, targetHeight;
	upscalerTargetSize(width, height, backWidth, backHeight,
	                   theApp.upscalerMaxScale, &targetWidth, &targetHeight);

	if (!updateFsrTargets(targetWidth, targetHeight))
		return NULL;

	// The constants are the floats the shaders read as the bits of uint4s, plus
	// the size of the pass input, which they read as integers.
	float constants[FSR_CONSTANT_FLOATS];
	fsrWriteConstants(constants, (float)width, (float)height,
	                  (float)targetWidth, (float)targetHeight, theApp.upscalerSharpness);

	if (!writeUpscalerConstants(pContext, pFsrCB, constants, sizeof(constants)))
		return NULL;

	D3D11VERTEX fullQuad[4];
	setFullscreenQuad(fullQuad);

	ID3D11ShaderResourceView *source = pFrameSRV;

	for (int pass = 0; pass < FSR_PASS_COUNT; pass++)
	{
		UpscaleTarget& target = fsrTargets[pass];

		ID3D11RenderTargetView *rtv = target.rtv;
		pContext->OMSetRenderTargets(1, &rtv, NULL);

		D3D11_VIEWPORT vp;
		vp.TopLeftX = 0.0f;
		vp.TopLeftY = 0.0f;
		vp.Width    = (float)target.width;
		vp.Height   = (float)target.height;
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		pContext->RSSetViewports(1, &vp);

		// The first pass reconstructs the frame, the second one sharpens it.
		ID3D11PixelShader *shader = (pass == 0) ? pFsrEasuPS : pFsrRcasPS;

		pContext->PSSetConstantBuffers(0, 1, &pFsrCB);
		drawQuad(source, fullQuad, false, shader);

		// The next pass reads what this one wrote, so drop the binding first.
		ID3D11ShaderResourceView *nullView = NULL;
		pContext->PSSetShaderResources(0, 1, &nullView);

		source = target.srv;
	}

	return source;
}

// --- The pixel filter shaders -------------------------------------------
//
// xBRZ and everything else added to the table below: pixel shaders that filter
// the frame rather than reconstruct it, drawn one after the other.  The passes
// map the whole of their input onto the whole of their target, the same mapping
// the textured quad would use, so an upscaler stacks with the CPU pixel filter
// and with the sharpening the way the two SDK upscalers above do.
//
// Most of them are a single pass.  ScaleFX is not: four of its passes work on
// the frame itself, at the size the emulation thread produced it, and the fifth
// interpolates the edge at whatever size the target has - which is why a pass
// either takes the size of the target or a multiple of the frame.
//
// Adding one is the row below, its source as a resource in vba.rc, a
// UPSCALER_* value and a menu item: no code, and nothing else in the renderer
// has to know about it.  The shaders themselves are src/win32/upscalers.

// The most passes any of them has; ScaleFX has five.
#define UPSCALER_MAX_PASSES 5

struct UpscalerPassSource
{
	const char *resource; // the RCDATA resource holding its source
	const char *entry;    // the entry point of its pixel shader
	int         scale;    // 0: the size of the target, n: n times the frame
	int         second;   // the texture it reads besides the one the pass before
	                      // it made, as an index of the outputs: 0 is the frame,
	                      // n the target of pass n - 1, and -1 that it reads one
	                      // texture only.  That is how a libretro shader names
	                      // OrigTexture, and the PassPrev<N> that reaches past
	                      // the start of the chain.
};

struct UpscalerSource
{
	int                        value;     // the UPSCALER_* value it implements
	const char                *name;      // for the trace log and the screen message
	const UpscalerPassSource  *passes;
	int                        passCount;
	const char                *profile;   // the shader model they are compiled for
};

static const UpscalerPassSource g_xbrzPasses[] =
{
	{ "XBRZ_SHADER_FREESCALE", "main", 0, -1 },
};

static const UpscalerPassSource g_xbrLv2Passes[] =
{
	{ "XBR_LV2_SHADER", "main", 0, -1 },
};

static const UpscalerPassSource g_xbrLv2NoBlendPasses[] =
{
	{ "XBR_LV2_NOBLEND_SHADER", "main", 0, -1 },
};

static const UpscalerPassSource g_xbrLv3Passes[] =
{
	{ "XBR_LV3_SHADER", "main", 0, -1 },
};

// ScaleFX: the metrics of the edge, then the subpixels.  The pass that
// resolves the junctions reads the metrics of pass 0 as well as the output of
// pass 1, and the last one draws its colours from the frame itself.
static const UpscalerPassSource g_scalefxPasses[] =
{
	{ "SCALEFX_PASS0_SHADER", "main", 1, -1 },
	{ "SCALEFX_PASS1_SHADER", "main", 1, -1 },
	{ "SCALEFX_PASS2_SHADER", "main", 1,  1 },
	{ "SCALEFX_PASS3_SHADER", "main", 1, -1 },
	{ "SCALEFX_PASS4_SHADER", "main", 0,  0 },
};

// xBR lv3 in the two passes of its preset: the edge data, then the
// interpolation, which blends it with the frame.
static const UpscalerPassSource g_xbrLv3MultipassPasses[] =
{
	{ "XBR_LV3_PASS0_SHADER", "main", 1, -1 },
	{ "XBR_LV3_PASS1_SHADER", "main", 0,  0 },
};

// 2xBR: the flags of every pixel, then the blend of the frame with itself.
static const UpscalerPassSource g_xbr2xPasses[] =
{
	{ "XBR_2X_PASS0_SHADER", "main", 1, -1 },
	{ "XBR_2X_PASS1_SHADER", "main", 0,  0 },
};

// xBR-mlv4: two frames of metrics, the picture doubled, the interpolation.
static const UpscalerPassSource g_xbrMlv4Passes[] =
{
	{ "XBR_MLV4_PASS1_SHADER", "main", 1, -1 },
	{ "XBR_MLV4_PASS2_SHADER", "main", 1,  0 },
	{ "XBR_MLV4_PASS3_SHADER", "main", 2,  0 },
	{ "XBR_MLV4_PASS4_SHADER", "main", 0,  0 },
};

// super-xBR: the edge data, the doubled picture blended with the frame, and
// the jinc filter that finishes it.
static const UpscalerPassSource g_superXbrPasses[] =
{
	{ "SUPER_XBR_PASS0_SHADER", "main", 1, -1 },
	{ "SUPER_XBR_PASS1_SHADER", "main", 2,  0 },
	{ "SUPER_XBR_JINC2_SHADER", "main", 0, -1 },
};

// The one pass of the transparency filter reads the frame twice: the pixels it
// draws and the ones it leaves alone.
static const UpscalerPassSource g_ptSkyWalkerPasses[] =
{
	{ "PT_SKYWALKER541_SHADER", "main", 0, 0 },
};

// Lanczos is a resampler rather than a pixel filter: three taps per axis at
// texel centres, over the whole window.
static const UpscalerPassSource g_lanczos3Passes[] =
{
	{ "LANCZOS3_SHADER", "main", 0, -1 },
};

// Bicubic is the other resampler: four taps per axis at texel centres, the
// Mitchell-Netravali kernel of the cubic folder over the whole window.
static const UpscalerPassSource g_bicubicPasses[] =
{
	{ "BICUBIC_SHADER", "main", 0, -1 },
};

static const UpscalerSource g_upscalers[] =
{
	{ UPSCALER_BICUBIC,           "Bicubic",           g_bicubicPasses,          1, "ps_4_0" },
	{ UPSCALER_XBR_LV2,           "xBR lv2",           g_xbrLv2Passes,           1, "ps_4_0" },
	{ UPSCALER_XBR_LV2_NOBLEND,   "xBR lv2 no-blend",  g_xbrLv2NoBlendPasses,    1, "ps_4_0" },
	{ UPSCALER_XBR_LV3,           "xBR lv3",           g_xbrLv3Passes,           1, "ps_4_0" },
	{ UPSCALER_XBR_LV3_MULTIPASS, "xBR lv3 multipass", g_xbrLv3MultipassPasses,  2, "ps_4_0" },
	{ UPSCALER_XBR_2X,            "2xBR",              g_xbr2xPasses,            2, "ps_4_0" },
	{ UPSCALER_XBR_MLV4,          "xBR-mlv4",          g_xbrMlv4Passes,          4, "ps_4_0" },
	{ UPSCALER_XBRZ,              "xBRZ",              g_xbrzPasses,             1, "ps_4_0" },
	{ UPSCALER_SUPER_XBR,         "super-xBR",         g_superXbrPasses,         3, "ps_4_0" },
	{ UPSCALER_SCALEFX,           "ScaleFX",           g_scalefxPasses,          5, "ps_4_0" },
	{ UPSCALER_PT_SKYWALKER,      "PT SkyWalker541",   g_ptSkyWalkerPasses,      1, "ps_4_0" },
	{ UPSCALER_LANCZOS3,          "Lanczos",           g_lanczos3Passes,         1, "ps_4_0" },
};

#define UPSCALER_COUNT (sizeof(g_upscalers) / sizeof(g_upscalers[0]))

// The block every one of them is handed, and the one their sources declare:
//
//   float4 SourceSize;  // xy: the size the pass reads, zw: 1 / xy, one texel
//   float4 OutputSize;  // xy: the size of its target, zw: 1 / xy
//   float4 Scale;       // xy: OutputSize.xy / SourceSize.xy
//   float4 SecondSize;  // xy: the size of the texture at t1, zw: 1 / xy
//
// They are sampled with the point sampler, so their taps land on texel centres.
// A shader that needs anything more is a pipeline of its own, like NIS.
struct SinglePassConstants
{
	float sourceSize[4];
	float outputSize[4];
	float scale[4];
	float secondSize[4];
};

struct UpscalerPass
{
	ID3D11PixelShader *shader;
	ID3D11Buffer      *constants;
	UpscaleTarget      target;
};

// What the running renderer built out of the table above.  There is never more
// than one display at a time, and the upscaling menu asks about the shaders in
// between two of them, so they live here rather than in the display object.
struct Upscaler
{
	UpscalerPass passes[UPSCALER_MAX_PASSES];
	bool         attempted; // compiled once, whether or not it worked
};

static Upscaler g_upscalerState[UPSCALER_COUNT];

// The row of the table above that implements `value`, or -1.
static int upscalerIndex(int value)
{
	for (UINT i = 0; i < UPSCALER_COUNT; i++)
		if (g_upscalers[i].value == value)
			return (int)i;

	return -1;
}

// False for an upscaler whose shader did not compile on this device, which is
// how the upscaling menu leaves it out.  A chain that has not been built yet is
// one this renderer runs: the shaders are compiled when they are selected, and
// the menu has to offer them before that.  The two SDK upscalers are built on
// demand as well, so there is nothing to answer for them, and the chains that
// only the OpenGL backend runs are the ones this table has no row for.
bool direct3D11UpscalerAvailable(int upscaler)
{
	int index = upscalerIndex(upscaler);

	if (index >= 0)
	{
		const Upscaler &state = g_upscalerState[index];

		return state.passes[0].shader != NULL || !state.attempted;
	}

	return upscaler == UPSCALER_OFF || upscaler == UPSCALER_NIS || upscaler == UPSCALER_FSR;
}

// The name of an upscaler the way the table spells it, or NULL.
const char *direct3D11UpscalerName(int upscaler)
{
	int index = upscalerIndex(upscaler);

	return index >= 0 ? g_upscalers[index].name : NULL;
}

static void releaseUpscaler(Upscaler &upscaler)
{
	for (int pass = 0; pass < UPSCALER_MAX_PASSES; pass++)
	{
		releaseUpscaleTarget(upscaler.passes[pass].target);

		if (upscaler.passes[pass].constants) { upscaler.passes[pass].constants->Release(); upscaler.passes[pass].constants = NULL; }
		if (upscaler.passes[pass].shader)    { upscaler.passes[pass].shader->Release();    upscaler.passes[pass].shader    = NULL; }
	}
}

// Builds the chain the user selected, and only that one.  Compiling all of
// them while the renderer starts is what held up the start of the program; the
// rest are built here when the menu selects them.
void Direct3D11Display::initUpscaler(int upscaler)
{
	int index = upscalerIndex(upscaler);

	// Off, the two SDK upscalers and the chains only the OpenGL backend runs
	// have no row in the table, so there is nothing to compile.
	if (index < 0)
		return;

	const UpscalerSource &source = g_upscalers[index];
	Upscaler             &state  = g_upscalerState[index];

	if (state.passes[0].shader != NULL || state.attempted)
		return;

	state.attempted = true;

	bool built = true;

	for (int pass = 0; pass < source.passCount && built; pass++)
	{
		UpscalerPass &statePass = state.passes[pass];

		statePass.shader = createUpscalerPixelShader(pDevice, source.passes[pass].resource,
		                                             source.name, source.passes[pass].entry,
		                                             source.profile);
		if (statePass.shader == NULL)
		{
			built = false;
			break;
		}

		if (!createUpscalerConstants(pDevice, sizeof(SinglePassConstants), &statePass.constants))
		{
			winlog(_T("%hs: cannot create the constant buffer\n"), source.name);
			built = false;
		}
	}

	if (!built)
	{
		winlog(_T("D3D11: the %hs upscaler is not available on this device\n"), source.name);
		releaseUpscaler(state);
	}
	else
		winlog(_T("D3D11: the %hs upscaler is ready, %d pass(es)\n"), source.name, source.passCount);
}

void Direct3D11Display::releaseUpscalers()
{
	for (UINT i = 0; i < UPSCALER_COUNT; i++)
	{
		releaseUpscaler(g_upscalerState[i]);
		g_upscalerState[i].attempted = false;
	}
}

ID3D11ShaderResourceView *Direct3D11Display::scaleFrameWithUpscaler(int upscaler)
{
	// The caller draws the frame itself when this returns NULL, so why it did
	// is logged - once per reason rather than once per frame.
	static int lastReason = 0;

	int index = upscalerIndex(upscaler);
	if (index < 0)
	{
		if (lastReason != 1)
			winlog(_T("D3D11: upscaler %d has no chain here, the frame is drawn as it is\n"), upscaler);
		lastReason = 1;
		return NULL;
	}

	const UpscalerSource &source = g_upscalers[index];
	Upscaler             &state  = g_upscalerState[index];

	if (state.passes[0].shader == NULL || pFrameSRV == NULL)
	{
		if (lastReason != 2)
			winlog(_T("D3D11: %hs is not built, the frame is drawn as it is\n"), source.name);
		lastReason = 2;
		return NULL;
	}

	// The pass maps the whole frame texture onto the whole back buffer, the
	// same mapping the textured quad would use.
	if (theApp.rect.right != width || theApp.rect.bottom != height)
	{
		if (lastReason != 3)
			winlog(_T("D3D11: %hs: the frame is %dx%d at %d,%d of a %dx%d texture, which it cannot scale\n"),
			       source.name, theApp.rect.right - theApp.rect.left, theApp.rect.bottom - theApp.rect.top,
			       theApp.rect.left, theApp.rect.top, width, height);
		lastReason = 3;
		return NULL;
	}

	lastReason = 0;

	int targetWidth, targetHeight;
	upscalerTargetSize(width, height, backWidth, backHeight,
	                   theApp.upscalerMaxScale, &targetWidth, &targetHeight);

	for (int pass = 0; pass < source.passCount; pass++)
	{
		// A fixed scale is a multiple of the frame rather than of the pass
		// before it, the way the presets of the libretro shaders ask for it.
		int passWidth  = source.passes[pass].scale > 0 ? width  * source.passes[pass].scale : targetWidth;
		int passHeight = source.passes[pass].scale > 0 ? height * source.passes[pass].scale : targetHeight;

		if (!updateUpscaleTarget(pDevice, state.passes[pass].target, passWidth, passHeight, source.name))
			return NULL;
	}

	D3D11VERTEX fullQuad[4];
	setFullscreenQuad(fullQuad);

	// outputs[0] is the frame and outputs[n + 1] the target of pass n, which
	// is how a pass names what it reads: the pass before it, or, for the ones
	// that ask for an earlier pass, that pass.
	ID3D11ShaderResourceView *outputs[UPSCALER_MAX_PASSES + 1];
	outputs[0] = pFrameSRV;

	int inputWidth  = width;
	int inputHeight = height;

	for (int pass = 0; pass < source.passCount; pass++)
	{
		UpscalerPass &statePass = state.passes[pass];

		ID3D11ShaderResourceView *input  = outputs[pass];
		ID3D11ShaderResourceView *second = NULL;

		if (source.passes[pass].second >= 0)
			second = outputs[source.passes[pass].second];

		// The size of that second texture, which the shaders of the chains
		// divide by: the frame for the ones that name OrigTexture, the target
		// of an earlier pass for the ones that name PassPrev<N>Texture.
		int secondWidth  = width;
		int secondHeight = height;

		if (source.passes[pass].second > 0)
		{
			int earlier = source.passes[pass].second - 1;

			secondWidth  = state.passes[earlier].target.width;
			secondHeight = state.passes[earlier].target.height;
		}

		SinglePassConstants constants;
		constants.sourceSize[0] = (float)inputWidth;
		constants.sourceSize[1] = (float)inputHeight;
		constants.sourceSize[2] = 1.0f / (float)inputWidth;
		constants.sourceSize[3] = 1.0f / (float)inputHeight;
		constants.outputSize[0] = (float)statePass.target.width;
		constants.outputSize[1] = (float)statePass.target.height;
		constants.outputSize[2] = 1.0f / (float)statePass.target.width;
		constants.outputSize[3] = 1.0f / (float)statePass.target.height;
		constants.scale[0]      = (float)statePass.target.width  / (float)inputWidth;
		constants.scale[1]      = (float)statePass.target.height / (float)inputHeight;
		constants.scale[2]      = 0.0f;
		constants.scale[3]      = 0.0f;
		constants.secondSize[0] = (float)secondWidth;
		constants.secondSize[1] = (float)secondHeight;
		constants.secondSize[2] = 1.0f / (float)secondWidth;
		constants.secondSize[3] = 1.0f / (float)secondHeight;

		if (!writeUpscalerConstants(pContext, statePass.constants, &constants, sizeof(constants)))
			return NULL;

		ID3D11RenderTargetView *rtv = statePass.target.rtv;
		pContext->OMSetRenderTargets(1, &rtv, NULL);

		D3D11_VIEWPORT vp;
		vp.TopLeftX = 0.0f;
		vp.TopLeftY = 0.0f;
		vp.Width    = (float)statePass.target.width;
		vp.Height   = (float)statePass.target.height;
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		pContext->RSSetViewports(1, &vp);

		pContext->PSSetConstantBuffers(0, 1, &statePass.constants);

		// The taps have to land on texel centres.
		int savedFilter = filterMode;
		filterMode = 0;

		// drawQuad() binds the texture of the pass at slot 0 and its sampler,
		// and leaves slot 1 alone, which is where the ones that read a second
		// one find it.
		if (second != NULL)
		{
			pContext->PSSetShaderResources(1, 1, &second);
			pContext->PSSetSamplers(1, 1, &pSamplerPoint);
		}

		drawQuad(input, fullQuad, false, statePass.shader);
		filterMode = savedFilter;

		// The next pass, or the caller, draws what this produced, so drop the
		// shader resource bindings it would read through.
		ID3D11ShaderResourceView *nullViews[2] = { NULL, NULL };
		pContext->PSSetShaderResources(0, 2, nullViews);

		outputs[pass + 1] = statePass.target.srv;
		inputWidth  = statePass.target.width;
		inputHeight = statePass.target.height;
	}

	return state.passes[source.passCount - 1].target.srv;
}

void Direct3D11Display::clear()
{
        if (pDevice == NULL || pContext == NULL || pSwapChain == NULL || pRTV == NULL)
                return;

        // A flip model swap chain owns the window's client area, so erasing or
        // painting the window does not change what is on screen; only a
        // present does.  Without this the last frame of a closed ROM stays
        // visible until something draws again.
        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        pContext->OMSetRenderTargets(1, &pRTV, NULL);
        pContext->ClearRenderTargetView(pRTV, clearColor);

        // In the flip model the back buffer that was just cleared is the one
        // the swap chain presents, and the view has to be rebuilt for the new
        // one afterwards, which render() does on its next frame.
        pSwapChain->Present(0, 0);
}

void Direct3D11Display::calculateVertices()
{
	float left   = 0.0f;
	float top    = 0.0f;
	float right  = (float)backWidth;
	float bottom = (float)backHeight;

	// Convert to normalised device coordinates.
	float l = (left   / (float)backWidth)  * 2.0f - 1.0f;
	float r = (right  / (float)backWidth)  * 2.0f - 1.0f;
	float t = 1.0f - (top    / (float)backHeight) * 2.0f;
	float b = 1.0f - (bottom / (float)backHeight) * 2.0f;

	float u = (float)theApp.rect.right / (float)width;
	float v = (float)theApp.rect.bottom / (float)height;

	quad[0].x = l; quad[0].y = b; quad[0].u = 0.0f; quad[0].v = v;
	quad[1].x = l; quad[1].y = t; quad[1].u = 0.0f; quad[1].v = 0.0f;
	quad[2].x = r; quad[2].y = b; quad[2].u = u;    quad[2].v = v;
	quad[3].x = r; quad[3].y = t; quad[3].u = u;    quad[3].v = 0.0f;
}

void Direct3D11Display::drawQuad(ID3D11ShaderResourceView *srv, D3D11VERTEX *vertices,
                                 bool blend, ID3D11PixelShader *pixelShader)
{
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(pContext->Map(pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return;
	memcpy(mapped.pData, vertices, sizeof(D3D11VERTEX) * 4);
	pContext->Unmap(pVertexBuffer, 0);

	UINT stride = sizeof(D3D11VERTEX);
	UINT offset = 0;
	pContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &stride, &offset);
	pContext->IASetInputLayout(pLayout);
	pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	pContext->VSSetShader(pVS, NULL, 0);
	pContext->PSSetShader(pixelShader ? pixelShader : pPS, NULL, 0);

	ID3D11SamplerState *sampler = pSamplerPoint;
	if (filterMode == 1)
		sampler = pSamplerLinear;
	else if (filterMode == 2 && pSamplerTrilinear != NULL)
		sampler = pSamplerTrilinear;
	pContext->PSSetSamplers(0, 1, &sampler);
	pContext->PSSetShaderResources(0, 1, &srv);

	float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	pContext->OMSetBlendState(blend ? pBlendAlpha : NULL, blendFactor, 0xFFFFFFFF);

	pContext->Draw(4, 0);
}

void Direct3D11Display::render()
{
	if (pDevice == NULL || pContext == NULL || pSwapChain == NULL)
		return;

	if (pDevice->GetDeviceRemovedReason() != S_OK)
		return;

	if (pFrameTexture == NULL && (theApp.rect.right > 0))
	{
		if (!initializeOffscreen(theApp.rect.right, theApp.rect.bottom))
			return;
		calculateVertices();
	}

	u8 *data = pix;
	int dataPitch = theApp.filterWidth * (systemColorDepth / 8) + (systemColorDepth == 24 ? 0 : 4);

	if (textMethod != 0) // do not draw Lua HUD to a video dump
	{
		systemClonePixBuffer(osd);
		data = osd;
		systemRenderLua((u8 *)data, dataPitch);
	}

	if (pFrameTexture != NULL)
	{
		D3D11_MAPPED_SUBRESOURCE mapped;
		if (SUCCEEDED(pContext->Map(pFrameTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		{
			if (theApp.filterFunction)
			{
				theApp.filterFunction(data + dataPitch,
				                      dataPitch,
				                      (u8 *)theApp.delta,
				                      (u8 *)mapped.pData,
				                      mapped.RowPitch,
				                      theApp.filterWidth,
				                      theApp.filterHeight);
			}
			else
			{
				int copyX, copyY;
				systemGetLCDResolution(copyX, copyY);

				int bytesPerPixel = systemColorDepth / 8;
				int srcPitch      = dataPitch;
				const u8 *srcRow  = data + srcPitch;
				u8 *dstRow        = (u8 *)mapped.pData;
				int rowBytes      = copyX * bytesPerPixel;
				if (rowBytes > (int)mapped.RowPitch)
					rowBytes = (int)mapped.RowPitch;

				for (int y = 0; y < copyY; y++)
				{
					memcpy(dstRow, srcRow, rowBytes);
					srcRow += srcPitch;
					dstRow += mapped.RowPitch;
				}
			}

			if (isFullscreenOption(theApp.videoOption) && theApp.showSpeed)
			{
				char buffer[30];
				if (theApp.showSpeed == 1)
					sprintf(buffer, "%3d%%", systemSpeed);
				else
					sprintf(buffer, "%3d%%(%d, %d fps)", systemSpeed,
					        systemFrameSkip, theApp.showRenderedFrames);
				if (theApp.showSpeedTransparent)
					drawTextTransp((u8 *)mapped.pData, mapped.RowPitch,
					               theApp.rect.left + 10, theApp.rect.bottom - 10, buffer);
				else
					drawText((u8 *)mapped.pData, mapped.RowPitch,
					         theApp.rect.left + 10, theApp.rect.bottom - 10, buffer);
			}

			if (textMethod == 1)
			{
				DrawTextMessages((u8 *)mapped.pData, mapped.RowPitch,
				                 theApp.rect.left, theApp.rect.bottom);
			}

			pContext->Unmap(pFrameTexture, 0);
		}
	}

	// The trilinear filter reads a mip chain, so the frame is copied into the
	// texture that has one and the device builds the chain from the copy.  The
	// chain covers the whole frame texture, which is exactly the frame, so
	// nothing of it is left over from an earlier one.
	bool trilinear = filterMode == 2 && pMipTexture != NULL && pMipSRV != NULL;
	if (trilinear && pFrameTexture != NULL)
	{
		// Level 0 only: the levels below it are what GenerateMips() makes.
		pContext->CopySubresourceRegion(pMipTexture, 0, 0, 0, 0, pFrameTexture, 0, NULL);
		pContext->GenerateMips(pMipSRV);
	}

	// Optionally let an upscaler rebuild the frame at the back buffer size
	// instead of letting the sampler stretch it.
	ID3D11ShaderResourceView *frameView = trilinear ? pMipSRV : pFrameSRV;
	bool scaled1to1 = false;

	if (pFrameSRV != NULL && theApp.upscaler != UPSCALER_OFF)
	{
		ID3D11ShaderResourceView *scaled = NULL;

		if (theApp.upscaler == UPSCALER_NIS)
			scaled = scaleFrameWithNis();
		else if (theApp.upscaler == UPSCALER_FSR)
			scaled = scaleFrameWithFsr();
		else
			// Everything else is one of the pixel filter shaders of the table
			// above, if it is one of ours at all.
			scaled = scaleFrameWithUpscaler(theApp.upscaler);

		if (scaled != NULL)
		{
			// An upscaler that was capped renders smaller than the back buffer
			// and the quad stretches the rest, so it is not a 1:1 blit.
			int scaledWidth, scaledHeight;
			upscalerTargetSize(width, height, backWidth, backHeight,
			                   theApp.upscalerMaxScale, &scaledWidth, &scaledHeight);

			frameView  = scaled;
			scaled1to1 = scaledWidth == backWidth && scaledHeight == backHeight;
		}
	}

        // A flip model swap chain hands a different back buffer over with every
        // Present.  Only buffer 0 can be queried and it is the current one again
        // after each Present, so the render target view is rebuilt for it here.
        // A bitblt swap chain keeps its buffer, but the view can still be
        // missing after a failed resize, so that case rebuilds it as well.
        if (pRTV == NULL || flipModel)
        {
                if (!createBackBufferViews())
                        return;
        }

	const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	pContext->OMSetRenderTargets(1, &pRTV, NULL);
	pContext->ClearRenderTargetView(pRTV, clearColor);

	D3D11_VIEWPORT vp;
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width    = (float)backWidth;
	vp.Height   = (float)backHeight;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	pContext->RSSetViewports(1, &vp);

	if (frameView != NULL)
	{
		if (scaled1to1)
		{
			// The upscaled frame covers exactly the back buffer, so it is
			// drawn 1:1.
			D3D11VERTEX fullQuad[4];
			setFullscreenQuad(fullQuad);

			int savedFilter = filterMode;
			filterMode = 0; // 1:1, no sampling needed
			drawQuad(frameView, fullQuad, false);
			filterMode = savedFilter;
		}
		else
		{
			drawQuad(frameView, quad, false);
		}
	}

	// The overlay follows the back buffer, so it is made with the swap chain and
	// remade whenever the window changes size.  This covers a path that
	// recreated one of them without the other.
	if (textMethod == 2 && pOverlayTexture == NULL && backWidth > 0 && backHeight > 0)
		createOverlayTarget(backWidth, backHeight);

	if (textMethod == 2)
	{
		// The on screen display is the one path whose behaviour depends on the
		// backend, so the trace log says which branch was taken, once per
		// change rather than once per frame.
		int state;
		if (pOverlayTexture == NULL || overlay.empty())
			state = 0;
		else if (!overlay.update())
			state = 1;
		else
			state = 2;

		static int tracedState = -1;
		if (state != tracedState)
		{
			static const char *const names[] = { "no target", "nothing visible", "visible" };
			winlog(_T("D3D11: on screen display %hs\n"), names[state]);
			tracedState = state;
		}

		if (state == 2)
		{
			// The overlay is the size of the back buffer and only changes when
			// a message does, so it is uploaded again only then; the texture
			// still holds the last picture.
			if (overlay.changed())
			{
				D3D11_MAPPED_SUBRESOURCE mapped;
				if (SUCCEEDED(pContext->Map(pOverlayTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
				{
					const unsigned char *src = overlay.pixels();
					int srcPitch = overlay.pitch();
					int copyBytes = overlay.width() * 4;
					if (copyBytes > (int)mapped.RowPitch)
						copyBytes = (int)mapped.RowPitch;
					for (int y = 0; y < overlay.height(); y++)
						memcpy((unsigned char *)mapped.pData + y * mapped.RowPitch,
						       src + y * srcPitch, copyBytes);
					pContext->Unmap(pOverlayTexture, 0);

					// The frame counter is a message like any other and it
					// changes every frame, so this is only worth saying a few
					// times - along with how many pixels of the band the
					// messages are drawn in are not transparent, which is what
					// says whether the text is in the buffer at all.
					static int tracedUploads = 0;
					if (tracedUploads < 3)
					{
						int opaque = 0;
						for (int y = overlay.usedTop(); y < overlay.height(); y++)
						{
							const unsigned char *row = src + y * srcPitch;
							for (int x = 0; x < overlay.width(); x++)
								opaque += row[x * 4 + 3] != 0;
						}

						winlog(_T("D3D11: on screen display uploaded, %dx%d, pitch %d, %d opaque pixels\n"),
						       overlay.width(), overlay.height(), (int)mapped.RowPitch, opaque);
						tracedUploads++;
					}
				}
				else
				{
					static bool tracedFailure = false;
					if (!tracedFailure)
					{
						winlog(_T("D3D11: on screen display cannot be mapped\n"));
						tracedFailure = true;
					}
				}
			}

			// The overlay is a top-down DIB, so its first row is the top of the
			// text and goes to the top of the screen, the same way the frame's
			// first row does in calculateVertices() - and in the same vertex
			// order, bottom left first: the renderer never sets a rasterizer
			// state, so the default one culls back faces and a strip wound the
			// other way loses the triangle that covers the bottom left, which
			// is where the messages are.
			D3D11VERTEX overlayQuad[4];
			overlayQuad[0].x = -1.0f; overlayQuad[0].y = -1.0f; overlayQuad[0].u = 0.0f; overlayQuad[0].v = 1.0f;
			overlayQuad[1].x = -1.0f; overlayQuad[1].y =  1.0f; overlayQuad[1].u = 0.0f; overlayQuad[1].v = 0.0f;
			overlayQuad[2].x =  1.0f; overlayQuad[2].y = -1.0f; overlayQuad[2].u = 1.0f; overlayQuad[2].v = 1.0f;
			overlayQuad[3].x =  1.0f; overlayQuad[3].y =  1.0f; overlayQuad[3].u = 1.0f; overlayQuad[3].v = 0.0f;

			int savedFilter = filterMode;
			filterMode = 0; // point sampling for a 1:1 overlay
			drawQuad(pOverlaySRV, overlayQuad, true);
			filterMode = savedFilter;
		}
	}

	// Nothing must stay bound as a shader resource: the next frame writes the
	// frame texture again and may bind an upscaler target as a UAV or a render
	// target.
	ID3D11ShaderResourceView *nullView = NULL;
	pContext->PSSetShaderResources(0, 1, &nullView);

	// Only wait for the vertical blank when the user asked for it, the same way
	// DirectDraw waits before its flip, see the VSync video option.  The thread
	// that comes through here is the one that also feeds the sound device, so a
	// sync interval of one (a block of a whole vblank per frame) starves the
	// audio buffer and is heard as crackling.
	bool fastForward = speedup || theApp.frameSearchSkipping;
	pSwapChain->Present(theApp.vsync && !fastForward ? 1 : 0, 0);
}

void Direct3D11Display::resize(int w, int h)
{
	if (pSwapChain == NULL || w <= 0 || h <= 0)
		return;

	// The window layout calls this whenever the picture area changes, which
	// includes the frames where it does not: a resize to the size the chain
	// already has would cost a frame and flash the picture black.
	if (w == backWidth && h == backHeight)
		return;

	backWidth  = w;
	backHeight = h;

	if (pRTV)
	{
		pRTV->Release();
		pRTV = NULL;
	}
	pContext->OMSetRenderTargets(0, NULL, NULL);

	// The buffer count has to be the one the swap chain was created with, and a
	// flip model swap chain cannot be resized down to a single buffer.
	HRESULT hr = pSwapChain->ResizeBuffers(flipModel ? 2 : 1, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
	if (SUCCEEDED(hr))
		createBackBufferViews();
	else
		systemMessage(0, "Failed swap chain resize %08x", hr);

	// Recreate the overlay at the new back buffer size.
	createOverlayTarget(w, h);

	calculateVertices();
}

bool Direct3D11Display::changeRenderSize(int w, int h)
{
	if (w != width || h != height)
	{
		if (!initializeOffscreen(w, h))
		{
			failed = true;
			return false;
		}
	}
	calculateVertices();
	return true;
}

void Direct3D11Display::setOption(const char *option, int value)
{
	if (!strcmp(option, "d3dFilter"))
	{
		updateFiltering(value);
	}
	else if (!strcmp(option, "upscaler"))
	{
		// Everything is built on demand: the pixel filter shaders here, and the
		// two SDK upscalers below.  So there is nothing to build while the
		// renderer starts for an upscaler nobody picked, only for the one the
		// menu just selected.
		initUpscaler(value);

		// What was picked is reported in the trace, and on the screen when it
		// is not there.
		const char *picked = direct3D11UpscalerName(value);

		if (picked == NULL)
			picked = value == UPSCALER_NIS ? "NIS"
			       : value == UPSCALER_FSR ? "FSR"
			       : "not one of this renderer's";

		winlog(_T("D3D11: upscaler %hs (%d), sharpening %d, max scale %d%hs\n"),
		       picked, value, theApp.upscalerSharpness, theApp.upscalerMaxScale,
		       direct3D11UpscalerAvailable(value) ? "" : " - this renderer does not run it");

		if (value == UPSCALER_NIS && !nisAvailable && !initNis())
			systemScreenMessage("NVIDIA Image Scaling is not available on this device");
		else if (value == UPSCALER_FSR && !fsrAvailable && !initFsr())
			systemScreenMessage("FidelityFX Super Resolution is not available on this device");
		else if (!direct3D11UpscalerAvailable(value))
		{
			const char *name = direct3D11UpscalerName(value);

			if (name != NULL)
			{
				char message[128];
				sprintf(message, "The %s upscaler is not available on this device", name);
				systemScreenMessage(message);
			}
			else if (value != UPSCALER_OFF)
				systemScreenMessage("That upscaler runs on the OpenGL 3.3 renderer");
		}
	}
}

IDisplay *newDirect3D11Display()
{
	return new Direct3D11Display();
}
