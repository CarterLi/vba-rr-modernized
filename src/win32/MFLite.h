#ifndef VBA_WIN32_MFLITE_H
#define VBA_WIN32_MFLITE_H

#pragma once

// The Media Foundation entry points the recorder uses, loaded at run time so
// the emulator still starts where mfplat.dll is not there.  That is the same
// rule D3D11, DirectDraw, DirectSound, XAudio2 and wgpu follow; see
// README.md, "Nothing is imported statically".
//
// Only the translation units that raise _WIN32_WINNT to 0x0602 may include
// this header.  The SDK hides MFTEnumEx below Windows 7 and
// MFT_ENUM_HARDWARE_VENDOR_ID_Attribute below Windows 8, while the rest of
// the emulator still targets the XP API level (_WIN32_WINNT 0x0501).

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfreadwrite.h>
#include <mferror.h>

// mfuuid.lib is a static library of GUID constants only: it puts nothing in
// the import table, so linking it does not tie the exe to mfplat.dll.
#pragma comment(lib, "mfuuid.lib")

namespace mf
{
	// Loads mfplat.dll and mfreadwrite.dll and calls MFStartup once.
	// Reference counted, so enumerating while a recording is open does not
	// tear the platform down.  False when the DLLs are missing or the
	// platform refuses to start.
	bool startup();
	void shutdown();

	// NULL until startup() succeeded.
	extern HRESULT(WINAPI *enumTransforms)(REFGUID, UINT32, const MFT_REGISTER_TYPE_INFO *,
	                                       const MFT_REGISTER_TYPE_INFO *, IMFActivate ***, UINT32 *);
	extern HRESULT(WINAPI *createAttributes)(IMFAttributes **, UINT32);
	extern HRESULT(WINAPI *createMediaType)(IMFMediaType **);
	extern HRESULT(WINAPI *createMemoryBuffer)(DWORD, IMFMediaBuffer **);
	extern HRESULT(WINAPI *createSample)(IMFSample **);
	extern HRESULT(WINAPI *createSinkWriterFromURL)(LPCWSTR, IMFByteStream *, IMFAttributes *,
	                                                IMFSinkWriter **);

	// A COM apartment for the calling thread, released on destruction.  The
	// emulator does not otherwise hold one; ImageWrite.cpp does the same
	// thing around its WIC calls.
	//
	// multiThreaded picks COINIT_MULTITHREADED, which is what a worker thread
	// that only ever talks to Media Foundation wants: an STA would need a
	// message pump that such a thread does not have.
	class ComApartment
	{
	public:
		explicit ComApartment(bool multiThreaded = false);
		~ComApartment();

		bool ok() const { return m_ok; }

	private:
		bool m_ok;
		bool m_uninit;

		ComApartment(const ComApartment &);
		ComApartment &operator=(const ComApartment &);
	};
}

#endif // VBA_WIN32_MFLITE_H
