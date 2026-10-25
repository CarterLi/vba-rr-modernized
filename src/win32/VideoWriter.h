#ifndef VBA_WIN32_VIDEOWRITER_H
#define VBA_WIN32_VIDEOWRITER_H

#pragma once

#include <vfw.h>

#include "../Port.h"

// What the recording settings dialog decided, and what VBA::loadSettings()
// remembers between runs.  A zero GUID means "not recorded": video and audio
// can each be switched off, but the dialog never allows both at once.
// What the bit rate box accepts, in kbps.
enum
{
	VIDEO_BITRATE_MIN	  = 100,
	VIDEO_BITRATE_MAX	  = 100000,
	VIDEO_BITRATE_DEFAULT = 8000,

	// The Low / Medium / High entries of the preset box.
	VIDEO_BITRATE_LOW	 = 2000,
	VIDEO_BITRATE_MEDIUM = 4000,
	VIDEO_BITRATE_HIGH	 = 8000
};

enum
{
	AUDIO_BITRATE_MIN	  = 32,
	AUDIO_BITRATE_MAX	  = 512,
	AUDIO_BITRATE_DEFAULT = 192,

	AUDIO_BITRATE_LOW	  = 64,
	AUDIO_BITRATE_MEDIUM  = 128,
	AUDIO_BITRATE_HIGH	  = 192
};

// H.264 profiles, as eAVEncH264VProfile_* carries them.
//
// HEVC and AV1 have their own profile numbers, but there is nothing to pick:
// both offer exactly one 8-bit 4:2:0 profile (Main), and asking this machine
// for HEVC Main 10 makes the sink writer fail outright, because the frames
// reaching the encoder are 8-bit NV12.  So the dialog only offers a choice for
// H.264 and leaves the box on Auto and disabled for the other two.
enum
{
	VIDEO_PROFILE_AUTO = 0,

	H264_PROFILE_BASELINE = 66,
	H264_PROFILE_MAIN	  = 77,
	H264_PROFILE_HIGH	  = 100
};

// Frame rate box.  Zero keeps the emulator's own rate; a target below it
// drops frames instead of duplicating them, so the duration stays right.
enum
{
	FRAMERATE_NATIVE  = 0,
	FRAMERATE_DEFAULT = 0
};

struct RecordParams
{
	GUID videoSubtype;	 // MFVideoFormat_H264 / HEVC / AV1, or all zero
	bool videoHardware;	 // ask the sink writer for a hardware encoder MFT
	GUID audioSubtype;	 // MFAudioFormat_*, or all zero
	int	 videoBitrate;	 // kbps, VIDEO_BITRATE_*
	int	 audioBitrate;	 // kbps, AUDIO_BITRATE_*
	int	 profile;		 // profile value of the chosen codec, 0 = auto
	int	 frameRate;		 // FRAMERATE_NATIVE or a target in fps
	bool async;			 // hand the frames to a worker thread to encode
};

// One interface for the two writers: the Media Foundation one the recording
// dialog creates, and the VFW one the legacy AVI menu still uses.
//
// The frame handed to AddFrame() is the buffer the emulator already produces:
// 24bpp BGR, bottom-up, stride == width * 3.  The Media Foundation writer
// passes it as RGB24 when that stride is 4-byte aligned and otherwise converts
// it to RGB32, which keeps the capture path in WinSystem.cpp unchanged.
class IVideoWriter
{
public:
	virtual ~IVideoWriter() {}

	virtual bool Open(LPCTSTR filename) = 0;

	virtual bool AddFrame(const u8 *bgr24BottomUp) = 0;
	virtual void SetVideoFormat(BITMAPINFOHEADER *bh) = 0;
	virtual int	 videoWidth() const	 = 0;
	virtual int	 videoHeight() const = 0;
	virtual int	 videoFrames()		 = 0;

	virtual bool IsSoundAdded()							 = 0;
	virtual void SetSoundFormat(WAVEFORMATEX *format)	 = 0;
	virtual bool AddSound(const u8 *sound, u32 len)		 = 0;

	virtual void Pause(bool pause) = 0;
	virtual bool IsPaused()		   = 0;
};

#endif // VBA_WIN32_VIDEOWRITER_H
