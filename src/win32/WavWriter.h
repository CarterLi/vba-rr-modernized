#if !defined(AFX_WAVWRITER_H__BE6C9DE9_60E7_4192_9797_8C7F55B3CE46__INCLUDED_)
#define AFX_WAVWRITER_H__BE6C9DE9_60E7_4192_9797_8C7F55B3CE46__INCLUDED_

#pragma once

// -*- C++ -*-
// WavWriter.h: interface for the WavWriter class.
//

#include <mmreg.h>

class WavWriter  
{
 private:
  FILE *m_file;
  int m_len;
  long m_posSize;

 public:
  WavWriter();
  ~WavWriter();

  // The path is TCHAR like IVideoWriter::Open: the writer is Win32 side code
  // and opens the file itself, so a name the code page cannot hold does not
  // have to be squeezed through ANSI on the way in.
  bool Open(LPCTSTR name);
  void SetFormat(const WAVEFORMATEX *format);
  void AddSound(const u8 *data, int len);

 private:
  void Close();
};

#endif // !defined(AFX_WAVWRITER_H__BE6C9DE9_60E7_4192_9797_8C7F55B3CE46__INCLUDED_)
