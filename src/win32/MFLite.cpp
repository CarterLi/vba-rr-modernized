// MFLite.cpp : the Media Foundation entry points the recorder uses.
//
// Nothing here is imported: mfplat.dll and mfreadwrite.dll are opened with
// LoadLibrary, and the emulator keeps starting on a system that has neither.
// The header explains why _WIN32_WINNT has to be raised for this file only.

// Declarations only: _WIN32_WINNT decides which SDK prototypes this file
// can see, not what the exe links against.  Every entry point used here is
// looked up with GetProcAddress, and none of them is newer than Windows 7,
// so the Windows 10 target costs nothing at run time -- it just unhides
// MFAudioFormat_FLAC / _ALAC / _Opus, which the SDK gates on WINVER.
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00

#include "MFLite.h"

namespace mf
{
	HRESULT(WINAPI *enumTransforms)(REFGUID, UINT32, const MFT_REGISTER_TYPE_INFO *,
	                                const MFT_REGISTER_TYPE_INFO *, IMFActivate ***, UINT32 *) = NULL;
	HRESULT(WINAPI *createAttributes)(IMFAttributes **, UINT32)				   = NULL;
	HRESULT(WINAPI *createMediaType)(IMFMediaType **)						   = NULL;
	HRESULT(WINAPI *createMemoryBuffer)(DWORD, IMFMediaBuffer **)			   = NULL;
	HRESULT(WINAPI *createSample)(IMFSample **)								   = NULL;
	HRESULT(WINAPI *createSinkWriterFromURL)(LPCWSTR, IMFByteStream *, IMFAttributes *,
	                                         IMFSinkWriter **)				   = NULL;

	namespace
	{
		HMODULE s_platform  = NULL;
		HMODULE s_readWrite = NULL;
		int		s_refs		= 0;
		bool	s_started	= false;

		HRESULT(WINAPI *s_startupPlatform)(ULONG, DWORD) = NULL;
		HRESULT(WINAPI *s_shutdownPlatform)()			 = NULL;

		// One GetProcAddress per entry point, all or nothing: a partially
		// resolved table would only move the failure further down.
		bool resolveAll()
		{
			FARPROC startup		= GetProcAddress(s_platform, "MFStartup");
			FARPROC shutdown	= GetProcAddress(s_platform, "MFShutdown");
			FARPROC enumerate	= GetProcAddress(s_platform, "MFTEnumEx");
			FARPROC attributes	= GetProcAddress(s_platform, "MFCreateAttributes");
			FARPROC mediaType	= GetProcAddress(s_platform, "MFCreateMediaType");
			FARPROC buffer		= GetProcAddress(s_platform, "MFCreateMemoryBuffer");
			FARPROC sample		= GetProcAddress(s_platform, "MFCreateSample");
			FARPROC sinkWriter	= GetProcAddress(s_readWrite, "MFCreateSinkWriterFromURL");

			if (!startup || !shutdown || !enumerate || !attributes || !mediaType || !buffer ||
			    !sample || !sinkWriter)
				return false;

			s_startupPlatform  = (HRESULT(WINAPI *)(ULONG, DWORD))startup;
			s_shutdownPlatform = (HRESULT(WINAPI *)())shutdown;
			enumTransforms	   = (HRESULT(WINAPI *)(REFGUID, UINT32, const MFT_REGISTER_TYPE_INFO *,
			                                        const MFT_REGISTER_TYPE_INFO *, IMFActivate ***,
			                                        UINT32 *))enumerate;
			createAttributes   = (HRESULT(WINAPI *)(IMFAttributes **, UINT32))attributes;
			createMediaType	   = (HRESULT(WINAPI *)(IMFMediaType **))mediaType;
			createMemoryBuffer = (HRESULT(WINAPI *)(DWORD, IMFMediaBuffer **))buffer;
			createSample	   = (HRESULT(WINAPI *)(IMFSample **))sample;
			createSinkWriterFromURL =
				(HRESULT(WINAPI *)(LPCWSTR, IMFByteStream *, IMFAttributes *, IMFSinkWriter **))sinkWriter;
			return true;
		}
	}

	bool startup()
	{
		if (s_started)
		{
			++s_refs;
			return true;
		}

		// The modules stay loaded once opened: a second recording session
		// would only open them again, and unloading them while a Media
		// Foundation object is still alive would be worse.
		if (s_platform == NULL)
			s_platform = LoadLibraryW(L"mfplat.dll");
		if (s_readWrite == NULL)
			s_readWrite = LoadLibraryW(L"mfreadwrite.dll");

		if (s_platform == NULL || s_readWrite == NULL || !resolveAll())
			return false;

		if (FAILED(s_startupPlatform(MF_VERSION, MFSTARTUP_FULL)))
			return false;

		s_started = true;
		s_refs	  = 1;
		return true;
	}

	void shutdown()
	{
		if (!s_started || s_refs <= 0)
			return;

		if (--s_refs > 0)
			return;

		s_shutdownPlatform();
		s_started = false;
	}

	// Same shape as beginCom() in ImageWrite.cpp: tolerate a caller that
	// already initialised COM, and only balance the call that succeeded here.
	ComApartment::ComApartment(bool multiThreaded)
	{
		m_ok	 = false;
		m_uninit = false;

		const DWORD model  = multiThreaded ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED;
		const HRESULT hr   = CoInitializeEx(NULL, model | COINIT_DISABLE_OLE1DDE);
		if (hr == S_OK || hr == S_FALSE)
		{
			m_uninit = true;
			m_ok	 = true;
			return;
		}

		// The thread is already in another apartment (an MTA, typically).
		// Media Foundation works there too, so this is not a failure.
		m_ok = hr == RPC_E_CHANGED_MODE;
	}

	ComApartment::~ComApartment()
	{
		if (m_uninit)
			CoUninitialize();
	}
}
