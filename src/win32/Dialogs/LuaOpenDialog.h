#if !defined(VBA_WIN32_LUAOPENDIALOG_H_INCLUDED)
#define VBA_WIN32_LUAOPENDIALOG_H_INCLUDED

#pragma once
// -*- C++ -*-
// LuaOpenDialog.h : header file
//
// One window per script.  "File -> Lua Scripting -> New Lua Script Window..."
// creates a new console every time; each window owns one VbaLuaState, so the
// scripts run side by side instead of replacing each other.

#include "../stdafx.h"
#include "../resource.h"
#include "../../common/LuaHost.h"
#include <cstdint>

// Creates a console window, cascaded after the ones already open.  The script
// it runs is chosen with the window's own path box and Run button.
HWND LuaWindowCreate(CWnd *parent);

// Creates a console window, fills in the given script and presses Run -- this
// is what a .lua file dropped on the main window does.
void LuaWindowRunScript(CWnd *parent, const TCHAR *path);

// Posts WM_CLOSE to every console window.
void LuaWindowsCloseAll();

bool LuaWindowsAny();

// Feeds one message to every console (the modeless dialogs need their own
// IsDialogMessage for tab/enter handling).  Returns true when consumed.
bool LuaWindowStepMessage(MSG *msg);

// The three console sinks the Lua engine writes through.  The handle is the
// dialog they belong to; the encoding says how the bytes of the text should be
// read (a script's strings take the encoding of its .lua file, everything the
// engine adds is ANSI).
void PrintToWindowConsole(std::intptr_t hDlgAsInt, lua_host::TextEncoding encoding,
	const char *str);
void WinLuaOnStart(std::intptr_t hDlgAsInt);
void WinLuaOnStop(std::intptr_t hDlgAsInt);

INT_PTR CALLBACK DlgLuaScriptDialog(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);

#endif // !defined(VBA_WIN32_LUAOPENDIALOG_H_INCLUDED)
