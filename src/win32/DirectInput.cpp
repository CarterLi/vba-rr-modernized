//#define USE_GETASYNCKEYSTATE_FOR_KEYBOARD

#include "stdafx.h"

#define DIRECTINPUT_VERSION 0x0800
#include "dinput.h"
#include <cstddef>

// requestSimpleHidReports() talks to the HID device directly: DirectInput has
// no output reports for joysticks, and the controller only switches to the
// report the OS can decode when a host asks it to.
#include <setupapi.h>
#include <hidsdi.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

#include "resource.h"
#include "Input.h"
#include "Reg.h"
#include "WinResUtil.h"
#include "VBA.h"

// master keyboard translation table
static const struct {
	int dik;
	int vk;
	int ascii;
} win_key_trans_table[] = {
	// dinput key		virtual key		ascii
	{ DIK_ESCAPE,		VK_ESCAPE,	 	27 },
	{ DIK_1,			'1',			'1' },
	{ DIK_2,			'2',			'2' },
	{ DIK_3,			'3',			'3' },
	{ DIK_4,			'4',			'4' },
	{ DIK_5,			'5',			'5' },
	{ DIK_6,			'6',			'6' },
	{ DIK_7,			'7',			'7' },
	{ DIK_8,			'8',			'8' },
	{ DIK_9,			'9',			'9' },
	{ DIK_0,			'0',			'0' },
	{ DIK_MINUS, 		VK_OEM_MINUS,	'-' },
	{ DIK_EQUALS,		VK_OEM_PLUS,	'=' },
	{ DIK_BACK, 		VK_BACK, 		8 },
	{ DIK_TAB, 			VK_TAB, 		9 },
	{ DIK_Q,			'Q',			'Q' },
	{ DIK_W,			'W',			'W' },
	{ DIK_E,			'E',			'E' },
	{ DIK_R,			'R',			'R' },
	{ DIK_T,			'T',			'T' },
	{ DIK_Y,			'Y',			'Y' },
	{ DIK_U,			'U',			'U' },
	{ DIK_I,			'I',			'I' },
	{ DIK_O,			'O',			'O' },
	{ DIK_P,			'P',			'P' },
	{ DIK_LBRACKET, 	VK_OEM_4,		'[' },
	{ DIK_RBRACKET, 	VK_OEM_6,		']' },
	{ DIK_RETURN, 		VK_RETURN, 		13 },
	{ DIK_LCONTROL, 	VK_LCONTROL, 	0 },
	{ DIK_A,			'A',			'A' },
	{ DIK_S,			'S',			'S' },
	{ DIK_D,			'D',			'D' },
	{ DIK_F,			'F',			'F' },
	{ DIK_G,			'G',			'G' },
	{ DIK_H,			'H',			'H' },
	{ DIK_J,			'J',			'J' },
	{ DIK_K,			'K',			'K' },
	{ DIK_L,			'L',			'L' },
	{ DIK_SEMICOLON,	VK_OEM_1,		';' },
	{ DIK_APOSTROPHE,	VK_OEM_7,		'\'' },
	{ DIK_GRAVE, 		VK_OEM_3,		'`' },
	{ DIK_LSHIFT, 		VK_LSHIFT, 		0 },
	{ DIK_BACKSLASH, 	VK_OEM_5,		'\\' },
	{ DIK_Z,			'Z',			'Z' },
	{ DIK_X,			'X',			'X' },
	{ DIK_C,			'C',			'C' },
	{ DIK_V,			'V',			'V' },
	{ DIK_B,			'B',			'B' },
	{ DIK_N,			'N',			'N' },
	{ DIK_M,			'M',			'M' },
	{ DIK_COMMA,		VK_OEM_COMMA,	',' },
	{ DIK_PERIOD, 		VK_OEM_PERIOD,	'.' },
	{ DIK_SLASH, 		VK_OEM_2,		'/' },
	{ DIK_RSHIFT, 		VK_RSHIFT, 		0 },
	{ DIK_MULTIPLY, 	VK_MULTIPLY,	'*' },
	{ DIK_LMENU, 		VK_LMENU, 		0 },
	{ DIK_SPACE, 		VK_SPACE,		' ' },
	{ DIK_CAPITAL, 		VK_CAPITAL, 	0 },
	{ DIK_F1,			VK_F1, 			0 },
	{ DIK_F2,			VK_F2, 			0 },
	{ DIK_F3,			VK_F3, 			0 },
	{ DIK_F4,			VK_F4, 			0 },
	{ DIK_F5,			VK_F5, 			0 },
	{ DIK_F6,			VK_F6, 			0 },
	{ DIK_F7,			VK_F7, 			0 },
	{ DIK_F8,			VK_F8, 			0 },
	{ DIK_F9,			VK_F9, 			0 },
	{ DIK_F10,			VK_F10, 		0 },
	{ DIK_NUMLOCK,		VK_NUMLOCK, 	0 },
	{ DIK_SCROLL,		VK_SCROLL, 		0 },
	{ DIK_NUMPAD7,		VK_NUMPAD7, 	0 },
	{ DIK_NUMPAD8,		VK_NUMPAD8, 	0 },
	{ DIK_NUMPAD9,		VK_NUMPAD9, 	0 },
	{ DIK_SUBTRACT,		VK_SUBTRACT, 	0 },
	{ DIK_NUMPAD4,		VK_NUMPAD4, 	0 },
	{ DIK_NUMPAD5,		VK_NUMPAD5, 	0 },
	{ DIK_NUMPAD6,		VK_NUMPAD6, 	0 },
	{ DIK_ADD,			VK_ADD, 		0 },
	{ DIK_NUMPAD1,		VK_NUMPAD1, 	0 },
	{ DIK_NUMPAD2,		VK_NUMPAD2, 	0 },
	{ DIK_NUMPAD3,		VK_NUMPAD3, 	0 },
	{ DIK_NUMPAD0,		VK_NUMPAD0, 	0 },
	{ DIK_DECIMAL,		VK_DECIMAL, 	0 },
	{ DIK_F11,			VK_F11, 		0 },
	{ DIK_F12,			VK_F12, 		0 },
	{ DIK_F13,			VK_F13, 		0 },
	{ DIK_F14,			VK_F14, 		0 },
	{ DIK_F15,			VK_F15, 		0 },
	{ DIK_NUMPADENTER,	VK_RETURN, 		0 },
	{ DIK_RCONTROL,		VK_RCONTROL, 	0 },
	{ DIK_DIVIDE,		VK_DIVIDE, 		0 },
	{ DIK_SYSRQ, 		0, 				0 },
	{ DIK_RMENU,		VK_RMENU, 		0 },
	{ DIK_HOME,			VK_HOME, 		0 },
	{ DIK_UP,			VK_UP, 			0 },
	{ DIK_PRIOR,		VK_PRIOR, 		0 },
	{ DIK_LEFT,			VK_LEFT, 		0 },
	{ DIK_RIGHT,		VK_RIGHT, 		0 },
	{ DIK_END,			VK_END, 		0 },
	{ DIK_DOWN,			VK_DOWN, 		0 },
	{ DIK_NEXT,			VK_NEXT, 		0 },
	{ DIK_INSERT,		VK_INSERT, 		0 },
	{ DIK_DELETE,		VK_DELETE, 		0 },
	{ DIK_LWIN,			VK_LWIN, 		0 },
	{ DIK_RWIN,			VK_RWIN, 		0 },
	{ DIK_APPS,			VK_APPS, 		0 },
	{ DIK_PAUSE,		VK_PAUSE,		0 },
	{ 0,				VK_CANCEL,		0 },

	// New keys introduced in Windows 2000. These have no MAME codes to
	// preserve compatibility with old config files that may refer to them
	// as e.g. FORWARD instead of e.g. KEYCODE_WEBFORWARD. They need table
	// entries anyway because otherwise they aren't recognized when
	// GetAsyncKeyState polling is used (as happens currently when MAME is
	// paused). Some codes are missing because the mapping to vkey codes
	// isn't clear, and MapVirtualKey is no help.

	{ DIK_MUTE,			VK_VOLUME_MUTE,			0 },
	{ DIK_VOLUMEDOWN,	VK_VOLUME_DOWN,			0 },
	{ DIK_VOLUMEUP,		VK_VOLUME_UP,			0 },
	{ DIK_WEBHOME,		VK_BROWSER_HOME,		0 },
	{ DIK_WEBSEARCH,	VK_BROWSER_SEARCH,		0 },
	{ DIK_WEBFAVORITES,	VK_BROWSER_FAVORITES,	0 },
	{ DIK_WEBREFRESH,	VK_BROWSER_REFRESH,		0 },
	{ DIK_WEBSTOP,		VK_BROWSER_STOP,		0 },
	{ DIK_WEBFORWARD,	VK_BROWSER_FORWARD,		0 },
	{ DIK_WEBBACK,		VK_BROWSER_BACK,		0 },
	{ DIK_MAIL,			VK_LAUNCH_MAIL,			0 },
	{ DIK_MEDIASELECT,	VK_LAUNCH_MEDIA_SELECT,	0 },
};

extern void directXMessage(const char *);

#define POV_UP    1
#define POV_DOWN  2
#define POV_RIGHT 4
#define POV_LEFT  8

class DirectInput : public Input
{
private:
	HINSTANCE dinputDLL;
public:
	virtual void checkDevices();
	DirectInput();
	virtual ~DirectInput();

	virtual bool initialize();
	virtual void refreshDevices();
	virtual void announceDevices();
	virtual int  gamepadCount();
	virtual bool gamepadInfo(int n, int &deviceIndex, CString &name);
	virtual int  getDeadzone();
	virtual void setDeadzone(int percent);
	virtual bool readDevices();
	virtual u32 readDevice(int which, bool sensor);
	virtual CString getKeyName(LONG_PTR key);
	// Driven by the Auto-Fire keys; uses VBAToggleAutoFireButton(), the same
	// routine the Auto-Fire menu commands (Alt+0-9) use.
	void updateAutoFire();
	u32 readAutoFireBound(int i);
	u32 readAutoFireHeld(int i);
	virtual void checkKeys();
	virtual void activate();
	virtual void loadSettings();
	virtual void saveSettings();
};

struct deviceInfo
{
	LPDIRECTINPUTDEVICE8 device;
	BOOL isPolled;
	int  nButtons;
	int  nAxes;
	int  nPovs;
	BOOL first;
	BOOL attached;		// the controller is plugged in right now
	int  failCount;		// reads that have failed in a row
	TCHAR name[128];		// product name, for the messages
	struct
	{
		DWORD offset;
		LONG  center;
		LONG  half;		// distance from the centre to either end
		LONG  negative;
		LONG  positive;
	} axis[8];
	int needed;
	union
	{
		UCHAR      data[256];
		DIJOYSTATE state;
	};
};

static deviceInfo *   currentDevice = NULL;
static int            numDevices    = 1;
static deviceInfo *   pDevices      = NULL;
static LPDIRECTINPUT8 pDirectInput  = NULL;
static int            joyDebug      = 0;
static int            axisNumber    = 0;

// Percent of the way from the centre to the end that a stick has to travel
// before it counts as pushed.  50 is what the axis thresholds always were:
// half of the half range.  Lower it for a stick that needs less travel,
// raise it for a stick that drifts.
static int            joypadDeadzone = 50;

// Controllers found by an enumeration, waiting to be reported to the user.
// The list is flushed once the enumeration has finished, so a pad that was
// plugged in before the emulator started produces one message and not one per
// device.
static CString pendingPadNames;
static int     pendingPadCount = 0;

// Recomputes where "pushed" starts for every axis of every controller.  The
// thresholds are kept per axis, so a changed setting only has to be pushed
// into them rather than the devices re-read.
static void applyDeadzone()
{
	if (pDevices == NULL)
		return;

	for (int i = 1; i < numDevices; i++)
	{
		for (int a = 0; a < 8; a++)
		{
			LONG threshold = pDevices[i].axis[a].half * joypadDeadzone / 100;

			pDevices[i].axis[a].negative = pDevices[i].axis[a].center - threshold;
			pDevices[i].axis[a].positive = pDevices[i].axis[a].center + threshold;
		}
	}
}

static void rememberPad(LPCTSTR name)
{
	if (name == NULL || name[0] == 0)
		return;

	if (pendingPadCount > 0)
		pendingPadNames += ", ";

	pendingPadNames += name;
	pendingPadCount++;
}

static void announcePads()
{
	if (pendingPadCount == 0)
		return;

	CString message;
	message.Format(winResLoadOsdString(IDS_JOYPAD_CONNECTED), (LPCTSTR)pendingPadNames);

	// the trace log keeps the record whether or not the message reaches the
	// screen: before a game is loaded there is no screen to put it on
	winlog(_T("%s\n"), (LPCTSTR)message);

	// a status message on the game screen, the way "state loaded" and the
	// autofire toggles are reported - a dialog would stop a frame of the game
	// (and would need a window of its own)
	systemScreenMessageRes(IDS_JOYPAD_CONNECTED, (LPCTSTR)pendingPadNames);

	pendingPadNames.Empty();
	pendingPadCount = 0;
}

// A controller came or went while the emulator was running.  These arrive at
// frame rate, so only the edges are reported and only after the read has
// failed twice: a single lost frame is not a disconnected controller.
static void reportPad(deviceInfo &info, bool connected)
{
	const UINT id = connected ? IDS_JOYPAD_CONNECTED : IDS_JOYPAD_DISCONNECTED;

	CString message;
	message.Format(winResLoadOsdString(id), info.name);

	// always in the trace log, so a controller that came or went can be traced
	// after the on-screen message is long gone
	winlog(_T("%s\n"), (LPCTSTR)message);

	systemScreenMessageRes(id, info.name);
}

// Feeds the result of a device read into the attached/lost state machine and
// answers whether there is a state to use this time.
static bool padReadResult(int index, HRESULT hret)
{
	if (hret == DI_OK)
	{
		pDevices[index].failCount = 0;

		if (!pDevices[index].attached)
		{
			pDevices[index].attached = TRUE;
			reportPad(pDevices[index], true);
		}

		return true;
	}

	if (++pDevices[index].failCount < 2)
		return false;

	if (pDevices[index].attached)
	{
		pDevices[index].attached = FALSE;
		reportPad(pDevices[index], false);
	}

	return false;
}

USHORT joypad[4][13] = {
	{
		DIK_LEFT,  DIK_RIGHT,
		DIK_UP,    DIK_DOWN,
		DIK_Z,     DIK_X,
		DIK_RETURN, DIK_BACK,
		DIK_A,     DIK_S,
		DIK_SPACE, DIK_F12,
		DIK_C
	},
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

// Auto-Fire keys: while one of these is held the matching button toggles every
// other frame.  Zero means "not assigned".
USHORT autoFireKeys[4][13] = {
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

USHORT motion[4] = {
	DIK_NUMPAD4, DIK_NUMPAD6, DIK_NUMPAD8, DIK_NUMPAD2
};

static int winReadKey(LPCTSTR name, int num)
{
	TCHAR buffer[80];

	_stprintf(buffer, _T("Joy%d_%s"), num, name);

	return regQueryDwordValue(buffer, (DWORD)-1);
}

void winReadKeys()
{
	int key = -1;

	for (int i = 0; i < 4; i++)
	{
		key = winReadKey(_T("Left"), i);
		if (key != -1)
			joypad[i][KEY_LEFT] = key;
		key = winReadKey(_T("Right"), i);
		if (key != -1)
			joypad[i][KEY_RIGHT] = key;
		key = winReadKey(_T("Up"), i);
		if (key != -1)
			joypad[i][KEY_UP] = key;
		key = winReadKey(_T("Down"), i);
		if (key != -1)
			joypad[i][KEY_DOWN] = key;
		key = winReadKey(_T("A"), i);
		if (key != -1)
			joypad[i][KEY_BUTTON_A] = key;
		key = winReadKey(_T("B"), i);
		if (key != -1)
			joypad[i][KEY_BUTTON_B] = key;
		key = winReadKey(_T("L"), i);
		if (key != -1)
			joypad[i][KEY_BUTTON_L] = key;
		key = winReadKey(_T("R"), i);
		if (key != -1)
			joypad[i][KEY_BUTTON_R] = key;
		key = winReadKey(_T("Start"), i);
		if (key != -1)
			joypad[i][KEY_BUTTON_START] = key;
		key = winReadKey(_T("Select"), i);
		if (key != -1)
			joypad[i][KEY_BUTTON_SELECT] = key;
		key = winReadKey(_T("Speed"), i);
		if (key != -1)
			joypad[i][KEY_BUTTON_SPEED] = key;
		key = winReadKey(_T("Capture"), i);
		if (key != -1)
			joypad[i][KEY_BUTTON_CAPTURE] = key;
		key = winReadKey(_T("GS"), i);
		if (key != -1)
			joypad[i][KEY_BUTTON_GS] = key;
		key = winReadKey(_T("AF_Up"), i);
		if (key != -1)
			autoFireKeys[i][KEY_UP] = key;
		key = winReadKey(_T("AF_Down"), i);
		if (key != -1)
			autoFireKeys[i][KEY_DOWN] = key;
		key = winReadKey(_T("AF_Left"), i);
		if (key != -1)
			autoFireKeys[i][KEY_LEFT] = key;
		key = winReadKey(_T("AF_Right"), i);
		if (key != -1)
			autoFireKeys[i][KEY_RIGHT] = key;
		key = winReadKey(_T("AF_A"), i);
		if (key != -1)
			autoFireKeys[i][KEY_BUTTON_A] = key;
		key = winReadKey(_T("AF_B"), i);
		if (key != -1)
			autoFireKeys[i][KEY_BUTTON_B] = key;
		key = winReadKey(_T("AF_L"), i);
		if (key != -1)
			autoFireKeys[i][KEY_BUTTON_L] = key;
		key = winReadKey(_T("AF_R"), i);
		if (key != -1)
			autoFireKeys[i][KEY_BUTTON_R] = key;
		key = winReadKey(_T("AF_Select"), i);
		if (key != -1)
			autoFireKeys[i][KEY_BUTTON_SELECT] = key;
		key = winReadKey(_T("AF_Start"), i);
		if (key != -1)
			autoFireKeys[i][KEY_BUTTON_START] = key;
	}
	key = regQueryDwordValue(_T("Motion_Left"), (DWORD)-1);
	if (key != -1)
		motion[KEY_LEFT] = key;
	key = regQueryDwordValue(_T("Motion_Right"), (DWORD)-1);
	if (key != -1)
		motion[KEY_RIGHT] = key;
	key = regQueryDwordValue(_T("Motion_Up"), (DWORD)-1);
	if (key != -1)
		motion[KEY_UP] = key;
	key = regQueryDwordValue(_T("Motion_Down"), (DWORD)-1);
	if (key != -1)
		motion[KEY_DOWN] = key;

	joypadDeadzone = (int)regQueryDwordValue(_T("Joy_Deadzone"), joypadDeadzone);
	if (joypadDeadzone < 0)
		joypadDeadzone = 0;
	if (joypadDeadzone > 100)
		joypadDeadzone = 100;

	applyDeadzone();
}

static void winSaveKey(LPCTSTR name, int num, USHORT value)
{
	TCHAR buffer[80];

	_stprintf(buffer, _T("Joy%d_%s"), num, name);

	regSetDwordValue(buffer, value);
}

void winSaveKeys()
{
	for (int i = 0; i < 4; i++)
	{
		winSaveKey(_T("Left"), i, joypad[i][KEY_LEFT]);
		winSaveKey(_T("Right"), i, joypad[i][KEY_RIGHT]);
		winSaveKey(_T("Up"), i, joypad[i][KEY_UP]);
		winSaveKey(_T("Speed"), i, joypad[i][KEY_BUTTON_SPEED]);
		winSaveKey(_T("Capture"), i, joypad[i][KEY_BUTTON_CAPTURE]);
		winSaveKey(_T("GS"), i, joypad[i][KEY_BUTTON_GS]);
		winSaveKey(_T("Down"), i, joypad[i][KEY_DOWN]);
		winSaveKey(_T("A"), i, joypad[i][KEY_BUTTON_A]);
		winSaveKey(_T("B"), i, joypad[i][KEY_BUTTON_B]);
		winSaveKey(_T("L"), i, joypad[i][KEY_BUTTON_L]);
		winSaveKey(_T("R"), i, joypad[i][KEY_BUTTON_R]);
		winSaveKey(_T("Start"), i, joypad[i][KEY_BUTTON_START]);
		winSaveKey(_T("Select"), i, joypad[i][KEY_BUTTON_SELECT]);
		winSaveKey(_T("AF_Up"), i, autoFireKeys[i][KEY_UP]);
		winSaveKey(_T("AF_Down"), i, autoFireKeys[i][KEY_DOWN]);
		winSaveKey(_T("AF_Left"), i, autoFireKeys[i][KEY_LEFT]);
		winSaveKey(_T("AF_Right"), i, autoFireKeys[i][KEY_RIGHT]);
		winSaveKey(_T("AF_A"), i, autoFireKeys[i][KEY_BUTTON_A]);
		winSaveKey(_T("AF_B"), i, autoFireKeys[i][KEY_BUTTON_B]);
		winSaveKey(_T("AF_L"), i, autoFireKeys[i][KEY_BUTTON_L]);
		winSaveKey(_T("AF_R"), i, autoFireKeys[i][KEY_BUTTON_R]);
		winSaveKey(_T("AF_Select"), i, autoFireKeys[i][KEY_BUTTON_SELECT]);
		winSaveKey(_T("AF_Start"), i, autoFireKeys[i][KEY_BUTTON_START]);
	}
	regSetDwordValue(_T("joyVersion"), 1);

	regSetDwordValue(_T("Motion_Left"),
	                 motion[KEY_LEFT]);
	regSetDwordValue(_T("Motion_Right"),
	                 motion[KEY_RIGHT]);
	regSetDwordValue(_T("Motion_Up"),
	                 motion[KEY_UP]);
	regSetDwordValue(_T("Motion_Down"),
	                 motion[KEY_DOWN]);

	regSetDwordValue(_T("Joy_Deadzone"), joypadDeadzone);
}

static BOOL CALLBACK EnumAxesCallback(const DIDEVICEOBJECTINSTANCE*pdidoi,
                                      VOID*pContext)
{
	DIPROPRANGE diprg;
	diprg.diph.dwSize       = sizeof(DIPROPRANGE);
	diprg.diph.dwHeaderSize = sizeof(DIPROPHEADER);
	diprg.diph.dwHow        = DIPH_BYOFFSET;
	diprg.diph.dwObj        = pdidoi->dwOfs; // Specify the enumerated axis

	diprg.lMin = -32768;
	diprg.lMax = 32767;
	// try to set the range
	if (FAILED(currentDevice->device->SetProperty(DIPROP_RANGE, &diprg.diph)))
	{
		// Get the range for the axis
		if (FAILED(currentDevice->device->
		           GetProperty(DIPROP_RANGE, &diprg.diph)))
		{
			// The device will not talk about this axis at all.  Keep going
			// instead of dropping every axis after it: the offset is what the
			// reads need, and a HID axis that says nothing is 0 to 65535 with
			// the stick at rest in the middle.
			diprg.lMin = 0;
			diprg.lMax = 65535;
		}
	}

	DIPROPDWORD didz;

	didz.diph.dwSize       = sizeof(didz);
	didz.diph.dwHeaderSize = sizeof(DIPROPHEADER);
	didz.diph.dwHow        = DIPH_BYOFFSET;
	didz.diph.dwObj        = pdidoi->dwOfs;

	didz.dwData = 5000;

	currentDevice->device->SetProperty(DIPROP_DEADZONE, &didz.diph);

	LONG center    = (diprg.lMin + diprg.lMax)/2;
	LONG half      = (diprg.lMax - center);
	LONG threshold = half * joypadDeadzone / 100;

	// only 8 axis supported
	if (axisNumber < 8)
	{
		currentDevice->axis[axisNumber].center   = center;
		currentDevice->axis[axisNumber].half     = half;
		currentDevice->axis[axisNumber].negative = center - threshold;
		currentDevice->axis[axisNumber].positive = center + threshold;
		currentDevice->axis[axisNumber].offset   = pdidoi->dwOfs;
	}
	axisNumber++;
	return DIENUM_CONTINUE;
}

static BOOL CALLBACK EnumPovsCallback(const DIDEVICEOBJECTINSTANCE*pdidoi,
                                      VOID*pContext)
{
	return DIENUM_CONTINUE;
}

// Everything that has to be redone whenever a joystick object is created: the
// capabilities, the data format, the axis thresholds and the first-poll guard.
// Used both when the devices are enumerated at startup and when the list is
// refreshed because a controller appeared later.
static void setupJoystick(int index)
{
	DIDEVCAPS caps;

	ZeroMemory(&caps, sizeof(caps));
	caps.dwSize = sizeof(DIDEVCAPS);

	pDevices[index].isPolled = FALSE;
	pDevices[index].nButtons = 0;
	pDevices[index].nAxes    = 0;
	pDevices[index].nPovs    = 0;

	HRESULT hRet = pDevices[index].device->GetCapabilities(&caps);

	if (hRet == DI_OK)
	{
		if (caps.dwFlags & DIDC_POLLEDDATAFORMAT ||
		    caps.dwFlags & DIDC_POLLEDDEVICE)
			pDevices[index].isPolled = TRUE;

		pDevices[index].nButtons = caps.dwButtons;
		pDevices[index].nAxes    = caps.dwAxes;
		pDevices[index].nPovs    = caps.dwPOVs;
	}
	else if (joyDebug)
		winlog(_T("Failed to get device capabilities %08x\n"), hRet);

	// Thresholds the axis callback has not reported yet.  All eight, because
	// checkKey() indexes the same array.
	for (int i = 0; i < 8; i++)
	{
		pDevices[index].axis[i].center   = 0x8000;
		pDevices[index].axis[i].half     = 0x4000;
		pDevices[index].axis[i].negative = 0x4000;
		pDevices[index].axis[i].positive = 0xc000;
	}

	pDevices[index].device->SetDataFormat(&c_dfDIJoystick);
	pDevices[index].needed = FALSE;
	pDevices[index].first  = TRUE;

	// background and non-exclusive, so the pad keeps working while the emulator
	// is not the focused window; without a level at all Acquire() is at the
	// mercy of whatever DirectInput defaults to.
	CWnd *wnd = AfxGetApp() != NULL ? AfxGetApp()->m_pMainWnd : NULL;

	if (wnd != NULL && wnd->GetSafeHwnd() != NULL)
		pDevices[index].device->SetCooperativeLevel(wnd->GetSafeHwnd(),
		                                            DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);

	currentDevice = &pDevices[index];
	axisNumber    = 0;
	currentDevice->device->EnumObjects(EnumAxesCallback, NULL, DIDFT_AXIS);
	currentDevice->device->EnumObjects(EnumPovsCallback, NULL, DIDFT_POV);

	if (joyDebug)
	{
		winlog(_T("Joystick %2d polled  : %d\n"),    index, currentDevice->isPolled);
		winlog(_T("Joystick %2d buttons : %d\n"),    index, currentDevice->nButtons);
		winlog(_T("Joystick %2d povs    : %d\n"),    index, currentDevice->nPovs);
		winlog(_T("Joystick %2d axes    : %d\n"),    index, currentDevice->nAxes);
		for (int j = 0; j < currentDevice->nAxes && j < 8; j++)
		{
			winlog(_T("Axis %2d offset      : %08lx\n"), j, currentDevice->axis[j].offset);
			winlog(_T("Axis %2d center      : %08lx\n"), j, currentDevice->axis[j].center);
			winlog(_T("Axis %2d negative    : %08lx\n"), j, currentDevice->axis[j].negative);
			winlog(_T("Axis %2d positive    : %08lx\n"), j, currentDevice->axis[j].positive);
		}
	}

	currentDevice = NULL;
}

// A Nintendo-style controller (the Pro Controller and the pads that copy its
// protocol) sends its standard report - 0x30, the one with the motion data -
// all the time, but the HID descriptor declares that report as an opaque
// vendor blob.  Only the "simple HID" report, 0x3F, carries the buttons and
// sticks that the descriptor names, and the pad only sends it when a host
// asks for it.  DirectInput reads through the OS parser, so a pad that streams
// 0x30 alone looks like a device that never sends anything: it enumerates, it
// acquires, and every read returns the defaults, which is what "the controller
// does nothing" looks like from the outside.
//
// Output report 0x01 carries rumble plus a subcommand; subcommand 0x03 sets the
// input report mode.  Asking for 0x3F makes the pad send the report the OS can
// decode, and it keeps sending it until the controller reconnects.  The write
// has to go to the HID device itself, because DirectInput has no way to send
// an output report to a joystick.
static void requestSimpleHidReports()
{
	GUID hid;
	HidD_GetHidGuid(&hid);

	HDEVINFO set = SetupDiGetClassDevsA(&hid, NULL, NULL,
	                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

	if (set == INVALID_HANDLE_VALUE)
		return;

	for (DWORD i = 0; ; i++)
	{
		SP_DEVICE_INTERFACE_DATA iface;

		ZeroMemory(&iface, sizeof(iface));
		iface.cbSize = sizeof(iface);

		if (!SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &iface))
			break;

		DWORD size = 0;

		SetupDiGetDeviceInterfaceDetailA(set, &iface, NULL, 0, &size, NULL);

		if (size == 0)
			continue;

		PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail =
			(PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(size);

		if (detail == NULL)
			continue;

		ZeroMemory(detail, size);
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

		if (SetupDiGetDeviceInterfaceDetailA(set, &iface, detail, size, NULL, NULL))
		{
			HIDD_ATTRIBUTES      attr;
			PHIDP_PREPARSED_DATA preparsed = NULL;
			HIDP_CAPS            caps;

			ZeroMemory(&attr, sizeof(attr));
			ZeroMemory(&caps, sizeof(caps));
			attr.Size = sizeof(attr);

			HANDLE device = CreateFileA(detail->DevicePath,
			                            GENERIC_READ | GENERIC_WRITE,
			                            FILE_SHARE_READ | FILE_SHARE_WRITE,
			                            NULL, OPEN_EXISTING, 0, NULL);

			if (device != INVALID_HANDLE_VALUE)
			{
				if (HidD_GetAttributes(device, &attr) &&
				    attr.VendorID == 0x057e &&
				    HidD_GetPreparsedData(device, &preparsed) &&
				    HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS &&
				    caps.UsagePage == 0x01 && caps.Usage == 0x05 &&
				    caps.OutputReportByteLength >= 12)
				{
					int   length = caps.OutputReportByteLength;
					BYTE *packet = (BYTE *)calloc(length, 1);

					if (packet != NULL)
					{
						DWORD written = 0;

						packet[0]  = 0x01;	// rumble + subcommand
						packet[1]  = 0x00;	// rumble counter
						packet[10] = 0x03;	// set input report mode
						packet[11] = 0x3f;	// simple HID reports

						WriteFile(device, packet, length, &written, NULL);
						free(packet);

						if (joyDebug)
							winlog(_T("Nintendo pad: asked for simple HID reports\n"));
					}
				}

				if (preparsed != NULL)
					HidD_FreePreparsedData(preparsed);

				CloseHandle(device);
			}
		}

		free(detail);
	}

	SetupDiDestroyDeviceInfoList(set);
}

static BOOL CALLBACK DIEnumDevicesCallback(LPCDIDEVICEINSTANCE pInst,
                                           LPVOID lpvContext)
{
	ZeroMemory(&pDevices[numDevices], sizeof(deviceInfo));

	HRESULT hRet = pDirectInput->CreateDevice(pInst->guidInstance,
	                                          &pDevices[numDevices].device,
	                                          NULL);

	if (hRet != DI_OK)
		return DIENUM_CONTINUE;

	if (joyDebug)
	{
		// don't translate. debug only
		winlog(_T("******************************\n"));
		winlog(_T("Joystick %2d name    : %s\n"), numDevices, pInst->tszProductName);
	}

	rememberPad(pInst->tszProductName);

	lstrcpyn(pDevices[numDevices].name, pInst->tszProductName,
	          sizeof(pDevices[numDevices].name));
	pDevices[numDevices].attached = TRUE;

	numDevices++;

	return DIENUM_CONTINUE;
}

BOOL CALLBACK DIEnumDevicesCallback2(LPCDIDEVICEINSTANCE pInst,
                                     LPVOID lpvContext)
{
	numDevices++;

	return DIENUM_CONTINUE;
}

static int getPovState(DWORD value)
{
	int state = 0;
	if (LOWORD(value) != 0xFFFF)
	{
		if (value < 9000 || value > 27000)
			state |= POV_UP;
		if (value > 0 && value < 18000)
			state |= POV_RIGHT;
		if (value > 9000 && value < 27000)
			state |= POV_DOWN;
		if (value > 18000)
			state |= POV_LEFT;
	}
	return state;
}

static void checkKeys()
{
	LONG_PTR dev = 0;
	int      i;

	for (i = 0; i < numDevices; i++)
		pDevices[i].needed = 0;

	for (i = 0; i < 4; i++)
	{
		dev = joypad[i][KEY_LEFT] >> 8;
		if (dev < numDevices && dev >= 0)
			pDevices[dev].needed = 1;
		else
			joypad[i][KEY_LEFT] = DIK_LEFT;

		dev = joypad[i][KEY_RIGHT] >> 8;
		if (dev < numDevices && dev >= 0)
			pDevices[dev].needed = 1;
		else
			joypad[i][KEY_RIGHT] = DIK_RIGHT;

		dev = joypad[i][KEY_UP] >> 8;
		if (dev < numDevices && dev >= 0)
			pDevices[dev].needed = 1;
		else
			joypad[i][KEY_UP] = DIK_UP;

		dev = joypad[i][KEY_DOWN] >> 8;
		if (dev < numDevices && dev >= 0)
			pDevices[dev].needed = 1;
		else
			joypad[i][KEY_DOWN] = DIK_DOWN;

		dev = joypad[i][KEY_BUTTON_A] >> 8;
		if (dev < numDevices && dev >= 0)
			pDevices[dev].needed = 1;
		else
			joypad[i][KEY_BUTTON_A] = DIK_Z;

		dev = joypad[i][KEY_BUTTON_B] >> 8;
		if (dev < numDevices && dev >= 0)
			pDevices[dev].needed = 1;
		else
			joypad[i][KEY_BUTTON_B] = DIK_X;

		dev = joypad[i][KEY_BUTTON_L] >> 8;
		if (dev < numDevices && dev >= 0)
			pDevices[dev].needed = 1;
		else
			joypad[i][KEY_BUTTON_L] = DIK_A;

		dev = joypad[i][KEY_BUTTON_R] >> 8;
		if (dev < numDevices && dev >= 0)
			pDevices[dev].needed = 1;
		else
			joypad[i][KEY_BUTTON_R] = DIK_S;

		dev = joypad[i][KEY_BUTTON_START] >> 8;
		if (dev < numDevices && dev >= 0)
			pDevices[dev].needed = 1;
		else
			joypad[i][KEY_BUTTON_START] = DIK_RETURN;

		dev = joypad[i][KEY_BUTTON_SELECT] >> 8;
		if (dev < numDevices && dev >= 0)
			pDevices[dev].needed = 1;
		else
			joypad[i][KEY_BUTTON_SELECT] = DIK_BACK;

		dev = joypad[i][KEY_BUTTON_SPEED] >> 8;
		if (dev < numDevices && dev >= 0)
			pDevices[dev].needed = 1;
		else
			joypad[i][KEY_BUTTON_SPEED] = DIK_SPACE;

		dev = joypad[i][KEY_BUTTON_CAPTURE] >> 8;
		if (dev < numDevices && dev >= 0)
			pDevices[dev].needed = 1;
		else
			joypad[i][KEY_BUTTON_CAPTURE] = DIK_F12;

		dev = joypad[i][KEY_BUTTON_GS] >> 8;
		if (dev < numDevices && dev >= 0)
			pDevices[dev].needed = 1;
		else
			joypad[i][KEY_BUTTON_GS] = DIK_C;
	}

	dev = motion[KEY_UP] >> 8;
	if (dev < numDevices && dev >= 0)
		pDevices[dev].needed = 1;
	else
		motion[KEY_UP] = DIK_NUMPAD8;

	dev = motion[KEY_DOWN] >> 8;
	if (dev < numDevices && dev >= 0)
		pDevices[dev].needed = 1;
	else
		motion[KEY_DOWN] = DIK_NUMPAD2;

	dev = motion[KEY_LEFT] >> 8;
	if (dev < numDevices && dev >= 0)
		pDevices[dev].needed = 1;
	else
		motion[KEY_LEFT] = DIK_NUMPAD4;

	dev = motion[KEY_RIGHT] >> 8;
	if (dev < numDevices && dev >= 0)
		pDevices[dev].needed = 1;
	else
		motion[KEY_RIGHT] = DIK_NUMPAD6;
}

#define KEYDOWN(buffer, key) (buffer[key] & 0x80)

static bool IsKeyDownAsync (WORD KeyIdent)
{
	//if (KeyIdent == 0 || KeyIdent == VK_ESCAPE) // if it's the 'disabled' key, it's never pressed
	//	return false;

	//if (!GUI.BackgroundInput && GUI.hWnd != GetForegroundWindow())
	//	return false;

	// the pause key is special, need this to catch all presses of it
	// Both GetKeyState and GetAsyncKeyState cannot catch it anyway,
	// so this should be handled in WM_KEYDOWN message.
	if (KeyIdent == VK_PAUSE)
	{
		return false;
//		if(GetAsyncKeyState(VK_PAUSE)) // not &'ing this with 0x8000 is intentional and necessary
//			return true;
	}

	if (KeyIdent == VK_CAPITAL || KeyIdent == VK_NUMLOCK || KeyIdent == VK_SCROLL)
		return ((GetKeyState(KeyIdent) & 0x01) != 0);
	else
		return ((GetAsyncKeyState(KeyIdent) & 0x8000) != 0);
	//return ((GetKeyState (KeyIdent) & 0x80) != 0);
}

static bool readKeyboard()
{
#ifndef USE_GETASYNCKEYSTATE_FOR_KEYBOARD
	if (pDevices[0].needed)
	{
		HRESULT hret = pDevices[0].device->
		               GetDeviceState(256,
		                              (LPVOID)pDevices[0].data);

		if (hret == DIERR_INPUTLOST || hret == DIERR_NOTACQUIRED)
		{
			hret = pDevices[0].device->Acquire();
			if (hret != DI_OK)
				return false;
			hret = pDevices[0].device->GetDeviceState(256, (LPVOID)pDevices[0].data);
		}

		return hret == DI_OK;
	}
#else
	for (int i = 0; i < sizeof(win_key_trans_table)/sizeof(win_key_trans_table[0]); i++) {
		pDevices[0].data[win_key_trans_table[i].dik] = IsKeyDownAsync(win_key_trans_table[i].vk) ? 0x80 : 0;
	}
#endif
	return true;
}

static bool readJoystick(int joy)
{
	if (pDevices[joy].needed)
	{
		if (pDevices[joy].isPolled)
			pDevices[joy].device->Poll();

		HRESULT hret = pDevices[joy].device->
		               GetDeviceState(sizeof(DIJOYSTATE),
		                              (LPVOID)&pDevices[joy].state);

		if (hret == DIERR_INPUTLOST || hret == DIERR_NOTACQUIRED)
		{
			hret = pDevices[joy].device->Acquire();

			if (hret == DI_OK)
			{
				if (pDevices[joy].isPolled)
					pDevices[joy].device->Poll();

				hret = pDevices[joy].device->
				       GetDeviceState(sizeof(DIJOYSTATE),
				                      (LPVOID)&pDevices[joy].state);
			}
		}

		return padReadResult(joy, hret);
	}

	return true;
}

static void checkKeyboard()
{
	// mham fix. Patch #1378104
	UCHAR   keystate[256];
	HRESULT hret = pDevices[0].device->Acquire();

	if (pDevices[0].first)
	{
		pDevices[0].device->GetDeviceState(256, (LPVOID)pDevices[0].data);
		pDevices[0].first = FALSE;
		return;
	}

	hret = pDevices[0].device->
	       GetDeviceState(256, (LPVOID)keystate);

	if (hret == DIERR_INPUTLOST || hret == DIERR_NOTACQUIRED)
	{
		return;
	}

	if (hret == DI_OK)
	{
		for (int i = 0; i < 256; i++)
		{
			if (keystate[i] == pDevices[0].data[i])
				continue;
			if (KEYDOWN(keystate, i))
			{
				SendMessage(GetFocus(), JOYCONFIG_MESSAGE, 0, i);
				break;
			}
		}
	}
	memcpy(pDevices[0].data, keystate, sizeof(UCHAR) * 256);
}

static void checkJoypads()
{
	DIDEVICEOBJECTINSTANCE di;

	ZeroMemory(&di, sizeof(DIDEVICEOBJECTINSTANCE));

	di.dwSize = sizeof(DIDEVICEOBJECTINSTANCE);

	int i = 0;

	DIJOYSTATE joystick;

	for (i = 1; i < numDevices; i++)
	{
		HRESULT hret = pDevices[i].device->Acquire();

		if (pDevices[i].isPolled)
			pDevices[i].device->Poll();

		hret = pDevices[i].device->GetDeviceState(sizeof(joystick), &joystick);

		int j;

		// Without a state there is nothing to compare against: the snapshot
		// would be whatever the stack held.  This is also where a controller
		// that was unplugged while the dialog was open gets noticed.
		if (!padReadResult(i, hret))
			continue;

		if (pDevices[i].first)
		{
			memcpy(&pDevices[i].state, &joystick, sizeof(joystick));
			pDevices[i].first = FALSE;
			continue;
		}

		for (j = 0; j < pDevices[i].nButtons; j++)
		{
			if (((pDevices[i].state.rgbButtons[j] ^ joystick.rgbButtons[j])
			     & joystick.rgbButtons[j]) & 0x80)
			{
				HWND focus = GetFocus();

				// joyDebug: a pad that is enumerated but delivers nothing and
				// a pad whose events never reach the field look the same from
				// the outside, so say which one it is.
				if (joyDebug)
					winlog(_T("Joystick %2d button %d -> key %d\n"), i, j, (i << 8) | (j + 128));

				SendMessage(focus, JOYCONFIG_MESSAGE, i, j+128);
			}
		}

		for (j = 0; j < pDevices[i].nAxes && j < 8; j++)
		{
			LONG value = pDevices[i].axis[j].center;
			LONG old   = 0;
			switch (pDevices[i].axis[j].offset)
			{
			case offsetof(DIJOYSTATE, lX):
				value = joystick.lX;
				old   = pDevices[i].state.lX;
				break;
			case offsetof(DIJOYSTATE, lY):
				value = joystick.lY;
				old   = pDevices[i].state.lY;
				break;
			case offsetof(DIJOYSTATE, lZ):
				value = joystick.lZ;
				old   = pDevices[i].state.lZ;
				break;
			case offsetof(DIJOYSTATE, lRx):
				value = joystick.lRx;
				old   = pDevices[i].state.lRx;
				break;
			case offsetof(DIJOYSTATE, lRy):
				value = joystick.lRy;
				old   = pDevices[i].state.lRy;
				break;
			case offsetof(DIJOYSTATE, lRz):
				value = joystick.lRz;
				old   = pDevices[i].state.lRz;
				break;
			case DIJOFS_SLIDER(0):
				value = joystick.rglSlider[0];
				old   = pDevices[i].state.rglSlider[0];
				break;
			case DIJOFS_SLIDER(1):
				value = joystick.rglSlider[1];
				old   = pDevices[i].state.rglSlider[1];
				break;
			}
			if (value != old)
			{
				if (value < pDevices[i].axis[j].negative)
					SendMessage(GetFocus(), JOYCONFIG_MESSAGE, i, (j<<1));
				else if (value > pDevices[i].axis[j].positive)
					SendMessage(GetFocus(), JOYCONFIG_MESSAGE, i, (j<<1)+1);
			}
		}

		for (j = 0; j < 4 && j < pDevices[i].nPovs; j++)
		{
			if (LOWORD(pDevices[i].state.rgdwPOV[j]) != LOWORD(joystick.rgdwPOV[j]))
			{
				int state = getPovState(joystick.rgdwPOV[j]);

				if (state & POV_UP)
					SendMessage(GetFocus(), JOYCONFIG_MESSAGE, i, (j<<2)+0x20);
				else if (state & POV_DOWN)
					SendMessage(GetFocus(), JOYCONFIG_MESSAGE, i, (j<<2)+0x21);
				else if (state & POV_RIGHT)
					SendMessage(GetFocus(), JOYCONFIG_MESSAGE, i, (j<<2)+0x22);
				else if (state & POV_LEFT)
					SendMessage(GetFocus(), JOYCONFIG_MESSAGE, i, (j<<2)+0x23);
			}
		}

		memcpy(&pDevices[i].state, &joystick, sizeof(joystick));
	}
}

BOOL checkKey(LONG_PTR key)
{
	LONG_PTR dev = (key >> 8);

	LONG_PTR k = (key & 255);

	if (dev == 0)
	{
		return KEYDOWN(pDevices[0].data, k);
	}
	else if (dev >= numDevices)
	{
		return FALSE;
	}
	else
	{
		if (k < 16)
		{
			LONG_PTR axis  = k >> 1;
			LONG     value = pDevices[dev].axis[axis].center;
			switch (pDevices[dev].axis[axis].offset)
			{
			case offsetof(DIJOYSTATE, lX):
				value = pDevices[dev].state.lX;
				break;
			case offsetof(DIJOYSTATE, lY):
				value = pDevices[dev].state.lY;
				break;
			case offsetof(DIJOYSTATE, lZ):
				value = pDevices[dev].state.lZ;
				break;
			case offsetof(DIJOYSTATE, lRx):
				value = pDevices[dev].state.lRx;
				break;
			case offsetof(DIJOYSTATE, lRy):
				value = pDevices[dev].state.lRy;
				break;
			case offsetof(DIJOYSTATE, lRz):
				value = pDevices[dev].state.lRz;
				break;
			case DIJOFS_SLIDER(0):
				value = pDevices[dev].state.rglSlider[0];
				break;
			case DIJOFS_SLIDER(1):
				value = pDevices[dev].state.rglSlider[1];
				break;
			}

			if (k & 1)
				return value > pDevices[dev].axis[axis].positive;
			return value < pDevices[dev].axis[axis].negative;
		}
		else if (k < 48)
		{
			LONG_PTR hat   = (k >> 2) & 3;
			int      state = getPovState(pDevices[dev].state.rgdwPOV[hat]);
			BOOL     res   = FALSE;
			switch (k & 3)
			{
			case 0:
				res = state & POV_UP;
				break;
			case 1:
				res = state & POV_DOWN;
				break;
			case 2:
				res = state & POV_RIGHT;
				break;
			case 3:
				res = state & POV_LEFT;
				break;
			}
			return res;
		}
		else if (k  >= 128)
		{
			return pDevices[dev].state.rgbButtons[k-128] & 0x80;
		}
	}

	return FALSE;
}

DirectInput::DirectInput()
{
	dinputDLL = NULL;
}

DirectInput::~DirectInput()
{
	saveSettings();
	if (pDirectInput != NULL)
	{
		if (pDevices)
		{
			for (int i = 0; i < numDevices; i++)
			{
				if (pDevices[i].device)
				{
					pDevices[i].device->Unacquire();
					pDevices[i].device->Release();
					pDevices[i].device = NULL;
				}
			}
			free(pDevices);
			pDevices = NULL;
		}

		pDirectInput->Release();
		pDirectInput = NULL;
	}

	if (dinputDLL)
	{
		/**/ ::FreeLibrary(dinputDLL);
		dinputDLL = NULL;
	}
}

// "VBA.ini" next to the executable.  A bare file name does not mean the
// working directory to the profile API - it means the Windows directory - so
// the debug switch below could never be turned on from where the emulator
// actually lives.
static LPCTSTR settingsPath()
{
	static TCHAR path[MAX_PATH] = _T("");

	if (path[0] == 0)
	{
		if (GetModuleFileName(NULL, path, MAX_PATH) == 0)
			lstrcpy(path, _T("VBA.ini"));
		else
		{
			TCHAR *slash = _tcsrchr(path, _T('\\'));

			if (slash != NULL)
				lstrcpy(slash + 1, _T("VBA.ini"));
		}
	}

	return path;
}

bool DirectInput::initialize()
{
	joyDebug = GetPrivateProfileInt(_T("config"),
	                                _T("joyDebug"),
	                                0,
	                                settingsPath());
	// DirectInput 8 rather than the DirectInput 5 in DINPUT.DLL: same
	// interfaces, but the HID side is the one every program written since
	// 2000 uses, and it is the one that gets the bug fixes.
	dinputDLL = /**/ ::LoadLibrary(_T("dinput8.dll"));

	decltype(&DirectInput8Create) DInputCreate;

	if (dinputDLL != NULL)
	{
		DInputCreate = reinterpret_cast<decltype(&DirectInput8Create)>(
			GetProcAddress(dinputDLL, "DirectInput8Create"));

		if (DInputCreate == NULL)
		{
			directXMessage("DirectInput8Create");
			return false;
		}
	}
	else
	{
		directXMessage("dinput8.dll");
		return false;
	}

	HRESULT hret = DInputCreate(AfxGetInstanceHandle(),
	                            DIRECTINPUT_VERSION,
	                            IID_IDirectInput8,
	                            (LPVOID *)&pDirectInput,
	                            NULL);
	if (hret != DI_OK)
	{
		//    errorMessage(myLoadString(IDS_ERROR_DISP_CREATE), hret);
		return false;
	}

	hret = pDirectInput->EnumDevices(DI8DEVCLASS_GAMECTRL,
	                                 DIEnumDevicesCallback2,
	                                 NULL,
	                                 DIEDFL_ATTACHEDONLY);

	pDevices = (deviceInfo *)calloc(numDevices, sizeof(deviceInfo));

	hret = pDirectInput->CreateDevice(GUID_SysKeyboard, &pDevices[0].device, NULL);
	pDevices[0].isPolled = false;
	pDevices[0].needed   = true;
	pDevices[0].first    = true;

	if (hret != DI_OK)
	{
		//    errorMessage(myLoadString(IDS_ERROR_DISP_CREATEDEVICE), hret);
		return false;
	}

	numDevices = 1;

	// before the pads are enumerated: a Nintendo-style controller has to be
	// asked for the report the OS can decode, or it never sends one
	requestSimpleHidReports();

	hret = pDirectInput->EnumDevices(DI8DEVCLASS_GAMECTRL,
	                                 DIEnumDevicesCallback,
	                                 NULL,
	                                 DIEDFL_ATTACHEDONLY);

	//  hret = pDevices[0].device->SetCooperativeLevel(hWindow,
	//                                             DISCL_FOREGROUND|
	//                                             DISCL_NONEXCLUSIVE);

	if (hret != DI_OK)
	{
		//    errorMessage(myLoadString(IDS_ERROR_DISP_LEVEL), hret);
		return false;
	}

	hret = pDevices[0].device->SetDataFormat(&c_dfDIKeyboard);

	if (hret != DI_OK)
	{
		//    errorMessage(myLoadString(IDS_ERROR_DISP_DATAFORMAT), hret);
		return false;
	}

	for (int i = 1; i < numDevices; i++)
		setupJoystick(i);

	for (int i = 0; i < numDevices; i++)
		pDevices[i].device->Acquire();

	return true;
}

namespace
{
	struct AttachedDevices
	{
		int  count;
		GUID guid[16];
	};

	BOOL CALLBACK collectDevice(LPCDIDEVICEINSTANCE inst, LPVOID param)
	{
		AttachedDevices *list = (AttachedDevices *)param;

		if (list->count < (int)(sizeof(list->guid) / sizeof(list->guid[0])))
			list->guid[list->count++] = inst->guidInstance;

		return DIENUM_CONTINUE;
	}

	bool deviceMatches(LPDIRECTINPUTDEVICE8 device, const GUID &guid)
	{
		if (device == NULL)
			return false;

		DIDEVICEINSTANCE info;
		ZeroMemory(&info, sizeof(info));
		info.dwSize = sizeof(info);

		return SUCCEEDED(device->GetDeviceInfo(&info)) &&
		       IsEqualGUID(info.guidInstance, guid) != 0;
	}
}

// The device list is built once, at startup.  A controller that was asleep,
// unplugged, or paired after the emulator started is not in it - and the
// setup dialog then captures nothing at all, without an error to show for it.
// The dialogs re-read the attached devices when they open.
//
// The index of a device is part of every saved binding, so a known device is
// recreated in place and keeps its index; only devices that were never seen
// before are appended.  A device that is gone keeps its slot dead, which is
// what an unplugged controller has always looked like.
void DirectInput::refreshDevices()
{
	if (pDirectInput == NULL || pDevices == NULL)
		return;

	// a pad that reconnected since startup is back in its standard report
	// mode, so ask again before the device list is rebuilt
	requestSimpleHidReports();

	AttachedDevices attached;
	attached.count = 0;

	pDirectInput->EnumDevices(DI8DEVCLASS_GAMECTRL, collectDevice, &attached,
	                          DIEDFL_ATTACHEDONLY);

	for (int i = 1; i < numDevices; i++)
	{
		GUID guid;
		bool attachedNow = false;

		for (int d = 0; d < attached.count && !attachedNow; d++)
		{
			if (deviceMatches(pDevices[i].device, attached.guid[d]))
			{
				guid        = attached.guid[d];
				attachedNow = true;
			}
		}

		if (!attachedNow)
		{
			// the pad was pulled out or went to sleep: nothing else polls the
			// device list, so this is where the loss is noticed
			if (pDevices[i].attached)
			{
				pDevices[i].attached  = FALSE;
				pDevices[i].failCount = 0;
				reportPad(pDevices[i], false);
			}
			continue;
		}

		// a controller that slept and woke up comes back with the old object
		// still pointing at a device that no longer exists
		pDevices[i].device->Unacquire();
		pDevices[i].device->Release();
		pDevices[i].device = NULL;

		if (SUCCEEDED(pDirectInput->CreateDevice(guid, &pDevices[i].device, NULL)) &&
		    pDevices[i].device != NULL)
		{
			setupJoystick(i);
			pDevices[i].device->Acquire();

			if (!pDevices[i].attached)
			{
				pDevices[i].attached = TRUE;
				reportPad(pDevices[i], true);
			}
		}
	}

	for (int d = 0; d < attached.count; d++)
	{
		bool known = false;

		for (int i = 1; i < numDevices && !known; i++)
			known = deviceMatches(pDevices[i].device, attached.guid[d]);

		if (known)
			continue;

		LPDIRECTINPUTDEVICE8 device = NULL;

		if (FAILED(pDirectInput->CreateDevice(attached.guid[d], &device, NULL)) ||
		    device == NULL)
			continue;

		deviceInfo *grown = (deviceInfo *)realloc(pDevices, (numDevices + 1) * sizeof(deviceInfo));

		if (grown == NULL)
		{
			device->Release();
			continue;
		}

		pDevices = grown;
		ZeroMemory(&pDevices[numDevices], sizeof(deviceInfo));
		pDevices[numDevices].device = device;

		if (joyDebug)
			winlog(_T("Joystick %2d added   : (attached after startup)\n"), numDevices);

		DIDEVICEINSTANCE instance;
		ZeroMemory(&instance, sizeof(instance));
		instance.dwSize = sizeof(instance);

		if (SUCCEEDED(device->GetDeviceInfo(&instance)))
		{
			rememberPad(instance.tszProductName);
			lstrcpyn(pDevices[numDevices].name, instance.tszProductName,
			          sizeof(pDevices[numDevices].name));
		}

		pDevices[numDevices].attached = TRUE;

		setupJoystick(numDevices);
		device->Acquire();
		numDevices++;
	}

	announcePads();
}

bool DirectInput::readDevices()
{
	updateAutoFire();

	bool ok = true;
	for (int i = 0; i < numDevices; i++)
	{
		if (pDevices[i].needed)
		{
			ok = (i > 0 ? readJoystick(i) : readKeyboard()) || ok;
		}
	}
	return ok;
}

bool inputActive = true; // used to disable all input when the window is inactive

static const USHORT s_autoFireMaskOrder[13] = {
	BUTTON_MASK_LEFT, BUTTON_MASK_RIGHT,
	BUTTON_MASK_UP, BUTTON_MASK_DOWN,
	BUTTON_MASK_A, BUTTON_MASK_B,
	BUTTON_MASK_START, BUTTON_MASK_SELECT,
	BUTTON_MASK_L, BUTTON_MASK_R,
	0, 0, 0
};

// Auto-Fire keys that were down during the previous poll, used to detect the
// key transitions that switch autofire on and off.
static u32 s_autoFireKeyLatch = 0;

// Keyboard Auto-Fire keys are polled through GetAsyncKeyState: the DirectInput
// device state is only refreshed while the window is active, and the Auto-Fire
// key is frequently a key that is not bound to any game button.
static bool isAutoFireKeyDown(USHORT key)
{
	LONG_PTR dev = (key >> 8);
	LONG_PTR dik = (key & 255);

	if (dev == 0)
	{
		for (int i = 0; i < sizeof(win_key_trans_table) / sizeof(win_key_trans_table[0]); i++)
		{
			if (win_key_trans_table[i].dik == dik)
				return IsKeyDownAsync((WORD)win_key_trans_table[i].vk);
		}
		return false;
	}
	return checkKey(key) != FALSE;
}

u32 DirectInput::readAutoFireBound(int i)
{
	extern int systemGetDefaultJoypad();
	extern int gbSgbMode, gbSgbMultiplayer;
	if (!(gbSgbMode && gbSgbMultiplayer))
		i = systemGetDefaultJoypad();

	u32 res = 0;
	for (int k = 0; k < 13; k++)
	{
		if (autoFireKeys[i][k] != 0)
			res |= s_autoFireMaskOrder[k];
	}
	return res;
}

u32 DirectInput::readAutoFireHeld(int i)
{
	extern int systemGetDefaultJoypad();
	extern int gbSgbMode, gbSgbMultiplayer;
	if (!(gbSgbMode && gbSgbMultiplayer))
		i = systemGetDefaultJoypad();

	if (!inputActive)
		return 0;

	u32 res = 0;
	for (int k = 0; k < 13; k++)
	{
		USHORT key = autoFireKeys[i][k];
		if (key != 0 && isAutoFireKeyDown(key))
			res |= s_autoFireMaskOrder[k];
	}
	return res;
}

// Drives the configurable Auto-Fire keys.  Pressing such a key switches
// autofire on for its button, releasing it switches it off again -- exactly what
// the Auto-Fire menu commands do for Alt+0-9.
// If the button already fires because Alt+? enabled it, the key inverts it
// while it is held: pressing it temporarily disables the autofire.
// Only key transitions are handled: keeping the mask in the active half on
// every poll re-armed it on every frame, so the button was XORed on each frame
// and stayed held down instead of alternating.
void DirectInput::updateAutoFire()
{
	extern int gbSgbMode, gbSgbMultiplayer;
	extern int systemGetDefaultJoypad();

	if (gbSgbMode && gbSgbMultiplayer)
	{
		s_autoFireKeyLatch = 0;
		return;
	}

	u32 bound = 0;
	for (int i = 0; i < 4; i++)
		bound |= readAutoFireBound(i);

	if (bound == 0)
	{
		s_autoFireKeyLatch = 0;
		return;
	}

	// readAutoFireHeld() returns 0 while the window is inactive
	u32 held     = readAutoFireHeld(systemGetDefaultJoypad());
	u32 pressed  = (held & ~s_autoFireKeyLatch) & bound;
	u32 released = (~held & s_autoFireKeyLatch) & bound;
	s_autoFireKeyLatch = held;

	if ((pressed | released) == 0)
		return;

	for (int k = 0; k < 13; k++)
	{
		u32 mask = s_autoFireMaskOrder[k];
		if (mask == 0)
			continue;

		if ((pressed ^ released) & mask)
			VBAToggleAutoFireButton(mask);
	}
}

u32 DirectInput::readDevice(int i, bool sensor)
{
	// this old hack is evil
	extern int systemGetDefaultJoypad();
	extern int gbSgbMode, gbSgbMultiplayer;
	if (!(gbSgbMode && gbSgbMultiplayer))
		i = systemGetDefaultJoypad();

	u32 res = 0;

	// manual input
	if (inputActive)
	{
		if (checkKey(joypad[i][KEY_BUTTON_A]))
			res |= BUTTON_MASK_A;
		if (checkKey(joypad[i][KEY_BUTTON_B]))
			res |= BUTTON_MASK_B;
		if (checkKey(joypad[i][KEY_BUTTON_SELECT]))
			res |= BUTTON_MASK_SELECT;
		if (checkKey(joypad[i][KEY_BUTTON_START]))
			res |= BUTTON_MASK_START;
		if (checkKey(joypad[i][KEY_RIGHT]))
			res |= BUTTON_MASK_RIGHT;
		if (checkKey(joypad[i][KEY_LEFT]))
			res |= BUTTON_MASK_LEFT;
		if (checkKey(joypad[i][KEY_UP]))
			res |= BUTTON_MASK_UP;
		if (checkKey(joypad[i][KEY_DOWN]))
			res |= BUTTON_MASK_DOWN;
		if (checkKey(joypad[i][KEY_BUTTON_R]))
			res |= BUTTON_MASK_R;
		if (checkKey(joypad[i][KEY_BUTTON_L]))
			res |= BUTTON_MASK_L;

		// unused
		if (checkKey(motion[KEY_LEFT]))
			res |= BUTTON_MASK_LEFT_MOTION;
		else if (checkKey(motion[KEY_RIGHT]))
			res |= BUTTON_MASK_RIGHT_MOTION;
		if (checkKey(motion[KEY_UP]))
			res |= BUTTON_MASK_UP_MOTION;
		else if (checkKey(motion[KEY_DOWN]))
			res |= BUTTON_MASK_DOWN_MOTION;
	}

	u32 hackedButtons = 0;
	if (inputActive)
	{
		// the "non-button" buttons (what a hack!)
		if (checkKey(joypad[i][KEY_BUTTON_SPEED]))
			hackedButtons |= BUTTON_MASK_SPEED;
		if (checkKey(joypad[i][KEY_BUTTON_CAPTURE]))
			hackedButtons |= BUTTON_MASK_CAPTURE;
		if (checkKey(joypad[i][KEY_BUTTON_GS]))
			hackedButtons |= BUTTON_MASK_GAMESHARK;
	}

	extern bool systemIsSpedUp();
	if (systemIsSpedUp())
		hackedButtons |= BUTTON_MASK_SPEED;

	return res | hackedButtons;
}

CString DirectInput::getKeyName(LONG_PTR key)
{
	// Nothing bound: the setup dialog clears a key by storing 0, which is
	// device 0, scan code 0 - a code no key produces.
	if (key == 0)
		return "";

	LONG_PTR d = (key >> 8);
	LONG_PTR k = key & 255;

	DIDEVICEOBJECTINSTANCE di;

	ZeroMemory(&di, sizeof(DIDEVICEOBJECTINSTANCE));

	di.dwSize = sizeof(DIDEVICEOBJECTINSTANCE);

	CString winBuffer = winResLoadString(IDS_ERROR);

	if (d == 0)
	{
		pDevices[0].device->GetObjectInfo(&di, (DWORD)key, DIPH_BYOFFSET);
		winBuffer = di.tszName;
	}
	else if (d < numDevices)
	{
		if (k < 16)
		{
			if (k < 4)
			{
				switch (k)
				{
				case 0:
					winBuffer.Format(winResLoadString(IDS_JOY_LEFT), d);
					break;
				case 1:
					winBuffer.Format(winResLoadString(IDS_JOY_RIGHT), d);
					break;
				case 2:
					winBuffer.Format(winResLoadString(IDS_JOY_UP), d);
					break;
				case 3:
					winBuffer.Format(winResLoadString(IDS_JOY_DOWN), d);
					break;
				}
			}
			else
			{
				pDevices[d].device->GetObjectInfo(&di,
				                                  pDevices[d].axis[k>>1].offset,
				                                  DIPH_BYOFFSET);
				if (k & 1)
					winBuffer.Format(_T("Joy %d %s +"), d, di.tszName);
				else
					winBuffer.Format(_T("Joy %d %s -"), d, di.tszName);
			}
		}
		else if (k < 48)
		{
			LONG_PTR hat = (k >> 2) & 3;
			pDevices[d].device->GetObjectInfo(&di,
			                                  (DWORD)DIJOFS_POV(hat),
			                                  DIPH_BYOFFSET);
			const TCHAR *dir = _T("up");
			LONG_PTR dd  = k & 3;
			if (dd == 1)
				dir = _T("down");
			else if (dd == 2)
				dir = _T("right");
			else if (dd == 3)
				dir = _T("left");
			winBuffer.Format(_T("Joy %d %s %s"), d, di.tszName, dir);
		}
		else
		{
			pDevices[d].device->GetObjectInfo(&di,
			                                  (DWORD)DIJOFS_BUTTON(k-128),
			                                  DIPH_BYOFFSET);
			winBuffer.Format(winResLoadString(IDS_JOY_BUTTON), d, di.tszName);
		}
	}
	else
	{
		// Joystick isn't plugged in.  We can't decipher k, so just show its value.
		winBuffer.Format(_T("Joy %d (%d)"), d, k);
	}

	return winBuffer;
}

void DirectInput::checkKeys()
{
	::checkKeys();
}

Input *newDirectInput()
{
	return new DirectInput;
}

void DirectInput::checkDevices()
{
	checkJoypads();
	checkKeyboard();
}

// The devices are enumerated before the main window exists, so the message
// about them waits until there is one to own it.
void DirectInput::announceDevices()
{
	announcePads();
}

int DirectInput::gamepadCount()
{
	int count = 0;

	if (pDevices == NULL)
		return 0;

	for (int i = 1; i < numDevices; i++)
		if (pDevices[i].attached && pDevices[i].name[0] != 0)
			count++;

	return count;
}

bool DirectInput::gamepadInfo(int n, int &deviceIndex, CString &name)
{
	if (pDevices == NULL)
		return false;

	for (int i = 1; i < numDevices; i++)
	{
		if (!pDevices[i].attached || pDevices[i].name[0] == 0)
			continue;

		if (n-- == 0)
		{
			deviceIndex = i;
			name        = pDevices[i].name;
			return true;
		}
	}

	return false;
}

int DirectInput::getDeadzone()
{
	return joypadDeadzone;
}

void DirectInput::setDeadzone(int percent)
{
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;

	joypadDeadzone = percent;

	applyDeadzone();
}

void DirectInput::activate()
{
	for (int i = 0; i < numDevices; i++)
	{
		if (pDevices != NULL && pDevices[i].device != NULL)
			pDevices[i].device->Acquire();
	}
}

void DirectInput::loadSettings()
{
	winReadKeys();
}

void DirectInput::saveSettings()
{
	winSaveKeys();
}

