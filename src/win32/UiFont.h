#if !defined(VBA_WIN32_UIFONT_H_INCLUDED)
#define VBA_WIN32_UIFONT_H_INCLUDED

#pragma once

#include <windows.h>

// The font the rest of Windows uses, instead of the 8 point bitmap faces the
// dialog templates were drawn with in 2004.
//
// The face comes from NONCLIENTMETRICS.lfMessageFont, so it follows the
// system language: "Segoe UI" on an English install, "Microsoft YaHei UI" on
// a Chinese one, and whatever the user selected in the display settings.
//
// A monospaced variant is kept for the hex, disassembly and cheat views.
// That one is TrueType too; the stock SYSTEM_FIXED_FONT it replaces is the
// bitmap "Fixedsys".
namespace uiFont
{
	// Call once from InitInstance, before the first dialog is created.
	// Installs the hook that gives every dialog the message font.
	void init();

	// The system font can be turned off (Options > Preferences > Use Modern
	// UI Font): the dialogs then keep the 8 point bitmap face their
	// templates ask for.  Takes effect for dialogs opened afterwards.
	void setModern(bool enabled);
	bool modern();

	// System default UI font.  Never NULL after init().
	HFONT message();

	// Fixed pitch font for machine generated text.  Never NULL after init().
	// Falls back to the stock bitmap face while modern fonts are off.
	HFONT mono();

	// Sends WM_SETFONT to the dialog and all of its children.  Called by the
	// hook for every WM_INITDIALOG; call it directly for dialogs that are
	// built without a template.
	void applyToDialog(HWND dialog);

	// The DPI the dialog was last laid out at.
	UINT layoutDpi(HWND dialog);

	// The DPI of the monitor the window is on right now.  This is the live
	// value, so it already reports the new monitor while WM_DPICHANGED is
	// still being dispatched - layoutDpi() only catches up afterwards.
	UINT dpiOf(HWND window);

	// Scales every child by the ratio of the two DPIs.  Used by dialogs that
	// handle WM_DPICHANGED themselves (ResizeDlg and its derived viewers).
	void scaleDialog(HWND dialog, UINT fromDpi, UINT toDpi);

	// Rebuilds the fonts for the new DPI, rescales nothing.
	void onDpiChanged(HWND dialog, UINT oldDpi, UINT newDpi);

	// AdjustWindowRectEx for a specific DPI, with the old API as fallback.
	BOOL adjustWindowRectForDpi(RECT *rect, DWORD style, BOOL menu, DWORD exStyle, UINT dpi);

	// Tells the font hook that this dialog's WM_DPICHANGED is handled by the
	// dialog itself, so the hook must not scale it a second time.
	void markResizable(HWND dialog);
}

#endif // !defined(VBA_WIN32_UIFONT_H_INCLUDED)
