// Wgpu.cpp: wgpu display backend.
//
// The emulator frame is uploaded into a dynamic RGBA texture and drawn as a
// single textured quad, the shape the Direct3D 11 backend has, only through
// the wgpu-native library instead of Direct3D: wgpu selects between Direct3D
// 12, Vulkan and Metal, and the trace log and the render method menu name
// whichever one the running renderer is on, the way the Direct3D 9 backend
// names its device flavor.
//
// The on screen display is rendered by the TextOverlay the D3D11 backend uses
// into a second texture and blended on top, so the two renderers show the same
// thing.  The upscalers are WGSL ports of the libretro shaders and of the
// Lanczos resampler, kept in a table of their own below; upscalerAvailable()
// answers true for those and Off only, which greys the rest of the upscaling
// menu out while this renderer runs, see the upscaling sections in
// Direct3D11.cpp.
//
// wgpu_native.dll is resolved at run time rather than linked, see
// loadWgpuLibrary() below, and nothing else about this backend leans on one
// platform either: x86, x64 and ARM64 all ask for the same entry points, each
// on its own build of the library, and the availability of the menu entry
// follows the library, the way the Direct3D 11 entry follows D3D11.dll.
//
// The desktop is scanned in with its colour depth the way the Direct3D
// backends do it.  Where the core runs 16 bit - a 565 frame - the upload
// widens each pixel into RGBA8, the one format wgpu has for that; the 32 bit
// frame is BGRA in memory and is uploaded as BGRA8Unorm unchanged.
//
// When the library is missing or no adapter comes back the renderer does not
// start, and VBA::updateRenderMethod() falls back to the renderer below it in
// the menu list.
//
// Which of those backends the renderer runs on is the user's to pick, see the
// backend menu in MainWndOptions.cpp and theApp.wgpuBackend: the request the
// adapter comes from asks for that backend, and a backend this machine has no
// adapter for is left out of the menu.  Automatic asks for none in particular
// and lets the library pick, which is Direct3D 12 on Windows.

#include "stdafx.h"
#include "WgpuConfig.h"
#include "Display.h"

#if VBA_ENABLE_WGPU

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#include "resource.h"
#include "MainWnd.h"
#include "Reg.h"
#include "VBA.h"
#include "TextOverlay.h"

#include "wgpu/wgpu.h"

#include "../common/SystemGlobals.h"
#include "../common/Text.h"
#include "../version.h"

extern int systemSpeed;
extern int Init_2xSaI(u32);

// ---------------------------------------------------------------------------
// The webgpu entry points the renderer calls, resolved from wgpu_native.dll.
//
// An executable that imports the library cannot run without it, so the way the
// Direct3D 11 backend loads D3D11.dll carries over: LoadLibrary on demand,
// silently, cached, and every entry point asked for by name.  Where the
// library is all there, the functions are all there too; where it is not,
// wgpuAvailable() is what tells the menu.

// The entry points are named once.  decltype takes each signature from the
// header itself, so a signature that moves in webgpu.h moves with this table
// instead of leaving a second, hand written copy to fall out of step.
#define WGPU_FUNCTIONS(X) \
	X(wgpuCreateInstance) \
	X(wgpuInstanceRequestAdapter) \
	X(wgpuInstanceProcessEvents) \
	X(wgpuInstanceEnumerateAdapters) \
	X(wgpuInstanceCreateSurface) \
	X(wgpuSurfaceGetCapabilities) \
	X(wgpuSurfaceCapabilitiesFreeMembers) \
	X(wgpuSurfaceConfigure) \
	X(wgpuSurfaceUnconfigure) \
	X(wgpuSurfacePresent) \
	X(wgpuSurfaceGetCurrentTexture) \
	X(wgpuSurfaceRelease) \
	X(wgpuAdapterRequestDevice) \
	X(wgpuAdapterGetInfo) \
	X(wgpuAdapterInfoFreeMembers) \
	X(wgpuAdapterRelease) \
	X(wgpuDeviceGetQueue) \
	X(wgpuDeviceCreateShaderModule) \
	X(wgpuDeviceCreateCommandEncoder) \
	X(wgpuDeviceCreateBindGroupLayout) \
	X(wgpuDeviceCreatePipelineLayout) \
	X(wgpuDeviceCreateRenderPipeline) \
	X(wgpuDeviceCreateSampler) \
	X(wgpuDeviceCreateBuffer) \
	X(wgpuDeviceCreateTexture) \
	X(wgpuTextureCreateView) \
	X(wgpuDeviceCreateBindGroup) \
	X(wgpuQueueWriteBuffer) \
	X(wgpuQueueWriteTexture) \
	X(wgpuQueueSubmit) \
	X(wgpuCommandEncoderBeginRenderPass) \
	X(wgpuCommandEncoderFinish) \
	X(wgpuRenderPassEncoderSetPipeline) \
	X(wgpuRenderPassEncoderSetBindGroup) \
	X(wgpuRenderPassEncoderSetVertexBuffer) \
	X(wgpuRenderPassEncoderDraw) \
	X(wgpuRenderPassEncoderEnd) \
	X(wgpuDeviceRelease) \
	X(wgpuDeviceDestroy) \
	X(wgpuQueueRelease) \
	X(wgpuShaderModuleRelease) \
	X(wgpuBindGroupLayoutRelease) \
	X(wgpuPipelineLayoutRelease) \
	X(wgpuRenderPipelineRelease) \
	X(wgpuSamplerRelease) \
	X(wgpuBufferRelease) \
	X(wgpuTextureRelease) \
	X(wgpuTextureViewRelease) \
	X(wgpuBindGroupRelease) \
	X(wgpuCommandEncoderRelease) \
	X(wgpuRenderPassEncoderRelease) \
	X(wgpuCommandBufferRelease) \
	X(wgpuGetVersion) \
	X(wgpuSetLogCallback) \
	X(wgpuSetLogLevel) \
	X(wgpuDevicePoll)

#define WGPU_FUNCTION(name) static decltype(&name) name##_fn = NULL;
	WGPU_FUNCTIONS(WGPU_FUNCTION)
#undef  WGPU_FUNCTION

// The names and the addresses the resolution loop goes over, in the order the
// table above lists them.
static const char *const g_wgpuFunctionNames[] =
{
#define WGPU_NAME_STRING(name) #name,
	WGPU_FUNCTIONS(WGPU_NAME_STRING)
#undef  WGPU_NAME_STRING
};
#define WGPU_FUNCTION_COUNT (sizeof(g_wgpuFunctionNames) / sizeof(g_wgpuFunctionNames[0]))

static void *const g_wgpuFunctionTargets[WGPU_FUNCTION_COUNT] =
{
#define WGPU_TARGET(name) (void **)&name##_fn,
	WGPU_FUNCTIONS(WGPU_TARGET)
#undef  WGPU_TARGET
};

static HINSTANCE wgpuModule = NULL;
static bool      wgpuProbed = false;

// ---------------------------------------------------------------------------
// What the library says about itself, and what it writes while it runs.
//
// A panic inside the library leaves it as a C++ exception, so the caller sees
// that something was thrown but never why: the reason went to the standard
// error stream, and a window program has none.  The stream is given a file
// before the library is loaded, so the message of a panic reads back afterwards
// the way a console build would print it.  The rest of what the library has to
// say - the errors it reports on the device it was asked for, the warnings a
// driver earns - arrives through the log callback and goes into the trace
// instead, because those are answers the renderer is meant to live with.

static void redirectStdErr(const char *fileName)
{
	HANDLE file = CreateFileA(fileName, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                          NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (file == INVALID_HANDLE_VALUE)
		return;

	SetStdHandle(STD_ERROR_HANDLE, file);
}

static const char *wgpuLogLevelName(int level)
{
	switch (level)
	{
	case WGPULogLevel_Error: return "error";
	case WGPULogLevel_Warn:  return "warn";
	case WGPULogLevel_Info:  return "info";
	case WGPULogLevel_Debug: return "debug";
	case WGPULogLevel_Trace: return "trace";
	default:                 return "log";
	}
}

// The messages are UTF-8 and the trace is TCHAR, so they are converted rather
// than handed to %hs, which would read them as the ANSI code page.
static void wgpuLogCallback(WGPULogLevel level, WGPUStringView message, void *)
{
	if (message.data == NULL)
		return;

	int length = message.length == WGPU_STRLEN ? (int)strlen(message.data)
	                                           : (int)message.length;
	if (length <= 0)
		return;

	wchar_t wide[2048];
	int wideLength = MultiByteToWideChar(CP_UTF8, 0, message.data, length,
	                                     wide, (int)(sizeof(wide) / sizeof(wide[0])) - 1);
	if (wideLength <= 0)
		return;
	wide[wideLength] = 0;

	winlog(_T("wgpu %hs: %ls\n"), wgpuLogLevelName((int)level), wide);
}

// The release of wgpu-native the headers in wgpu/ were taken from, as
// wgpuGetVersion() spells it: the four bytes most significant first, so
// 0x1D000101 is 29.0.1.1.
//
// Those headers are a contract, not a description: every struct this file
// hands to the library is read there at the offsets the header lays out, and a
// release that adds, moves or removes a field reads the same bytes as
// something else entirely - the call then fails inside the library, or panics
// in it, with nothing on the way in to say why.  The version the library
// reports is compared with this one before it is used for anything, and a
// library from another release is turned down the way one that is not there
// at all is.
#define WGPU_HEADERS_VERSION 0x1D000101u

static void wgpuVersionText(unsigned version, TCHAR *text, int length)
{
	_sntprintf(text, length, _T("%u.%u.%u.%u"),
	           (version >> 24) & 0xFF, (version >> 16) & 0xFF,
	           (version >> 8) & 0xFF, version & 0xFF);
}

// Major and minor are what a struct layout moves on; a patch release of the
// library fixes the inside of the same interface and is taken as it is.
static bool wgpuVersionMatches(unsigned library)
{
	return (library >> 16) == (WGPU_HEADERS_VERSION >> 16);
}

// Installs the log callback and states which build of the library answered.
// The level comes from the registry so that a run that is being chased can ask
// for the trace level without a rebuild.
static void hookWgpuLog()
{
	int level = (int)regQueryDwordValue(_T("wgpuLog"), WGPULogLevel_Warn);

	if (level < WGPULogLevel_Off || level > WGPULogLevel_Trace)
		level = WGPULogLevel_Warn;

	TCHAR version[32];
	wgpuVersionText(wgpuGetVersion_fn(), version, sizeof(version) / sizeof(version[0]));

	winlog(_T("wgpu: wgpu_native.dll %s, %d entry points, log level %hs\n"),
	       version, (int)WGPU_FUNCTION_COUNT, wgpuLogLevelName(level));

	wgpuSetLogLevel_fn((WGPULogLevel)level);

	if (level != WGPULogLevel_Off)
		wgpuSetLogCallback_fn(wgpuLogCallback, NULL);
}

// Resolves the DLL once.  Silent, because the render method menu uses the
// outcome and a message box per menu would be absurd.
static bool loadWgpuLibrary()
{
	if (wgpuProbed)
		return wgpuModule != NULL;

	wgpuProbed = true;

	// Before the library runs a line of its own, or a panic inside it would
	// have nowhere to write the reason.
	redirectStdErr("wgpu-stderr.log");

	wgpuModule = LoadLibrary(_T("wgpu_native.dll"));
	if (wgpuModule == NULL)
	{
		winlog(_T("wgpu: wgpu_native.dll is not available\n"));
		return false;
	}

	for (unsigned i = 0; i < WGPU_FUNCTION_COUNT; i++)
	{
		FARPROC resolved = GetProcAddress(wgpuModule, g_wgpuFunctionNames[i]);
		if (resolved == NULL)
		{
			winlog(_T("wgpu: wgpu_native.dll does not carry %hs - is it another wgpu-native build?\n"),
			       g_wgpuFunctionNames[i]);
			wgpuModule = NULL;
			return false;
		}
		memcpy(g_wgpuFunctionTargets[i], &resolved, sizeof(resolved));
	}

	// Before anything of the library is called: a struct this build lays out
	// one way and the library reads another way is a crash inside it, with no
	// way back to the reason.  The settings then fall back to Direct3D 11 the
	// way they do on a machine without WebGPU, and the menu does not offer it.
	unsigned version = wgpuGetVersion_fn();

	if (!wgpuVersionMatches(version))
	{
		TCHAR library[32], headers[32];

		wgpuVersionText(version, library, sizeof(library) / sizeof(library[0]));
		wgpuVersionText(WGPU_HEADERS_VERSION, headers, sizeof(headers) / sizeof(headers[0]));

		winlog(_T("wgpu: wgpu_native.dll is %s, the headers this build was made with are %s "
		          "- the renderer is not started\n"), library, headers);

		FreeLibrary(wgpuModule);
		wgpuModule = NULL;
		wgpuProbed = true;
		return false;
	}

	hookWgpuLog();
	return true;
}

// Whether the library itself is there, which is a different question from
// whether this machine has an adapter to run it on: the menu entries leave the
// menu where it is not, and are greyed where it is but no adapter can carry it.
bool wgpuLibraryAvailable()
{
	return loadWgpuLibrary();
}

// ---------------------------------------------------------------------------
// The instance the requests go through; made once and kept for the life of
// the process, the way the Direct3D 11 backend keeps its DLLs loaded across
// display recreation.

static WGPUInstance wgpuInstance = NULL;

static bool createWgpuInstance();
static bool enumerateWgpuAdapters();

// Whether this machine has an adapter the renderer can actually run on.  The
// answer is worked out once, by the enumeration below, because it costs a
// driver round and both the menu and the render method ask for it.  A machine
// whose only adapter is the software one (WARP) has no device the renderer can
// present from, and there WebGPU is disabled outright instead of being offered
// and then falling over on the first present.
static bool wgpuUsable      = false;
static bool wgpuUsableKnown = false;

bool wgpuAvailable()
{
	if (!loadWgpuLibrary())
		return false;

	if (wgpuUsableKnown)
		return wgpuUsable;

	wgpuUsableKnown = true;

	if (!createWgpuInstance())
		return false;

	wgpuUsable = enumerateWgpuAdapters();

	return wgpuUsable;
}

static void uncapturedErrorCallback(WGPUDevice const * device, WGPUErrorType type,
                                    WGPUStringView message, void *, void *)
{
	winlog(_T("wgpu: uncaptured error %d: %.*hs\n"), (int)type, (int)message.length, message.data);
}

static bool createWgpuInstance()
{
	if (wgpuInstance != NULL)
		return true;

	if (!loadWgpuLibrary())
		return false;

	WGPUInstanceDescriptor descriptor;
	descriptor = WGPU_INSTANCE_DESCRIPTOR_INIT;

	wgpuInstance = wgpuCreateInstance_fn(&descriptor);
	if (wgpuInstance == NULL)
		winlog(_T("wgpu: the instance cannot be created\n"));

	return wgpuInstance != NULL;
}

static WGPUAdapter      adapterGotten   = NULL;
static volatile LONG    adapterAnswered = 0;

static void adapterCallback(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                            WGPUStringView message, void *, void *)
{
	adapterGotten = adapter;

	if (status != WGPURequestAdapterStatus_Success)
		winlog(_T("wgpu: no adapter comes back, %.*hs\n"), (int)message.length, message.data);

	InterlockedExchange(&adapterAnswered, 1);
}

// wgpu-native leaves wgpuInstanceWaitAny unimplemented, and calling it ends
// the process on the spot, so the asynchronous requests are driven by
// processing the instance's events until the callback has answered.  The poll
// has a sane time limit, so a driver that never answers costs a few seconds
// instead of holding the process.
static bool waitForCallback(volatile LONG *answered, const char *what)
{
	for (unsigned waited = 0; waited <= 30000; waited++)
	{
		wgpuInstanceProcessEvents_fn(wgpuInstance);

		if (InterlockedCompareExchange(answered, 0, 0) != 0)
			return true;

		Sleep(1);
	}

	winlog(_T("wgpu: %hs did not complete in 30 s, giving up\n"), what);
	return false;
}

static WGPUAdapter requestAdapter(WGPUInstance instance, const WGPURequestAdapterOptions *options)
{
	adapterGotten   = NULL;
	adapterAnswered = 0;

	WGPURequestAdapterCallbackInfo callbackInfo;
	callbackInfo.nextInChain = NULL;
	callbackInfo.mode        = WGPUCallbackMode_AllowProcessEvents;
	callbackInfo.callback    = adapterCallback;
	callbackInfo.userdata1   = NULL;
	callbackInfo.userdata2   = NULL;

	wgpuInstanceRequestAdapter_fn(instance, options, callbackInfo);

	waitForCallback(&adapterAnswered, "the adapter request");

	return adapterGotten;
}

static WGPUDevice   deviceGotten   = NULL;
static char         deviceError[512] = "";
static volatile LONG deviceAnswered = 0;

static void deviceCallback(WGPURequestDeviceStatus status, WGPUDevice device,
                           WGPUStringView message, void *, void *)
{
	deviceGotten = device;

	if (status != WGPURequestDeviceStatus_Success)
	{
		deviceError[0] = 0;
		if (message.data != NULL)
		{
			unsigned characters = message.length < sizeof(deviceError) - 1
			                      ? (unsigned)message.length : sizeof(deviceError) - 1;
			memcpy(deviceError, message.data, characters);
			deviceError[characters] = 0;
		}
	}

	InterlockedExchange(&deviceAnswered, 1);
}

static WGPUDevice requestDevice(WGPUAdapter adapter)
{
	deviceGotten   = NULL;
	deviceAnswered = 0;
	deviceError[0] = 0;

	WGPUUncapturedErrorCallbackInfo errorInfo;
	errorInfo.nextInChain = NULL;
	errorInfo.callback    = uncapturedErrorCallback;
	errorInfo.userdata1   = NULL;
	errorInfo.userdata2   = NULL;

	WGPUDeviceDescriptor descriptor;
	descriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
	descriptor.uncapturedErrorCallbackInfo = errorInfo;

	WGPURequestDeviceCallbackInfo callbackInfo;
	callbackInfo.nextInChain = NULL;
	callbackInfo.mode        = WGPUCallbackMode_AllowProcessEvents;
	callbackInfo.callback    = deviceCallback;
	callbackInfo.userdata1   = NULL;
	callbackInfo.userdata2   = NULL;

	wgpuAdapterRequestDevice_fn(adapter, &descriptor, callbackInfo);

	waitForCallback(&deviceAnswered, "the device request");

	return deviceGotten;
}

// The name of a backend the way the adapter information spells it, or NULL for
// one that is not one of the entries.
static const char *wgpuBackendTypeName(int backend)
{
	switch (backend)
	{
	case WGPUBackendType_Null:     return "Null";
	case WGPUBackendType_WebGPU:   return "WebGPU";
	case WGPUBackendType_D3D11:    return "D3D11";
	case WGPUBackendType_D3D12:    return "D3D12";
	case WGPUBackendType_Metal:    return "Metal";
	case WGPUBackendType_Vulkan:   return "Vulkan";
	case WGPUBackendType_OpenGL:   return "OpenGL";
	case WGPUBackendType_OpenGLES: return "GLES";
	}

	return NULL;
}

// The backend the menu asked for, as the adapter request spells it.  Automatic
// is Undefined, which is the request that lets the library decide, and the one
// every other value falls back to when it is not one of the entries.
static WGPUBackendType wgpuRequestedBackendType(int backend)
{
	switch (backend)
	{
	case VBA_WGPU_BACKEND_D3D12:  return WGPUBackendType_D3D12;
	case VBA_WGPU_BACKEND_VULKAN: return WGPUBackendType_Vulkan;
	case VBA_WGPU_BACKEND_METAL:  return WGPUBackendType_Metal;
	case VBA_WGPU_BACKEND_OPENGL: return WGPUBackendType_OpenGL;
	}

	return WGPUBackendType_Undefined;
}

// The adapter types wgpu reports, in the same one line per adapter the
// enumeration below writes to the trace.
static const char *wgpuAdapterTypeName(int type)
{
	switch (type)
	{
	case WGPUAdapterType_DiscreteGPU:   return "discrete GPU";
	case WGPUAdapterType_IntegratedGPU: return "integrated GPU";
	case WGPUAdapterType_CPU:           return "software";
	case WGPUAdapterType_Unknown:       return "unknown";
	}

	return "unknown";
}

// Whether the software adapter (WARP) is allowed to run the renderer: it is
// turned down by default, and this is the way to ask for it while chasing a
// driver bug.
static bool softwareAdapterAllowed()
{
	TCHAR value[8];
	DWORD length = GetEnvironmentVariable(_T("VBA_WGPU_ALLOW_SOFTWARE"), value, ARRAYSIZE(value));
	return length > 0 && length < ARRAYSIZE(value) && value[0] != 0 && value[0] != _T('0');
}

// The bytes of a string view the library owns, into a plain buffer.
static void copyView(char *to, size_t bytes, WGPUStringView view)
{
	unsigned chars = view.length < bytes - 1 ? (unsigned)view.length : (unsigned)bytes - 1;

	if (view.data != NULL && chars > 0)
		memcpy(to, view.data, chars);
	else
		chars = 0;

	to[chars] = 0;
}

// The adapter the renderer must not run on: the software one (WARP) advertises
// D3D12 on machines whose driver stack cannot carry it, and its first present
// wedges the process in a wait even the task manager cannot break.
static bool adapterIsSoftware(WGPUAdapterType type)
{
	return type == WGPUAdapterType_CPU;
}

// The backends the enumeration below found an adapter on that can carry the
// renderer, as the instance spells them.  The backend menu greys out what is
// not in here, so a backend this machine cannot run is not offered.
static WGPUInstanceBackend wgpuPresentBackends = 0;

// The adapters of every backend the library was built with, one line each in
// the trace: a backend with no adapter tells the same story as one that fails
// later, only before anything is created.  The adapters are the enumeration
// own, and are released here again.  The answer is whether one of them can
// carry the renderer at all.
static bool enumerateWgpuAdapters()
{
	if (wgpuInstance == NULL)
		return false;

	static const struct { WGPUInstanceBackend flag; const char *name; } backends[] =
	{
		{ WGPUInstanceBackend_Vulkan, "Vulkan" },
		{ WGPUInstanceBackend_GL,     "OpenGL" },
		{ WGPUInstanceBackend_Metal,  "Metal" },
		{ WGPUInstanceBackend_DX12,   "Direct3D 12" },
	};

	bool usable = false;
	char usableName[128] = "";

	wgpuPresentBackends = 0;

	for (unsigned i = 0; i < sizeof(backends) / sizeof(backends[0]); i++)
	{
		WGPUInstanceEnumerateAdapterOptions options;
		options.nextInChain = NULL;
		options.backends    = backends[i].flag;

		size_t count = wgpuInstanceEnumerateAdapters_fn(wgpuInstance, &options, NULL);
		winlog(_T("wgpu: %hs, %d adapter(s)\n"), backends[i].name, (int)count);

		if (count == 0)
			continue;

		WGPUAdapter *adapters = new WGPUAdapter[count];
		size_t found = wgpuInstanceEnumerateAdapters_fn(wgpuInstance, &options, adapters);

		for (size_t a = 0; a < found && a < count; a++)
		{
			WGPUAdapterInfo info;
			info = WGPU_ADAPTER_INFO_INIT;

			if (wgpuAdapterGetInfo_fn(adapters[a], &info) == WGPUStatus_Success)
			{
				char name[128];
				copyView(name, sizeof(name), info.device);

				bool software = adapterIsSoftware(info.adapterType);

				// The escape hatch is for chasing a driver bug on a machine
				// whose adapter is the software one.
				if (software && softwareAdapterAllowed())
					software = false;

				winlog(_T("wgpu:   %.*hs (%.*hs), %hs, %hs, vendor %04x device %04x%hs\n"),
				       (int)info.device.length, info.device.data,
				       (int)info.description.length, info.description.data,
				       wgpuBackendTypeName((int)info.backendType),
				       wgpuAdapterTypeName((int)info.adapterType),
				       (unsigned)info.vendorID, (unsigned)info.deviceID,
				       software ? ", not usable" : "");

				if (!software && !usable)
				{
					usable = true;
					lstrcpynA(usableName, name, sizeof(usableName));
				}

				if (!software)
					wgpuPresentBackends |= backends[i].flag;

				wgpuAdapterInfoFreeMembers_fn(info);
			}
			else
				winlog(_T("wgpu:   an adapter whose information cannot be read\n"));

			wgpuAdapterRelease_fn(adapters[a]);
		}

		delete[] adapters;
	}

	if (usable)
		winlog(_T("wgpu: %hs can carry WebGPU\n"), usableName);
	else
		winlog(_T("wgpu: no adapter here can carry WebGPU, the render method is disabled\n"));

	return usable;
}

// Whether the backend the menu names is one this machine can run the renderer
// on: a backend the enumeration found no usable adapter on is greyed out
// rather than offered and then fallen back from.  Automatic is always there,
// because it is the request that leaves the choice to the library.
bool wgpuBackendAvailable(int backend)
{
	if (!wgpuAvailable())
		return false;

	switch (backend)
	{
	case VBA_WGPU_BACKEND_AUTO:   return true;
	case VBA_WGPU_BACKEND_D3D12:  return (wgpuPresentBackends & WGPUInstanceBackend_DX12) != 0;
	case VBA_WGPU_BACKEND_VULKAN: return (wgpuPresentBackends & WGPUInstanceBackend_Vulkan) != 0;
	case VBA_WGPU_BACKEND_METAL:  return (wgpuPresentBackends & WGPUInstanceBackend_Metal) != 0;
	case VBA_WGPU_BACKEND_OPENGL: return (wgpuPresentBackends & WGPUInstanceBackend_GL) != 0;
	}

	return false;
}

// The backend the running renderer is on, named by the render method menu the
// way the core version is for OpenGL 3.3.  Empty while the renderer is not
// running, and the entry is then the plain "WebGPU" the resource spelling it.
static char backendModel[24] = "";

const char *wgpuBackendModel()
{
	return backendModel[0] != 0 ? backendModel : NULL;
}

// The string view a few labels are handed over in.
static WGPUStringView textInWgpu(const char *text)
{
	WGPUStringView view;
	view.data   = text;
	view.length = strlen(text);
	return view;
}

// ---------------------------------------------------------------------------
// The display.

struct WGPUVERTEX
{
	float x, y;
	float u, v;
};

// The quad every pass of the renderer is drawn with.  Four vertices are the
// frame and four are the on screen display, see calculateVertices().
#define VERTEX_BUFFER_BYTES  (sizeof(WGPUVERTEX) * 8)
#define OVERLAY_QUAD_OFFSET  (sizeof(WGPUVERTEX) * 4)

static const char *g_quadShader =
    "struct VSOut {\n"
    "    @builtin(position) pos: vec4f,\n"
    "    @location(0) uv: vec2f,\n"
    "}\n"
    "\n"
    "@vertex\n"
    "fn vsMain(@location(0) position: vec2f, @location(1) texCoord: vec2f) -> VSOut\n"
    "{\n"
    "    var out: VSOut;\n"
    "    out.pos = vec4f(position, 0.0, 1.0);\n"
    "    out.uv = texCoord;\n"
    "    return out;\n"
    "}\n"
    "\n"
    "@group(0) @binding(0) var sourceTex: texture_2d<f32>;\n"
    "@group(0) @binding(1) var sourceSampler: sampler;\n"
    "\n"
    "@fragment\n"
    "fn fsMain(inside: VSOut) -> @location(0) vec4f\n"
    "{\n"
    "    return textureSample(sourceTex, sourceSampler, inside.uv);\n"
    "}\n";

// ---------------------------------------------------------------------------
// The upscalers.
//
// A chain of WGSL passes over the frame, the shape the other two renderers
// run: a pass reads the texture the pass before it made - the frame for the
// first one - and writes one of its own.  The last pass writes the back buffer
// itself whenever it comes out at its size, so a single pass upscaler costs no
// blit at all and a chain costs one render pass per pass and nothing more.
//
// Nothing is uploaded per frame.  The constants of a pass are the sizes of the
// pass, which change when the frame, the window or the scale cap does and not
// otherwise, so they are written then and read from the same buffer after.

// The most passes any of them has; ScaleFX has five in the other renderers.
#define WGPU_MAX_PASSES 5

// The four float4s a pass is handed, in the order and the layout of the struct
// the shaders declare, and the same numbers Direct3D 11 puts in its constant
// buffer.
struct WgpuUpscalerConstants
{
	float sourceSize[4]; // xy: the size the pass reads, zw: one texel
	float outputSize[4]; // xy: the size of its target, zw: one pixel of it
	float scale[4];      // xy: outputSize.xy / sourceSize.xy
	float secondSize[4]; // xy: the size of a second texture, zw: one texel
};

struct WgpuUpscalerPass
{
	WGPURenderPipeline pipeline;  // into a texture of the chain
	WGPUBindGroup      group;     // what it reads, and its constants
	WGPUBuffer         constants;
	WGPUTexture        texture;   // the target it draws into
	WGPUTextureView    view;
};

struct WgpuUpscalerSource
{
	int         value;    // the UPSCALER_* value it implements
	const char *name;     // for the trace log and the screen message
	const char *const *resources; // per pass: the RCDATA resource holding its WGSL
	const int  *scales;   // per pass: 0 is the target size, n is n times the frame
	const int  *seconds;  // per pass: the texture it reads besides the one the
	                      // pass before it made, as an index of the outputs:
	                      // 0 is the frame, n the target of pass n - 1, and -1
	                      // that it reads one texture only.  That is how a
	                      // libretro shader names OrigTexture and the
	                      // PassPrev<N> that reaches past the start of the
	                      // chain.
	int         passCount;
};

// Only the upscalers with a WGSL port are here, and the menu greys out the
// rest while this renderer runs: the shaders of the other two are GLSL and
// HLSL, and wgpu compiles neither.
static const char *const g_xbrzResources[] = { "XBRZ_FREESCALE_WGSL" };
static const char *const g_lanczos3Resources[] = { "LANCZOS3_WGSL" };
static const char *const g_bicubicResources[] = { "BICUBIC_WGSL" };
static const char *const g_xbrLv2Resources[]   = { "XBR_LV2_WGSL" };
static const char *const g_xbrLv2NoBlendResources[] = { "XBR_LV2_NOBLEND_WGSL" };
static const char *const g_xbrLv3Resources[]   = { "XBR_LV3_WGSL" };
static const char *const g_ptSkyWalkerResources[] = { "PT_SKYWALKER541_WGSL" };

// The flags of every pixel at the frame size, then the blend that reads them
// and the frame together.
static const char *const g_xbr2xResources[] = { "XBR_2X_PASS0_WGSL", "XBR_2X_PASS1_WGSL" };

// xBR lv3 in the two passes of its preset: the edge data of every pixel, then
// the interpolation that reads it and the frame together.
static const char *const g_xbrLv3MultipassResources[] = { "XBR_LV3_PASS0_WGSL", "XBR_LV3_PASS1_WGSL" };

// super-xBR: the edge data, the picture doubled and blended with the frame,
// then the jinc filter over it.
static const char *const g_superXbrResources[] = { "SUPER_XBR_PASS0_WGSL", "SUPER_XBR_PASS1_WGSL", "SUPER_XBR_JINC2_WGSL" };

// ScaleFX: the metrics of the edge, the strengths of the corners, the
// junctions, the subpixels, and the pass that takes the colour from the frame
// at each of them.
static const char *const g_scalefxResources[] = { "SCALEFX_PASS0_WGSL", "SCALEFX_PASS1_WGSL", "SCALEFX_PASS2_WGSL", "SCALEFX_PASS3_WGSL", "SCALEFX_PASS4_WGSL" };

// xBR-mlv4: two passes of edge data, the line each quarter of the pixel
// follows at twice the frame size, and the interpolation along it.
static const char *const g_xbrMlv4Resources[] = { "XBR_MLV4_PASS1_WGSL", "XBR_MLV4_PASS2_WGSL", "XBR_MLV4_PASS3_WGSL", "XBR_MLV4_PASS4_WGSL" };

static const int g_xbrzScales[] = { 0 };
static const int g_lanczos3Scales[] = { 0 };
static const int g_bicubicScales[] = { 0 };
static const int g_xbrLv2Scales[]   = { 0 };
static const int g_xbrLv2NoBlendScales[] = { 0 };
static const int g_xbrLv3Scales[]   = { 0 };
static const int g_ptSkyWalkerScales[] = { 0 };
static const int g_xbr2xScales[]    = { 1, 0 };
static const int g_xbrLv3MultipassScales[] = { 1, 0 };
static const int g_superXbrScales[] = { 1, 2, 0 };
static const int g_scalefxScales[] = { 1, 1, 1, 1, 0 };
static const int g_xbrMlv4Scales[] = { 1, 1, 2, 0 };

static const int g_xbrzSeconds[] = { -1 };
static const int g_lanczos3Seconds[] = { -1 };
static const int g_bicubicSeconds[] = { -1 };
static const int g_xbrLv2Seconds[]   = { -1 };
static const int g_xbrLv2NoBlendSeconds[] = { -1 };
static const int g_xbrLv3Seconds[]   = { -1 };
static const int g_ptSkyWalkerSeconds[] = { -1 };
static const int g_xbr2xSeconds[]    = { -1, 0 };
static const int g_xbrLv3MultipassSeconds[] = { -1, 0 };
static const int g_superXbrSeconds[] = { -1, 0, -1 };
static const int g_scalefxSeconds[] = { -1, -1, 1, -1, 0 };
static const int g_xbrMlv4Seconds[] = { -1, 0, 0, 0 };

static const WgpuUpscalerSource g_wgpuUpscalers[] =
{
	{ UPSCALER_XBRZ,              "xBRZ",              g_xbrzResources,            g_xbrzScales,           g_xbrzSeconds,           1 },
	{ UPSCALER_BICUBIC,           "Bicubic",           g_bicubicResources,         g_bicubicScales,        g_bicubicSeconds,        1 },
	{ UPSCALER_LANCZOS3,          "Lanczos",           g_lanczos3Resources,        g_lanczos3Scales,       g_lanczos3Seconds,       1 },
	{ UPSCALER_XBR_LV2,           "xBR lv2",           g_xbrLv2Resources,          g_xbrLv2Scales,         g_xbrLv2Seconds,         1 },
	{ UPSCALER_XBR_LV2_NOBLEND,   "xBR lv2 no-blend",  g_xbrLv2NoBlendResources,   g_xbrLv2NoBlendScales,  g_xbrLv2NoBlendSeconds,  1 },
	{ UPSCALER_XBR_LV3,           "xBR lv3",           g_xbrLv3Resources,          g_xbrLv3Scales,         g_xbrLv3Seconds,         1 },
	{ UPSCALER_XBR_LV3_MULTIPASS,  "xBR lv3 multipass", g_xbrLv3MultipassResources,  g_xbrLv3MultipassScales, g_xbrLv3MultipassSeconds, 2 },
	{ UPSCALER_SUPER_XBR,         "super-xBR",         g_superXbrResources,        g_superXbrScales,       g_superXbrSeconds,       3 },
	{ UPSCALER_SCALEFX,           "ScaleFX",           g_scalefxResources,         g_scalefxScales,        g_scalefxSeconds,        5 },
	{ UPSCALER_XBR_2X,            "2xBR",              g_xbr2xResources,           g_xbr2xScales,          g_xbr2xSeconds,          2 },
	{ UPSCALER_XBR_MLV4,          "xBR-mlv4",          g_xbrMlv4Resources,         g_xbrMlv4Scales,        g_xbrMlv4Seconds,        4 },
	{ UPSCALER_PT_SKYWALKER,      "PT SkyWalker541",   g_ptSkyWalkerResources,     g_ptSkyWalkerScales,    g_ptSkyWalkerSeconds,    1 },
};

#define WGPU_UPSCALER_COUNT (sizeof(g_wgpuUpscalers) / sizeof(g_wgpuUpscalers[0]))

// The row of the table a UPSCALER_* value is, or -1.
static int wgpuUpscalerIndex(int value)
{
	for (int i = 0; i < (int)WGPU_UPSCALER_COUNT; i++)
		if (g_wgpuUpscalers[i].value == value)
			return i;

	return -1;
}

// The shader sources ship inside the executable as RCDATA, the way the ones of
// the other two renderers do, so that they stay readable and diffable against
// upstream.  The resource is not NUL terminated, so its length goes with it.
static WGPUStringView resourceInWgpu(LPCSTR name)
{
	WGPUStringView view;
	view.data   = NULL;
	view.length = 0;

	HRSRC resource = FindResource(NULL, winWideFromAnsi(name), RT_RCDATA);
	if (resource == NULL)
	{
		winlog(_T("wgpu: the shader resource %hs is missing from the executable\n"), name);
		return view;
	}

	HGLOBAL handle = LoadResource(NULL, resource);
	if (handle == NULL)
	{
		winlog(_T("wgpu: cannot load %hs\n"), name);
		return view;
	}

	view.data   = (const char *)LockResource(handle);
	view.length = (size_t)SizeofResource(NULL, resource);
	return view;
}

class WgpuDisplay : public IDisplay
{
public:
	WgpuDisplay();
	virtual ~WgpuDisplay();

	virtual bool initialize();
	virtual void cleanup();
	virtual void render();
	virtual void clear();
	virtual bool changeRenderSize(int w, int h);
	virtual void resize(int w, int h);
	virtual DISPLAY_TYPE getType() { return WGPU; };
	virtual void setOption(const char *, int);
	// The upscalers this renderer has a WGSL port of, plus Off.  One whose
	// WGSL did not compile on this device is left out, the way the other two
	// renderers leave out theirs.
	virtual bool upscalerAvailable(int upscaler)
	{
		return upscaler == UPSCALER_OFF ||
		       (wgpuUpscalerIndex(upscaler) >= 0 && !(upscalerValue == upscaler && upscalerFailed));
	};
	// The frame is uploaded straight into the texture that is sampled, and
	// wgpu makes no mip chain of its own: a chain would take a render pass per
	// level here, so the two filters the sampler has are the two on offer.
	virtual bool filterAvailable(int filter) { return filter == 0 || filter == 1; };

private:
	bool createSurface();
	bool createDevice();
	bool createPipeline();
	bool readSurfaceCapabilities();
	bool setPresentMode(bool withVsync);
	void releaseSurface();
	void nameBackend();
	void releaseFrameTexture();
	void releaseOverlayTexture();
	void releaseFrameStaging();
	void releaseOverlayStaging();
	void makeFrameTexture(int w, int h);
	void makeOverlayTexture(int w, int h);
	WGPUBindGroup makeBindGroup(WGPUTextureView view, bool linear);
	bool calculateVertices();
	void updateFrameData();
	void uploadOverlay();
	bool drawAndPresent(bool clearOnly);
	bool prepareUpscaler();
	void releaseUpscaler();
	void releaseUpscalerTargets();

	HWND          hwnd;
	WGPUSurface   surface;
	WGPUAdapter   adapter;
	WGPUDevice    device;
	WGPUQueue     queue;
	WGPUShaderModule    shader;
	WGPUBindGroupLayout bindGroupLayout;
	WGPUPipelineLayout  pipelineLayout;
	WGPURenderPipeline  framePipeline;
	WGPURenderPipeline  overlayPipeline;
	WGPUSampler   samplerPoint;
	WGPUSampler   samplerLinear;
	WGPUBuffer    vertexBuffer;

	WGPUTexture frameTexture;
	WGPUTextureView frameView;
	WGPUBindGroup frameGroupPoint;
	WGPUBindGroup frameGroupLinear;

	WGPUTexture     overlayTexture;
	WGPUTextureView overlayView;
	WGPUBindGroup   overlayGroup;

	// The upscaling chain the menu selected, when this renderer has a WGSL
	// port of it: one bind group layout and one pipeline layout of their own,
	// because a pass is handed a uniform buffer as well as the texture it
	// reads, and one module and one pipeline per pass - plus the variant of
	// the last pass that writes the back buffer, which is a different format.
	WGPUBindGroupLayout upscalerGroupLayout;
	WGPUPipelineLayout  upscalerPipelineLayout;
	WGPUShaderModule    upscalerModules[WGPU_MAX_PASSES];
	WGPURenderPipeline  upscalerSurfacePipeline;
	WGPUBindGroup       upscalerBlitGroup; // the last target, for the capped path
	WgpuUpscalerPass    upscalerPasses[WGPU_MAX_PASSES];
	int                 upscalerPassCount;
	int                 upscalerValue;  // the value the pipelines were built for, -1 for none
	bool                upscalerFailed; // the WGSL did not compile on this device
	int                 upscalerFrameWidth;
	int                 upscalerFrameHeight;
	int                 upscalerTargetWidth;
	int                 upscalerTargetHeight;

	// A desktop in 16 bit mode gives the core a 565 frame, and the upload
	// widens those pixels into the RGBA8 texture; the 32 bit frame is BGRA
	// already and rides over itself.
	bool frameBgr565;

	// What the adapter calls itself, kept from the one read the name comes
	// from: the software (WARP) adapter is turned down by it, before a device
	// or a surface is made on one.
	WGPUAdapterType adapterType;
	char            adapterName[128];

	// Set when the adapter was turned down on purpose (a software one), so the
	// failure path knows not to put a message box in front of a fallback.
	bool refused;

	WGPUTextureFormat surfaceFormat;
	uint32_t          surfaceWidth;
	uint32_t          surfaceHeight;
	WGPUPresentMode   presentMode;
	bool              presentModeSupported[WGPUPresentMode_Mailbox + 1];

	// The rows a frame is made of, as the core side lays them out, and the
	// rows the copy_texture takes.  The row distances the copy_texture wants
	// - a multiple of 256 - live with each of them, and the overlay has its
	// own, because its DIB is nowhere near aligned the way a texture is.
	//
	// The two frame buffers are one allocation or two: the 32 bit frame is
	// BGRA already and is uploaded straight out of stagingFrame, so
	// stagingTexture points at it and owns nothing, and only the 565 widening
	// has a second block of its own.  stagingTextureOwned is what tells the
	// release which of the two it is holding - comparing the pointers after
	// the first block is gone would read a pointer that is not a buffer any
	// more, which is a second delete[] of it.
	unsigned char *stagingFrame;
	unsigned char *stagingTexture;
	unsigned char *stagingOverlay;
	int            stagingSourceRowBytes;
	int            stagingTextureRowBytes;
	int            stagingOverlayRowBytes;
	int            stagingRows;
	bool           stagingTextureOwned;

	int   width;
	int   height;
	int   backWidth;
	int   backHeight;
	int   filterMode;
	TextOverlay overlay;
	WGPUVERTEX quad[4];
};

WgpuDisplay::WgpuDisplay()
{
	hwnd             = NULL;
	surface          = NULL;
	adapter          = NULL;
	device           = NULL;
	queue            = NULL;
	shader           = NULL;
	bindGroupLayout  = NULL;
	pipelineLayout   = NULL;
	framePipeline    = NULL;
	overlayPipeline  = NULL;
	samplerPoint     = NULL;
	samplerLinear    = NULL;
	vertexBuffer     = NULL;
	frameTexture     = NULL;
	frameView        = NULL;
	frameGroupPoint  = NULL;
	frameGroupLinear = NULL;
	overlayTexture   = NULL;
	overlayView      = NULL;
	overlayGroup     = NULL;
	upscalerGroupLayout     = NULL;
	upscalerPipelineLayout  = NULL;
	ZeroMemory(upscalerModules, sizeof(upscalerModules));
	upscalerSurfacePipeline = NULL;
	upscalerBlitGroup       = NULL;
	upscalerPassCount       = 0;
	upscalerValue           = -1;
	upscalerFailed          = false;
	upscalerFrameWidth      = 0;
	upscalerFrameHeight     = 0;
	upscalerTargetWidth     = 0;
	upscalerTargetHeight    = 0;
	ZeroMemory(upscalerPasses, sizeof(upscalerPasses));
	frameBgr565      = false;
	surfaceFormat    = WGPUTextureFormat_Undefined;
	surfaceWidth     = 0;
	surfaceHeight    = 0;
	presentMode      = WGPUPresentMode_Undefined;
	ZeroMemory(presentModeSupported, sizeof(presentModeSupported));
	stagingFrame     = NULL;
	stagingTexture   = NULL;
	stagingOverlay   = NULL;
	stagingSourceRowBytes  = 0;
	stagingTextureRowBytes = 0;
	stagingOverlayRowBytes = 0;
	stagingRows      = 0;
	stagingTextureOwned = false;
	width            = 0;
	height           = 0;
	backWidth        = 0;
	backHeight       = 0;
	filterMode       = 0;
	adapterType      = WGPUAdapterType_Unknown;
	adapterName[0]   = 0;
	refused          = false;
}

WgpuDisplay::~WgpuDisplay()
{
	cleanup();
}

void WgpuDisplay::cleanup()
{
	overlay.destroy();

	// The objects the device made go before the device itself: every one of
	// them holds a reference to it, and the device is the last thing to fall.
	releaseUpscaler();
	releaseFrameTexture();
	releaseOverlayTexture();

	if (upscalerSurfacePipeline) { wgpuRenderPipelineRelease_fn(upscalerSurfacePipeline); upscalerSurfacePipeline = NULL; }
	if (upscalerPipelineLayout)  { wgpuPipelineLayoutRelease_fn(upscalerPipelineLayout);   upscalerPipelineLayout = NULL; }
	if (upscalerGroupLayout)     { wgpuBindGroupLayoutRelease_fn(upscalerGroupLayout);     upscalerGroupLayout = NULL; }

	if (vertexBuffer)  { wgpuBufferRelease_fn(vertexBuffer);          vertexBuffer = NULL; }
	if (samplerLinear) { wgpuSamplerRelease_fn(samplerLinear);        samplerLinear = NULL; }
	if (samplerPoint)  { wgpuSamplerRelease_fn(samplerPoint);         samplerPoint = NULL; }
	if (framePipeline) { wgpuRenderPipelineRelease_fn(framePipeline); framePipeline = NULL; }
	if (overlayPipeline){ wgpuRenderPipelineRelease_fn(overlayPipeline); overlayPipeline = NULL; }
	if (pipelineLayout){ wgpuPipelineLayoutRelease_fn(pipelineLayout);  pipelineLayout = NULL; }
	if (bindGroupLayout) { wgpuBindGroupLayoutRelease_fn(bindGroupLayout); bindGroupLayout = NULL; }
	if (shader)        { wgpuShaderModuleRelease_fn(shader);          shader = NULL; }
	if (queue)         { wgpuQueueRelease_fn(queue);                  queue = NULL; }

	releaseSurface();

	if (device != NULL)
	{
		// Let the work the queue carried out drain before the device goes.
		wgpuDeviceDestroy_fn(device);
		wgpuDeviceRelease_fn(device);
		device = NULL;
	}

	// The instance is kept for the life of the program, so what the library
	// still holds queued for the window - the destruction of the swap chain
	// above all - is given a chance to run here rather than inside the next
	// renderer's surface.
	if (wgpuInstance != NULL)
		wgpuInstanceProcessEvents_fn(wgpuInstance);

	if (adapter)
	{
		wgpuAdapterRelease_fn(adapter);
		adapter = NULL;
	}

	releaseFrameStaging();
	releaseOverlayStaging();

	backendModel[0] = 0;
}

// The surface is the one object the window owns: dropped with the display, and
// the next display asks for its own, the way the Direct3D 11 backend's swap
// chain is one per display too.
void WgpuDisplay::releaseSurface()
{
	if (surface != NULL)
	{
		wgpuSurfaceUnconfigure_fn(surface);

		// The swap chain behind the surface belongs to the window, and a
		// window is held by one swap chain at a time.  The frames that were
		// drawn on its images keep it alive until the device has run them,
		// so the device is waited on here, while it is still there to be
		// asked: the next surface on this window then gets the window from a
		// renderer that has really let go of it, instead of being answered
		// with "Native window is in use" (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR).
		if (device != NULL)
			wgpuDevicePoll_fn(device, true, NULL);

		wgpuSurfaceRelease_fn(surface);
	}
	surface      = NULL;
	surfaceWidth = 0;
	surfaceHeight = 0;
	presentMode  = WGPUPresentMode_Undefined;
}

void WgpuDisplay::releaseFrameTexture()
{
	if (frameGroupPoint)
	{
		wgpuBindGroupRelease_fn(frameGroupPoint);
		frameGroupPoint = NULL;
	}
	if (frameGroupLinear)
	{
		wgpuBindGroupRelease_fn(frameGroupLinear);
		frameGroupLinear = NULL;
	}
	if (frameView)
	{
		wgpuTextureViewRelease_fn(frameView);
		frameView = NULL;
	}
	if (frameTexture)
	{
		wgpuTextureRelease_fn(frameTexture);
		frameTexture = NULL;
	}
}

void WgpuDisplay::releaseOverlayTexture()
{
	if (overlayGroup)
	{
		wgpuBindGroupRelease_fn(overlayGroup);
		overlayGroup = NULL;
	}
	if (overlayView)
	{
		wgpuTextureViewRelease_fn(overlayView);
		overlayView = NULL;
	}
	if (overlayTexture)
	{
		wgpuTextureRelease_fn(overlayTexture);
		overlayTexture = NULL;
	}
}

// The frame's staging rows.  The pair is one block or two, and which one it is
// is what stagingTextureOwned says: the 32 bit frame has stagingTexture
// pointing at stagingFrame and owning nothing, so the release has to know that
// before the first block goes.  Asking whether the two pointers differ after
// stagingFrame is freed cannot tell - it is compared against a pointer that is
// no longer a buffer, and the block behind it would be freed a second time.
void WgpuDisplay::releaseFrameStaging()
{
	delete[] stagingFrame;
	stagingFrame = NULL;

	if (stagingTextureOwned)
		delete[] stagingTexture;

	stagingTexture      = NULL;
	stagingTextureOwned = false;
	stagingSourceRowBytes  = 0;
	stagingTextureRowBytes = 0;
	stagingRows            = 0;
}

void WgpuDisplay::releaseOverlayStaging()
{
	delete[] stagingOverlay;
	stagingOverlay = NULL;
	stagingOverlayRowBytes = 0;
}

WGPUBindGroup WgpuDisplay::makeBindGroup(WGPUTextureView view, bool linear)
{
	WGPUBindGroupEntry entries[2];
	entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
	entries[0].binding     = 0;
	entries[0].textureView = view;

	entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
	entries[1].binding = 1;
	entries[1].sampler = linear ? samplerLinear : samplerPoint;

	WGPUBindGroupDescriptor groupDescriptor;
	groupDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	groupDescriptor.layout     = bindGroupLayout;
	groupDescriptor.entryCount = 2;
	groupDescriptor.entries    = entries;

	return wgpuDeviceCreateBindGroup_fn(device, &groupDescriptor);
}

// The frame texture is (re)made when the frame size changes, and the value the
// copy_texture calls a row distance comes with it, a multiple of 256 bytes a
// row, which the next 4 bytes a pixel in do not buy for free.
void WgpuDisplay::makeFrameTexture(int w, int h)
{
	releaseFrameTexture();
	releaseFrameStaging();

	if (device == NULL || w <= 0 || h <= 0)
		return;

	WGPUExtent3D size;
	size.width             = (uint32_t)w;
	size.height            = (uint32_t)h;
	size.depthOrArrayLayers = 1;

	WGPUTextureDescriptor descriptor;
	descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
	descriptor.size          = size;
	descriptor.mipLevelCount = 1;
	descriptor.sampleCount   = 1;
	descriptor.dimension     = WGPUTextureDimension_2D;
	descriptor.format        = frameBgr565 ? WGPUTextureFormat_RGBA8Unorm : WGPUTextureFormat_BGRA8Unorm;
	descriptor.usage         = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
	descriptor.label         = textInWgpu("frame");

	frameTexture = wgpuDeviceCreateTexture_fn(device, &descriptor);
	if (frameTexture == NULL)
	{
		winlog(_T("wgpu: the %dx%d frame texture cannot be made\n"), w, h);
		return;
	}

	frameView = wgpuTextureCreateView_fn(frameTexture, NULL);
	if (frameView == NULL)
	{
		winlog(_T("wgpu: the frame texture view cannot be made\n"));
		releaseFrameTexture();
		return;
	}

	frameGroupPoint  = makeBindGroup(frameView, false);
	frameGroupLinear = makeBindGroup(frameView, true);

	if (frameGroupPoint == NULL || frameGroupLinear == NULL)
	{
		winlog(_T("wgpu: the %dx%d frame bind groups cannot be made\n"), w, h);
		releaseFrameTexture();
		return;
	}

	// The 565 frame is two bytes a pixel and is widened into the RGBA8 rows
	// above, so the staging rows follow the frame's own size, not the
	// texture's.
	int bpp = systemColorDepth / 8;

	stagingSourceRowBytes  = ((w * bpp) + 255) & ~255;
	stagingTextureRowBytes = ((w * 4) + 255) & ~255;
	stagingRows            = h;

	stagingFrame = new unsigned char[stagingSourceRowBytes * h];
	if (frameBgr565)
	{
		stagingTexture      = new unsigned char[stagingTextureRowBytes * h];
		stagingTextureOwned = true;
	}
	else
	{
		stagingTexture      = stagingFrame;
		stagingTextureOwned = false;
	}

	width  = w;
	height = h;

	winlog(_T("wgpu: frame texture %dx%d, %hs format\n"), w, h,
	       frameBgr565 ? "RGBA8 widened from 565" : "BGRA8 as the core laid it out");
}

// The on screen display is the size of the back buffer, so it is made when the
// window is and remade whenever the window changes size.
void WgpuDisplay::makeOverlayTexture(int w, int h)
{
	releaseOverlayTexture();
	releaseOverlayStaging();

	if (device == NULL || w <= 0 || h <= 0)
		return;

	WGPUExtent3D size;
	size.width             = (uint32_t)w;
	size.height            = (uint32_t)h;
	size.depthOrArrayLayers = 1;

	WGPUTextureDescriptor descriptor;
	descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
	descriptor.size          = size;
	descriptor.mipLevelCount = 1;
	descriptor.sampleCount   = 1;
	descriptor.dimension     = WGPUTextureDimension_2D;
	descriptor.format        = WGPUTextureFormat_BGRA8Unorm;
	descriptor.usage         = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
	descriptor.label         = textInWgpu("overlay");

	overlayTexture = wgpuDeviceCreateTexture_fn(device, &descriptor);

	if (overlayTexture != NULL)
	{
		overlayView = wgpuTextureCreateView_fn(overlayTexture, NULL);
		overlayGroup = overlayView != NULL ? makeBindGroup(overlayView, false) : NULL;
	}

	overlay.create(w, h);

	winlog(_T("wgpu: on screen display %dx%d%hs\n"), w, h, overlayGroup != NULL ? "" : ", no view");

	stagingOverlayRowBytes = ((w * 4) + 255) & ~255;
	if (overlayGroup != NULL)
		stagingOverlay = new unsigned char[stagingOverlayRowBytes * h];
}

bool WgpuDisplay::createSurface()
{
	if (!createWgpuInstance())
	{
		winlog(_T("wgpu: the library or the instance is not available\n"));
		return false;
	}

	WGPUSurfaceSourceWindowsHWND source;
	source = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
	source.hinstance = AfxGetInstanceHandle();
	source.hwnd      = hwnd;

	WGPUSurfaceDescriptor descriptor;
	descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
	descriptor.nextInChain = &source.chain;

	surface = wgpuInstanceCreateSurface_fn(wgpuInstance, &descriptor);
	if (surface == NULL)
	{
		winlog(_T("wgpu: the window surface cannot be made\n"));
		return false;
	}

	return true;
}

void WgpuDisplay::nameBackend()
{
	WGPUAdapterInfo info;
	info = WGPU_ADAPTER_INFO_INIT;

	WGPUStatus status = wgpuAdapterGetInfo_fn(adapter, &info);
	if (status != WGPUStatus_Success)
	{
		winlog(_T("wgpu: the adapter information cannot be read\n"));
		return;
	}

	const char *model = wgpuBackendTypeName((int)info.backendType);

	lstrcpynA(backendModel, model != NULL ? model : "?", sizeof(backendModel));

	adapterType = info.adapterType;
	copyView(adapterName, sizeof(adapterName), info.device);

	winlog(_T("wgpu: adapter %.*hs (%.*hs) backend %hs, %hs, vendor %.*hs device %x\n"),
	       (int)info.description.length, info.description.data,
	       (int)info.device.length, info.device.data,
	       backendModel,
	       wgpuAdapterTypeName((int)info.adapterType),
	       (int)info.vendor.length, info.vendor.data,
	       (unsigned)info.deviceID);

	wgpuAdapterInfoFreeMembers_fn(info);
}

// What the surface shows in: the format comes first in the adapter's own
// preference order, and the present modes it lists are remembered so the
// settings are re-made against what the adapter answers with - Fifo is the
// only one the needed every time, so the rest are a preference order, the way
// the Direct3D 11 swap chain walks its flip model levels.
// What a surface format is called in the trace, and whether it encodes the
// colour on the way out.  The frame arrives encoded already, so an sRGB
// surface is the one thing this renderer cannot hand those bytes to.
static const char *surfaceFormatName(WGPUTextureFormat format)
{
	switch (format)
	{
	case WGPUTextureFormat_RGBA8Unorm:     return "RGBA8";
	case WGPUTextureFormat_RGBA8UnormSrgb: return "RGBA8 sRGB";
	case WGPUTextureFormat_BGRA8Unorm:     return "BGRA8";
	case WGPUTextureFormat_BGRA8UnormSrgb: return "BGRA8 sRGB";
	default:                               return "another format";
	}
}

static bool surfaceFormatEncodes(WGPUTextureFormat format)
{
	return format == WGPUTextureFormat_RGBA8UnormSrgb ||
	       format == WGPUTextureFormat_BGRA8UnormSrgb;
}

bool WgpuDisplay::readSurfaceCapabilities()
{
	WGPUSurfaceCapabilities capabilities;
	capabilities = WGPU_SURFACE_CAPABILITIES_INIT;

	WGPUStatus status = wgpuSurfaceGetCapabilities_fn(surface, adapter, &capabilities);
	if (status != WGPUStatus_Success || capabilities.formatCount == 0)
	{
		winlog(_T("wgpu: the surface capabilities cannot be read\n"));
		wgpuSurfaceCapabilitiesFreeMembers_fn(capabilities);
		return false;
	}

	surfaceFormat = capabilities.formats[0];

	// The core hands the frame over as bytes it has already encoded, and the
	// other two renderers give those bytes to the swap chain as they are.  A
	// format that encodes on the way out would encode them a second time,
	// which washes the picture out, and that is the one the backends offer
	// first, so a plain format is taken where there is one.
	for (size_t i = 0; i < capabilities.formatCount; i++)
	{
		if (capabilities.formats[i] == WGPUTextureFormat_BGRA8Unorm ||
		    capabilities.formats[i] == WGPUTextureFormat_RGBA8Unorm)
		{
			surfaceFormat = capabilities.formats[i];
			break;
		}
	}

	ZeroMemory(presentModeSupported, sizeof(presentModeSupported));
	for (size_t mode = 0; mode < capabilities.presentModeCount; mode++)
	{
		int index = (int)capabilities.presentModes[mode];
		if (index >= 0 && (size_t)index < sizeof(presentModeSupported) / sizeof(presentModeSupported[0]))
			presentModeSupported[index] = true;
	}

	CStringA names;
	for (int index = 0; index < (int)(sizeof(presentModeSupported) / sizeof(presentModeSupported[0])); index++)
	{
		if (!presentModeSupported[index])
			continue;

		static const char *modeNames[] = { "-", "Fifo", "Fifo relaxed", "Immediate", "Mailbox" };
		CStringA modeText;
		modeText.Format("%s%s", names.GetLength() > 0 ? ", " : "", modeNames[index]);
		names += modeText;
	}

	winlog(_T("wgpu: surface format %hs (%d), present modes %hs%hs\n"),
	       surfaceFormatName(surfaceFormat), (int)surfaceFormat, (const char *)names,
	       surfaceFormatEncodes(surfaceFormat) ? " - this one encodes the frame twice" : "");

	wgpuSurfaceCapabilitiesFreeMembers_fn(capabilities);
	return true;
}

// (Re)shows the surface with the back buffer size and the present mode, the
// way ResizeBuffers does for the Direct3D 11 swap chain.  Nothing inside wgpu
// is thrown away for it, and the change happens on the next frame only.
bool WgpuDisplay::setPresentMode(bool withVsync)
{
	WGPUPresentMode wanted = WGPUPresentMode_Fifo;

	if (withVsync)
	{
		// Fifo, the one present mode every adapter reports, is the meaning of
		// the VSync option: the frame the thread hands over waits on the
		// vblank like a sync interval of one.
		wanted = WGPUPresentMode_Fifo;
	}
	else if (presentModeSupported[WGPUPresentMode_Mailbox])
	{
		// The closest thing to Present(0): the frame goes out as soon as a
		// free slot arrives, without tearing where the compositor lets it.
		wanted = WGPUPresentMode_Mailbox;
	}
	else if (presentModeSupported[WGPUPresentMode_Immediate])
	{
		wanted = WGPUPresentMode_Immediate;
	}

	if (wanted == presentMode)
		return true;

	WGPUSurfaceConfiguration configuration;
	configuration = WGPU_SURFACE_CONFIGURATION_INIT;
	configuration.device      = device;
	configuration.format      = surfaceFormat;
	configuration.usage       = WGPUTextureUsage_RenderAttachment;
	configuration.width       = surfaceWidth > 0 ? surfaceWidth : 1;
	configuration.height      = surfaceHeight > 0 ? surfaceHeight : 1;
	configuration.alphaMode   = WGPUCompositeAlphaMode_Auto;
	configuration.presentMode = wanted;

	bool configured = true;

	try
	{
		wgpuSurfaceConfigure_fn(surface, &configuration);
	}
	catch (...)
	{
		// A surface the library cannot take - the window still held by the
		// swap chain of a renderer that has not let go of it, a device that
		// was lost - is reported to the log callback and then thrown out as
		// a C++ exception.  What has to come out of that is a render method
		// that does not start, not the process: the caller falls back to the
		// next one in the menu, and the trace already carries the reason.
		configured = false;
	}

	if (!configured)
	{
		winlog(_T("wgpu: the surface cannot be configured here\n"));
		return false;
	}

	presentMode = wanted;

	winlog(_T("wgpu: present mode %hs\n"),
	       wanted == WGPUPresentMode_Fifo ? "Fifo (waiting on the vblank)" :
	       wanted == WGPUPresentMode_Mailbox ? "Mailbox (no wait on the vblank)" :
	       wanted == WGPUPresentMode_Immediate ? "Immediate (no wait on the vblank)" :
	       "Fifo");

	return true;
}

bool WgpuDisplay::createDevice()
{
	// The backend the menu asked for.  Automatic asks for none, which is the
	// request that lets the library pick - Direct3D 12 first on Windows, then
	// Vulkan - the way the entry read before there was a backend menu.
	WGPUBackendType wanted = wgpuRequestedBackendType(theApp.wgpuBackend);

	WGPURequestAdapterOptions options;
	options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
	options.powerPreference   = WGPUPowerPreference_HighPerformance;
	options.compatibleSurface = surface;
	options.backendType       = wanted;
	options.forceFallbackAdapter = false;

	adapter = requestAdapter(wgpuInstance, &options);

	// A backend this machine has no adapter for answers with nothing, and a
	// renderer that does not start for it would fall the whole render method
	// back to Direct3D 11 over a choice that only concerned one backend.  The
	// request is made again without one instead, and the trace says which
	// backend the adapter really came from, the menu names it too.
	if (adapter == NULL && wanted != WGPUBackendType_Undefined)
	{
		winlog(_T("wgpu: no adapter on backend %hs, the request is made without one\n"),
		       wgpuBackendTypeName((int)wanted));

		options.backendType = WGPUBackendType_Undefined;
		adapter = requestAdapter(wgpuInstance, &options);
	}

	if (adapter == NULL)
	{
		winlog(_T("wgpu: no adapter for this window\n"));
		return false;
	}

	nameBackend();

	// A software adapter advertises D3D12 on machines whose driver stack
	// cannot carry it, and WARP's first present wedges the process in a wait
	// even the task manager cannot break.  The Direct3D 11 renderer is the
	// one that does run there, so a software adapter is turned down here and
	// the render method falls back to it.
	if (!softwareAdapterAllowed() &&
	    adapterIsSoftware(adapterType))
	{
		winlog(_T("wgpu: %hs is a software adapter, WebGPU is not started on it\n"), adapterName);
		refused = true;
		wgpuAdapterRelease_fn(adapter);
		adapter = NULL;
		return false;
	}

	device = requestDevice(adapter);
	if (device == NULL)
	{
		winlog(_T("wgpu: no device comes back%hs%hs\n"), deviceError[0] != 0 ? ": " : "", deviceError);
		return false;
	}

	queue = wgpuDeviceGetQueue_fn(device);
	if (queue == NULL)
	{
		winlog(_T("wgpu: no queue comes back\n"));
		return false;
	}

	return true;
}

bool WgpuDisplay::createPipeline()
{
	WGPUShaderSourceWGSL source;
	source = WGPU_SHADER_SOURCE_WGSL_INIT;
	source.code = textInWgpu(g_quadShader);

	WGPUShaderModuleDescriptor shaderDescriptor;
	shaderDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
	shaderDescriptor.nextInChain = &source.chain;
	shaderDescriptor.label       = textInWgpu("VBA quads");

	shader = wgpuDeviceCreateShaderModule_fn(device, &shaderDescriptor);
	if (shader == NULL)
	{
		winlog(_T("wgpu: the shader module cannot be made\n"));
		return false;
	}

	WGPUBindGroupLayoutEntry layoutEntries[2];
	// The two bindings are the shader's: 0 is the texture and 1 the sampler.
	layoutEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
	layoutEntries[0].binding    = 0;
	layoutEntries[0].visibility = WGPUShaderStage_Fragment;
	layoutEntries[0].texture.sampleType    = WGPUTextureSampleType_Float;
	layoutEntries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
	layoutEntries[0].texture.multisampled  = false;

	layoutEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
	layoutEntries[1].binding    = 1;
	layoutEntries[1].visibility = WGPUShaderStage_Fragment;
	layoutEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;

	WGPUBindGroupLayoutDescriptor layoutDescriptor;
	layoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
	layoutDescriptor.entryCount = 2;
	layoutDescriptor.entries    = layoutEntries;
	layoutDescriptor.label      = textInWgpu("frame layout");

	bindGroupLayout = wgpuDeviceCreateBindGroupLayout_fn(device, &layoutDescriptor);
	if (bindGroupLayout == NULL)
	{
		winlog(_T("wgpu: the bind group layout cannot be made\n"));
		return false;
	}

	WGPUPipelineLayoutDescriptor layoutFull;
	layoutFull = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	layoutFull.bindGroupLayoutCount = 1;
	layoutFull.bindGroupLayouts     = &bindGroupLayout;
	layoutFull.label                = textInWgpu("frame pipeline");

	pipelineLayout = wgpuDeviceCreatePipelineLayout_fn(device, &layoutFull);
	if (pipelineLayout == NULL)
	{
		winlog(_T("wgpu: the pipeline layout cannot be made\n"));
		return false;
	}

	// The upscaler passes read the texture the pass before them made and are
	// handed their sizes in a uniform buffer, so their layout carries a third
	// binding.  The sampler is declared although the taps of the shaders that
	// land on texel centres read them with textureLoad: a layout entry the
	// shader does not use costs nothing, and the ones that do need it.
	//
	// The fourth and fifth are the second texture of a pass of a longer chain -
	// the frame for 2xBR's blend, which reads the flags the pass before it
	// made and the frame itself - and its sampler.  A shader of one texture
	// leaves them unread.
	WGPUBindGroupLayoutEntry upscalerEntries[5];

	upscalerEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
	upscalerEntries[0].binding    = 0;
	upscalerEntries[0].visibility = WGPUShaderStage_Fragment;
	upscalerEntries[0].texture.sampleType    = WGPUTextureSampleType_Float;
	upscalerEntries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
	upscalerEntries[0].texture.multisampled  = false;

	upscalerEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
	upscalerEntries[1].binding    = 1;
	upscalerEntries[1].visibility = WGPUShaderStage_Fragment;
	upscalerEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;

	upscalerEntries[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
	upscalerEntries[2].binding    = 2;
	upscalerEntries[2].visibility = WGPUShaderStage_Fragment;
	upscalerEntries[2].buffer.type            = WGPUBufferBindingType_Uniform;
	upscalerEntries[2].buffer.hasDynamicOffset = false;
	upscalerEntries[2].buffer.minBindingSize  = sizeof(WgpuUpscalerConstants);

	upscalerEntries[3] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
	upscalerEntries[3].binding    = 3;
	upscalerEntries[3].visibility = WGPUShaderStage_Fragment;
	upscalerEntries[3].texture.sampleType    = WGPUTextureSampleType_Float;
	upscalerEntries[3].texture.viewDimension = WGPUTextureViewDimension_2D;
	upscalerEntries[3].texture.multisampled  = false;

	upscalerEntries[4] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
	upscalerEntries[4].binding    = 4;
	upscalerEntries[4].visibility = WGPUShaderStage_Fragment;
	upscalerEntries[4].sampler.type = WGPUSamplerBindingType_Filtering;

	WGPUBindGroupLayoutDescriptor upscalerLayoutDescriptor;
	upscalerLayoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
	upscalerLayoutDescriptor.entryCount = 5;
	upscalerLayoutDescriptor.entries    = upscalerEntries;
	upscalerLayoutDescriptor.label      = textInWgpu("upscaler layout");

	upscalerGroupLayout = wgpuDeviceCreateBindGroupLayout_fn(device, &upscalerLayoutDescriptor);
	if (upscalerGroupLayout == NULL)
	{
		winlog(_T("wgpu: the upscaler layout cannot be made\n"));
		return false;
	}

	WGPUPipelineLayoutDescriptor upscalerFull;
	upscalerFull = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	upscalerFull.bindGroupLayoutCount = 1;
	upscalerFull.bindGroupLayouts     = &upscalerGroupLayout;
	upscalerFull.label                = textInWgpu("upscaler pipeline");

	upscalerPipelineLayout = wgpuDeviceCreatePipelineLayout_fn(device, &upscalerFull);
	if (upscalerPipelineLayout == NULL)
	{
		winlog(_T("wgpu: the upscaler pipeline layout cannot be made\n"));
		return false;
	}

	WGPUVertexAttribute attributes[2];
	attributes[0] = WGPU_VERTEX_ATTRIBUTE_INIT;
	attributes[0].shaderLocation = 0;
	attributes[0].offset         = offsetof(WGPUVERTEX, x);
	attributes[0].format         = WGPUVertexFormat_Float32x2;

	attributes[1] = WGPU_VERTEX_ATTRIBUTE_INIT;
	attributes[1].shaderLocation = 1;
	attributes[1].offset         = offsetof(WGPUVERTEX, u);
	attributes[1].format         = WGPUVertexFormat_Float32x2;

	WGPUVertexBufferLayout bufferLayout;
	bufferLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
	bufferLayout.stepMode       = WGPUVertexStepMode_Vertex;
	bufferLayout.arrayStride    = sizeof(WGPUVERTEX);
	bufferLayout.attributeCount = 2;
	bufferLayout.attributes     = attributes;

	WGPUVertexState vertex;
	vertex = WGPU_VERTEX_STATE_INIT;
	vertex.module       = shader;
	vertex.entryPoint   = textInWgpu("vsMain");
	vertex.bufferCount  = 1;
	vertex.buffers      = &bufferLayout;

	// The frame, and the overlay as one alpha blended over the frame beneath
	// it: exactly the blend state the Direct3D 11 renderer carries.
	WGPUBlendComponent frameColor;
	frameColor = WGPU_BLEND_COMPONENT_INIT;
	frameColor.operation = WGPUBlendOperation_Add;
	frameColor.srcFactor = WGPUBlendFactor_Zero;
	frameColor.dstFactor = WGPUBlendFactor_One;

	WGPUBlendComponent overlayColor;
	overlayColor = WGPU_BLEND_COMPONENT_INIT;
	overlayColor.operation = WGPUBlendOperation_Add;
	overlayColor.srcFactor = WGPUBlendFactor_SrcAlpha;
	overlayColor.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

	WGPUBlendComponent overlayAlpha;
	overlayAlpha = WGPU_BLEND_COMPONENT_INIT;
	overlayAlpha.operation = WGPUBlendOperation_Add;
	overlayAlpha.srcFactor = WGPUBlendFactor_One;
	overlayAlpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

	WGPUBlendState overlayBlend;
	overlayBlend.color = overlayColor;
	overlayBlend.alpha = overlayAlpha;

	WGPUColorTargetState frameTarget;
	frameTarget          = WGPU_COLOR_TARGET_STATE_INIT;
	frameTarget.format   = surfaceFormat;
	frameTarget.blend    = NULL;
	frameTarget.writeMask = WGPUColorWriteMask_All;

	WGPUColorTargetState overlayTarget;
	overlayTarget        = WGPU_COLOR_TARGET_STATE_INIT;
	overlayTarget.format = surfaceFormat;
	overlayTarget.blend  = &overlayBlend;
	overlayTarget.writeMask = WGPUColorWriteMask_All;

	WGPUFragmentState fragment;
	fragment = WGPU_FRAGMENT_STATE_INIT;
	fragment.module      = shader;
	fragment.entryPoint  = textInWgpu("fsMain");
	fragment.targetCount = 1;
	fragment.targets     = &frameTarget;

	WGPURenderPipelineDescriptor pipelineDescriptor;
	pipelineDescriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	pipelineDescriptor.layout    = pipelineLayout;
	pipelineDescriptor.vertex    = vertex;
	pipelineDescriptor.primitive = WGPU_PRIMITIVE_STATE_INIT;
	pipelineDescriptor.primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
	pipelineDescriptor.primitive.cullMode = WGPUCullMode_None;
	pipelineDescriptor.multisample = WGPU_MULTISAMPLE_STATE_INIT;
	pipelineDescriptor.fragment  = &fragment;

	framePipeline = wgpuDeviceCreateRenderPipeline_fn(device, &pipelineDescriptor);

	fragment.targets = &overlayTarget;
	overlayPipeline = wgpuDeviceCreateRenderPipeline_fn(device, &pipelineDescriptor);

	if (framePipeline == NULL || overlayPipeline == NULL)
	{
		winlog(_T("wgpu: the frame%hs pipeline cannot be made\n"),
		       framePipeline == NULL && overlayPipeline == NULL ? " and the overlay" :
		       framePipeline == NULL ? "" : " or the overlay");
		return false;
	}

	WGPUSamplerDescriptor samplerDescriptor;
	samplerDescriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
	samplerDescriptor.addressModeU = WGPUAddressMode_ClampToEdge;
	samplerDescriptor.addressModeV = WGPUAddressMode_ClampToEdge;
	samplerDescriptor.addressModeW = WGPUAddressMode_ClampToEdge;
	samplerDescriptor.magFilter    = WGPUFilterMode_Nearest;
	samplerDescriptor.minFilter    = WGPUFilterMode_Nearest;
	samplerDescriptor.mipmapFilter = WGPUMipmapFilterMode_Nearest;
	samplerDescriptor.maxAnisotropy = 1;

	samplerPoint = wgpuDeviceCreateSampler_fn(device, &samplerDescriptor);

	samplerDescriptor.magFilter = WGPUFilterMode_Linear;
	samplerDescriptor.minFilter = WGPUFilterMode_Linear;
	samplerLinear = wgpuDeviceCreateSampler_fn(device, &samplerDescriptor);

	if (samplerPoint == NULL || samplerLinear == NULL)
	{
		winlog(_T("wgpu: the samplers cannot be made\n"));
		return false;
	}

	WGPUBufferDescriptor bufferDescriptor;
	bufferDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
	bufferDescriptor.size    = VERTEX_BUFFER_BYTES;
	bufferDescriptor.usage   = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
	bufferDescriptor.label   = textInWgpu("quads");
	bufferDescriptor.mappedAtCreation = false;

	vertexBuffer = wgpuDeviceCreateBuffer_fn(device, &bufferDescriptor);
	if (vertexBuffer == NULL)
	{
		winlog(_T("wgpu: the vertex buffer cannot be made\n"));
		return false;
	}

	// The on screen display quad never changes: it covers the back buffer,
	// see the draw in render().
	WGPUVERTEX displayQuad[4];
	displayQuad[0].x = -1.0f; displayQuad[0].y = -1.0f; displayQuad[0].u = 0.0f; displayQuad[0].v = 1.0f;
	displayQuad[1].x = -1.0f; displayQuad[1].y =  1.0f; displayQuad[1].u = 0.0f; displayQuad[1].v = 0.0f;
	displayQuad[2].x =  1.0f; displayQuad[2].y = -1.0f; displayQuad[2].u = 1.0f; displayQuad[2].v = 1.0f;
	displayQuad[3].x =  1.0f; displayQuad[3].y =  1.0f; displayQuad[3].u = 1.0f; displayQuad[3].v = 0.0f;

	wgpuQueueWriteBuffer_fn(queue, vertexBuffer, OVERLAY_QUAD_OFFSET, displayQuad, sizeof(displayQuad));

	return true;
}

bool WgpuDisplay::initialize()
{
	// A machine the renderer cannot run on - no library, or no adapter beyond
	// the software one - is turned down here, before a surface or a device is
	// made: the menu entry follows the same answer, so this only happens when
	// the settings still carry WebGPU, and the render method then falls back
	// without a message box.
	if (!wgpuAvailable())
	{
		winlog(_T("wgpu: the renderer cannot run here, it is not started\n"));
		refused = true;
		return false;
	}

	CWnd *pWnd = theApp.winRenderWnd();

	if (pWnd == NULL || pWnd->GetSafeHwnd() == NULL)
	{
		winlog(_T("wgpu: no window to draw into\n"));
		return false;
	}

	hwnd       = pWnd->GetSafeHwnd();
	backWidth  = theApp.surfaceSizeX;
	backHeight = theApp.surfaceSizeY;


	// Pick the emulator pixel format from the desktop mode, like the
	// Direct3D backends do.
	HDC dc = GetDC(hwnd);
	int bits = GetDeviceCaps(dc, BITSPIXEL);
	ReleaseDC(hwnd, dc);

	if (bits <= 16)
	{
		systemColorDepth = 16;
		systemRedShift   = 11;
		systemGreenShift = 6;
		systemBlueShift  = 0;
		frameBgr565      = true;
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
		frameBgr565      = false;
		Init_2xSaI(32);
	}
	theApp.fsColorDepth = systemColorDepth;

	if (!createSurface())
	{
		systemMessage(0, "Failed to create the wgpu surface");
		return false;
	}

	if (!createDevice())
	{
		// A software adapter is not something to put a message box in front of:
		// the render method falls back to Direct3D 11 on its own, and the trace
		// says why.
		if (!refused)
			systemMessage(0, "Failed to create the wgpu device");
		return false;
	}

	if (!readSurfaceCapabilities())
	{
		systemMessage(0, "Failed to read the wgpu surface capabilities");
		return false;
	}
	if (!createPipeline())
	{
		systemMessage(0, "Failed to initialise the wgpu pipelines");
		return false;
	}

	// The surface starts with the window the way that is, and is reconfigured
	// for every size and every VSync change from there on.
	surfaceWidth  = backWidth > 0 ? backWidth : 1;
	surfaceHeight = backHeight > 0 ? backHeight : 1;

	if (!setPresentMode(theApp.vsync))
	{
		// The window was not taken.  That is what a render method that did not
		// start looks like, and the settings fall back to the next one on
		// their own; the trace carries the reason, so no dialog goes up.
		return false;
	}

	if (backWidth > 0 && backHeight > 0)
		makeOverlayTexture(backWidth, backHeight);

	filterMode = regQueryDwordValue(_T("wgpuFilter"), 0);
	if (filterMode < 0 || filterMode > 1)
		filterMode = 0;

	systemUpdateColorMaps();

	theApp.updateFilter();
	theApp.updateIFB();

	pWnd->DragAcceptFiles(TRUE);
	return true;
}

bool WgpuDisplay::changeRenderSize(int w, int h)
{
	if (w != width || h != height)
		makeFrameTexture(w, h);

	return calculateVertices();
}

void WgpuDisplay::resize(int w, int h)
{
	if (w <= 0 || h <= 0)
		return;

	backWidth  = w;
	backHeight = h;

	// The surface is only reconfigured: nothing inside wgpu is thrown away
	// and nothing else has to be rebuilt, which is what ResizeBuffers is like
	// without the render target that falls with it.  The present mode is
	// marked unset so the configure happens even when the mode itself stays
	// the same, or the surface would keep presenting the old size.
	surfaceWidth  = w;
	surfaceHeight = h;
	presentMode   = WGPUPresentMode_Undefined;
	setPresentMode(theApp.vsync);

	// The on screen display follows the back buffer, so it is remade with it,
	// which is what the Direct3D 11 renderer does for its swap chain resize.
	// It is remade whatever textMethod says: the target is made with the
	// surface and not with the OSD being on, and one left at the old size
	// would be drawn stretched over the whole back buffer the moment the OSD
	// is switched on without a resize in between.
	makeOverlayTexture(w, h);

	calculateVertices();
}

bool WgpuDisplay::calculateVertices()
{
	if (width <= 0 || height <= 0 || backWidth <= 0 || backHeight <= 0)
		return false;

	// The same normalised device coordinates the Direct3D 11 backend carries:
	// y grows up on the screen in both, and the first row of the texture is
	// the top of the picture in both, so the picture goes into the front the
	// right way up with the same vertices.
	float l = -1.0f;
	float r =  1.0f;
	float t =  1.0f;
	float b = -1.0f;

	float u = (float)theApp.rect.right  / (float)width;
	float v = (float)theApp.rect.bottom / (float)height;

	quad[0].x = l; quad[0].y = b; quad[0].u = 0.0f; quad[0].v = v;
	quad[1].x = l; quad[1].y = t; quad[1].u = 0.0f; quad[1].v = 0.0f;
	quad[2].x = r; quad[2].y = b; quad[2].u = u;    quad[2].v = v;
	quad[3].x = r; quad[3].y = t; quad[3].u = u;    quad[3].v = 0.0f;

	wgpuQueueWriteBuffer_fn(queue, vertexBuffer, 0, quad, sizeof(quad));

	return true;
}

void WgpuDisplay::setOption(const char *option, int value)
{
	if (!strcmp(option, "wgpuFilter"))
	{
		if (value == 0 || value == 1)
			filterMode = value;
	}
	else if (!strcmp(option, "upscaler"))
	{
		// The chain is built when the next frame is drawn, so all this has to
		// do is let go of the one that was there.
		releaseUpscaler();

		if (value != UPSCALER_OFF && wgpuUpscalerIndex(value) < 0)
		{
			winlog(_T("wgpu: upscaler %d has no WGSL port, Off is in effect\n"), value);
			systemScreenMessage("The wgpu renderer does not run that upscaler yet");
		}
		else
			winlog(_T("wgpu: upscaler %d picked, the chain is built with the next frame\n"), value);
	}
}

// Writes the frame the core side made into the texture the quad samples: the
// rows go over the row distance the copy_texture takes, and in a 565 upload
// every pixel is widened while it is there.  The speed text and the message
// text land on the same rows the Direct3D 11 renderer draws theirs on, only
// in the staging rows first, one padded row pitch apart.
void WgpuDisplay::updateFrameData()
{
	u8 *data = pix;
	int dataPitch = theApp.filterWidth * (systemColorDepth / 8) + (systemColorDepth == 24 ? 0 : 4);

	if (textMethod != 0) // do not draw Lua HUD to a video dump
	{
		systemClonePixBuffer(osd);
		data = osd;
		systemRenderLua((u8 *)data, dataPitch);
	}

	if (stagingFrame == NULL)
		return;

	if (theApp.filterFunction)
	{
		theApp.filterFunction(data + dataPitch,
		                      dataPitch,
		                      (u8 *)theApp.delta,
		                      stagingFrame,
		                      stagingSourceRowBytes,
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
		u8 *dstRow        = stagingFrame;
		int rowBytes      = copyX * bytesPerPixel;
		if (rowBytes > stagingSourceRowBytes)
			rowBytes = stagingSourceRowBytes;

		// The staging rows are the frame's, so a core that reports more rows
		// than the texture holds is clipped instead of written past it.
		if (copyY > stagingRows)
			copyY = stagingRows;

		for (int y = 0; y < copyY; y++)
		{
			memcpy(dstRow, srcRow, rowBytes);
			srcRow += srcPitch;
			dstRow += stagingSourceRowBytes;
		}
	}

	if (theApp.showSpeed && isFullscreenOption(theApp.videoOption))
	{
		char buffer[30];
		if (theApp.showSpeed == 1)
			sprintf(buffer, "%3d%%", systemSpeed);
		else
			sprintf(buffer, "%3d%%(%d, %d fps)", systemSpeed,
			        systemFrameSkip, theApp.showRenderedFrames);

		if (theApp.showSpeedTransparent)
			drawTextTransp(stagingFrame,
			               stagingSourceRowBytes,
			               theApp.rect.left + 10,
			               theApp.rect.bottom - 10,
			               buffer);
		else
			drawText(stagingFrame,
			         stagingSourceRowBytes,
			         theApp.rect.left + 10,
			         theApp.rect.bottom - 10,
			         buffer);
	}

	if (textMethod == 1)
		DrawTextMessages(stagingFrame, stagingSourceRowBytes,
		                 theApp.rect.left, theApp.rect.bottom);

	if (frameBgr565)
	{
		// Widen every 565 pixel into the RGBA8 rows the texture takes: three
		// 5-bit containers and one 6-bit container become the same colour
		// values the RGBA8 format carries, and the alpha fills out opaque.
		const unsigned char *src = stagingFrame;
		unsigned char       *dst = stagingTexture;

		for (int y = 0; y < height; y++)
		{
			const u16 *s = (const u16 *)src;
			unsigned char *d = dst;
			for (int x = 0; x < width; x++, s++)
			{
				unsigned v = *s;
				unsigned r = (v >> 11) & 0x1F;
				unsigned g = (v >> 5) & 0x3F;
				unsigned b = v & 0x1F;
				*d++ = (unsigned char)((r << 3) | (r >> 2));
				*d++ = (unsigned char)((g << 2) | (g >> 4));
				*d++ = (unsigned char)((b << 3) | (b >> 2));
				*d++ = 0xFF;
			}
			src += stagingSourceRowBytes;
			dst += stagingTextureRowBytes;
		}
	}

	WGPUTexelCopyTextureInfo destination;
	destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	destination.texture  = frameTexture;
	destination.mipLevel = 0;
	destination.aspect   = WGPUTextureAspect_All;

	WGPUTexelCopyBufferLayout layout;
	layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
	layout.bytesPerRow = (uint32_t)stagingTextureRowBytes;

	WGPUExtent3D writeSize;
	writeSize.width             = (uint32_t)width;
	writeSize.height            = (uint32_t)height;
	writeSize.depthOrArrayLayers = 1;

	wgpuQueueWriteTexture_fn(queue, &destination, stagingTexture,
	                      (size_t)stagingTextureRowBytes * stagingRows,
	                      &layout, &writeSize);
}

// The on screen display lands in the texture the same way, once per change of
// what it says.
void WgpuDisplay::uploadOverlay()
{
	const unsigned char *src = overlay.pixels();
	int srcPitch = overlay.pitch();
	int overlayHeight = overlay.height();

	if (stagingOverlay == NULL)
		return;

	for (int y = 0; y < overlayHeight; y++)
		memcpy(stagingOverlay + (size_t)y * stagingOverlayRowBytes,
		       src + (size_t)y * srcPitch,
		       overlay.width() * 4);

	WGPUTexelCopyTextureInfo destination;
	destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	destination.texture  = overlayTexture;
	destination.mipLevel = 0;
	destination.aspect   = WGPUTextureAspect_All;

	WGPUTexelCopyBufferLayout layout;
	layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
	layout.bytesPerRow = (uint32_t)stagingOverlayRowBytes;

	WGPUExtent3D writeSize;
	writeSize.width             = (uint32_t)overlay.width();
	writeSize.height            = (uint32_t)overlayHeight;
	writeSize.depthOrArrayLayers = 1;

	wgpuQueueWriteTexture_fn(queue, &destination, stagingOverlay,
	                      (size_t)stagingOverlayRowBytes * overlayHeight,
	                      &layout, &writeSize);
}

// One pass of a chain: the quad the vertex shader makes out of the vertex
// index, drawn over the whole of its target.  It covers every pixel of it, so
// the pass clears rather than loads - there is nothing in the target worth
// keeping, and a clear is what the tile based backends want.
static void drawUpscalerPass(WGPUCommandEncoder encoder, WGPURenderPipeline pipeline,
                             WGPUBindGroup group, WGPUTextureView target)
{
	WGPURenderPassColorAttachment attachment;
	attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	attachment.view    = target;
	attachment.loadOp  = WGPULoadOp_Clear;
	attachment.storeOp = WGPUStoreOp_Store;
	attachment.clearValue.r = 0.0;
	attachment.clearValue.g = 0.0;
	attachment.clearValue.b = 0.0;
	attachment.clearValue.a = 1.0;

	WGPURenderPassDescriptor descriptor;
	descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	descriptor.colorAttachmentCount = 1;
	descriptor.colorAttachments     = &attachment;

	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass_fn(encoder, &descriptor);
	if (pass == NULL)
		return;

	wgpuRenderPassEncoderSetPipeline_fn(pass, pipeline);
	wgpuRenderPassEncoderSetBindGroup_fn(pass, 0, group, 0, NULL);
	wgpuRenderPassEncoderDraw_fn(pass, 4, 1, 0, 0);
	wgpuRenderPassEncoderEnd_fn(pass);
	wgpuRenderPassEncoderRelease_fn(pass);
}

// A pipeline of one pass, for one target format: the chain's own targets are
// RGBA8, the back buffer is whatever the surface came with, and the last pass
// needs both.
static bool createUpscalerPipeline(WGPUDevice device, WGPUPipelineLayout layout,
                                   WGPUShaderModule module, WGPUTextureFormat format,
                                   const char *name, WGPURenderPipeline *pipeline)
{
	WGPUVertexState vertex;
	vertex = WGPU_VERTEX_STATE_INIT;
	vertex.module      = module;
	vertex.entryPoint  = textInWgpu("vsMain");
	vertex.bufferCount = 0; // the vertex index makes the quad

	WGPUColorTargetState target;
	target = WGPU_COLOR_TARGET_STATE_INIT;
	target.format    = format;
	target.blend     = NULL;
	target.writeMask = WGPUColorWriteMask_All;

	WGPUFragmentState fragment;
	fragment = WGPU_FRAGMENT_STATE_INIT;
	fragment.module      = module;
	fragment.entryPoint  = textInWgpu("fsMain");
	fragment.targetCount = 1;
	fragment.targets     = &target;

	WGPURenderPipelineDescriptor descriptor;
	descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	descriptor.label     = textInWgpu(name);
	descriptor.layout    = layout;
	descriptor.vertex    = vertex;
	descriptor.primitive = WGPU_PRIMITIVE_STATE_INIT;
	descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
	descriptor.primitive.cullMode = WGPUCullMode_None;
	descriptor.multisample = WGPU_MULTISAMPLE_STATE_INIT;
	descriptor.fragment  = &fragment;

	*pipeline = wgpuDeviceCreateRenderPipeline_fn(device, &descriptor);

	return *pipeline != NULL;
}

void WgpuDisplay::releaseUpscalerTargets()
{
	for (int pass = 0; pass < WGPU_MAX_PASSES; pass++)
	{
		WgpuUpscalerPass &state = upscalerPasses[pass];

		if (state.group)     { wgpuBindGroupRelease_fn(state.group);  state.group = NULL; }
		if (state.constants) { wgpuBufferRelease_fn(state.constants); state.constants = NULL; }
		if (state.view)      { wgpuTextureViewRelease_fn(state.view); state.view = NULL; }
		if (state.texture)   { wgpuTextureRelease_fn(state.texture);  state.texture = NULL; }
	}

	if (upscalerBlitGroup) { wgpuBindGroupRelease_fn(upscalerBlitGroup); upscalerBlitGroup = NULL; }

	upscalerPassCount    = 0;
	upscalerFrameWidth   = 0;
	upscalerFrameHeight  = 0;
	upscalerTargetWidth  = 0;
	upscalerTargetHeight = 0;
}

void WgpuDisplay::releaseUpscaler()
{
	releaseUpscalerTargets();

	for (int pass = 0; pass < WGPU_MAX_PASSES; pass++)
	{
		if (upscalerPasses[pass].pipeline)
		{
			wgpuRenderPipelineRelease_fn(upscalerPasses[pass].pipeline);
			upscalerPasses[pass].pipeline = NULL;
		}
	}

	if (upscalerSurfacePipeline) { wgpuRenderPipelineRelease_fn(upscalerSurfacePipeline); upscalerSurfacePipeline = NULL; }

	for (int pass = 0; pass < WGPU_MAX_PASSES; pass++)
	{
		if (upscalerModules[pass])
		{
			wgpuShaderModuleRelease_fn(upscalerModules[pass]);
			upscalerModules[pass] = NULL;
		}
	}

	upscalerValue  = -1;
	upscalerFailed = false;
}

// Builds what the menu selected, and only that: the pipelines when the
// selection changes, the targets when the sizes do.  False leaves the frame
// drawn as it is, which is what a renderer without a port of the selected
// shader does.
bool WgpuDisplay::prepareUpscaler()
{
	int index = wgpuUpscalerIndex(theApp.upscaler);

	if (index < 0 || device == NULL || queue == NULL || frameTexture == NULL || frameView == NULL ||
	    upscalerGroupLayout == NULL || upscalerPipelineLayout == NULL)
	{
		releaseUpscalerTargets();
		return false;
	}

	const WgpuUpscalerSource &source = g_wgpuUpscalers[index];

	if (upscalerValue != source.value)
	{
		releaseUpscaler();

		upscalerValue = source.value;

		// One module per pass: the passes of a chain longer than one are files
		// of their own, not entry points of one.
		bool built = true;

		for (int pass = 0; pass < source.passCount && built; pass++)
		{
			WGPUShaderSourceWGSL code;
			code = WGPU_SHADER_SOURCE_WGSL_INIT;
			code.code = resourceInWgpu(source.resources[pass]);

			WGPUShaderModuleDescriptor moduleDescriptor;
			moduleDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
			moduleDescriptor.nextInChain = &code.chain;
			moduleDescriptor.label       = textInWgpu(source.name);

			if (code.code.data == NULL)
				built = false;
			else
				upscalerModules[pass] = wgpuDeviceCreateShaderModule_fn(device, &moduleDescriptor);

			if (upscalerModules[pass] != NULL)
				built = createUpscalerPipeline(device, upscalerPipelineLayout, upscalerModules[pass],
				                               WGPUTextureFormat_RGBA8Unorm, source.name,
				                               &upscalerPasses[pass].pipeline);
			else
				built = false;
		}

		// The last pass writes the back buffer whenever it comes out at its
		// size, and the surface format is not the one of the targets.
		if (built)
			built = createUpscalerPipeline(device, upscalerPipelineLayout,
			                               upscalerModules[source.passCount - 1],
			                               surfaceFormat, source.name, &upscalerSurfacePipeline);

		if (!built)
		{
			winlog(_T("wgpu: the %hs upscaler is not available on this device\n"), source.name);
			releaseUpscaler();
			upscalerValue  = source.value;
			upscalerFailed = true;
			return false;
		}

		winlog(_T("wgpu: the %hs upscaler is ready, %d pass(es)\n"), source.name, source.passCount);
	}

	if (upscalerFailed)
		return false;

	// The window, unless the user capped the ratio the upscaler may reach.
	int targetWidth, targetHeight;
	upscalerTargetSize(width, height, backWidth, backHeight,
	                   theApp.upscalerMaxScale, &targetWidth, &targetHeight);

	if (upscalerPassCount != source.passCount ||
	    upscalerFrameWidth != width || upscalerFrameHeight != height ||
	    upscalerTargetWidth != targetWidth || upscalerTargetHeight != targetHeight)
	{
		releaseUpscalerTargets();

		upscalerFrameWidth   = width;
		upscalerFrameHeight  = height;
		upscalerTargetWidth  = targetWidth;
		upscalerTargetHeight = targetHeight;

		WGPUTextureView input = frameView;
		int inputWidth  = width;
		int inputHeight = height;

		// What each pass made, so that a later one can be handed it as its
		// second texture: 2xBR's blend reads the flags of the pass in front of
		// it and the frame together.
		WGPUTextureView passViews[WGPU_MAX_PASSES];
		int passWidths[WGPU_MAX_PASSES];
		int passHeights[WGPU_MAX_PASSES];

		for (int pass = 0; pass < source.passCount; pass++)
		{
			WgpuUpscalerPass &state = upscalerPasses[pass];

			int second = source.seconds[pass];
			WGPUTextureView secondView;
			int secondWidth, secondHeight;

			if (second <= 0)
			{
				secondView   = frameView;
				secondWidth  = width;
				secondHeight = height;
			}
			else
			{
				secondView   = passViews[second - 1];
				secondWidth  = passWidths[second - 1];
				secondHeight = passHeights[second - 1];
			}

			// A fixed scale is a multiple of the frame rather than of the
			// pass before it, which is what the presets ask for.
			int passWidth  = source.scales[pass] > 0 ? width  * source.scales[pass] : targetWidth;
			int passHeight = source.scales[pass] > 0 ? height * source.scales[pass] : targetHeight;

			WGPUExtent3D size;
			size.width              = (uint32_t)passWidth;
			size.height             = (uint32_t)passHeight;
			size.depthOrArrayLayers = 1;

			WGPUTextureDescriptor textureDescriptor;
			textureDescriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
			textureDescriptor.size          = size;
			textureDescriptor.mipLevelCount = 1;
			textureDescriptor.sampleCount   = 1;
			textureDescriptor.dimension     = WGPUTextureDimension_2D;
			textureDescriptor.format        = WGPUTextureFormat_RGBA8Unorm;
			textureDescriptor.usage         = WGPUTextureUsage_RenderAttachment |
			                                  WGPUTextureUsage_TextureBinding;
			textureDescriptor.label         = textInWgpu(source.name);

			state.texture = wgpuDeviceCreateTexture_fn(device, &textureDescriptor);
			state.view    = state.texture != NULL ? wgpuTextureCreateView_fn(state.texture, NULL) : NULL;

			WGPUBufferDescriptor bufferDescriptor;
			bufferDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
			bufferDescriptor.size    = sizeof(WgpuUpscalerConstants);
			bufferDescriptor.usage   = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
			bufferDescriptor.label   = textInWgpu(source.name);
			bufferDescriptor.mappedAtCreation = false;

			state.constants = wgpuDeviceCreateBuffer_fn(device, &bufferDescriptor);

			if (state.view == NULL || state.constants == NULL)
			{
				winlog(_T("wgpu: %hs pass %d cannot be made\n"), source.name, pass + 1);
				releaseUpscalerTargets();
				return false;
			}

			// The sizes of this pass, written once: they change when the
			// window or the cap does, and not per frame.
			WgpuUpscalerConstants constants;
			constants.sourceSize[0] = (float)inputWidth;
			constants.sourceSize[1] = (float)inputHeight;
			constants.sourceSize[2] = 1.0f / (float)inputWidth;
			constants.sourceSize[3] = 1.0f / (float)inputHeight;
			constants.outputSize[0] = (float)passWidth;
			constants.outputSize[1] = (float)passHeight;
			constants.outputSize[2] = 1.0f / (float)passWidth;
			constants.outputSize[3] = 1.0f / (float)passHeight;
			constants.scale[0]      = (float)passWidth  / (float)inputWidth;
			constants.scale[1]      = (float)passHeight / (float)inputHeight;
			constants.scale[2]      = 0.0f;
			constants.scale[3]      = 0.0f;
			constants.secondSize[0] = (float)secondWidth;
			constants.secondSize[1] = (float)secondHeight;
			constants.secondSize[2] = 1.0f / (float)secondWidth;
			constants.secondSize[3] = 1.0f / (float)secondHeight;

			wgpuQueueWriteBuffer_fn(queue, state.constants, 0, &constants, sizeof(constants));

			WGPUBindGroupEntry entries[5];
			entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
			entries[0].binding     = 0;
			entries[0].textureView = input;

			entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
			entries[1].binding = 1;
			entries[1].sampler = samplerPoint;

			entries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
			entries[2].binding = 2;
			entries[2].buffer  = state.constants;
			entries[2].offset  = 0;
			entries[2].size    = sizeof(WgpuUpscalerConstants);

			entries[3] = WGPU_BIND_GROUP_ENTRY_INIT;
			entries[3].binding     = 3;
			entries[3].textureView = secondView;

			entries[4] = WGPU_BIND_GROUP_ENTRY_INIT;
			entries[4].binding = 4;
			entries[4].sampler = samplerPoint;

			WGPUBindGroupDescriptor groupDescriptor;
			groupDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
			groupDescriptor.layout     = upscalerGroupLayout;
			groupDescriptor.entryCount = 5;
			groupDescriptor.entries    = entries;

			state.group = wgpuDeviceCreateBindGroup_fn(device, &groupDescriptor);

			if (state.group == NULL)
			{
				winlog(_T("wgpu: %hs pass %d has no bind group\n"), source.name, pass + 1);
				releaseUpscalerTargets();
				return false;
			}

			input       = state.view;
			inputWidth  = passWidth;
			inputHeight = passHeight;

			passViews[pass]   = state.view;
			passWidths[pass]  = passWidth;
			passHeights[pass] = passHeight;
		}

		// The capped path draws the last target over the back buffer with the
		// sampler stretching it the rest of the way.
		upscalerBlitGroup = makeBindGroup(upscalerPasses[source.passCount - 1].view, true);
		upscalerPassCount = source.passCount;

		if (upscalerBlitGroup == NULL)
		{
			releaseUpscalerTargets();
			return false;
		}

		winlog(_T("wgpu: %hs: %dx%d frame over %dx%d, pass(es) at %dx%d\n"),
		       source.name, width, height, backWidth, backHeight, targetWidth, targetHeight);
	}

	return upscalerPassCount == source.passCount;
}

// The pass and the present: whatever comes in front of the frame draws into
// the buffer the surface hands out, and that buffer is what ends up shown.
// The states the buffer comes back in are the ones the flip model swap chain
// hands over, and the way the Direct3D 11 backend reacts to them is what this
// mirrors: a lost or outdated buffer is reconfigured for, and the frame that
// was on its way leaves the screen for the next one.
bool WgpuDisplay::drawAndPresent(bool clearOnly)
{
	WGPUSurfaceTexture surfaceTexture;
	surfaceTexture = WGPU_SURFACE_TEXTURE_INIT;

	wgpuSurfaceGetCurrentTexture_fn(surface, &surfaceTexture);

	if (surfaceTexture.texture == NULL)
	{
		if (surfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
		    surfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_Lost)
			setPresentMode(theApp.vsync);

		// The status is worth one line per change, not one per frame: a
		// surface that cannot hand out a texture does it over and over until
		// the mode it was configured for is picked up again.
		static int lastStatus = -1;
		if (lastStatus != (int)surfaceTexture.status)
		{
			winlog(_T("wgpu: the surface gave no texture back (%d)\n"), (int)surfaceTexture.status);
			lastStatus = (int)surfaceTexture.status;
		}
		return false;
	}

	WGPUTextureView backBuffer = wgpuTextureCreateView_fn(surfaceTexture.texture, NULL);
	if (backBuffer == NULL)
	{
		winlog(_T("wgpu: the back buffer view cannot be made\n"));
		wgpuTextureRelease_fn(surfaceTexture.texture);
		return false;
	}

	WGPURenderPassColorAttachment colorAttachment;
	colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	colorAttachment.view    = backBuffer;
	colorAttachment.loadOp  = WGPULoadOp_Clear;
	colorAttachment.storeOp = WGPUStoreOp_Store;

	colorAttachment.clearValue.r = 0.0;
	colorAttachment.clearValue.g = 0.0;
	colorAttachment.clearValue.b = 0.0;
	colorAttachment.clearValue.a = 1.0;

	WGPURenderPassDescriptor passDescriptor;
	passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	passDescriptor.colorAttachmentCount = 1;
	passDescriptor.colorAttachments     = &colorAttachment;

	// No bind group means the frame texture could not be made, and a pass
	// drawn against one would be a validation error per frame: the pass is
	// then the clear it already is, and the rest of the path is the same.
	if (frameGroupPoint == NULL || frameGroupLinear == NULL)
		clearOnly = true;

	WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder_fn(device, NULL);
	if (encoder == NULL)
	{
		winlog(_T("wgpu: the command encoder cannot be made\n"));
		wgpuTextureViewRelease_fn(backBuffer);
		wgpuTextureRelease_fn(surfaceTexture.texture);
		return false;
	}

	// The upscaling chain, when the menu selected one this renderer has.  Its
	// passes are encoded before the pass that draws the back buffer, and the
	// last of them writes the back buffer itself when it comes out at its
	// size - which is the whole point of the last pass having a pipeline for
	// the surface format, and what saves the blit the other two renderers pay
	// for on every frame.
	bool chain  = !clearOnly && prepareUpscaler();
	bool direct = chain && upscalerTargetWidth == backWidth && upscalerTargetHeight == backHeight;

	if (chain)
	{
		int last = upscalerPassCount - 1;

		for (int i = 0; i <= last; i++)
		{
			bool toBackBuffer = direct && i == last;

			drawUpscalerPass(encoder,
			                 toBackBuffer ? upscalerSurfacePipeline : upscalerPasses[i].pipeline,
			                 upscalerPasses[i].group,
			                 toBackBuffer ? backBuffer : upscalerPasses[i].view);
		}
	}

	// What is left to draw on the back buffer is the on screen display, over
	// the picture the chain made.  With the chain in place and nothing to put
	// over it, no pass is opened at all.
	bool overlayPass = !direct || textMethod == 2;

	WGPURenderPassEncoder pass = NULL;

	if (overlayPass)
	{
		// The pass below is the overlay over what the chain drew, and it must
		// not clear it.
		colorAttachment.loadOp = direct ? WGPULoadOp_Load : WGPULoadOp_Clear;

		pass = wgpuCommandEncoderBeginRenderPass_fn(encoder, &passDescriptor);
		if (pass == NULL)
		{
			winlog(_T("wgpu: the render pass cannot be opened\n"));
			wgpuCommandEncoderRelease_fn(encoder);
			wgpuTextureViewRelease_fn(backBuffer);
			wgpuTextureRelease_fn(surfaceTexture.texture);
			return false;
		}
	}

	if (!clearOnly && pass != NULL)
	{
		if (chain && !direct)
		{
			// The cap asked for a smaller target than the window, so the last
			// one is stretched over the back buffer by the sampler.
			wgpuRenderPassEncoderSetPipeline_fn(pass, framePipeline);
			wgpuRenderPassEncoderSetBindGroup_fn(pass, 0, upscalerBlitGroup, 0, NULL);
			wgpuRenderPassEncoderSetVertexBuffer_fn(pass, 0, vertexBuffer, OVERLAY_QUAD_OFFSET, sizeof(quad));
			wgpuRenderPassEncoderDraw_fn(pass, 4, 1, 0, 0);
		}
		else if (!chain)
		{
			wgpuRenderPassEncoderSetPipeline_fn(pass, framePipeline);

			WGPUBindGroup frameGroup = filterMode == 1 ? frameGroupLinear : frameGroupPoint;
			wgpuRenderPassEncoderSetBindGroup_fn(pass, 0, frameGroup, 0, NULL);
			wgpuRenderPassEncoderSetVertexBuffer_fn(pass, 0, vertexBuffer, 0, sizeof(quad));
			wgpuRenderPassEncoderDraw_fn(pass, 4, 1, 0, 0);
		}

		// The overlay follows the back buffer, so it is made with the surface
		// and remade whenever the window changes size.  This covers a path
		// that recreated one of them without the other, like the D3D11 renderer.
		if (textMethod == 2 && overlayGroup == NULL && backWidth > 0 && backHeight > 0)
			makeOverlayTexture(backWidth, backHeight);

		if (textMethod == 2)
		{
			// The on screen display is the one path whose behaviour depends on
			// the backend, so the trace log says which state it was in, once
			// per change, the way the D3D11 renderer traces its own.
			int state;
			if (overlayGroup == NULL || overlay.empty())
			{
				state = 0;
			}
			else if (!overlay.update())
			{
				state = 1;
			}
			else
			{
				state = 2;
			}

			static int traced = -1;
			if (traced != state)
			{
				static const char *const names[] = { "no target", "nothing visible", "visible" };
				winlog(_T("wgpu: on screen display %hs\n"), names[state]);
				traced = state;
			}

			if (state == 2)
			{
				// The overlay is the size of the back buffer and only changes
				// when a message does, so it is uploaded again only then; the
				// texture still holds the last picture.
				if (overlay.changed())
					uploadOverlay();

				wgpuRenderPassEncoderSetPipeline_fn(pass, overlayPipeline);
				wgpuRenderPassEncoderSetBindGroup_fn(pass, 0, overlayGroup, 0, NULL);
				wgpuRenderPassEncoderSetVertexBuffer_fn(pass, 0, vertexBuffer, OVERLAY_QUAD_OFFSET, sizeof(quad));
				wgpuRenderPassEncoderDraw_fn(pass, 4, 1, 0, 0);
			}
		}
	}

	if (pass != NULL)
	{
		wgpuRenderPassEncoderEnd_fn(pass);
		wgpuRenderPassEncoderRelease_fn(pass);
	}

	WGPUCommandBuffer commands = wgpuCommandEncoderFinish_fn(encoder, NULL);
	wgpuCommandEncoderRelease_fn(encoder);

	if (commands == NULL)
	{
		winlog(_T("wgpu: the command buffer cannot be finished\n"));
		wgpuTextureViewRelease_fn(backBuffer);
		wgpuTextureRelease_fn(surfaceTexture.texture);
		return false;
	}

	wgpuQueueSubmit_fn(queue, 1, &commands);
	wgpuCommandBufferRelease_fn(commands);
	wgpuTextureViewRelease_fn(backBuffer);

	bool presented = wgpuSurfacePresent_fn(surface) == WGPUStatus_Success;

	if (!presented)
	{
		static bool logged = false;
		if (!logged)
		{
			winlog(_T("wgpu: the present failed\n"));
			logged = true;
		}
	}

	wgpuTextureRelease_fn(surfaceTexture.texture);

	return presented;
}

void WgpuDisplay::render()
{
	if (surface == NULL || device == NULL)
		return;

	// The frame texture exists from the first frame on: everything after that
	// rides the same texture until the size the frame comes with changes.  A
	// texture that could not be made leaves its bind groups gone with it, and
	// a pass drawn against those would be a validation error per frame, so
	// the frame is not drawn at all until one is there.
	if (frameTexture == NULL)
	{
		if (!changeRenderSize(theApp.rect.right, theApp.rect.bottom) || frameTexture == NULL)
			return;
	}

	if (!calculateVertices())
		return;

	// The VSync option is honoured like the Direct3D 11 backend honours the
	// interval it hands to Present, and like there, the fast forward path,
	// which feeds the sound device on the same thread, drops the wait rather
	// than starve the audio buffer.
	bool fastForward = speedup || theApp.frameSearchSkipping;
	setPresentMode(!fastForward ? theApp.vsync : false);

	updateFrameData();
	drawAndPresent(false);
}

void WgpuDisplay::clear()
{
	if (surface == NULL || device == NULL)
		return;

	// A window where the surface owns the client area only takes what the
	// surface throws at it, so the last frame of a closed ROM stays until the
	// pass erases a buffer and shows it, the way the Direct3D 11 clear does.
	drawAndPresent(true);
}

IDisplay *newWgpuDisplay()
{
	return new WgpuDisplay();
}

#else // ndef VBA_ENABLE_WGPU

bool wgpuAvailable()
{
	return false;
}

bool wgpuLibraryAvailable()
{
	return false;
}

const char *wgpuBackendModel()
{
	return NULL;
}

IDisplay *newWgpuDisplay()
{
	return NULL;
}

#endif // !VBA_ENABLE_WGPU
