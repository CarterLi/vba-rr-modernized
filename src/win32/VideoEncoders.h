#ifndef VBA_WIN32_VIDEOENCODERS_H
#define VBA_WIN32_VIDEOENCODERS_H

#pragma once

#include <windows.h>

#include <string>
#include <vector>

// One row of the video encoder list: a codec, plus whether it should be
// encoded on the GPU or on the CPU.
//
// The sink writer chooses the MFT itself -- there is no way to hand it a
// CLSID -- so a row means "this codec, hardware or software", not "this
// particular vendor".  The vendor name is shown for information only.
// See documentation/recording-settings-design.md, section 4.
struct VideoEncoderChoice
{
	GUID		 subtype;
	bool		 hardware;
	std::wstring name;	   // friendly name of a representative MFT
	std::wstring vendor;   // "NVIDIA", "Intel"... empty for software
	bool		 available;	   // false for the "None (audio only)" row
};

struct AudioEncoderChoice
{
	GUID		 subtype;
	std::wstring name;
	bool		 available;	   // false for the "None" row
};

struct RecordContainer
{
	const wchar_t *extension;	// L".mp4"
	int			   filterId;	// IDS_FILTER_MP4, the save dialog's filter string
};

namespace videoEncoders
{
	// True when Media Foundation is present and starts.
	bool available();

	// Fills both lists.  The last entry of each is the "None" row, so the
	// dialog can offer it without a special case.  Video rows come grouped by
	// codec with the hardware row first.
	void enumerate(std::vector<VideoEncoderChoice> &video, std::vector<AudioEncoderChoice> &audio);

	// NULL when the pair cannot be stored in one container, or when neither
	// is set.
	const RecordContainer *containerFor(const GUID &video, const GUID &audio);

	// "H.264", "HEVC", "AAC"...  Empty for an unknown GUID.
	const wchar_t *codecName(const GUID &subtype);

	// True for the codecs that carry a sample-exact stream, where a bit rate
	// target means nothing to the encoder.
	bool isLosslessAudio(const GUID &subtype);

	// Codec predicates, so the dialog can pick a profile list without
	// including the Media Foundation headers.
	bool isH264(const GUID &subtype);
	bool isHevc(const GUID &subtype);
	bool isAv1(const GUID &subtype);

	// How many emulator frames make one recorded frame at the requested rate.
	// 1 means every frame is written; 0 or a rate at least the native one also
	// gives 1.  The writer and the dialog's summary must agree, so both call
	// this.
	int videoFrameStep(double nativeFps, int targetFps);

	// What a first run records: H.264 and AAC.  Kept here so that callers
	// which cannot include the Media Foundation headers still get a value.
	GUID defaultVideoCodec();
	GUID defaultAudioCodec();

	bool		 isNullGuid(const GUID &guid);
	std::wstring guidToString(const GUID &guid);

	// Accepts the "{........-....-....-....-............}" form.  Empty text
	// and L"none" both mean "not recorded" and return false.
	bool guidFromString(const std::wstring &text, GUID *guid);
}

#endif // VBA_WIN32_VIDEOENCODERS_H
