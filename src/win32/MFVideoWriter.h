#ifndef VBA_WIN32_MFVIDEOWRITER_H
#define VBA_WIN32_MFVIDEOWRITER_H

#pragma once

#include "VideoWriter.h"

// Writes what the recording dialog asked for through an IMFSinkWriter:
// MP4 with H.264/HEVC/AV1, or an audio-only .m4a / .mp3 / .flac.
//
// The sink writer owns the encoder, so no CLSID is handed to it; the only
// choice that reaches it is MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, which
// is what the "Hardware" row of the encoder list sets.
//
// Everything Media Foundation is behind a pimpl: the header stays usable
// from the rest of the Win32 layer, which is still compiled at the XP API
// level and must not see <mfapi.h>.
class MFVideoWriter : public IVideoWriter
{
public:
	MFVideoWriter(const RecordParams &params);
	virtual ~MFVideoWriter();

	virtual bool Open(LPCTSTR filename);

	virtual bool AddFrame(const u8 *bgr24BottomUp);
	virtual void SetVideoFormat(BITMAPINFOHEADER *bh);
	virtual int	 videoWidth() const;
	virtual int	 videoHeight() const;
	virtual int	 videoFrames();

	virtual bool IsSoundAdded();
	virtual void SetSoundFormat(WAVEFORMATEX *format);
	virtual bool AddSound(const u8 *sound, u32 len);

	virtual void Pause(bool pause);
	virtual bool IsPaused();

private:
	struct Impl;
	Impl *m_impl;

	MFVideoWriter(const MFVideoWriter &);
	MFVideoWriter &operator=(const MFVideoWriter &);
};

#endif // VBA_WIN32_MFVIDEOWRITER_H
