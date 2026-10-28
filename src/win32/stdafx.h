// stdafx.h : include file for standard system include files,
//  or project specific include files that are used frequently, but
//      are changed infrequently
//

// Some build target defines.
//
// The whole project compiles against the Windows 10 SDK level.  That decides
// which prototypes the SDK shows, not what the executable links against:
// every optional library (Media Foundation, Direct3D, DirectSound, XAudio2,
// DWM, wgpu, 7z) is opened with LoadLibrary and its entry points resolved
// with GetProcAddress, so the binary still starts on the older systems the
// README promises.  See README.md, "Nothing is imported statically".

#ifndef _WIN32_WINNT
#	define _WIN32_WINNT 0x0A00 // Windows 10
#endif

#ifndef WINVER
#	define WINVER 0x0A00 // Windows 10
#endif

#if !defined(AFX_STDAFX_H__A7126ECB_A234_4116_A7D0_BE50547E87F8__INCLUDED_)
#define AFX_STDAFX_H__A7126ECB_A234_4116_A7D0_BE50547E87F8__INCLUDED_

#pragma once

// Insert your headers here
//#define WIN32_LEAN_AND_MEAN           // Exclude rarely-used stuff from Windows headers

#include <afxwin.h>
#include <afxcmn.h>
#include <afxdlgs.h>

#include "../Port.h"
#include "WinText.h"

// TODO: reference additional headers your program requires here

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_STDAFX_H__A7126ECB_A234_4116_A7D0_BE50547E87F8__INCLUDED_)
