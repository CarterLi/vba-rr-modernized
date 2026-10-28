// DarkMode.cpp : system dark mode for the emulator's own windows.
//
// Only the window frame and the menu bar follow the system setting; the
// dialogs keep their original light colours.  The dark dialog surfaces were
// tried and parked: see .workbuddy-ai/backup/dark-content-20261018.
//
// Everything that knows about Vista and later is looked up at run time with
// LoadLibrary/GetProcAddress, so the emulator still starts on Windows XP and
// on Windows 10 builds that do not have the dark mode entry points at all.
// A WINXP build compiles the feature out completely.
//
// The visual styles dependency below is what gives the process a themed
// window; without it uxtheme never asks us to paint the menu bar.

#if defined(WINXP)
#	define _WIN32_WINNT 0x0501
#	define WINVER       0x0501
#else
// This translation unit includes windows.h directly instead of stdafx.h, so
// it sets the target level itself.  Declarations only: the DWM entry point
// is still resolved with GetProcAddress.
#	define _WIN32_WINNT 0x0A00
#	define WINVER       0x0A00
#endif

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>

#include "DarkMode.h"

#pragma comment(lib, "comctl32.lib")

#if !defined(WINXP)
#	pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace
{
	const COLORREF kBarColor          = RGB(32, 32, 32);
	const COLORREF kBorderColor       = RGB(56, 56, 56);	// same grey the frame uses
	const COLORREF kTextColor         = RGB(228, 228, 228);
	// The status bar is a control face, so it takes the dark counterpart of
	// BTNFACE (0x333333) rather than the window colour above.  It is a shade
	// lighter than the window, which is how Windows' own dark panels read.
	const COLORREF kPanelColor        = RGB(0x33, 0x33, 0x33);
	const COLORREF kPanelTextColor    = RGB(0xE4, 0xE4, 0xE4);
	const COLORREF kDisabledTextColor = RGB(128, 128, 128);
	const COLORREF kMenuHotColor      = RGB(58, 58, 58);
	const COLORREF kMenuOpenColor     = RGB(74, 74, 74);

	HHOOK g_hook    = NULL;
	bool  g_enabled = false;

	// uxtheme exports these by ordinal only and the SDK does not declare them,
	// so their types stay hand written.  Everything with a prototype uses
	// decltype.  Nothing here is linked statically: dwmapi is not present
	// before Windows Vista, and uxtheme's ordinals may be missing too.
	typedef int     (WINAPI *FnSetPreferredAppMode)(int);
	typedef void    (WINAPI *FnFlushMenuThemes)();
	typedef void    (WINAPI *FnRefreshImmersiveColorPolicyState)();

	decltype(&DwmSetWindowAttribute) g_setDwmAttribute    = NULL;

	// PreferredAppMode, as used by the ordinal 135 entry point.
	enum
	{
		AppModeDefault   = 0,
		AppModeAllowDark = 1,
		AppModeForceDark = 2
	};

	bool systemUsesDarkMode()
	{
		DWORD value = 1;
		DWORD size  = sizeof(value);
		HKEY  key   = NULL;

		if (RegOpenKeyExW(HKEY_CURRENT_USER,
		                  L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		                  0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
			return false;

		if (RegQueryValueExW(key, L"AppsUseLightTheme", NULL, NULL,
		                     reinterpret_cast<LPBYTE>(&value), &size) != ERROR_SUCCESS)
			value = 1;

		RegCloseKey(key);
		return value == 0;
	}

#if !defined(WINXP)
	// The menu bar is painted by the system, and neither owner-draw nor any
	// documented message covers it.  Windows does send these four messages to
	// the window so that the active theme can draw the bar; the meanings and
	// the layout of the structures come from reverse engineering (see
	// adzm/win32-custom-menubar-aero-theme), and they have been stable since
	// Windows Vista.
#	define WM_UAHDRAWMENU         0x0091
#	define WM_UAHDRAWMENUITEM     0x0092
#	define WM_UAHINITMENU         0x0093
#	define WM_UAHMEASUREMENUITEM  0x0094

	struct UAHMENUITEMMETRICS
	{
		DWORD cx;
		DWORD cy;
	};

	struct UAHMENUPOPUPMETRICS
	{
		DWORD rgcx;
		DWORD fUpdateMaxWidths : 2;
	};

	struct UAHMENU
	{
		HMENU hmenu;
		HDC   hdc;
		DWORD dwFlags;
	};

	struct UAHMENUITEM
	{
		int                 iPosition;
		UAHMENUITEMMETRICS  umim;
		UAHMENUPOPUPMETRICS umpm;
	};

	struct UAHDRAWMENUITEM
	{
		DRAWITEMSTRUCT dis;
		UAHMENU        um;
		UAHMENUITEM    umi;
	};

	// The menu bar has no owner-draw switch and no documented message; the
	// only way in is to answer the messages uxtheme itself uses for it.
	HFONT g_menuFont = NULL;

	decltype(&GetDpiForWindow)            g_getDpiForWindow = NULL;
	decltype(&SystemParametersInfoForDpi) g_spiForDpi       = NULL;

	HFONT menuFont(HWND hwnd)
	{
		if (g_menuFont == NULL)
		{
			NONCLIENTMETRICSW metrics;
			ZeroMemory(&metrics, sizeof(metrics));
			metrics.cbSize = sizeof(metrics);

			UINT dpi = 96;
			if (g_getDpiForWindow != NULL)
				dpi = g_getDpiForWindow(hwnd);

			bool ok = false;
			if (g_spiForDpi != NULL)
				ok = g_spiForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi) != FALSE;
			if (!ok)
				ok = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != FALSE;

			if (ok)
				g_menuFont = CreateFontIndirectW(&metrics.lfMenuFont);
		}
		return g_menuFont;
	}

	bool drawMenuBar(UAHMENU *menu, HWND hwnd)
	{
		MENUBARINFO info;
		ZeroMemory(&info, sizeof(info));
		info.cbSize = sizeof(info);

		if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &info))
			return false;

		RECT window;
		GetWindowRect(hwnd, &window);

		RECT rc = info.rcBar;
		OffsetRect(&rc, -window.left, -window.top);
		rc.bottom += 1;		// also covers the separator above the client area

		HBRUSH brush = CreateSolidBrush(kBarColor);
		FillRect(menu->hdc, &rc, brush);
		DeleteObject(brush);
		return true;
	}

	void drawMenuItem(const UAHDRAWMENUITEM *item, HWND hwnd)
	{
		HDC dc = item->um.hdc;

		wchar_t text[256] = L"";
		MENUITEMINFOW info;
		ZeroMemory(&info, sizeof(info));
		info.cbSize     = sizeof(info);
		info.fMask      = MIIM_STRING;
		info.dwTypeData = text;
		info.cch        = 255;
		GetMenuItemInfoW(item->um.hmenu, item->umi.iPosition, TRUE, &info);

		COLORREF background = kBarColor;
		if (item->dis.itemState & ODS_SELECTED)
			background = kMenuOpenColor;
		else if (item->dis.itemState & ODS_HOTLIGHT)
			background = kMenuHotColor;

		COLORREF foreground = (item->dis.itemState & (ODS_GRAYED | ODS_DISABLED))
		                    ? kDisabledTextColor : kTextColor;

		HBRUSH brush = CreateSolidBrush(background);
		FillRect(dc, &item->dis.rcItem, brush);
		DeleteObject(brush);

		UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
		if (item->dis.itemState & ODS_NOACCEL)
			flags |= DT_HIDEPREFIX;

		HFONT font = menuFont(hwnd);
		HGDIOBJ oldFont = font != NULL ? SelectObject(dc, font) : NULL;

		RECT rc = item->dis.rcItem;
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, foreground);
		DrawTextW(dc, text, -1, &rc, flags);

		if (oldFont != NULL)
			SelectObject(dc, oldFont);
	}

	// The theme paints a one pixel light border where the menu bar meets the
	// client area, and it does so after the bar has been drawn.  Painting the
	// same strip afterwards in the frame colour makes the bar read as part of
	// the window frame instead of a black band.
	void drawMenuBorder(HWND hwnd)
	{
		if (GetMenu(hwnd) == NULL)
			return;

		RECT client;
		GetClientRect(hwnd, &client);
		MapWindowPoints(hwnd, NULL, reinterpret_cast<POINT *>(&client), 2);

		RECT window;
		GetWindowRect(hwnd, &window);
		OffsetRect(&client, -window.left, -window.top);

		int thickness = GetSystemMetrics(SM_CYBORDER);
		if (thickness < 1)
			thickness = 1;

		RECT line;
		line.left   = client.left;
		line.right  = client.right;
		line.bottom = client.top;
		line.top    = client.top - thickness;

		HDC dc = GetWindowDC(hwnd);
		HBRUSH brush = CreateSolidBrush(kBorderColor);
		FillRect(dc, &line, brush);
		DeleteObject(brush);
		ReleaseDC(hwnd, dc);
	}

	LRESULT CALLBACK frameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
	                           UINT_PTR, DWORD_PTR)
	{
		switch (msg)
		{
		case WM_UAHDRAWMENU:
			if (drawMenuBar(reinterpret_cast<UAHMENU *>(lParam), hwnd))
				return 0;
			break;

		case WM_UAHDRAWMENUITEM:
			drawMenuItem(reinterpret_cast<const UAHDRAWMENUITEM *>(lParam), hwnd);
			return 0;

		case WM_NCPAINT:
		case WM_NCACTIVATE:
			{
				// The theme draws its own light border along the bottom of
				// the menu bar, after the bar itself has been painted; cover
				// it once the window is done with the non client area.
				LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
				drawMenuBorder(hwnd);
				return result;
			}
		}

		return DefSubclassProc(hwnd, msg, wParam, lParam);
	}

	// The emulator is a MultiByte build, so its windows are ANSI ones: the
	// messages handed to a window procedure carry char strings.  Moving the
	// procedure with SetWindowLongPtrW() flips the window to Unicode and the
	// chain then reads a wide WM_SETTEXT as ANSI, which shows up as a caption
	// consisting of its first character only.  SetWindowSubclass() keeps the
	// procedure's character set and DefSubclassProc() converts for it.
	void subclassFrame(HWND hwnd)
	{
		SetWindowSubclass(hwnd, frameProc, 1, 0);
	}
#endif

	// The title bar and the menu bar follow the system setting; the client
	// area is left to the window itself.
	void applyFrame(HWND hwnd)
	{
		if (g_setDwmAttribute != NULL)
		{
			// Windows fades a new top level window in over a white surface.
			// The emulator creates a window whenever the render method
			// changes, so the fade is seen as a white flash.
			BOOL noTransition = TRUE;
			g_setDwmAttribute(hwnd, 3 /* DWMWA_TRANSITIONS_FORCEDISABLED */,
			                  &noTransition, sizeof(noTransition));

			if (g_enabled)
			{
				BOOL dark = TRUE;
				// 20 since Windows 10 2004, 19 on 1809/1903; the failing one
				// is simply ignored.
				g_setDwmAttribute(hwnd, 20, &dark, sizeof(dark));
				g_setDwmAttribute(hwnd, 19, &dark, sizeof(dark));
			}
		}

#if !defined(WINXP)
		if (g_enabled)
			subclassFrame(hwnd);
#endif
	}

	bool isTopLevel(HWND hwnd)
	{
		return GetAncestor(hwnd, GA_PARENT) == GetDesktopWindow();
	}

	void onWindowCreated(HWND hwnd, UINT)
	{
		if (isTopLevel(hwnd))
			applyFrame(hwnd);
	}

	LRESULT CALLBACK hookProc(int code, WPARAM wParam, LPARAM lParam)
	{
		if (code == HC_ACTION)
		{
			const CWPRETSTRUCT *info = reinterpret_cast<const CWPRETSTRUCT *>(lParam);
			if (info->message == WM_INITDIALOG || info->message == WM_CREATE)
				onWindowCreated(info->hwnd, info->message);
		}

		return CallNextHookEx(g_hook, code, wParam, lParam);
	}
}

namespace darkMode
{
	bool enabled()
	{
		return g_enabled;
	}

	COLORREF panelColor()
	{
		return kPanelColor;
	}

	COLORREF panelTextColor()
	{
		return kPanelTextColor;
	}

	void resetFont()
	{
#if !defined(WINXP)
		if (g_menuFont != NULL)
		{
			DeleteObject(g_menuFont);
			g_menuFont = NULL;
		}
#endif
	}

	void init()
	{
		// The visual styles the menu bar is drawn with are common controls
		// version 6 ones, and those have to be initialised before the first
		// window is created.
		INITCOMMONCONTROLSEX controls;
		controls.dwSize = sizeof(controls);
		controls.dwICC  = ICC_WIN95_CLASSES;
		InitCommonControlsEx(&controls);

#if defined(WINXP)
		return;
#else
		// Loaded before the colour mode is looked at: turning the window
		// transitions off has nothing to do with it, and every window the
		// emulator creates needs that.
		HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
		if (dwmapi != NULL)
			g_setDwmAttribute = reinterpret_cast<decltype(&DwmSetWindowAttribute)>(
				GetProcAddress(dwmapi, "DwmSetWindowAttribute"));

		if (!systemUsesDarkMode())
			return;

		HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
		if (uxtheme == NULL)
			return;

		FnSetPreferredAppMode setPreferredAppMode =
			reinterpret_cast<FnSetPreferredAppMode>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));

		if (setPreferredAppMode == NULL)
			return;		// Windows 10 before 1809 has no dark mode for Win32 windows

		FnFlushMenuThemes flushMenuThemes =
			reinterpret_cast<FnFlushMenuThemes>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
		FnRefreshImmersiveColorPolicyState refreshPolicy =
			reinterpret_cast<FnRefreshImmersiveColorPolicyState>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(104)));

		// ForceDark is what makes the drop down menus dark; they have no per
		// window switch of their own.
		setPreferredAppMode(AppModeForceDark);
		if (flushMenuThemes != NULL)
			flushMenuThemes();
		if (refreshPolicy != NULL)
			refreshPolicy();

		g_enabled = true;

		g_hook = SetWindowsHookExW(WH_CALLWNDPROCRET, hookProc, NULL, GetCurrentThreadId());
#endif
	}
}
