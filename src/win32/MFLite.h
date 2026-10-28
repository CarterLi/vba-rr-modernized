#ifndef VBA_WIN32_MFLITE_H
#define VBA_WIN32_MFLITE_H

#pragma once

// The entry points the recorder uses, loaded at run time so the emulator
// still starts where mfplat.dll is not there.  That is the same rule D3D11,
// DirectDraw, DirectSound, XAudio2 and wgpu follow; see README.md, "Nothing
// is imported statically".
//
// The project targets Windows 10 (see stdafx.h), so the SDK declares
// everything used here: MFTEnumEx appears from Windows 7 on and
// MFT_ENUM_HARDWARE_VENDOR_ID_Attribute from Windows 8 on.

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

	// NULL until startup() succeeded.  The pointer types are the SDK
	// prototypes themselves, so they cannot drift: MFTEnumEx takes its
	// category GUID by value, and a hand written REFGUID here shifted every
	// later argument by one register.
	extern decltype(&MFTEnumEx) enumTransforms;
	extern decltype(&MFCreateAttributes) createAttributes;
	extern decltype(&MFCreateMediaType) createMediaType;
	extern decltype(&MFCreateMemoryBuffer) createMemoryBuffer;
	extern decltype(&MFCreateSample) createSample;
	extern decltype(&MFCreateSinkWriterFromURL) createSinkWriterFromURL;

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
