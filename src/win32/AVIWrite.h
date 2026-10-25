#ifndef VBA_WIN32_AVIWRITE_H
#define VBA_WIN32_AVIWRITE_H

#pragma once

#include "VideoWriter.h"

// The legacy writer: Video for Windows, with the codec picked in the
// AVISaveOptions dialog when the file is opened.  Behaviour is unchanged;
// the recording settings dialog uses MFVideoWriter instead, and this one is
// only reachable from the "Legacy AVI Recording" submenu.
class AVIWrite : public IVideoWriter {
 public:
  AVIWrite();
  virtual ~AVIWrite();

  virtual bool Open(LPCTSTR filename);
  virtual bool AddFrame(const u8 *bmp);
  virtual void SetVideoFormat(BITMAPINFOHEADER *);
  virtual bool IsSoundAdded();
  virtual void SetSoundFormat(WAVEFORMATEX *);
  virtual bool AddSound(const u8 *sound, u32 len);
  virtual int videoFrames();
  virtual int videoWidth() const { return m_bitmap.biWidth; }
  virtual int videoHeight() const { return m_bitmap.biHeight; }
  virtual void Pause(bool pause);
  virtual bool IsPaused();

 private:
  WAVEFORMATEX m_soundFormat;
  BITMAPINFOHEADER m_bitmap;
  AVISTREAMINFO m_header;
  AVISTREAMINFO m_soundHeader;
  PAVIFILE m_file;
  PAVISTREAM m_stream;
  PAVISTREAM m_streamCompressed;
  PAVISTREAM m_streamSound;
  AVICOMPRESSOPTIONS m_options;
  AVICOMPRESSOPTIONS *m_arrayOptions[1];
  LONG m_videoFrames;
  LONG m_samplesSound;
  LONG m_videoFramesTotal;
  LONG m_samplesSoundTotal;
  LONG m_totalBytes;
  bool m_failed;
  int m_segmentNumber;
  bool m_usePrevOptions;
  bool m_pauseRecording;
  TCHAR m_aviFileName[MAX_PATH];
  TCHAR m_aviBaseName[MAX_PATH];
  TCHAR m_aviExtension[MAX_PATH];
  void CleanUp();
  bool NextSegment();
};

#endif // VBA_WIN32_AVIWRITE_H
