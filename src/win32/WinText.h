#ifndef VBA_WIN32_WINTEXT_H
#define VBA_WIN32_WINTEXT_H

#pragma once

// Where the Unicode Win32 layer meets the narrow emulation core.
//
// The core (src/common, src/gba, src/gb) passes strings around as `char *` and
// opens its files with the CRT's narrow calls, so those strings are byte
// strings in the process ANSI code page.  The Win32 layer is built with
// UNICODE and keeps everything in TCHAR.  Every crossing between the two goes
// through one of the helpers below, so there is a single place that decides
// what the narrow encoding is.
//
// Anything that stays inside the Win32 layer must not use these: it should be
// TCHAR all the way, otherwise the text is limited to what the ANSI code page
// can hold and translations outside it are lost.

#include <string>

// ANSI -> TCHAR.  NULL becomes an empty string.
extern CString winWideFromAnsi(const char *text);
extern CString winWideFromAnsi(const std::string &text);

// UTF-8 -> TCHAR, but only when the bytes really are UTF-8; anything else is
// read as ANSI instead.  This is what a Lua script's output needs, because a
// single string can mix the two: the script's own text comes out of the .lua
// file (UTF-8 if it is), while a file name or an English diagnostic appended
// to it comes from the core in the process code page.
extern CString winWideFromUtf8OrAnsi(const char *text);

// TCHAR -> ANSI.  Characters the code page cannot represent become '?'.
// The returned buffer is only valid until the end of the expression that
// created it, which is what the core-facing calls need.
extern CStringA winAnsiFromWide(LPCTSTR text);
extern CStringA winAnsiFromWide(const CString &text);

// Wide counterpart of the core's systemMessage(int, const char *, ...).  The
// core keeps the narrow one for its own diagnostics; the Win32 layer uses this
// one so the message text and its arguments stay TCHAR and a translated string
// is shown as the translation file wrote it.
extern void systemMessage(int number, const wchar_t *defaultMsg, ...);

// Wide counterpart of the core's systemSetTitle(const char *), for the same
// reason: the title is UI text and must not be squeezed through ANSI.
extern void systemSetTitle(const wchar_t *title);

// A screen message whose text is a string table entry rather than something
// the core assembled.  The message shows up to twice - over the picture and in
// the status bar - and the two fonts behind those have different repertoires,
// so the id is resolved twice: the built-in ASCII font that "in game" and
// "on game" use gets the executable's English string, while "on screen" and
// the status bar get the translation.  Arguments format both.
extern void systemScreenMessageRes(UINT id, ...);

// The driver trace (vba-trace-*.log), which every source in this layer writes
// to.  Declared once here instead of in each renderer: the format is TCHAR now,
// and a copy that said `const char *` would silently take the wrong arguments.
extern void winlog(LPCTSTR msg, ...);

#endif // VBA_WIN32_WINTEXT_H
