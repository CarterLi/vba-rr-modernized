// UiFont.cpp : the system's own UI font for the emulator's dialogs, per
// monitor DPI aware.
//
// The dialog templates in vba.rc all ask for 8 point "MS Sans Serif", which
// is a bitmap face from the Windows 95 era.  Their control positions are in
// dialog units, and those are derived from the template font, so the face is
// replaced at run time instead of in the .rc.
//
// The font comes from NONCLIENTMETRICS for the DPI of the window that is
// being created, so a per monitor v2 process gets the right size on every
// monitor: "Segoe UI" on an English install, "Microsoft YaHei UI" on a
// Chinese one, scaled by the monitor's DPI.
//
// The system message font can be taller than the single line the templates
// were drawn for (YaHei UI is 17 pixels against 13).  A static control that
// is too short clips its text and wraps labels that used to fit on one line,
// so the face is used at the largest size that still fits the template's
// line height; one pixel of descender is tolerated.  Dialog units, and with
// them every control position, stay untouched.
//
// Dialogs are subclassed to handle WM_DPICHANGED: the controls are scaled by
// the ratio of the two DPIs and the font is rebuilt for the new one.  The
// resizable viewers derive from ResizeDlg and ask for the same through
// uiFont::scaleDialog() after they have told their own sizer about the new
// client size (otherwise the anchored controls would be moved twice).

// This translation unit includes windows.h directly instead of stdafx.h, so
// it sets the target level itself.  Declarations only: every entry point
// below is still resolved with GetProcAddress.
#define _WIN32_WINNT 0x0A00
#define WINVER       0x0A00

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <wchar.h>

#include "UiFont.h"

#pragma comment(lib, "comctl32.lib")

#if !defined(WM_DPICHANGED)
#	define WM_DPICHANGED 0x02E0
#endif

namespace
{
	decltype(&GetDpiForWindow)            g_getDpiForWindow = NULL;
	decltype(&GetDpiForSystem)            g_getDpiForSystem = NULL;
	decltype(&SystemParametersInfoForDpi) g_spiForDpi       = NULL;
	decltype(&AdjustWindowRectExForDpi)   g_adjustForDpi    = NULL;

	const int kMaxDpiSets = 8;

	struct FontSet
	{
		UINT  dpi;
		HFONT message;		// message font at this DPI
		HFONT fitted;		// message font cut down to fit a template line
		int   fitLimit;		// template line height the fitted font was made for
		HFONT mono;			// fixed pitch face at this DPI
	};

	FontSet g_sets[kMaxDpiSets];
	int     g_setCount = 0;
	HHOOK   g_hook     = NULL;
	bool    g_modern   = true;

	UINT systemDpi()
	{
		if (g_getDpiForSystem != NULL)
			return g_getDpiForSystem();

		HDC dc = GetDC(NULL);
		int dpi = GetDeviceCaps(dc, LOGPIXELSY);
		ReleaseDC(NULL, dc);
		return dpi > 0 ? (UINT)dpi : 96;
	}

	UINT windowDpi(HWND window)
	{
		if (window != NULL && g_getDpiForWindow != NULL)
		{
			UINT dpi = g_getDpiForWindow(window);
			if (dpi != 0)
				return dpi;
		}
		return systemDpi();
	}

	int fontHeight(HFONT font)
	{
		if (font == NULL)
			return 0;

		HDC dc = GetDC(NULL);
		HGDIOBJ old = SelectObject(dc, font);
		TEXTMETRICW metrics;
		BOOL ok = GetTextMetricsW(dc, &metrics);
		SelectObject(dc, old);
		ReleaseDC(NULL, dc);
		return ok ? metrics.tmHeight : 0;
	}

	int CALLBACK countFace(const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM param)
	{
		(*(int *)param)++;
		return 1;
	}

	bool faceExists(const wchar_t *face)
	{
		LOGFONTW filter;
		ZeroMemory(&filter, sizeof(filter));
		filter.lfCharSet = DEFAULT_CHARSET;
		wcscpy_s(filter.lfFaceName, face);

		int count = 0;
		HDC dc = GetDC(NULL);
		EnumFontFamiliesExW(dc, &filter, countFace, (LPARAM)&count, 0);
		ReleaseDC(NULL, dc);
		return count > 0;
	}

	void messageFontForDpi(UINT dpi, LOGFONTW *lf)
	{
		NONCLIENTMETRICSW metrics;
		ZeroMemory(&metrics, sizeof(metrics));
		metrics.cbSize = sizeof(metrics);

		bool ok = false;
		if (g_spiForDpi != NULL)
			ok = g_spiForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi) != FALSE;
		if (!ok)
			ok = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != FALSE;

		if (ok)
		{
			*lf = metrics.lfMessageFont;
			if (g_spiForDpi == NULL)
			{
				// Pre Windows 10: the metrics are 96 DPI ones.
				lf->lfHeight = MulDiv(lf->lfHeight, dpi, 96);
				lf->lfWidth  = MulDiv(lf->lfWidth, dpi, 96);
			}
			return;
		}

		ZeroMemory(lf, sizeof(*lf));
		lf->lfHeight = -MulDiv(9, dpi, 72);
		lf->lfWeight = FW_NORMAL;
		lf->lfCharSet = DEFAULT_CHARSET;
		wcscpy_s(lf->lfFaceName, L"MS Shell Dlg");
	}

	HFONT createMono(const LOGFONTW *message, UINT)
	{
		LOGFONTW lf = *message;
		lf.lfWeight         = FW_NORMAL;
		lf.lfItalic         = FALSE;
		lf.lfUnderline      = FALSE;
		lf.lfStrikeOut      = FALSE;
		lf.lfCharSet        = DEFAULT_CHARSET;
		lf.lfOutPrecision   = OUT_TT_ONLY_PRECIS;	// never a bitmap face
		lf.lfClipPrecision  = CLIP_DEFAULT_PRECIS;
		lf.lfQuality        = DEFAULT_QUALITY;
		lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;

		const wchar_t *candidates[] = { L"Consolas", L"Cascadia Mono", L"Courier New" };
		for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++)
		{
			if (!faceExists(candidates[i]))
				continue;

			wcscpy_s(lf.lfFaceName, candidates[i]);
			HFONT font = CreateFontIndirectW(&lf);
			if (font != NULL)
				return font;
		}

		return CreateFontIndirectW(&lf);
	}

	FontSet *setFor(UINT dpi)
	{
		for (int i = 0; i < g_setCount; i++)
		{
			if (g_sets[i].dpi == dpi)
				return &g_sets[i];
		}

		if (g_setCount >= kMaxDpiSets)
			return &g_sets[0];	// cannot happen with sane monitor counts

		FontSet *set = &g_sets[g_setCount++];
		ZeroMemory(set, sizeof(*set));
		set->dpi = dpi;
		set->fitLimit = -1;

		LOGFONTW lf;
		messageFontForDpi(dpi, &lf);
		set->message = CreateFontIndirectW(&lf);
		set->mono    = createMono(&lf, dpi);
		return set;
	}

	// Largest size of this DPI's message font that fits limit pixels.
	HFONT fittedFor(FontSet *set, int limit)
	{
		if (set == NULL || set->message == NULL || limit <= 0)
			return set != NULL ? set->message : NULL;

		if (set->fitted != NULL && set->fitLimit == limit)
			return set->fitted;

		if (set->fitted != NULL)
		{
			DeleteObject(set->fitted);
			set->fitted = NULL;
			set->fitLimit = -1;
		}

		if (fontHeight(set->message) <= limit + 1)
			return set->message;

		LOGFONTW lf;
		if (GetObjectW(set->message, sizeof(lf), &lf) == 0)
			return set->message;

		for (int height = lf.lfHeight + 1; height < -4; height++)
		{
			lf.lfHeight = height;
			HFONT candidate = CreateFontIndirectW(&lf);
			if (candidate == NULL)
				break;

			if (fontHeight(candidate) <= limit + 1)
			{
				set->fitted = candidate;
				set->fitLimit = limit;
				return set->fitted;
			}

			DeleteObject(candidate);
		}

		return set->message;
	}

	BOOL CALLBACK applyFont(HWND child, LPARAM font)
	{
		SendMessageW(child, WM_SETFONT, (WPARAM)font, FALSE);
		return TRUE;
	}

	struct ScaleContext
	{
		UINT from;
		UINT to;
	};

	BOOL CALLBACK scaleChild(HWND child, LPARAM param)
	{
		const ScaleContext *scale = reinterpret_cast<const ScaleContext *>(param);
		if (scale->from == scale->to)
			return TRUE;

		RECT rect;
		GetWindowRect(child, &rect);
		MapWindowPoints(NULL, GetParent(child), reinterpret_cast<POINT *>(&rect), 2);

		int x = MulDiv(rect.left, scale->to, scale->from);
		int y = MulDiv(rect.top, scale->to, scale->from);
		int w = MulDiv(rect.right - rect.left, scale->to, scale->from);
		int h = MulDiv(rect.bottom - rect.top, scale->to, scale->from);

		SetWindowPos(child, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
		return TRUE;
	}

	void storeLayout(HWND dialog, UINT dpi, int limit)
	{
		SetPropW(dialog, L"VbaUiDpi", (HANDLE)(UINT_PTR)dpi);
		SetPropW(dialog, L"VbaUiLimit", (HANDLE)(UINT_PTR)limit);
	}

	// Only the emulator's own templates are converted.  Message boxes, the
	// common file dialog and the font picker are system windows that already
	// ask for the right font; walking into them would shrink their text.
	bool isLegacyTemplate(HFONT templ)
	{
		if (templ == NULL)
			return false;

		LOGFONTW lf;
		if (GetObjectW(templ, sizeof(lf), &lf) == 0)
			return false;

		return _wcsicmp(lf.lfFaceName, L"MS Sans Serif") == 0
		    || _wcsicmp(lf.lfFaceName, L"MS Shell Dlg") == 0
		    || _wcsicmp(lf.lfFaceName, L"MS Shell Dlg 2") == 0;
	}

	void applyToDialogInternal(HWND dialog)
	{
		if (!g_modern || dialog == NULL)
			return;

		HFONT templ = (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0);
		if (!isLegacyTemplate(templ))
			return;

		UINT dpi = windowDpi(dialog);
		FontSet *set = setFor(dpi);
		if (set == NULL || set->message == NULL)
			return;

		int limit = (int)(INT_PTR)GetPropW(dialog, L"VbaUiLimit");
		if (limit <= 0)
		{
			// First time: the dialog manager has just set the template font.
			limit = fontHeight(templ);
			if (limit > 0)
				storeLayout(dialog, dpi, limit);
		}

		HFONT font = fittedFor(set, limit);
		if (font == NULL)
			return;

		SendMessageW(dialog, WM_SETFONT, (WPARAM)font, FALSE);
		EnumChildWindows(dialog, applyFont, (LPARAM)font);
	}

	struct FontSwap
	{
		HFONT from;
		HFONT to;
	};

	BOOL CALLBACK replaceFont(HWND child, LPARAM param)
	{
		const FontSwap *swap = reinterpret_cast<const FontSwap *>(param);

		if ((HFONT)SendMessageW(child, WM_GETFONT, 0, 0) == swap->from)
			SendMessageW(child, WM_SETFONT, (WPARAM)swap->to, FALSE);
		return TRUE;
	}

	void onDpiChangedInternal(HWND dialog, UINT oldDpi, UINT newDpi)
	{
		int oldLimit = (int)(INT_PTR)GetPropW(dialog, L"VbaUiLimit");
		int newLimit = oldLimit;

		if (newLimit > 0 && oldDpi != 0 && oldDpi != newDpi)
			newLimit = MulDiv(newLimit, newDpi, oldDpi);

		HFONT oldFont = fittedFor(setFor(oldDpi), oldLimit);

		storeLayout(dialog, newDpi, newLimit);

		FontSet *set = setFor(newDpi);
		if (set == NULL || set->message == NULL)
			return;

		HFONT newFont = fittedFor(set, newLimit);
		if (newFont == NULL || newFont == oldFont)
			return;

		// Only touch the controls that still carry the dialog font; the hex
		// and disassembly views have a fixed pitch font of their own.
		if ((HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0) == oldFont)
			SendMessageW(dialog, WM_SETFONT, (WPARAM)newFont, FALSE);

		FontSwap swap = { oldFont, newFont };
		EnumChildWindows(dialog, replaceFont, (LPARAM)&swap);	}

	LRESULT CALLBACK dialogProc(HWND dialog, UINT message, WPARAM wParam,
	                            LPARAM lParam, UINT_PTR, DWORD_PTR)
	{
		if (message == WM_DPICHANGED)
		{
			// The resizable viewers update their own cached client size first,
			// otherwise their sizer would move the anchored controls twice.
			if (GetPropW(dialog, L"VbaResizeDlg") == NULL)
			{
				UINT oldDpi = (UINT)(UINT_PTR)GetPropW(dialog, L"VbaUiDpi");
				UINT newDpi = LOWORD(wParam);
				if (oldDpi == 0)
					oldDpi = newDpi;

				ScaleContext scale = { oldDpi, newDpi };
				EnumChildWindows(dialog, scaleChild, (LPARAM)&scale);

				RECT *target = reinterpret_cast<RECT *>(lParam);
				SetWindowPos(dialog, NULL, target->left, target->top,
				             target->right - target->left,
				             target->bottom - target->top,
				             SWP_NOZORDER | SWP_NOACTIVATE);

				onDpiChangedInternal(dialog, oldDpi, newDpi);
				return 0;
			}
		}
		else if (message == WM_INITDIALOG)
		{
			applyToDialogInternal(dialog);
		}

		return DefSubclassProc(dialog, message, wParam, lParam);
	}

	LRESULT CALLBACK hookProc(int code, WPARAM wParam, LPARAM lParam)
	{
		if (code == HC_ACTION)
		{
			const CWPSTRUCT *info = reinterpret_cast<const CWPSTRUCT *>(lParam);
			if (info->message == WM_INITDIALOG)
			{
				applyToDialogInternal(info->hwnd);
				SetWindowSubclass(info->hwnd, dialogProc, 1, 0);
			}
		}

		return CallNextHookEx(g_hook, code, wParam, lParam);
	}
}

namespace uiFont
{
	void init()
	{
		if (g_hook != NULL)
			return;

		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		g_getDpiForWindow = reinterpret_cast<decltype(&GetDpiForWindow)>(
			GetProcAddress(user32, "GetDpiForWindow"));
		g_getDpiForSystem = reinterpret_cast<decltype(&GetDpiForSystem)>(
			GetProcAddress(user32, "GetDpiForSystem"));
		g_spiForDpi = reinterpret_cast<decltype(&SystemParametersInfoForDpi)>(
			GetProcAddress(user32, "SystemParametersInfoForDpi"));
		g_adjustForDpi = reinterpret_cast<decltype(&AdjustWindowRectExForDpi)>(
			GetProcAddress(user32, "AdjustWindowRectExForDpi"));

		setFor(systemDpi());

		g_hook = SetWindowsHookExW(WH_CALLWNDPROC, hookProc, NULL, GetCurrentThreadId());
	}

	HFONT message()
	{
		FontSet *set = setFor(systemDpi());
		return set != NULL ? set->message : NULL;
	}

	HFONT mono()
	{
		if (!g_modern)
			return (HFONT)GetStockObject(SYSTEM_FIXED_FONT);

		FontSet *set = setFor(systemDpi());
		return set != NULL ? set->mono : NULL;
	}

	void setModern(bool enabled)
	{
		g_modern = enabled;
	}

	bool modern()
	{
		return g_modern;
	}

	void applyToDialog(HWND dialog)
	{
		applyToDialogInternal(dialog);
	}

	void scaleDialog(HWND dialog, UINT fromDpi, UINT toDpi)
	{
		if (dialog == NULL || fromDpi == toDpi)
			return;

		ScaleContext scale = { fromDpi, toDpi };
		EnumChildWindows(dialog, scaleChild, (LPARAM)&scale);
	}

	UINT dpiOf(HWND window)
	{
		return windowDpi(window);
	}

	UINT layoutDpi(HWND dialog)
	{
		UINT dpi = (UINT)(UINT_PTR)GetPropW(dialog, L"VbaUiDpi");
		return dpi != 0 ? dpi : windowDpi(dialog);
	}

	void onDpiChanged(HWND dialog, UINT oldDpi, UINT newDpi)
	{
		if (dialog == NULL)
			return;

		if (oldDpi == 0)
			oldDpi = newDpi;
		onDpiChangedInternal(dialog, oldDpi, newDpi);
	}

	BOOL adjustWindowRectForDpi(RECT *rect, DWORD style, BOOL menu, DWORD exStyle, UINT dpi)
	{
		if (g_adjustForDpi != NULL)
			return g_adjustForDpi(rect, style, menu, exStyle, dpi);

		return AdjustWindowRectEx(rect, style, menu, exStyle);
	}

	void markResizable(HWND dialog)
	{
		if (dialog != NULL)
			SetPropW(dialog, L"VbaResizeDlg", (HANDLE)1);
	}
}
