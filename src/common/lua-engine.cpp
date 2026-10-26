#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <string>
#include <cassert>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cstdint>

#include <vector>
#include <map>
#include <string>
#include <algorithm>

using std::string;
using std::vector;
using std::map;

#ifdef __linux
	#include <unistd.h> // for unlink
	#include <sys/types.h>
	#include <sys/wait.h>
#endif
#if (defined(WIN32) && !defined(SDL))
	#include <direct.h>
	// Win32 API only. The MFC/frontend types used to come from
	// ../win32/stdafx.h + MainWnd.h + VBA.h + Dialogs/LuaOpenDialog.h; all of
	// that is now behind common/LuaHost.h, implemented per frontend.
	#include <windows.h>
	#include <winver.h>
#else
	#define stricmp strcasecmp
	#define strnicmp strncasecmp
#endif

#include "../Port.h"
#include "System.h"
#include "movie.h"
#include "../common/SystemGlobals.h"
#include "../gba/GBA.h"
#include "../gba/GBAinline.h"
#include "../gba/GBAGlobals.h"
#include "../gb/GB.h"
#include "../gb/gbGlobals.h"
#include "../gba/GBASound.h"

#ifdef _WIN32
//#include "../win32/WinMiscUtil.h"
#else
#endif
#include "LuaHost.h" // frontend UI services: dialogs, geometry, AVI, savestates, console

bool DemandLua()
{
#ifdef _WIN32
	HMODULE mod = LoadLibraryA("lua51.dll");
	if(!mod)
	{
		lua_host::messageBox("lua51.dll was not found. Please get it into your PATH or in the same directory as VBA.exe", "VBA", lua_host::MsgBoxType::Ok, true);
		return false;	}
	FreeLibrary(mod);
	return true;
#else
	return true;
#endif
}

// The engine is whatever lua51.dll sits next to the exe.  This build ships
// LuaJIT 2.1 (win32/lib/{x86,x64,arm64}/lua51.dll) and compiles against its
// headers, which are Lua 5.1's API plus `bit`, `jit` and `ffi`; dropping a
// stock Lua 5.1 DLL in its place still works, because the 5.1 ABI is what
// LuaJIT implements.  So nothing in here may depend on a LuaJIT extension -
// that is why the five legacy bit globals were moved to plain C.
extern "C"
{
#include "../lua/src/lua.h"
#include "../lua/src/lauxlib.h"
#include "../lua/src/lualib.h"
}
#include "vbalua.h"

// Lua's math.random()/math.randomseed() run on SFMT 1.5.1 (period 2^19937-1).
// SFMT.c is compiled as its own translation unit; SFMT_MEXP comes from the
// project settings so that SFMT.c and this file agree on the parameters.
#include "../SFMT/SFMT.h"

// Console sinks.  The frontend supplies three functions that write into "the
// console" named by a handle; the handle belongs to a script (VbaLuaState::
// console), which is what lets several consoles exist at the same time.  The
// encoding travels with the text because each console may be rendering a
// script saved in a different one (see "Script text encoding" below).
static void (*info_print)(std::intptr_t uid, lua_host::TextEncoding encoding, const char *str);
static void (*info_onstart)(std::intptr_t uid);
static void (*info_onstop)(std::intptr_t uid);

#ifndef countof
	#define countof(a)  (sizeof(a) / sizeof(a[0]))
#endif

// Registry keys, not per-script slots: every script owns a registry.
static const char *frameAdvanceThread = "VBA.FrameAdvance";
static const char *guiCallbackTable	  = "VBA.GUI";

// Remap input display
struct LuaInputDisplayRemap
{
	uint32 from;
	uint32 to[10];
};

// the purpose of this structure is to provide a way of
// QUICKLY determining whether a memory address range has a hook associated with it,
// with a bias toward fast rejection because the majority of addresses will not be hooked.
// (it must not use any part of Lua or perform any per-script operations,
//  otherwise it would definitely be too slow.)
// calculating the regions when a hook is added/removed may be slow,
// but this is an intentional tradeoff to obtain a high speed of checking during later execution
struct TieredRegion
{
	template<unsigned int maxGap>
	struct Region
	{
		struct Island
		{
			unsigned int	   start;
			unsigned int	   end;
			__forceinline bool Contains(unsigned int address, int size) const { return address < end && address + size > start; }
		};
		std::vector<Island> islands;

		void Calculate(const std::vector<unsigned int> &bytes)
		{
			islands.clear();

			unsigned int lastEnd = ~0;

			std::vector<unsigned int>::const_iterator iter = bytes.begin();
			std::vector<unsigned int>::const_iterator end  = bytes.end();
			for (; iter != end; ++iter)
			{
				unsigned int addr = *iter;
				if (addr < lastEnd || addr > lastEnd + (long long)maxGap)
				{
					islands.push_back(Island());
					islands.back().start = addr;
				}
				islands.back(). end = addr + 1;
				lastEnd = addr + 1;
			}
		}

		bool Contains(unsigned int address, int size) const
		{
			//for (size_t i = 0, j = islands.size(); i != j; ++i)
			for (size_t i = islands.size(); i--; )
			{
				if (islands[i].Contains(address, size))
					return true;
			}
			return false;
		}
	};

	Region<0xFFFFFFFF> broad;
	Region<0x1000>	   mid;
	Region<0>		   narrow;

	void Calculate(std::vector<unsigned int> &bytes)
	{
		std:: sort(bytes.begin(), bytes.end());

		broad.Calculate(bytes);
		mid.Calculate(bytes);
		narrow.Calculate(bytes);
	}

	TieredRegion()
	{
		std::vector <unsigned int> temp;
		Calculate(temp);
	}

	__forceinline int NotEmpty()
	{
		return (int)broad.islands.size();
	}

	// note: it is illegal to call this if NotEmpty() returns 0
	__forceinline bool Contains(unsigned int address, int size)
	{
		return broad.islands[0].Contains(address, size) &&
				mid.Contains(address, size) &&
				narrow.Contains(address, size);
	}
};

// ---------------------------------------------------------------------------
// VbaLuaState -- one script window
//
// Everything that used to be a file-scope static in this file lives here now,
// because the emulator can run several scripts at once: each console window
// owns one VbaLuaState, and two scripts share nothing but the emulated machine.
//
// What is deliberately still process-wide:
//   * the three console sinks above: every console is served by the same three
//     functions, they differ only in the handle they are given;
//   * hookedRegions, the merged view of every script's memory hooks, which is
//     what the (very hot) dispatch tests before it looks at any script;
//   * g_luaStates.  Its order is load order, and that is also the order in
//     which frame and gui callbacks run, so the same set of scripts produces
//     the same sequence of callbacks on every run.
// ---------------------------------------------------------------------------
class VbaLuaState
{
public:
	enum SpeedMode { SPEED_NORMAL, SPEED_NOTHROTTLE, SPEED_TURBO, SPEED_MAXIMUM };

	// --- identity ---------------------------------------------------------
	lua_State	 *L;			// NULL when no script is loaded
	std::string	  scriptName;	// the file it was loaded from
	std::intptr_t console;		// the frontend console its output goes to

	// How the bytes of this script's strings should be read: UTF-8 when the
	// file starts with a BOM, the process code page otherwise.  Set by load().
	lua_host::TextEncoding encoding;

	// --- run state --------------------------------------------------------
	bool  running;				// callbacks are still live (see frameBoundaryStep)
	bool  frameBoundary;		// true while inside frameBoundaryStep()
	bool  frameAdvanceWaiting;	// a coroutine is parked at a frame advance
	int   numTries;				// runaway guard, see VBALuaHookFunction
	SpeedMode speedmode;
	bool8 skipRerecords;

	// --- what this script feeds the emulated machine ----------------------
	int	   transparency;		// 255 = opaque, 0 = invisible
	uint16 joypad[4];
	uint8  joypadUsed;
	LuaInputDisplayRemap inputRemap[4], nextInputRemap[4];

	// --- gui canvas -------------------------------------------------------
	bool   guiUsed;
	uint8 *guiData;				// BGRA, allocated on the first draw

	// --- memory hooks -----------------------------------------------------
	unsigned int memHooks;		// hooked bytes, this script only
	TieredRegion memRegions[LUAMEMHOOK_COUNT];

	// --- math.random() ----------------------------------------------------
	sfmt_t rng;

	VbaLuaState();
	~VbaLuaState();

	bool load(const char *filename);
	void stop();
	void resetAfterStop();
	void frameBoundaryStep();
	void callCallbacks(LuaCallID calltype);
	void callExitFunction();
	void guiFrame(uint8 *screen, int pitch, int width, int height);

	// The instance a lua_State belongs to, or NULL.  Coroutines work too: a
	// thread shares the registry of the state that created it.
	static VbaLuaState *fromLua(lua_State *L);
};

// Live scripts, in load order.
static std::vector<VbaLuaState *> g_luaStates;

// address only; used as a registry key so that no string is hashed
static char s_luaStateKey;

VbaLuaState *VbaLuaState::fromLua(lua_State *L)
{
	if (!L)
		return NULL;

	lua_pushlightuserdata(L, &s_luaStateKey);
	lua_rawget(L, LUA_REGISTRYINDEX);
	VbaLuaState *state = (VbaLuaState *)lua_touserdata(L, -1);
	lua_pop(L, 1);

	return state;
}

VbaLuaState::VbaLuaState()
	: L(NULL), console(0), encoding(lua_host::TextEncoding::Ansi), running(false),
	  frameBoundary(false), frameAdvanceWaiting(false), numTries(0),
	  speedmode(SPEED_NORMAL), skipRerecords(false), transparency(255),
	  joypadUsed(0), guiUsed(false), guiData(NULL), memHooks(0)
{
	memset(joypad, 0, sizeof(joypad));
	memset(inputRemap, 0, sizeof(inputRemap));
	memset(nextInputRemap, 0, sizeof(nextInputRemap));
	memset(&rng, 0, sizeof(rng));
}

VbaLuaState::~VbaLuaState()
{
	free(guiData);
}

// The merged view of what every script is feeding the emulator this frame.
// joypad.set() replaces the whole pad, so two scripts that both want pad 0 can
// only be combined by or-ing their buttons together.
static uint8 mergedLuaJoypadMask()
{
	uint8 mask = 0;

	for (size_t i = 0; i < g_luaStates.size(); ++i)
		mask |= g_luaStates[i]->joypadUsed;

	return mask;
}

static uint16 mergedLuaJoypad(int which)
{
	uint16 buttons = 0;

	for (size_t i = 0; i < g_luaStates.size(); ++i)
	{
		if (g_luaStates[i]->joypadUsed & (1 << which))
			buttons |= g_luaStates[i]->joypad[which];
	}

	return buttons;
}

// Installed once; the sink functions themselves are the same for every console.
static void ensureConsoleSinks()
{
	if (info_print)
		return;

#ifdef WIN32
	lua_host::ConsoleSinks sinks = lua_host::consoleSinks();
	info_print	 = sinks.print;
	info_onstart = sinks.onStart;
	info_onstop	 = sinks.onStop;
#endif
}

static void CalculateMemHookRegions(LuaMemHookType hookType);

// Look in inputglobal.h for macros named like BUTTON_MASK_UP to determine the order.
static const char *button_mappings[] = {
	"A", "B", "select", "start", "right", "left", "up", "down", "R", "L"
};

#ifdef _MSC_VER
	#define snprintf _snprintf
	#define vscprintf _vscprintf
#else
	#define stricmp strcasecmp
	#define strnicmp strncasecmp
	#define __forceinline __attribute__((always_inline))
#endif

static const char *luaCallIDStrings[] =
{
	"CALL_BEFOREEMULATION",
	"CALL_AFTEREMULATION",
	"CALL_BEFOREEXIT",
	"CALL_AFTERPOWERON",
	"CALL_BEFOREPOWEROFF",
	"CALL_BEFORESTATELOAD",
	"CALL_AFTERSTATELOAD",
	"CALL_BEFORESTATESAVE",
	"CALL_AFTERSTATESAVE",
};

//make sure we have the right number of strings
CTASSERT(sizeof(luaCallIDStrings) / sizeof(*luaCallIDStrings) == LUACALL_COUNT)

static const char *luaMemHookTypeStrings [] =
{
	"MEMHOOK_WRITE",
	"MEMHOOK_READ",
	"MEMHOOK_EXEC",

	"MEMHOOK_WRITE_SUB",
	"MEMHOOK_READ_SUB",
	"MEMHOOK_EXEC_SUB",
};

//make sure we have the right number of strings
CTASSERT(sizeof(luaMemHookTypeStrings) / sizeof(*luaMemHookTypeStrings) ==  LUAMEMHOOK_COUNT)

static const char* luaJoypadTypeStrings[] =
{
	"JOYPAD_USER",
	"JOYPAD_PLAYBACK",
	"JOYPAD_RECORD",
	"JOYPAD_LUA",
	"JOYPAD_GAME",
};

//make sure we have the right number of strings
CTASSERT(sizeof(luaJoypadTypeStrings) / sizeof(*luaJoypadTypeStrings) == LUAJOYPAD_COUNT)

static char *rawToCString(lua_State * L, int idx = 0);
static const char *toCString(lua_State *L, int idx = 0);

typedef void (*GetColorFunc)(const uint8 *, uint8 *, uint8 *, uint8 *);
typedef void (*SetColorFunc)(uint8 *, uint8, uint8, uint8);

static void getColor16(const uint8 *s, uint8 *r, uint8 *g, uint8 *b)
{
	u16 v = *(const uint16 *)s;
	*r = ((v >> systemBlueShift) & 0x001f) << 3;
	*g = ((v >> systemGreenShift) & 0x001f) << 3;
	*b = ((v >> systemRedShift) & 0x001f) << 3;
}

static void getColor24(const uint8 *s, uint8 *r, uint8 *g, uint8 *b)
{
	if (systemRedShift > systemBlueShift)
		*b = s[0], *g = s[1], *r = s[2];
	else
		*r = s[0], *g = s[1], *b = s[2];
}

static void getColor32(const uint8 *s, uint8 *r, uint8 *g, uint8 *b)
{
	u32 v = *(const uint32 *)s;
	*b = ((v >> systemBlueShift) & 0x001f) << 3;
	*g = ((v >> systemGreenShift) & 0x001f) << 3;
	*r = ((v >> systemRedShift) & 0x001f) << 3;
}

static void setColor16(uint8 *s, uint8 r, uint8 g, uint8 b)
{
	*(uint16 *)s = ((b >> 3) & 0x01f) <<
				   systemBlueShift |
				   ((g >> 3) & 0x01f) <<
				   systemGreenShift |
				   ((r >> 3) & 0x01f) <<
				   systemRedShift;
}

static void setColor24(uint8 *s, uint8 r, uint8 g, uint8 b)
{
	if (systemRedShift > systemBlueShift)
		s[0] = b, s[1] = g, s[2] = r;
	else
		s[0] = r, s[1] = g, s[2] = b;
}

static void setColor32(uint8 *s, uint8 r, uint8 g, uint8 b)
{
	*(uint32 *)s = ((b >> 3) & 0x01f) <<
				   systemBlueShift |
				   ((g >> 3) & 0x01f) <<
				   systemGreenShift |
				   ((r >> 3) & 0x01f) <<
				   systemRedShift;
}

static bool getColorIOFunc(int depth, GetColorFunc *getColor, SetColorFunc *setColor)
{
	switch (depth)
	{
	case 16:
		if (getColor)
			*getColor = getColor16;
		if (setColor)
			*setColor = setColor16;
		return true;
	case 24:
		if (getColor)
			*getColor = getColor24;
		if (setColor)
			*setColor = setColor24;
		return true;
	case 32:
		if (getColor)
			*getColor = getColor32;
		if (setColor)
			*setColor = setColor32;
		return true;
	default:
		return false;
	}
}

/**
 * Resets emulator speed / pause states after script exit.
 */
void VbaLuaState::resetAfterStop(void)
{
	running		= false;
	joypadUsed	= 0;
	guiUsed		= false;
	for (int i = 0; i < 4; ++i)
	{
		inputRemap[i].from	   = 0;
		nextInputRemap[i].from = 0;
	}
	//if (wasPaused)
	//	systemSetPause(true);
}

/**
 * Asks Lua if it wants control of the emulator's speed.
 * Returns 0 if no, 1 if yes. If yes, we also tamper with the
 * IPPU's settings for speed ourselves, so the calling code
 * need not do anything.
 *
 * Several scripts may disagree; the fastest mode wins, the same way the merged
 * joypad takes every script's buttons.
 */
int VBALuaSpeed(void)
{
	VbaLuaState::SpeedMode fastest = VbaLuaState::SPEED_NORMAL;

	for (size_t i = 0; i < g_luaStates.size(); ++i)
	{
		VbaLuaState *state = g_luaStates[i];
		if (state->L && state->running && state->speedmode > fastest)
			fastest = state->speedmode;
	}

	if (fastest == VbaLuaState::SPEED_NORMAL)
		return 0;

	//printf("%d\n", speedmode);
	switch (fastest)
	{
	/*
	case SPEED_NORMAL:
		return 0;
	case SPEED_NOTHROTTLE:
		IPPU.RenderThisFrame = true;
		return 1;

	case SPEED_TURBO:
		IPPU.SkippedFrames++;
		if (IPPU.SkippedFrames >= 40) {
			IPPU.SkippedFrames = 0;
			IPPU.RenderThisFrame = true;
		}
		else
			IPPU.RenderThisFrame = false;
		return 1;

	// In mode 3, SkippedFrames is set to zero so that the frame
	// skipping code doesn't try anything funny.
	case SPEED_MAXIMUM:
		IPPU.SkippedFrames=0;
		IPPU.RenderThisFrame = false;
		return 1;
	 */
	case 0: // FIXME: to get rid of the warning
	default:
		assert(false);
		return 0;
	}
}

///////////////////////////
// vba.speedmode(string mode)
//
//   Takes control of the emulation speed
//   of the system. Normal is normal speed (60fps, 50 for PAL),
//   nothrottle disables speed control but renders every frame,
//   turbo renders only a few frames in order to speed up emulation,

//   maximum renders no frames
static int vba_speedmode(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);
	const char *mode = luaL_checkstring(L, 1);

	if (!state)
		return luaL_error(L, "vba.speedmode: no script is running");

	if (strcasecmp(mode, "normal") == 0)
	{
		state->speedmode = VbaLuaState::SPEED_NORMAL;
	}
	else if (strcasecmp(mode, "nothrottle") == 0)
	{
		state->speedmode = VbaLuaState::SPEED_NOTHROTTLE;
	}
	else if (strcasecmp(mode, "turbo") == 0)
	{
		state->speedmode = VbaLuaState::SPEED_TURBO;
	}
	else if (strcasecmp(mode, "maximum") == 0)
	{
		state->speedmode = VbaLuaState::SPEED_MAXIMUM;
	}
	else
		luaL_error(L, "Invalid mode %s to vba.speedmode", mode);

	//printf("new speed mode:  %d\n", state->speedmode);
	return 0;
}

// vba.frameadvnace()
//
//  Executes a frame advance. Occurs by yielding the coroutine, then re-running

//  when we break out.
static int vba_frameadvance(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	// We're going to sleep for a frame-advance. Take notes.
	if (state && state->frameAdvanceWaiting)
		return luaL_error(L, "can't call vba.frameadvance() from here");

	if (state)
		state->frameAdvanceWaiting = true;

	// Don't do this! The user won't like us sending their emulator out of control!
	//	Settings.FrameAdvance = true;
	// Now we can yield to the main
	return lua_yield(L, 0);

	// It's actually rather disappointing...
}

// vba.pause()
//
//  Pauses the emulator, function "waits" until the user unpauses.
//  This function MAY be called from a non-frame boundary, but the frame

//  finishes executing anwyays. In this case, the function returns immediately.
static int vba_pause(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	systemSetPause(true);
	if (state)
		state->speedmode = VbaLuaState::SPEED_NORMAL;

	// Return control if we're midway through a frame. We can't pause here.
	if (state && state->frameAdvanceWaiting)
	{
		return 0;
	}

	// If it's on a frame boundary, we also yield.
	if (state)
		state->frameAdvanceWaiting = true;
	return lua_yield(L, 0);
}

static int vba_registerpoweron(lua_State *L)
{
	if (!lua_isnil(L, 1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTERPOWERON]);
	lua_insert(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTERPOWERON]);

	//StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}

static int vba_registerpoweroff(lua_State *L)
{
	if (!lua_isnil(L, 1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREPOWEROFF]);
	lua_insert(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREPOWEROFF]);

	//StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}

static int vba_registerloading(lua_State *L)
{
	if (!lua_isnil(L, 1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFORESTATELOAD]);
	lua_insert(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFORESTATELOAD]);

	//StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}

static int vba_registerloaded(lua_State *L)
{
	if (!lua_isnil(L, 1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTERSTATELOAD]);
	lua_insert(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTERSTATELOAD]);

	//StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}

static int vba_registersaving(lua_State *L)
{
	if (!lua_isnil(L, 1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFORESTATESAVE]);
	lua_insert(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFORESTATESAVE]);

	//StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}

static int vba_registersaved(lua_State *L)
{
	if (!lua_isnil(L, 1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTERSTATESAVE]);
	lua_insert(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTERSTATESAVE]);

	//StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}

static int vba_registerbefore(lua_State *L)
{
	if (!lua_isnil(L, 1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEMULATION]);
	lua_insert(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEMULATION]);

	//StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}

static int vba_registerafter(lua_State *L)
{
	if (!lua_isnil(L, 1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTEREMULATION]);
	lua_insert(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTEREMULATION]);

	//StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}

static int vba_registerexit(lua_State *L)
{
	if (!lua_isnil(L, 1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEXIT]);
	lua_insert(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEXIT]);

	//StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}

static inline bool isalphaorunderscore(char c)
{
	return isalpha(c) || c == '_';
}

static std::vector<const void *> s_tableAddressStack; // prevents infinite recursion of a table within a table (when cycle is
													  // found, print something like table:parent)
static std::vector<const void *> s_metacallStack; // prevents infinite recursion if something's __tostring returns another table
												  // that contains that something (when cycle is found, print the inner result
												  // without using __tostring)

#define APPENDSPRINT(...) { \
	int _n = snprintf(ptr, remaining, __VA_ARGS__); \
	if (_n >= 0) { ptr += _n; remaining -= _n; } \
	else { remaining = 0; } \
	}
static void toCStringConverter(lua_State *L, int i, char * &ptr, int &remaining)
{
	if (remaining <= 0)
		return;

	const char *str = ptr; // for debugging

	// if there is a __tostring metamethod then call it
	int usedMeta = luaL_callmeta(L, i, "__tostring");
	if (usedMeta)
	{
		std::vector<const void *>::const_iterator foundCycleIter = std::find(s_metacallStack.begin(), s_metacallStack.end(), lua_topointer(L, i));
		if (foundCycleIter != s_metacallStack.end())
		{
			lua_pop(L, 1);
			usedMeta = false;
		}
		else
		{
			s_metacallStack.push_back(lua_topointer(L, i));
			i = lua_gettop(L);
		}
	}

	switch (lua_type(L, i))
	{
	case LUA_TNONE:
		break;
	case LUA_TNIL:
		APPENDSPRINT("nil") break;
	case LUA_TBOOLEAN:
		APPENDSPRINT(lua_toboolean(L, i) ? "true" : "false") break;
	case LUA_TSTRING:
		APPENDSPRINT("%s", lua_tostring(L, i)) break;
	case LUA_TNUMBER:
		APPENDSPRINT("%.12Lg", lua_tonumber(L, i)) break;
	case LUA_TFUNCTION:
		{
			// LuaJIT does not expose the Lua 5.1 Proto internals, so report the
			// source location instead of the parameter list.
			lua_Debug ar;
			lua_pushvalue(L, i);
			if (lua_getinfo(L, ">S", &ar) && ar.what[0] != 'C')
			{
				APPENDSPRINT("function(%s:%d)", ar.short_src, ar.linedefined)
			}
			else
			{
				goto defcase;
			}
		}
		break;
defcase: default:
		APPENDSPRINT("%s:%p", luaL_typename(L, i), lua_topointer(L, i))
		break;
	case LUA_TTABLE:
		{
			// first make sure there's enough stack space
			if (!lua_checkstack(L, 4))
			{
				// note that even if lua_checkstack never returns false,
				// that doesn't mean we didn't need to call it,
				// because calling it retrieves stack space past LUA_MINSTACK
				goto defcase;
			}

			std::vector<const void *>::const_iterator foundCycleIter =
				std::find(s_tableAddressStack.begin(), s_tableAddressStack.end(), lua_topointer(L, i));
			if (foundCycleIter != s_tableAddressStack.end())
			{
				int parentNum = (int)(s_tableAddressStack.end() - foundCycleIter);
				if (parentNum > 1)
					APPENDSPRINT("%s:parent^%d", luaL_typename(L, i), parentNum)
				else
					APPENDSPRINT("%s:parent", luaL_typename(L, i))
			}
			else
			{
				s_tableAddressStack.push_back(lua_topointer(L, i));
				struct Scope { ~Scope(){ s_tableAddressStack.pop_back(); } } scope;

				APPENDSPRINT("{")

				lua_pushnil(L); // first key
				int		   keyIndex = lua_gettop(L);
				int		   valueIndex = keyIndex + 1;
				bool	   first = true;
				bool	   skipKey = true; // true if we're still in the "array part" of the table
				lua_Number arrayIndex = (lua_Number)0;
				while (lua_next(L, i))
				{
					if (first)
						first = false;
					else
						APPENDSPRINT(", ")
					if (skipKey)
					{
						arrayIndex += (lua_Number)1;
						bool keyIsNumber = (lua_type(L, keyIndex) == LUA_TNUMBER);
						skipKey = keyIsNumber && (lua_tonumber(L, keyIndex) == arrayIndex);
					}
					if (!skipKey)
					{
						bool keyIsString = (lua_type(L, keyIndex) == LUA_TSTRING);
						bool invalidLuaIdentifier = (!keyIsString || !isalphaorunderscore(*lua_tostring(L, keyIndex)));
						if (invalidLuaIdentifier)
							if (keyIsString)
								APPENDSPRINT("['")
							else
								APPENDSPRINT("[")

						toCStringConverter(L, keyIndex, ptr, remaining);
						// key

						if (invalidLuaIdentifier)
							if (keyIsString)
								APPENDSPRINT("']=")
							else
								APPENDSPRINT("]=")
						else
							APPENDSPRINT("=")
					}

					bool valueIsString = (lua_type(L, valueIndex) == LUA_TSTRING);
					if (valueIsString)
						APPENDSPRINT("'")

					toCStringConverter(L, valueIndex, ptr, remaining);  // value

					if (valueIsString)
						APPENDSPRINT("'")

					lua_pop(L, 1);

					if (remaining <= 0)
					{
						lua_settop(L, keyIndex - 1); // stack might not be clean yet if we're breaking
													// early
						break;
					}
				}
				APPENDSPRINT("}")
			}
		}
		break;
	}

	if (usedMeta)
	{
		s_metacallStack.pop_back();
		lua_pop(L, 1);
	}
}

static const int s_tempStrMaxLen = 64 * 1024;
static char s_tempStr [s_tempStrMaxLen];

static char *rawToCString(lua_State *L, int idx)
{
	int a = idx > 0 ? idx : 1;
	int n = idx > 0 ? idx : lua_gettop(L);

	char *ptr = s_tempStr;
	*ptr = 0;

	int remaining = s_tempStrMaxLen;
	for (int i = a; i <= n; i++)
	{
		toCStringConverter(L, i, ptr, remaining);
		if (i != n)
			APPENDSPRINT(" ")
	}

	if (remaining < 3)
	{
		while (remaining < 6)
			remaining++, ptr--;
		APPENDSPRINT("...")
	}
	APPENDSPRINT("\r\n")
	// the trailing newline is so print() can avoid having to do wasteful things to print its newline
	// (string copying would be wasteful and calling info.print() twice can be extremely slow)
	// at the cost of functions that don't want the newline needing to trim off the last two characters
	// (which is a very fast operation and thus acceptable in this case)

	return s_tempStr;
}
#undef APPENDSPRINT

// replacement for luaB_tostring() that is able to show the contents of tables (and formats numbers better, and show function
// prototypes)
// can be called directly from lua via tostring(), assuming tostring hasn't been reassigned
static int tostring(lua_State *L)
{
	char *str = rawToCString(L);
	str[strlen(str) - 2] = 0; // hack: trim off the \r\n (which is there to simplify the print function's
								// task)
	lua_pushstring(L, str);
	return 1;
}

// like rawToCString, but will check if the global Lua function tostring()
// has been replaced with a custom function, and call that instead if so
static const char *toCString(lua_State *L, int idx)
{
	int a = idx > 0 ? idx : 1;
	int n = idx > 0 ? idx : lua_gettop(L);
	lua_getglobal(L, "tostring");
	lua_CFunction cf = lua_tocfunction(L, -1);
	if (cf == tostring || lua_isnil(L, -1)) // optimization: if using our own C tostring function, we can
											// bypass the call through Lua and all the string object
											// allocation that would entail
	{
		lua_pop(L, 1);
		return rawToCString(L, idx);
	}
	else // if the user overrided the tostring function, we have to actually call it and store the
		 // temporarily allocated string it returns
	{
		lua_pushstring(L, "");
		for (int i = a; i <= n; i++)
		{
			lua_pushvalue(L, -2); // function to be called
			lua_pushvalue(L, i); // value to print
			lua_call(L, 1, 1);
			if (lua_tostring(L, -1) == NULL)
				luaL_error(L, LUA_QL("tostring") " must return a string to " LUA_QL("print"));
			lua_pushstring(L, (i < n) ? " " : "\r\n");
			lua_concat(L, 3);
		}
		const char *str = lua_tostring(L, -1);
		strncpy(s_tempStr, str, s_tempStrMaxLen);
		s_tempStr[s_tempStrMaxLen - 1] = 0;
		lua_pop(L, 2);
		return s_tempStr;
	}
}

// The encoding of everything a given script produces.  Text that cannot be
// attributed to a script is the engine's own and therefore ANSI.
static lua_host::TextEncoding scriptTextEncoding(const VbaLuaState *state)
{
	return state ? state->encoding : lua_host::TextEncoding::Ansi;
}

// replacement for luaB_print() that goes to the appropriate textbox instead of stdout
static int print(lua_State *L)
{
	const char *str = toCString(L);
	VbaLuaState *state = VbaLuaState::fromLua(L);

	if (info_print)
		info_print(state ? state->console : 0, scriptTextEncoding(state), str);
	else
		puts(str);

	//worry(L, 100);
	return 0;
}

static int printerror(lua_State *L, int idx)
{
	lua_checkstack(L, lua_gettop(L) + 4);

	if (idx < 0)
		idx = lua_gettop(L) + 1 + idx;

	const char *str = rawToCString(L, idx);
	VbaLuaState *state = VbaLuaState::fromLua(L);

	if (info_print)
		info_print(state ? state->console : 0, scriptTextEncoding(state), str);
	else
		fputs(str, stderr);

	//worry(L, 100);
	return 0;
}

// vba.message(string msg)
//
//  Displays the given message on the screen.
static int vba_message(lua_State *L)
{
	const char *msg = luaL_checkstring(L, 1);
	systemScreenMessage(msg);

	return 0;
}

// provides an easy way to copy a table from Lua
// (simple assignment only makes an alias, but sometimes an independent table is desired)
// currently this function only performs a shallow copy,
// but I think it should be changed to do a deep copy (possibly of configurable depth?)
// that maintains the internal table reference structure
static int copytable(lua_State *L)
{
	int origIndex = 1; // we only care about the first argument
	int origType = lua_type(L, origIndex);
	if (origType == LUA_TNIL)
	{
		lua_pushnil(L);
		return 1;
	}
	if (origType != LUA_TTABLE)
	{
		luaL_typerror(L, 1, lua_typename(L, LUA_TTABLE));
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, (int)lua_objlen(L, 1), 0);
	int copyIndex = lua_gettop(L);

	lua_pushnil(L); // first key
	int keyIndex = lua_gettop(L);
	int valueIndex = keyIndex + 1;

	while (lua_next(L, origIndex))
	{
		lua_pushvalue(L, keyIndex);
		lua_pushvalue(L, valueIndex);
		lua_rawset(L, copyIndex); // copytable[key] = value
		lua_pop(L, 1);
	}

	// copy the reference to the metatable as well, if any
	if (lua_getmetatable(L, origIndex))
		lua_setmetatable(L, copyIndex);

	return 1; // return the new table
}

// because print traditionally shows the address of tables,
// and the print function I provide instead shows the contents of tables,
// I also provide this function
// (otherwise there would be no way to see a table's address, AFAICT)
static int addressof(lua_State *L)
{
	const void *ptr = lua_topointer(L, -1);
	lua_pushinteger(L, (lua_Integer)ptr);
	return 1;
}

struct registerPointerMap
{
	const char *  registerName;
	unsigned int *pointer;
	int dataSize;
};

extern gbRegister AF;
extern gbRegister BC;
extern gbRegister DE;
extern gbRegister HL;
extern gbRegister SP;
extern gbRegister PC;
extern u16 IFF;

#define RPM_ENTRY(name, var) \
	{ name, (unsigned int *)&var, sizeof(var) },

registerPointerMap regPointerMap [] = {
	// gba registers
	RPM_ENTRY("r0",	  reg[0].I)
	RPM_ENTRY("r1",	  reg[1].I)
	RPM_ENTRY("r2",	  reg[2].I)
	RPM_ENTRY("r3",	  reg[3].I)
	RPM_ENTRY("r4",	  reg[4].I)
	RPM_ENTRY("r5",	  reg[5].I)
	RPM_ENTRY("r6",	  reg[6].I)
	RPM_ENTRY("r7",	  reg[7].I)
	RPM_ENTRY("r8",	  reg[8].I)
	RPM_ENTRY("r9",	  reg[9].I)
	RPM_ENTRY("r10",  reg[10].I)
	RPM_ENTRY("r11",  reg[11].I)
	RPM_ENTRY("r12",  reg[12].I)
	RPM_ENTRY("r13",  reg[13].I)
	RPM_ENTRY("r14",  reg[14].I)
	RPM_ENTRY("r15",  reg[15].I)
	RPM_ENTRY("cpsr", reg[16].I)
	RPM_ENTRY("spsr", reg[17].I)
	// gb registers
	RPM_ENTRY("a",	  AF.B.B1)
	RPM_ENTRY("f",	  AF.B.B0)
	RPM_ENTRY("b",	  BC.B.B1)
	RPM_ENTRY("c",	  BC.B.B0)
	RPM_ENTRY("d",	  DE.B.B1)
	RPM_ENTRY("e",	  DE.B.B0)
	RPM_ENTRY("h",	  HL.B.B1)
	RPM_ENTRY("l",	  HL.B.B0)
	RPM_ENTRY("af",	  AF.W)
	RPM_ENTRY("bc",	  BC.W)
	RPM_ENTRY("de",	  DE.W)
	RPM_ENTRY("hl",	  HL.W)
	RPM_ENTRY("sp",	  SP.W)
	RPM_ENTRY("pc",	  PC.W)
	{}
};

#undef RPM_ENTRY

struct cpuToRegisterMap
{
	const char *cpuName;
	registerPointerMap *rpmap;
}
cpuToRegisterMaps [] =
{
	{ "", regPointerMap },
};

//DEFINE_LUA_FUNCTION(memory_getregister, "cpu_dot_registername_string")
static int memory_getregister(lua_State *L)
{
	const char *qualifiedRegisterName = luaL_checkstring(L, 1);
	lua_settop(L, 0);
	for (int cpu = 0; cpu < sizeof(cpuToRegisterMaps) / sizeof(*cpuToRegisterMaps); cpu++)
	{
		cpuToRegisterMap ctrm = cpuToRegisterMaps[cpu];
		size_t cpuNameLen		  = strlen(ctrm.cpuName);
		if (!strnicmp(qualifiedRegisterName, ctrm.cpuName, cpuNameLen))
		{
			qualifiedRegisterName += cpuNameLen;
			for (int reg = 0; ctrm.rpmap[reg].dataSize; reg++)
			{
				registerPointerMap rpm = ctrm.rpmap[reg];
				if (!stricmp(qualifiedRegisterName, rpm.registerName))
				{
					switch (rpm.dataSize)
					{
					default:
					case 1:
						lua_pushinteger(L, *(unsigned char *)rpm.pointer); break;
					case 2:
						lua_pushinteger(L, *(unsigned short *)rpm.pointer); break;
					case 4:
						lua_pushinteger(L, *(unsigned long *)rpm.pointer); break;
					}
					return 1;
				}
			}
			lua_pushnil(L);
			return 1;
		}
	}
	lua_pushnil(L);
	return 1;
}

//DEFINE_LUA_FUNCTION(memory_setregister, "cpu_dot_registername_string,value")
static int memory_setregister(lua_State *L)
{
	const char *  qualifiedRegisterName = luaL_checkstring(L, 1);
	unsigned long value = (unsigned long)(luaL_checkinteger(L, 2));
	lua_settop(L, 0);
	for (int cpu = 0; cpu < sizeof(cpuToRegisterMaps) / sizeof(*cpuToRegisterMaps); cpu++)
	{
		cpuToRegisterMap ctrm = cpuToRegisterMaps[cpu];
		size_t cpuNameLen		  = strlen(ctrm.cpuName);
		if (!strnicmp(qualifiedRegisterName, ctrm.cpuName, cpuNameLen))
		{
			qualifiedRegisterName += cpuNameLen;
			for (int reg = 0; ctrm.rpmap[reg].dataSize; reg++)
			{
				registerPointerMap rpm = ctrm.rpmap[reg];
				if (!stricmp(qualifiedRegisterName, rpm.registerName))
				{
					switch (rpm.dataSize)
					{
					default:
					case 1:
						*(unsigned char *)rpm.pointer = (unsigned char)(value & 0xFF); break;
					case 2:
						*(unsigned short *)rpm.pointer = (unsigned short)(value & 0xFFFF); break;
					case 4:
						*(unsigned long *)rpm.pointer = value; break;
					}
					return 0;
				}
			}
			return 0;
		}
	}
	return 0;
}

static void KillScriptAfterError(VbaLuaState *state)
{
	// A callback of this script crashed.  The old engine closed the one and only
	// lua_State here, which is not an option any more: another script may be
	// running, and the caller is still standing on this one.  Drop every
	// callback the script registered instead, so it costs nothing per frame,
	// and leave the state itself to be closed when its window goes away.
	lua_State *L = state->L;
	if (!L)
		return;

	lua_settop(L, 0);
	for (int i = 0; i < LUACALL_COUNT; ++i)
	{
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[i]);
	}
	lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, guiCallbackTable);
	lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, frameAdvanceThread);
	lua_settop(L, 0);

	state->resetAfterStop();
	state->memHooks = 0;
	for (int i = 0; i < LUAMEMHOOK_COUNT; ++i)
		CalculateMemHookRegions((LuaMemHookType)i);

	if (info_onstop)
		info_onstop(state->console);
}

void HandleCallbackError(lua_State *L)
{
	// Called right after lua_pcall() returned an error, so there is no active
	// protected frame left to rethrow into.  LuaJIT keeps lua_State opaque, so
	// the old errfunc/errorJmp probe is gone and we always report directly.
	VbaLuaState *state = VbaLuaState::fromLua(L);

	// The gui callback goes first: a draw function that crashed would be called
	// again next frame, and the message box it may pop up causes a paint event
	// which calls it once more (see the note in guiFrame).
	lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, guiCallbackTable);

	printerror(L, -1);

	if (state)
		KillScriptAfterError(state);
}

// One callback kind, in one script.  Calls are made with the main state of the
// script, which is what the engine always did: the coroutine parked in
// vba.frameadvance() keeps its own stack, so a callback can neither see nor
// disturb it.
void VbaLuaState::callCallbacks(LuaCallID calltype)
{
	const char *idstring = luaCallIDStrings[calltype];

	if (!L)
		return;

	lua_settop(L, 0);
	lua_getfield(L, LUA_REGISTRYINDEX, idstring);

	int errorcode = 0;
	if (lua_isfunction(L, -1))
	{
		errorcode = lua_pcall(L, 0, 0, 0);
		if (errorcode)
			HandleCallbackError(L);
	}
	else
	{
		lua_pop(L, 1);
	}
}

void CallRegisteredLuaFunctions(LuaCallID calltype)
{
	assert((unsigned int)calltype < (unsigned int)LUACALL_COUNT);

	// load order, every time: two scripts must not be able to swap rounds
	for (size_t i = 0; i < g_luaStates.size(); ++i)
		g_luaStates[i]->callCallbacks(calltype);
}

// The merged view of every script's hooks, rebuilt whenever any script adds or
// removes one.  This is what the dispatch below tests first: one region check
// for the common "nothing is hooked here" case, and only then a look at the
// individual scripts.
static TieredRegion hookedRegions[LUAMEMHOOK_COUNT];

static void CalculateMemHookRegions(LuaMemHookType hookType)
{
	std::vector<unsigned int> unionBytes;

	for (size_t s = 0; s < g_luaStates.size(); ++s)
	{
		VbaLuaState *state = g_luaStates[s];
		std::vector<unsigned int> hookedBytes;

		if (state->memHooks && state->L)
		{
			lua_State *L = state->L;
			lua_settop(L, 0);
			lua_getfield(L, LUA_REGISTRYINDEX, luaMemHookTypeStrings[hookType]);
			lua_pushnil(L);
			while (lua_next(L, -2))
			{
				if (lua_isfunction(L, -1))
				{
					unsigned int addr = (unsigned int)lua_tointeger(L, -2);
					hookedBytes.push_back(addr);
				}
				lua_pop(L, 1);
			}
			lua_settop(L, 0);
		}

		state->memRegions[hookType].Calculate(hookedBytes);
		unionBytes.insert(unionBytes.end(), hookedBytes.begin(), hookedBytes.end());
	}

	hookedRegions[hookType].Calculate(unionBytes);
}

static void CallRegisteredLuaMemHook_InScript(VbaLuaState *state, unsigned int address, int size, unsigned int value, LuaMemHookType hookType)
{
	lua_State *L = state->L;
	if (!L)
		return;

	lua_settop(L, 0);
	lua_getfield(L, LUA_REGISTRYINDEX, luaMemHookTypeStrings[hookType]);
	for (int i = address; i != address + size; i++)
	{
		lua_rawgeti(L, -1, i);
		if (lua_isfunction(L, -1))
		{
			// A hook fires in the middle of a frame, from C code the script
			// knows nothing about; it must not be taken for "the script died".
			bool wasRunning = state->running;
			state->running = true;
			lua_pushinteger(L, address);
			lua_pushinteger(L, size);
			int errorcode = lua_pcall(L, 2, 0, 0);
			state->running = wasRunning;
			if (errorcode)
				HandleCallbackError(L);
			break;
		}
		else
		{
			lua_pop(L, 1);
		}
	}
	lua_settop(L, 0);
}

void CallRegisteredLuaMemHook(unsigned int address, int size, unsigned int value, LuaMemHookType hookType)
{
	// performance critical! (called VERY frequently)
	// I suggest timing a large number of calls to this function in Release if you change anything in here,
	// before and after, because even the most innocent change can make it become 30% to 400% slower.
	// a good amount to test is: 100000000 calls with no hook set, and another 100000000 with a hook set.
	// (on my system that consistently took 200 ms total in the former case and 350 ms total in the latter
	// case)
	//
	// The first test is against the merged map of every script, so a run with no
	// hooks at all still costs one empty-island check and nothing else.  Only a
	// hit pays for looking at the scripts one by one.
	if (hookedRegions[hookType].NotEmpty())
	{
		//if((hookType <= LUAMEMHOOK_EXEC) && (address >= 0xE00000))
		//	address |= 0xFF0000; // account for mirroring of RAM
		if (hookedRegions[hookType].Contains(address, size))
		{
			for (size_t i = 0; i < g_luaStates.size(); ++i)
			{
				VbaLuaState *state = g_luaStates[i];
				if (state->memHooks && state->memRegions[hookType].NotEmpty() &&
					state->memRegions[hookType].Contains(address, size))
					CallRegisteredLuaMemHook_InScript(state, address, size, value, hookType);  // this script hooked that address
			}
		}
	}
}

static int memory_registerHook(lua_State *L, LuaMemHookType hookType, int defaultSize)
{
	// get first argument: address
	unsigned int addr = (unsigned int)luaL_checkinteger(L, 1);
	//if((addr & ~0xFFFFFF) == ~0xFFFFFF)
	//	addr &= 0xFFFFFF;

	// get optional second argument: size
	int size	= defaultSize;
	int funcIdx = 2;
	if (lua_isnumber(L, 2))
	{
		size = (int)luaL_checkinteger(L, 2);
		if (size < 0)
		{
			size  = -size;
			addr -= size;
		}
		funcIdx++;
	}

	// check last argument: callback function
	bool clearing = lua_isnil(L, funcIdx);
	if (!clearing)
		luaL_checktype(L, funcIdx, LUA_TFUNCTION);
	lua_settop(L, funcIdx);

	// get the address-to-callback table for this hook type of the current script
	lua_getfield(L, LUA_REGISTRYINDEX, luaMemHookTypeStrings[hookType]);

	// count how many callback functions we'll be displacing
	int numFuncsAfter  = clearing ? 0 : size;
	int numFuncsBefore = 0;
	for (unsigned int i = addr; i != addr + size; i++)
	{
		lua_rawgeti(L, -1, i);
		if (lua_isfunction(L, -1))
			numFuncsBefore++;
		lua_pop(L, 1);
	}

	// put the callback function in the address slots
	for (unsigned int i = addr; i != addr + size; i++)
	{
		lua_pushvalue(L, -2);
		lua_rawseti(L, -2, i);
	}

	// adjust the count of active hooks of this script
	VbaLuaState *state = VbaLuaState::fromLua(L);
	if (state)
		state->memHooks += numFuncsAfter - numFuncsBefore;

	// re-cache regions of hooked memory across all scripts
	CalculateMemHookRegions(hookType);

	//StopScriptIfFinished(luaStateToUIDMap[L]);
	return 0;
}

LuaMemHookType MatchHookTypeToCPU(lua_State *L, LuaMemHookType hookType)
{
	int cpuID = 0;

	int cpunameIndex = 0;
	if (lua_type(L, 2) == LUA_TSTRING)
		cpunameIndex = 2;
	else if (lua_type(L, 3) == LUA_TSTRING)
		cpunameIndex = 3;

	if (cpunameIndex)
	{
		const char *cpuName = lua_tostring(L, cpunameIndex);
		if (!stricmp(cpuName, "sub"))
			cpuID = 1;
		lua_remove(L, cpunameIndex);
	}

	switch (cpuID)
	{
	case 0:
		return hookType;

	case 1:
		switch (hookType)
		{
		case LUAMEMHOOK_WRITE:
			return LUAMEMHOOK_WRITE_SUB;
		case LUAMEMHOOK_READ:
			return LUAMEMHOOK_READ_SUB;
		case LUAMEMHOOK_EXEC:
			return LUAMEMHOOK_EXEC_SUB;
		}
	}
	return hookType;
}

static int memory_registerwrite(lua_State *L)
{
	return memory_registerHook(L, MatchHookTypeToCPU(L, LUAMEMHOOK_WRITE), 1);
}

static int memory_registerread(lua_State *L)
{
	return memory_registerHook(L, MatchHookTypeToCPU(L, LUAMEMHOOK_READ), 1);
}

static int memory_registerexec(lua_State *L)
{
	return memory_registerHook(L, MatchHookTypeToCPU(L, LUAMEMHOOK_EXEC), 1);
}

//int vba.lagcount
//

//Returns the lagcounter variable
static int vba_getlagcount(lua_State *L)
{
	lua_pushinteger(L, systemCounters.lagCount);
	return 1;
}

//int vba.lagged
//
//Returns true if the current frame is a lag frame
static int vba_lagged(lua_State *L)
{
	lua_pushboolean(L, systemCounters.laggedLast);
	return 1;
}

// boolean vba.emulating()
int vba_emulating(lua_State *L)
{
	lua_pushboolean(L, systemIsEmulating());
	return 1;
}

// void vba.screenshot()
int vba_screenshot(lua_State* L)
{
	systemScreenCapture(0);
	return 0;
}

int movie_isactive(lua_State *L)
{
	lua_pushboolean(L, VBAMovieIsActive());
	return 1;
}

int movie_isrecording(lua_State *L)
{
	lua_pushboolean(L, VBAMovieIsRecording());
	return 1;
}

int movie_isplaying(lua_State *L)
{
	lua_pushboolean(L, VBAMovieIsPlaying());
	return 1;
}

int movie_getlength(lua_State *L)
{
	if (VBAMovieIsActive())
		lua_pushinteger(L, VBAMovieGetLength());
	else
		lua_pushinteger(L, 0);
	return 1;
}

static int memory_readbyte(lua_State *L)
{
	u32 addr;
	u8	val;

	addr = (u32)luaL_checkinteger(L, 1);
	if (systemIsRunningGBA())
	{
		val = CPUReadByteQuick(addr);
	}
	else
	{
		val = gbReadMemoryQuick8(addr);
	}

	lua_pushinteger(L, val);
	return 1;
}

static int memory_readbytesigned(lua_State *L)
{
	u32 addr;
	s8	val;

	addr = (u32)luaL_checkinteger(L, 1);
	if (systemIsRunningGBA())
	{
		val = (s8) CPUReadByteQuick(addr);
	}
	else
	{
		val = (s8) gbReadMemoryQuick8(addr);
	}

	lua_pushinteger(L, val);
	return 1;
}

static int memory_readword(lua_State *L)
{
	u32 addr;
	u16 val;

	addr = (u32)luaL_checkinteger(L, 1);
	if (systemIsRunningGBA())
	{
		val = CPUReadHalfWordQuick(addr);
	}
	else
	{
		val = gbReadMemoryQuick16(addr & 0x0000FFFF);
	}

	lua_pushinteger(L, val);
	return 1;
}

static int memory_readwordsigned(lua_State *L)
{
	u32 addr;
	s16 val;

	addr = (u32)luaL_checkinteger(L, 1);
	if (systemIsRunningGBA())
	{
		val = (s16) CPUReadHalfWordQuick(addr);
	}
	else
	{
		val = (s16) gbReadMemoryQuick16(addr);
	}

	lua_pushinteger(L, val);
	return 1;
}

static int memory_readdword(lua_State *L)
{
	u32 addr;
	u32 val;

	addr = (u32)luaL_checkinteger(L, 1);
	if (systemIsRunningGBA())
	{
		val = CPUReadMemoryQuick(addr);
	}
	else
	{
		val = gbReadMemoryQuick32(addr & 0x0000FFFF);
	}

	// lua_pushinteger doesn't work properly for 32bit system, does it?
	if (val >= 0x80000000 && sizeof(int) <= 4)
		lua_pushnumber(L, val);
	else
		lua_pushinteger(L, val);
	return 1;
}

static int memory_readdwordsigned(lua_State *L)
{
	u32 addr;
	s32 val;

	addr = (u32)luaL_checkinteger(L, 1);
	if (systemIsRunningGBA())
	{
		val = (s32) CPUReadMemoryQuick(addr);
	}
	else
	{
		val = (s32) gbReadMemoryQuick32(addr);
	}

	lua_pushinteger(L, val);
	return 1;
}

static int memory_readbyterange(lua_State *L)
{
	uint32 address = (uint32)luaL_checkinteger(L, 1);
	int	   length  = (int)luaL_checkinteger(L, 2);

	if (length < 0)
	{
		address += length;
		length	 = -length;
	}

	// push the array
	lua_createtable(L, abs(length), 0);

	// put all the values into the (1-based) array
	for (int a = address, n = 1; n <= length; a++, n++)
	{
		unsigned char value;

		if (systemIsRunningGBA())
		{
			value = CPUReadByteQuick(a);
		}
		else
		{
			value = gbReadMemoryQuick8(a);
		}

		lua_pushinteger(L, value);
		lua_rawseti(L, -2, n);
	}

	return 1;
}

static int memory_writebyte(lua_State *L)
{
	u32 addr;
	int val;

	addr = (u32)luaL_checkinteger(L, 1);
	val	 = (int)luaL_checkinteger(L, 2);
	if (systemIsRunningGBA())
	{
		CPUWriteByteQuick(addr, val);
	}
	else
	{
		gbWriteMemoryQuick8(addr, val);
	}

	CallRegisteredLuaMemHook(addr, 1, val, LUAMEMHOOK_WRITE);
	return 0;
}

static int memory_writeword(lua_State *L)
{
	u32 addr;
	int val;

	addr = (u32)luaL_checkinteger(L, 1);
	val	 = (int)luaL_checkinteger(L, 2);
	if (systemIsRunningGBA())
	{
		CPUWriteHalfWordQuick(addr, val);
	}
	else
	{
		gbWriteMemoryQuick16(addr, val);
	}

	CallRegisteredLuaMemHook(addr, 2, val, LUAMEMHOOK_WRITE);
	return 0;
}

static int memory_writedword(lua_State *L)
{
	u32 addr;
	u32 val;

	addr = (u32)luaL_checkinteger(L, 1);
	val	 = (u32)(s64)luaL_checknumber(L, 2);
	if (systemIsRunningGBA())
	{
		CPUWriteMemoryQuick(addr, val);
	}
	else
	{
		gbWriteMemoryQuick32(addr, val);
	}

	CallRegisteredLuaMemHook(addr, 4, val, LUAMEMHOOK_WRITE);
	return 0;
}

static int memory_gbromreadbyte(lua_State *L)
{
	u32 addr;
	u8	val;

	addr = (u32)luaL_checkinteger(L, 1);
	if (systemIsRunningGBA())
	{
		lua_pushnil(L);
		return 1;
	}
	else
	{
		val = gbReadROMQuick8(addr);
	}

	lua_pushinteger(L, val);
	return 1;
}

static int memory_gbromreadbytesigned(lua_State *L)
{
	u32 addr;
	s8	val;

	addr = (u32)luaL_checkinteger(L, 1);
	if (systemIsRunningGBA())
	{
		lua_pushnil(L);
		return 1;
	}
	else
	{
		val = (s8) gbReadROMQuick8(addr);
	}

	lua_pushinteger(L, val);
	return 1;
}

static int memory_gbromreadword(lua_State *L)
{
	u32 addr;
	u16 val;

	addr = (u32)luaL_checkinteger(L, 1);
	if (systemIsRunningGBA())
	{
		lua_pushnil(L);
		return 1;
	}
	else
	{
		val = gbReadROMQuick16(addr);
	}

	lua_pushinteger(L, val);
	return 1;
}

static int memory_gbromreadwordsigned(lua_State *L)
{
	u32 addr;
	s16 val;

	addr = (u32)luaL_checkinteger(L, 1);
	if (systemIsRunningGBA())
	{
		lua_pushnil(L);
		return 1;
	}
	else
	{
		val = (s16) gbReadROMQuick16(addr);
	}

	lua_pushinteger(L, val);
	return 1;
}

static int memory_gbromreaddword(lua_State *L)
{
	u32 addr;
	u32 val;

	addr = (u32)luaL_checkinteger(L, 1);
	if (systemIsRunningGBA())
	{
		lua_pushnil(L);
		return 1;
	}
	else
	{
		val = gbReadROMQuick32(addr);
	}

	// lua_pushinteger doesn't work properly for 32bit system, does it?
	if (val >= 0x80000000 && sizeof(int) <= 4)
		lua_pushnumber(L, val);
	else
		lua_pushinteger(L, val);
	return 1;
}

static int memory_gbromreaddwordsigned(lua_State *L)
{
	u32 addr;
	s32 val;

	addr = (u32)luaL_checkinteger(L, 1);
	if (systemIsRunningGBA())
	{
		lua_pushnil(L);
		return 1;
	}
	else
	{
		val = (s32) gbReadROMQuick32(addr);
	}

	lua_pushinteger(L, val);
	return 1;
}

static int memory_gbromreadbyterange(lua_State *L)
{
	uint32 address = (uint32)luaL_checkinteger(L, 1);
	int	   length  = (int)luaL_checkinteger(L, 2);

	if (length < 0)
	{
		address += length;
		length	 = -length;
	}

	// push the array
	lua_createtable(L, abs(length), 0);

	// put all the values into the (1-based) array
	for (int a = address, n = 1; n <= length; a++, n++)
	{
		unsigned char value;

		if (systemIsRunningGBA())
		{
			lua_pushnil(L);
			return 1;
		}
		else
		{
			value = gbReadROMQuick8(a);
		}

		lua_pushinteger(L, value);
		lua_rawseti(L, -2, n);
	}

	return 1;
}

// parse the joypad input type string to get the internal type ID
//
static enum LuaJoypadType joypad_parse_type(lua_State* L, int numArg)
{
	const char* str = luaL_optlstring(L, numArg, 0, 0);
	if (str == 0)
		return LuaJoypadType::LUAJOYPAD_COUNT;

	for (int i = 0; i < LuaJoypadType::LUAJOYPAD_COUNT; ++i)
	{
		if (strcmp(luaJoypadTypeStrings[i], str) == 0)
		{
			return (enum LuaJoypadType)i;
		}
	}

	luaL_error(L, "Invalid input type '%s')", str);

	return LuaJoypadType::LUAJOYPAD_COUNT;
}

// table joypad.get(int which = 0, string padtype = "JOYPAD_GAME")
// 
//  Reads the joypad input that comes from the specified source.
static int joy_get_internal(lua_State *L, bool reportUp, bool reportDown, enum LuaJoypadType type)
{
	int which = (int)luaL_checkinteger(L, 1);
	
	if (which < 0 || which > 4)
	{
		luaL_error(L, "Invalid input port (valid range 0-4, specified %d)", which);
	}

	if (which == 0)
		which = systemGetDefaultJoypad();
	else
		which -= 1;

	uint32 buttons = 0;

	if (type == LuaJoypadType::LUAJOYPAD_USER)
	{
		// Reads the joypad input by the user
		buttons = joypadButtons[which];
	}
	else if (type == LuaJoypadType::LUAJOYPAD_PLAYBACK)
	{
		// Reads the joypad input from the currently active movie
		buttons = movieButtons[which];
	}
	else if (type == LuaJoypadType::LUAJOYPAD_RECORD)
	{
		// Reads the mixed input that is supposed to be recorded in a movie
		buttons = currentButtons[which];
	}
	else if (type == LuaJoypadType::LUAJOYPAD_LUA)
	{
		// Reads the overriding joypad input set by the lua engine.  With more
		// than one script running that is the merged pad, i.e. what the emulator
		// is actually going to apply.
		buttons = mergedLuaJoypad(which);
	}
	else
	{
		// Reads the mixed input that the game actually receives and reacts to
		buttons = systemGetJoypad(which);
	}

	lua_newtable(L);

	for (int i = 0; i < 10; i++)
	{
		bool pressed = (buttons & (1 << i)) != 0;
		if ((pressed && reportDown) || (!pressed && reportUp))
		{
			lua_pushboolean(L, pressed);
			lua_setfield(L, -2, button_mappings[i]);
		}
	}

	return 1;
}

// joypad.get(which = 0, padtype = "JOYPAD_GAME")
//   returns a table of every game button,
//   true meaning currently-held and false meaning not-currently-held
//   (as of last frame boundary)
//   by default this WILL read input from a currently-playing movie
static int joypad_get(lua_State *L)
{
	enum LuaJoypadType type = joypad_parse_type(L, 2);
	return joy_get_internal(L, true, true, type);
}

// joypad.getdown(which = 0, padtype = "JOYPAD_GAME")
// returns a table of every game button that is currently held
static int joypad_getdown(lua_State *L)
{
	enum LuaJoypadType type = joypad_parse_type(L, 2);
	return joy_get_internal(L, false, true, type);
}

// joypad.getup(which = 0, padtype = "JOYPAD_GAME")
//   returns a table of every game button that is not currently held
static int joypad_getup(lua_State *L)
{
	enum LuaJoypadType type = joypad_parse_type(L, 2);
	return joy_get_internal(L, true, false, type);
}

// joypad.set(int which = 0, table buttons, string padtype = "JOYPAD_GAME")
//
//   Sets the given buttons to be pressed during the next frame
//   when the game may receive them via the specified input source.
//   The table should have the right keys (no pun intended) set.
static int joy_set_internal(lua_State *L, enum LuaJoypadType type)
{
	// Which joypad we're tampering with
	int which = (int)luaL_checkinteger(L, 1);
	if (which < 0 || which > 4)
	{
		luaL_error(L, "Invalid output port (valid range 0-4, specified %d)", which);
	}

	if (which == 0)
		which = systemGetDefaultJoypad();
	else
		which -= 1;

	// And the table of buttons.
	luaL_checktype(L, 2, LUA_TTABLE);

	u16 *buttons = NULL;
	// The record input is 32 bits wide (speed, capture and GameShark
	// buttons live above bit 15), so build the regular buttons here and
	// hand them over once.
	u16 recordButtons = 0;
	if (type == LuaJoypadType::LUAJOYPAD_USER)
	{
		// Overwrites the joypad input by the user
		buttons = &joypadButtons[which];
	}
	else if (type == LuaJoypadType::LUAJOYPAD_PLAYBACK)
	{
		// Overwrites the joypad input from the currently active movie
		buttons = &movieButtons[which];
	}
	else if (type == LuaJoypadType::LUAJOYPAD_RECORD)
	{
		// Overwrites the mixed input that is supposed to be recorded in a movie
		buttons = &recordButtons;
	}
	else //if (type == LuaJoypadType::LUAJOYPAD_LUA)
	{
		// Overwrites the overriding joypad input set by the lua engine.  Each
		// script keeps its own pad; VBALuaUsingJoypad()/VBALuaReadJoypad() or
		// the two together when several scripts claim the same port.
		VbaLuaState *state = VbaLuaState::fromLua(L);
		if (!state)
			return luaL_error(L, "joypad.set: no script is running");
		state->joypadUsed |= 1 << (which);
		buttons = &state->joypad[which];
	}

	// Set up for taking control of the indicated controller
	*buttons = 0;

	for (int i = 0; i < 10; ++i)
	{
		const char *name = button_mappings[i];
		lua_getfield(L, 2, name);
		if (!lua_isnil(L, -1))
		{
			bool pressed = lua_toboolean(L, -1) != 0;
			if (pressed)
				*buttons |= 1 << i;
			else
				*buttons &= ~(1 << i);
		}
		lua_pop(L, 1);
	}

	if (type == LuaJoypadType::LUAJOYPAD_RECORD)
		currentButtons[which] = recordButtons;

	// re-mix the input
	systemMixJoypadInput(which, type);

	return 0;
}

// joypad.set(which, padtype = "JOYPAD_GAME")
//   returns a table of every game button that is not currently held
static int joypad_set(lua_State* L)
{
	enum LuaJoypadType type = joypad_parse_type(L, 3);
	return joy_set_internal(L, type);
}

// Helper function to convert a savestate object to the filename it represents.
static const char *savestateobj2filename(lua_State *L, int offset)
{
	// First we get the metatable of the indicated object
	int result = lua_getmetatable(L, offset);

	if (!result)
		luaL_error(L, "object not a savestate object");

	// Also check that the type entry is set
	lua_getfield(L, -1, "__metatable");
	if (strcmp(lua_tostring(L, -1), "vba Savestate") != 0)
		luaL_error(L, "object not a savestate object");
	lua_pop(L, 1);

	// Now, get the field we want
	lua_getfield(L, -1, "filename");

	// Return it
	return lua_tostring(L, -1);
}

// Helper function for garbage collection.
static int savestate_gc(lua_State *L)
{
	// The object we're collecting is on top of the stack
	lua_getmetatable(L, 1);

	// Get the filename
	const char *filename;
	lua_getfield(L, -1, "filename");
	filename = lua_tostring(L, -1);

	// Delete the file
	remove(filename);

	// We exit, and the garbage collector takes care of the rest.
	// Edit: Visual Studio needs a return value anyway, so returns 0.
	return 0;
}

static std::string get_savestate_filename(int which)
{
	if (which > 0)
	{
		// Find an appropriate filename. This is OS specific, unfortunately.
#if (defined(WIN32) && !defined(SDL))
		return lua_host::savestateFilename(which);
#else
		extern char saveDir[2048];
		extern char filename[2048];
		extern char *sdlGetFilename(char *name);

		char stateName[2048];

		if (saveDir[0])
			sprintf(stateName, "%s/%s%d.sgm", saveDir, sdlGetFilename(filename), which);
		else
			sprintf(stateName, "%s%d.sgm", filename, which);

		return stateName;
#endif
	}
	else
	{
		char *stateNameTemp = tempnam(NULL, "snlua");
		std::string stateName = stateNameTemp;

		if (stateNameTemp)
			free(stateNameTemp);

		return stateName;
	}
}

// object savestate.create(int which = nil)
//
//  Creates an object used for savestates.
//  The object can be associated with a player-accessible savestate

//  ("which" between 1 and 12) or not (which == nil).
static int savestate_create(lua_State *L)
{
	int which = -1;
	if (lua_gettop(L) >= 1)
	{
		which = (int)luaL_checkinteger(L, 1);
		if (which < 1 || which > 12)
		{
			luaL_error(L, "invalid player's savestate %d", which);
		}
	}

	std::string stateName = get_savestate_filename(which);

	// Our "object". We don't care about the type, we just need the memory and GC services.
	lua_newuserdata(L, 1);

	// The metatable we use, protected from Lua and contains garbage collection info and stuff.
	lua_newtable(L);

	// First, we must protect it
	lua_pushstring(L, "vba Savestate");
	lua_setfield(L, -2, "__metatable");

	// Now we need to save the file itself.
	lua_pushstring(L, stateName.c_str());
	lua_setfield(L, -2, "filename");

	// If it's an anonymous savestate, we must delete the file from disk should it be gargage collected
	if (which < 0)
	{
		lua_pushcfunction(L, savestate_gc);
		lua_setfield(L, -2, "__gc");
	}

	// Set the metatable
	lua_setmetatable(L, -2);

	// Awesome. Return the object
	return 1;
}

// savestate.save(object state)
//

//   Saves a state to the given object.
static int savestate_save(lua_State *L)
{
	const char *filename = savestateobj2filename(L, 1);

	//	printf("saving %s\n", filename);
	// Save states are very expensive. They take time.
	if (VbaLuaState *state = VbaLuaState::fromLua(L))
		state->numTries--;

	bool8 retvalue = theEmulator.emuWriteState ? theEmulator.emuWriteState(filename) : false;
	if (!retvalue)
	{
		// Uh oh
		luaL_error(L, "savestate failed");
	}

	return 0;
}

// savestate.load(object state)
//

//   Loads the given state
static int savestate_load(lua_State *L)
{
	const char *filename = savestateobj2filename(L, 1);

	if (VbaLuaState *state = VbaLuaState::fromLua(L))
		state->numTries--;

	//	printf("loading %s\n", filename);
	bool8 retvalue = theEmulator.emuReadState ? theEmulator.emuReadState(filename) : false;
	if (!retvalue)
	{
		// Uh oh
		luaL_error(L, "loadstate failed");
	}

	return 0;
}

// int vba.framecount()
//

//   Gets the frame counter for the movie, or the number of frames since last reset.
int vba_framecount(lua_State *L)
{
	if (!VBAMovieIsActive())
	{
		lua_pushinteger(L, systemCounters.frameCount);
	}
	else
	{
		lua_pushinteger(L, VBAMovieGetFrameCounter());
	}

	return 1;
}

// vba.remapinputdisplay(int which, table new_input_mapping)
//

//
int vba_remapinputdisplay(lua_State* L)
{
	// Reads the joypads as inputted by the user
	int which = (int)luaL_checkinteger(L, 1);

	if (which < 0 || which > 4)
	{
		luaL_error(L, "Invalid input port (valid range 0-4, specified %d)", which);
	}

	if (which > 0)
		which -= 1;
	else
		which = systemGetDefaultJoypad();

	VbaLuaState *state = VbaLuaState::fromLua(L);
	if (!state)
		return luaL_error(L, "vba.remapinputdisplay: no script is running");

	// Prepair the remapping
	LuaInputDisplayRemap *remap = state->inputRemap;

	// If we are remapping the "next frame input display" instead
	if (!lua_isnoneornil(L, 3) && joypad_parse_type(L, 3) == LuaJoypadType::LUAJOYPAD_PLAYBACK)
		remap = state->nextInputRemap;

	remap[which].from = 0;

	// Either clear it
	if (lua_isnoneornil(L, 2))
		return 0;

	// Or then get the table of buttons
	luaL_checktype(L, 2, LUA_TTABLE);

	for (int i = 0; i < 10; ++i)
	{
		const char* from = button_mappings[i];
		lua_getfield(L, 2, from);
		if (!lua_isnil(L, -1))
		{
			const char* to = lua_tostring(L, -1);
			for (int j = 0; j < 10; ++j)
			{
				if (strcmp(to, button_mappings[j]) == 0)
				{
					remap[which].from |= 1 << i;
					remap[which].to[i] = 1 << j;
					break;
				}
			}
		}
		lua_pop(L, 1);
	}

	return 0;
}

//string movie.getauthor
//

// returns author info field of .vbm file
int movie_getauthor(lua_State *L)
{
	if (!VBAMovieIsActive())
	{
		//lua_pushnil(L);
		lua_pushstring(L, "");
		return 1;
	}

	lua_pushstring(L, VBAMovieGetAuthorInfo().c_str());
	return 1;
}

//string movie.filename
int movie_getfilename(lua_State *L)
{
	if (!VBAMovieIsActive())
	{
		//lua_pushnil(L);
		lua_pushstring(L, "");
		return 1;
	}

	lua_pushstring(L, VBAMovieGetFilename().c_str());
	return 1;
}

// string movie.mode()
//

//   "record", "playback" or nil
int movie_getmode(lua_State *L)
{
	assert(!VBAMovieIsLoading());
	if (!VBAMovieIsActive())
	{
		lua_pushnil(L);
		return 1;
	}

	if (VBAMovieIsRecording())
		lua_pushstring(L, "record");
	else
		lua_pushstring(L, "playback");
	return 1;
}

static int movie_rerecordcount(lua_State *L)
{
	if (VBAMovieIsActive())
		lua_pushinteger(L, VBAMovieGetRerecordCount());
	else
		lua_pushinteger(L, 0);
	return 1;
}

static int movie_setrerecordcount(lua_State *L)
{
	if (VBAMovieIsActive())
		VBAMovieSetRerecordCount((uint32)luaL_checkinteger(L, 1));
	return 0;
}

static int movie_rerecordcounting(lua_State *L)
{
	if (lua_gettop(L) == 0)
		luaL_error(L, "no parameters specified");

	VbaLuaState *state = VbaLuaState::fromLua(L);
	if (state)
		state->skipRerecords = lua_toboolean(L, 1) != 0;
	return 0;
}

// movie.stop()
//

//   Stops movie playback/recording. Bombs out if movie is not running.
static int movie_stop(lua_State *L)
{
	if (!VBAMovieIsActive())
		luaL_error(L, "no movie");

	VBAMovieStop(false);
	return 0;
}

#define LUA_SCREEN_WIDTH	256
#define LUA_SCREEN_HEIGHT	224

// Common code by the gui library: make sure this script's screen array is ready.
// Every script draws on a canvas of its own, so two scripts that both draw a HUD
// cannot erase each other's pixels; VBALuaGui() blends the canvases in load
// order at the end of the frame.
static void gui_prepare(VbaLuaState *state)
{
	if (!state->guiData)
		state->guiData = (uint8 *)malloc(LUA_SCREEN_WIDTH * LUA_SCREEN_HEIGHT * 4);
	if (!state->guiUsed)
		memset(state->guiData, 0, LUA_SCREEN_WIDTH * LUA_SCREEN_HEIGHT * 4);
	state->guiUsed = true;
}

// pixform for lua graphics
#define BUILD_PIXEL_ARGB8888(A, R, G, B)	(((int)(A) << 24) | ((int)(R) << 16) | ((int)(G) << 8) | (int)(B))
#define DECOMPOSE_PIXEL_ARGB8888(PIX, A, R, G, B) \
	{											\
		(A) = ((PIX) >> 24) & 0xff;				\
		(R) = ((PIX) >> 16) & 0xff;				\
		(G) = ((PIX) >> 8) & 0xff;				\
		(B) = (PIX) & 0xff;						\
	}
#define LUA_BUILD_PIXEL	 BUILD_PIXEL_ARGB8888
#define LUA_DECOMPOSE_PIXEL DECOMPOSE_PIXEL_ARGB8888
#define LUA_PIXEL_A(PIX) (((PIX) >> 24) & 0xff)
#define LUA_PIXEL_R(PIX) (((PIX) >> 16) & 0xff)
#define LUA_PIXEL_G(PIX) (((PIX) >> 8) & 0xff)
#define LUA_PIXEL_B(PIX) ((PIX) & 0xff)

template<class T>
static void swap(T &one, T &two)
{
	T temp = one;
	one = two;
	two = temp;
}

// write a pixel to buffer
static inline void blend32(uint32 *dstPixel, uint32 colour)
{
	uint8 *dst = (uint8 *)dstPixel;
	int	   a, r, g, b;
	LUA_DECOMPOSE_PIXEL(colour, a, r, g, b);

	if (a == 255 || dst[3] == 0)
	{
		// direct copy
		*(uint32 *) (dst) = colour;
	}
	else if (a == 0)
	{
		// do not copy
	}
	else
	{
		// alpha-blending
		int a_dst = ((255 - a) * dst[3] + 128) / 255;
		int a_new = a + a_dst;

		dst[0] = (uint8) (((dst[0] * a_dst + b * a) + (a_new / 2)) / a_new);
		dst[1] = (uint8) (((dst[1] * a_dst + g * a) + (a_new / 2)) / a_new);
		dst[2] = (uint8) (((dst[2] * a_dst + r * a) + (a_new / 2)) / a_new);
		dst[3] = (uint8) a_new;
	}
}

// check if a pixel is in the lua canvas
static inline bool gui_check_boundary(int x, int y)
{
	return !(x < 0 || x >= LUA_SCREEN_WIDTH || y < 0 || y >= LUA_SCREEN_HEIGHT);
}

// check if any part of a box is in the lua canvas
static inline bool gui_checkbox(int x1, int y1, int x2, int y2)
{
	if ((x1 <  0 && x2 <  0)
		|| (x1 >= LUA_SCREEN_WIDTH && x2 >= LUA_SCREEN_WIDTH)
		|| (y1 <  0 && y2 <  0)
		|| (y1 >= LUA_SCREEN_HEIGHT && y2 >= LUA_SCREEN_HEIGHT))
		return false;
	return true;
}

// write a pixel to the canvas (do not check boundaries for speedup)
static inline void gui_drawpixel_fast(VbaLuaState *state, int x, int y, uint32 colour)
{
	blend32((uint32 *) &state->guiData[(y * LUA_SCREEN_WIDTH + x) * 4], colour);
}

// write a pixel to the canvas (check boundaries)
static inline void gui_drawpixel_internal(VbaLuaState *state, int x, int y, uint32 colour)
{
	if (gui_check_boundary(x, y))
		gui_drawpixel_fast(state, x, y, colour);
}

// draw a line on the canvas (checks boundaries)
static void gui_drawline_internal(VbaLuaState *state, int x1, int y1, int x2, int y2, bool lastPixel, uint32 colour)
{
	// Note: New version of Bresenham's Line Algorithm
	// http://groups.google.co.jp/group/rec.games.roguelike.development/browse_thread/thread/345f4c42c3b25858/29e07a3af3a450e6?show_docid=29e07a3af3a450e6
	int swappedx = 0;
	int swappedy = 0;

	int xtemp = x1 - x2;
	int ytemp = y1 - y2;
	if (xtemp == 0 && ytemp == 0)
	{
		gui_drawpixel_internal(state, x1, y1, colour);
		return;
	}

	if (xtemp < 0)
	{
		xtemp	 = -xtemp;
		swappedx = 1;
	}

	if (ytemp < 0)
	{
		ytemp	 = -ytemp;
		swappedy = 1;
	}

	int delta_x = xtemp << 1;
	int delta_y = ytemp << 1;

	signed char ix = x1 > x2 ? 1 : -1;
	signed char iy = y1 > y2 ? 1 : -1;

	if (lastPixel)
		gui_drawpixel_internal(state, x2, y2, colour);

	if (delta_x >= delta_y)
	{
		int error = delta_y - (delta_x >> 1);

		while (x2 != x1)
		{
			if (error == 0 && !swappedx)
				gui_drawpixel_internal(state, x2 + ix, y2, colour);
			if (error >= 0)
			{
				if (error || (ix > 0))
				{
					y2	  += iy;
					error -= delta_x;
				}
			}

			x2 += ix;
			gui_drawpixel_internal(state, x2, y2, colour);
			if (error == 0 && swappedx)
				gui_drawpixel_internal(state, x2, y2 + iy, colour);
			error += delta_y;
		}
	}
	else
	{
		int error = delta_x - (delta_y >> 1);

		while (y2 != y1)
		{
			if (error == 0 && !swappedy)
				gui_drawpixel_internal(state, x2, y2 + iy, colour);
			if (error >= 0)
			{
				if (error || (iy > 0))
				{
					x2	  += ix;
					error -= delta_y;
				}
			}

			y2 += iy;
			gui_drawpixel_internal(state, x2, y2, colour);
			if (error == 0 && swappedy)
				gui_drawpixel_internal(state, x2 + ix, y2, colour);
			error += delta_x;
		}
	}
}

// draw a rect on the canvas
static void gui_drawbox_internal(VbaLuaState *state, int x1, int y1, int x2, int y2, uint32 colour)
{
	if (x1 > x2)
		std::swap(x1, x2);
	if (y1 > y2)
		std::swap(y1, y2);
	if (x1 < 0)
		x1 = -1;
	if (y1 < 0)
		y1 = -1;
	if (x2 >= LUA_SCREEN_WIDTH)
		x2 = LUA_SCREEN_WIDTH;
	if (y2 >= LUA_SCREEN_HEIGHT)
		y2 = LUA_SCREEN_HEIGHT;

	if (!gui_checkbox(x1, y1, x2, y2))
		return;

	gui_drawline_internal(state, x1, y1, x2, y1, true, colour);
	gui_drawline_internal(state, x1, y2, x2, y2, true, colour);
	gui_drawline_internal(state, x1, y1, x1, y2, true, colour);
	gui_drawline_internal(state, x2, y1, x2, y2, true, colour);
}

// draw a circle on the canvas
static void gui_drawcircle_internal(VbaLuaState *state, int x0, int y0, int radius, uint32 colour)
{
	if (radius < 0)
		radius = -radius;
	if (radius == 0)
		return;
	if (radius == 1)
	{
		gui_drawpixel_internal(state, x0, y0, colour);
		return;
	}

	// http://en.wikipedia.org/wiki/Midpoint_circle_algorithm
	int f	  = 1 - radius;
	int ddF_x = 1;
	int ddF_y = -2 * radius;
	int x	  = 0;
	int y	  = radius;

	if (!gui_checkbox(x0 - radius, y0 - radius, x0 + radius, y0 + radius))
		return;

	gui_drawpixel_internal(state, x0, y0 + radius, colour);
	gui_drawpixel_internal(state, x0, y0 - radius, colour);
	gui_drawpixel_internal(state, x0 + radius, y0, colour);
	gui_drawpixel_internal(state, x0 - radius, y0, colour);

	// same pixel shouldn't be drawed twice,
	// because each pixel has opacity.
	// so now the routine gets ugly.
	while (true)
	{
		assert(ddF_x == 2 * x + 1);
		assert(ddF_y == -2 * y);
		assert(f == x * x + y * y - radius * radius + 2 * x - y + 1);
		if (f >= 0)
		{
			y--;
			ddF_y += 2;
			f	  += ddF_y;
		}

		x++;
		ddF_x += 2;
		f	  += ddF_x;
		if (x < y)
		{
			gui_drawpixel_internal(state, x0 + x, y0 + y, colour);
			gui_drawpixel_internal(state, x0 - x, y0 + y, colour);
			gui_drawpixel_internal(state, x0 + x, y0 - y, colour);
			gui_drawpixel_internal(state, x0 - x, y0 - y, colour);
			gui_drawpixel_internal(state, x0 + y, y0 + x, colour);
			gui_drawpixel_internal(state, x0 - y, y0 + x, colour);
			gui_drawpixel_internal(state, x0 + y, y0 - x, colour);
			gui_drawpixel_internal(state, x0 - y, y0 - x, colour);
		}
		else if (x == y)
		{
			gui_drawpixel_internal(state, x0 + x, y0 + y, colour);
			gui_drawpixel_internal(state, x0 - x, y0 + y, colour);
			gui_drawpixel_internal(state, x0 + x, y0 - y, colour);
			gui_drawpixel_internal(state, x0 - x, y0 - y, colour);
			break;
		}
		else
			break;
	}
}

// draw fill rect on the canvas
static void gui_fillbox_internal(VbaLuaState *state, int x1, int y1, int x2, int y2, uint32 colour)
{
	if (x1 > x2)
		std::swap(x1, x2);
	if (y1 > y2)
		std::swap(y1, y2);
	if (x1 < 0)
		x1 = 0;
	if (y1 < 0)
		y1 = 0;
	if (x2 >= LUA_SCREEN_WIDTH)
		x2 = LUA_SCREEN_WIDTH - 1;
	if (y2 >= LUA_SCREEN_HEIGHT)
		y2 = LUA_SCREEN_HEIGHT - 1;

	int ix, iy;
	for (iy = y1; iy <= y2; iy++)
	{
		for (ix = x1; ix <= x2; ix++)
		{
			gui_drawpixel_fast(state, ix, iy, colour);
		}
	}
}

// fill a circle on the canvas
static void gui_fillcircle_internal(VbaLuaState *state, int x0, int y0, int radius, uint32 colour)
{
	if (radius < 0)
		radius = -radius;
	if (radius == 0)
		return;
	if (radius == 1)
	{
		gui_drawpixel_internal(state, x0, y0, colour);
		return;
	}

	// http://en.wikipedia.org/wiki/Midpoint_circle_algorithm
	int f	  = 1 - radius;
	int ddF_x = 1;
	int ddF_y = -2 * radius;
	int x	  = 0;
	int y	  = radius;

	if (!gui_checkbox(x0 - radius, y0 - radius, x0 + radius, y0 + radius))
		return;

	gui_drawline_internal(state, x0, y0 - radius, x0, y0 + radius, true, colour);

	while (true)
	{
		assert(ddF_x == 2 * x + 1);
		assert(ddF_y == -2 * y);
		assert(f == x * x + y * y - radius * radius + 2 * x - y + 1);
		if (f >= 0)
		{
			y--;
			ddF_y += 2;
			f	  += ddF_y;
		}

		x++;
		ddF_x += 2;
		f	  += ddF_x;

		if (x < y)
		{
			gui_drawline_internal(state, x0 + x, y0 - y, x0 + x, y0 + y, true, colour);
			gui_drawline_internal(state, x0 - x, y0 - y, x0 - x, y0 + y, true, colour);
			if (f >= 0)
			{
				gui_drawline_internal(state, x0 + y, y0 - x, x0 + y, y0 + x, true, colour);
				gui_drawline_internal(state, x0 - y, y0 - x, x0 - y, y0 + x, true, colour);
			}
		}
		else if (x == y)
		{
			gui_drawline_internal(state, x0 + x, y0 - y, x0 + x, y0 + y, true, colour);
			gui_drawline_internal(state, x0 - x, y0 - y, x0 - x, y0 + y, true, colour);
			break;
		}
		else
			break;
	}
}

// Helper for a simple hex parser
static int hex2int(lua_State *L, char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return luaL_error(L, "invalid hex in colour");
}

static const struct ColorMapping
{
	const char *name;
	uint32		value;
}
s_colorMapping[] =
{
	{ "white",		0xFFFFFFFF		  },
	{ "black",		0x000000FF		  },
	{ "clear",		0x00000000		  },
	{ "gray",		0x7F7F7FFF		  },
	{ "grey",		0x7F7F7FFF		  },
	{ "red",		0xFF0000FF		  },
	{ "orange",		0xFF7F00FF		  },
	{ "yellow",		0xFFFF00FF		  },
	{ "chartreuse", 0x7FFF00FF		  },
	{ "green",		0x00FF00FF		  },
	{ "teal",		0x00FF7FFF		  },
	{ "cyan",		0x00FFFFFF		  },
	{ "blue",		0x0000FFFF		  },
	{ "purple",		0x7F00FFFF		  },
	{ "magenta",	0xFF00FFFF		  },
};

/**
* Converts an integer or a string on the stack at the given
* offset to a RGB32 colour. Several encodings are supported.
* The user may construct their own RGB value, given a simple colour name,
* or an HTML-style "#09abcd" colour. 16 bit reduction doesn't occur at this time.
*/
static inline bool str2colour(uint32 *colour, lua_State *L, const char *str)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	if (str[0] == '#')
	{
		int color;
		sscanf(str + 1, "%X", &color);

		int len		= (int)strlen(str + 1);
		int missing = max(0, 8 - len);
		color <<= missing << 2;
		if (missing >= 2)
			color |= 0xFF;
		*colour = color;
		return true;
	}
	else
	{
		if (!strnicmp(str, "rand", 4))
		{
			// every script has its own generator, so one script calling
			// math.randomseed() cannot shift another script's colours
			if (state)
				*colour = sfmt_genrand_uint32(&state->rng) | 0xFF;
			else
				*colour = 0xFFFFFF;
			return true;
		}

		for (int i = 0; i < sizeof(s_colorMapping) / sizeof(*s_colorMapping); i++)
		{
			if (!stricmp(str, s_colorMapping[i].name))
			{
				*colour = s_colorMapping[i].value;
				return true;
			}
		}
	}

	return false;
}

static inline uint32 gui_getcolour_wrapped(lua_State *L, int offset, bool hasDefaultValue, uint32 defaultColour)
{
	switch (lua_type(L, offset))
	{
	case LUA_TSTRING:
		{
			const char *str = lua_tostring(L, offset);
			uint32		colour;

			if (str2colour(&colour, L, str))
				return colour;
			else
			{
				if (hasDefaultValue)
					return defaultColour;
				else
					return luaL_error(L, "unknown colour %s", str);
			}
		}

	case LUA_TNUMBER:
		{
			uint32 colour = (uint32)lua_tonumber(L, offset);
			return colour;
		}

	case LUA_TTABLE:
		{
			int color = 0xFF;
			lua_pushnil(L); // first key
			int	 keyIndex	= lua_gettop(L);
			int	 valueIndex = keyIndex + 1;
			bool first		= true;
			while (lua_next(L, offset))
			{
				bool keyIsString = (lua_type(L, keyIndex) == LUA_TSTRING);
				bool keyIsNumber = (lua_type(L, keyIndex) == LUA_TNUMBER);
				int	 key		 = (int)(keyIsString ? tolower(*lua_tostring(L, keyIndex)) : (keyIsNumber ? lua_tointeger(L, keyIndex) : 0));
				int	 value		 = (int)lua_tointeger(L, valueIndex);
				if (value < 0) value = 0;
				if (value > 255) value = 255;
				switch (key)
				{
				case 1:
				case 'r':
					color |= value << 24; break;
				case 2:
				case 'g':
					color |= value << 16; break;
				case 3:
				case 'b':
					color |= value << 8; break;
				case 4:
				case 'a':
					color = (color & ~0xFF) | value; break;
				}
				lua_pop(L, 1);
			}
			return color;
		}   break;

	case LUA_TFUNCTION:
		luaL_error(L, "invalid colour"); // NYI
		return 0;

	default:
		if (hasDefaultValue)
			return defaultColour;
		else
			return luaL_error(L, "invalid colour");
	}
}

static uint32 gui_getcolour(lua_State *L, int offset)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	uint32 colour;
	int	   a, r, g, b;

	colour = gui_getcolour_wrapped(L, offset, false, 0);
	a	   = ((colour & 0xff) * state->transparency) / 255;
	if (a > 255)
		a = 255;
	b = (colour >> 8) & 0xff;
	g = (colour >> 16) & 0xff;
	r = (colour >> 24) & 0xff;
	return LUA_BUILD_PIXEL(a, r, g, b);
}

static uint32 gui_optcolour(lua_State *L, int offset, uint32 defaultColour)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	uint32 colour;
	int	   a, r, g, b;
	uint8  defA, defB, defG, defR;

	LUA_DECOMPOSE_PIXEL(defaultColour, defA, defR, defG, defB);
	defaultColour = (defR << 24) | (defG << 16) | (defB << 8) | defA;

	colour = gui_getcolour_wrapped(L, offset, true, defaultColour);
	a	   = ((colour & 0xff) * state->transparency) / 255;
	if (a > 255)
		a = 255;
	b = (colour >> 8) & 0xff;
	g = (colour >> 16) & 0xff;
	r = (colour >> 24) & 0xff;
	return LUA_BUILD_PIXEL(a, r, g, b);
}

// gui.drawpixel(x,y,colour)
static int gui_drawpixel(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	int x = (int)luaL_checkinteger(L, 1);
	int y = (int)luaL_checkinteger(L, 2);

	uint32 colour = gui_getcolour(L, 3);

	//	if (!gui_check_boundary(x, y))
	//		luaL_error(L,"bad coordinates");
	gui_prepare(state);

	gui_drawpixel_internal(state, x, y, colour);

	return 0;
}

// gui.drawline(x1,y1,x2,y2,color,skipFirst)
static int gui_drawline(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	int	   x1, y1, x2, y2;
	uint32 color;
	x1	  = (int)luaL_checkinteger(L, 1);
	y1	  = (int)luaL_checkinteger(L, 2);
	x2	  = (int)luaL_checkinteger(L, 3);
	y2	  = (int)luaL_checkinteger(L, 4);
	color = gui_optcolour(L, 5, LUA_BUILD_PIXEL(255, 255, 255, 255));
	int skipFirst = lua_toboolean(L, 6);

	gui_prepare(state);

	gui_drawline_internal(state, x2, y2, x1, y1, !skipFirst, color);

	return 0;
}

// gui.drawbox(x1, y1, x2, y2, fillcolor, outlinecolor)
static int gui_drawbox(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	int	   x1, y1, x2, y2;
	uint32 fillcolor;
	uint32 outlinecolor;

	x1 = (int)luaL_checkinteger(L, 1);
	y1 = (int)luaL_checkinteger(L, 2);
	x2 = (int)luaL_checkinteger(L, 3);
	y2 = (int)luaL_checkinteger(L, 4);
	fillcolor	 = gui_optcolour(L, 5, LUA_BUILD_PIXEL(63, 255, 255, 255));
	outlinecolor = gui_optcolour(L, 6, LUA_BUILD_PIXEL(255, LUA_PIXEL_R(fillcolor), LUA_PIXEL_G(fillcolor), LUA_PIXEL_B(fillcolor)));

	if (x1 > x2)
		std::swap(x1, x2);
	if (y1 > y2)
		std::swap(y1, y2);

	gui_prepare(state);

	gui_drawbox_internal(state, x1, y1, x2, y2, outlinecolor);
	if ((x2 - x1) >= 2 && (y2 - y1) >= 2)
		gui_fillbox_internal(state, x1 + 1, y1 + 1, x2 - 1, y2 - 1, fillcolor);

	return 0;
}

// gui.drawcircle(x0, y0, radius, colour)
static int gui_drawcircle(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	int	   x, y, r;
	uint32 colour;

	x	   = (int)luaL_checkinteger(L, 1);
	y	   = (int)luaL_checkinteger(L, 2);
	r	   = (int)luaL_checkinteger(L, 3);
	colour = gui_getcolour(L, 4);

	gui_prepare(state);

	gui_drawcircle_internal(state, x, y, r, colour);

	return 0;
}

// gui.fillbox(x1, y1, x2, y2, colour)
static int gui_fillbox(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	int	   x1, y1, x2, y2;
	uint32 colour;

	x1	   = (int)luaL_checkinteger(L, 1);
	y1	   = (int)luaL_checkinteger(L, 2);
	x2	   = (int)luaL_checkinteger(L, 3);
	y2	   = (int)luaL_checkinteger(L, 4);
	colour = gui_getcolour(L, 5);

	//	if (!gui_check_boundary(x1, y1))
	//		luaL_error(L,"bad coordinates");
	//
	//	if (!gui_check_boundary(x2, y2))
	//		luaL_error(L,"bad coordinates");
	gui_prepare(state);

	if (!gui_checkbox(x1, y1, x2, y2))
		return 0;

	gui_fillbox_internal(state, x1, y1, x2, y2, colour);

	return 0;
}

// gui.fillcircle(x0, y0, radius, colour)
static int gui_fillcircle(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	int	   x, y, r;
	uint32 colour;

	x	   = (int)luaL_checkinteger(L, 1);
	y	   = (int)luaL_checkinteger(L, 2);
	r	   = (int)luaL_checkinteger(L, 3);
	colour = gui_getcolour(L, 4);

	gui_prepare(state);

	gui_fillcircle_internal(state, x, y, r, colour);

	return 0;
}

static int gui_getpixel(lua_State *L)
{
	int x = (int)luaL_checkinteger(L, 1);
	int y = (int)luaL_checkinteger(L, 2);

	int pixWidth, pixHeight;
	int scrWidth, scrHeight;
	int scrOffsetX, scrOffsetY;
	systemGetLCDResolution(pixWidth, pixHeight);
	systemGetLCDBaseSize(scrWidth, scrHeight);
	systemGetLCDBaseOffset(scrOffsetX, scrOffsetY);
	int pitch = pixWidth * (systemColorDepth / 8) + (systemColorDepth == 24 ? 0 : 4);
	++scrOffsetY; // FIXME: don't know why it's needed

	if (!(x >= 0 && y >= 0 && x < scrWidth && y < scrHeight) /*!gui_check_boundary(x,y)*/)
	{
		lua_pushinteger(L, 0);
		lua_pushinteger(L, 0);
		lua_pushinteger(L, 0);
	}
	else
	{
		switch (systemColorDepth)
		{
		case 16:
			{
				uint16 *screen	 = (uint16 *) (&pix[scrOffsetY * pitch + scrOffsetX * 2]);
				uint16	pixColor = screen[y * pitch / 2 + x];
				lua_pushinteger(L, (pixColor >> 8) & 0xF8); // red
				lua_pushinteger(L, (pixColor >> 3) & 0xFC); // green
				lua_pushinteger(L, (pixColor << 3) & 0xF8); // blue
			}
			break;
		case 24:
			{
				uint8 *screen = &pix[scrOffsetY * pitch + scrOffsetX * 3];
				lua_pushinteger(L, screen[y * pitch + x * 3 + 2]); // red
				lua_pushinteger(L, screen[y * pitch + x * 3 + 1]); // green
				lua_pushinteger(L, screen[y * pitch + x * 3 + 0]); // blue
			}
			break;
		case 32:
			{
				uint8 *screen = &pix[scrOffsetY * pitch + scrOffsetX * 4];
				lua_pushinteger(L, screen[y * pitch + x * 4 + 2]); // red
				lua_pushinteger(L, screen[y * pitch + x * 4 + 1]); // green
				lua_pushinteger(L, screen[y * pitch + x * 4 + 0]); // blue
			}
			break;
		default:
			lua_pushinteger(L, 0);
			lua_pushinteger(L, 0);
			lua_pushinteger(L, 0);
			break;
		}
	}
	return 3;
}

static int gui_parsecolor(lua_State *L)
{
	int	   r, g, b, a;
	uint32 color = gui_getcolour(L, 1);
	LUA_DECOMPOSE_PIXEL(color, a, r, g, b);
	lua_pushinteger(L, r);
	lua_pushinteger(L, g);
	lua_pushinteger(L, b);
	lua_pushinteger(L, a);
	return 4;
}

// gui.gdscreenshot()
//
//  Returns a screen shot as a string in gd's v1 file format.
//  This allows us to make screen shots available without gd installed locally.
//  Users can also just grab pixels via substring selection.
//
//  I think...  Does lua support grabbing byte values from a string? // yes, string.byte(str,offset)
//  Well, either way, just install gd and do what you like with it.
//  It really is easier that way.

// example: gd.createFromGdStr(gui.gdscreenshot()):png("outputimage.png")
static int gui_gdscreenshot(lua_State *L)
{
	int ppl, ppldummy;
	int width, height;
	int xofs, yofs;
	systemGetLCDResolution(ppl, ppldummy);
	systemGetLCDBaseSize(width, height);
	systemGetLCDBaseOffset(xofs, yofs);

	++yofs; // FIXME: don't know why it's needed

	//int pitch = (((ppl * systemColorDepth + 7)>>3)+3)&~3;
	int	   pitch  = ppl * (systemColorDepth / 8) + (systemColorDepth == 24 ? 0 : 4);
	uint8 *screen = &pix[yofs * pitch + xofs * (systemColorDepth / 8)];

	int	  size = 11 + width * height * 4;
	char *str  = new char[size + 1];
	str[size] = 0;

	unsigned char *ptr = (unsigned char *)str;

	// GD format header for truecolor image (11 bytes)
	*ptr++ = (65534 >> 8) & 0xFF;
	*ptr++ = (65534) & 0xFF;
	*ptr++ = (width >> 8) & 0xFF;
	*ptr++ = (width) & 0xFF;
	*ptr++ = (height >> 8) & 0xFF;
	*ptr++ = (height) & 0xFF;
	*ptr++ = 1;
	*ptr++ = 255;
	*ptr++ = 255;
	*ptr++ = 255;
	*ptr++ = 255;

	GetColorFunc getColor = NULL;
	if (!getColorIOFunc(systemColorDepth, &getColor, NULL) || getColor == NULL)
	{
		delete[] str;
		return 0;
	}

	int x, y;
	for (y = 0; y < height; y++)
	{
		uint8 *s = &screen[y * pitch];
		for (x = 0; x < width; x++, s += systemColorDepth / 8)
		{
			uint8 r, g, b;
			getColor(s, &r, &g, &b);

			*ptr++ = 0;
			*ptr++ = r;
			*ptr++ = g;
			*ptr++ = b;
		}
	}

	lua_pushlstring(L, str, size);
	delete[] str;
	return 1;
}

// gui.opacity(number alphaValue)
// sets the transparency of subsequent draw calls
// 0.0 is completely transparent, 1.0 is completely opaque
// non-integer values are supported and meaningful, as are values greater than 1.0
// it is not necessary to use this function to get transparency (or the less-recommended gui.transparency() either),
// because you can provide an alpha value in the color argument of each draw call.

// however, it can be convenient to be able to globally modify the drawing transparency
static int gui_setopacity(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	double opacF = luaL_checknumber(L, 1);
	state->transparency = (int)(opacF * 255);
	if (state->transparency < 0)
		state->transparency = 0;
	return 0;
}

// gui.transparency(int strength)
//

//  0 = solid,
static int gui_transparency(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	double trans = luaL_checknumber(L, 1);
	state->transparency = (int)((4.0 - trans) / 4.0 * 255);
	if (state->transparency < 0)
		state->transparency = 0;
	return 0;
}

static const uint32 Small_Font_Data[] =
{
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 32
	0x00000000, 0x00000300, 0x00000400, 0x00000500, 0x00000000, 0x00000700, 0x00000000, // 33	!
	0x00000000, 0x00040002, 0x00050003, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 34	"
	0x00000000, 0x00040002, 0x00050403, 0x00060004, 0x00070605, 0x00080006, 0x00000000, // 35	#
	0x00000000, 0x00040300, 0x00000403, 0x00000500, 0x00070600, 0x00000706, 0x00000000, // 36	$
	0x00000000, 0x00000002, 0x00050000, 0x00000500, 0x00000005, 0x00080000, 0x00000000, // 37	%
	0x00000000, 0x00000300, 0x00050003, 0x00000500, 0x00070005, 0x00080700, 0x00000000, // 38	&
	0x00000000, 0x00000300, 0x00000400, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 39	'
	0x00000000, 0x00000300, 0x00000003, 0x00000004, 0x00000005, 0x00000700, 0x00000000, // 40	(
	0x00000000, 0x00000300, 0x00050000, 0x00060000, 0x00070000, 0x00000700, 0x00000000, // 41	)
	0x00000000, 0x00000000, 0x00000400, 0x00060504, 0x00000600, 0x00080006, 0x00000000, // 42	*
	0x00000000, 0x00000000, 0x00000400, 0x00060504, 0x00000600, 0x00000000, 0x00000000, // 43	+
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000600, 0x00000700, 0x00000007, // 44	,
	0x00000000, 0x00000000, 0x00000000, 0x00060504, 0x00000000, 0x00000000, 0x00000000, // 45	-
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000700, 0x00000000, // 46	.
	0x00030000, 0x00040000, 0x00000400, 0x00000500, 0x00000005, 0x00000006, 0x00000000, // 47	/
	0x00000000, 0x00000300, 0x00050003, 0x00060004, 0x00070005, 0x00000700, 0x00000000, // 48	0
	0x00000000, 0x00000300, 0x00000403, 0x00000500, 0x00000600, 0x00000700, 0x00000000, // 49	1
	0x00000000, 0x00000302, 0x00050000, 0x00000500, 0x00000005, 0x00080706, 0x00000000, // 50	2
	0x00000000, 0x00000302, 0x00050000, 0x00000504, 0x00070000, 0x00000706, 0x00000000, // 51	3
	0x00000000, 0x00000300, 0x00000003, 0x00060004, 0x00070605, 0x00080000, 0x00000000, // 52	4
	0x00000000, 0x00040302, 0x00000003, 0x00000504, 0x00070000, 0x00000706, 0x00000000, // 53	5
	0x00000000, 0x00000300, 0x00000003, 0x00000504, 0x00070005, 0x00000700, 0x00000000, // 54	6
	0x00000000, 0x00040302, 0x00050000, 0x00000500, 0x00000600, 0x00000700, 0x00000000, // 55	7
	0x00000000, 0x00000300, 0x00050003, 0x00000500, 0x00070005, 0x00000700, 0x00000000, // 56	8
	0x00000000, 0x00000300, 0x00050003, 0x00060500, 0x00070000, 0x00000700, 0x00000000, // 57	9
	0x00000000, 0x00000000, 0x00000400, 0x00000000, 0x00000000, 0x00000700, 0x00000000, // 58	:
	0x00000000, 0x00000000, 0x00000000, 0x00000500, 0x00000000, 0x00000700, 0x00000007, // 59	;
	0x00000000, 0x00040000, 0x00000400, 0x00000004, 0x00000600, 0x00080000, 0x00000000, // 60	<
	0x00000000, 0x00000000, 0x00050403, 0x00000000, 0x00070605, 0x00000000, 0x00000000, // 61	=
	0x00000000, 0x00000002, 0x00000400, 0x00060000, 0x00000600, 0x00000006, 0x00000000, // 62	>
	0x00000000, 0x00000302, 0x00050000, 0x00000500, 0x00000000, 0x00000700, 0x00000000, // 63	?
	0x00000000, 0x00000300, 0x00050400, 0x00060004, 0x00070600, 0x00000000, 0x00000000, // 64	@
	0x00000000, 0x00000300, 0x00050003, 0x00060504, 0x00070005, 0x00080006, 0x00000000, // 65	A
	0x00000000, 0x00000302, 0x00050003, 0x00000504, 0x00070005, 0x00000706, 0x00000000, // 66	B
	0x00000000, 0x00040300, 0x00000003, 0x00000004, 0x00000005, 0x00080700, 0x00000000, // 67	C
	0x00000000, 0x00000302, 0x00050003, 0x00060004, 0x00070005, 0x00000706, 0x00000000, // 68	D
	0x00000000, 0x00040302, 0x00000003, 0x00000504, 0x00000005, 0x00080706, 0x00000000, // 69	E
	0x00000000, 0x00040302, 0x00000003, 0x00000504, 0x00000005, 0x00000006, 0x00000000, // 70	F
	0x00000000, 0x00040300, 0x00000003, 0x00060004, 0x00070005, 0x00080700, 0x00000000, // 71	G
	0x00000000, 0x00040002, 0x00050003, 0x00060504, 0x00070005, 0x00080006, 0x00000000, // 72	H
	0x00000000, 0x00000300, 0x00000400, 0x00000500, 0x00000600, 0x00000700, 0x00000000, // 73	I
	0x00000000, 0x00040000, 0x00050000, 0x00060000, 0x00070005, 0x00000700, 0x00000000, // 74	J
	0x00000000, 0x00040002, 0x00050003, 0x00000504, 0x00070005, 0x00080006, 0x00000000, // 75	K
	0x00000000, 0x00000002, 0x00000003, 0x00000004, 0x00000005, 0x00080706, 0x00000000, // 76	l
	0x00000000, 0x00040002, 0x00050403, 0x00060004, 0x00070005, 0x00080006, 0x00000000, // 77	M
	0x00000000, 0x00000302, 0x00050003, 0x00060004, 0x00070005, 0x00080006, 0x00000000, // 78	N
	0x00000000, 0x00040302, 0x00050003, 0x00060004, 0x00070005, 0x00080706, 0x00000000, // 79	O
	0x00000000, 0x00000302, 0x00050003, 0x00000504, 0x00000005, 0x00000006, 0x00000000, // 80	P
	0x00000000, 0x00040302, 0x00050003, 0x00060004, 0x00070005, 0x00080706, 0x00090000, // 81	Q
	0x00000000, 0x00000302, 0x00050003, 0x00000504, 0x00070005, 0x00080006, 0x00000000, // 82	R
	0x00000000, 0x00040300, 0x00000003, 0x00000500, 0x00070000, 0x00000706, 0x00000000, // 83	S
	0x00000000, 0x00040302, 0x00000400, 0x00000500, 0x00000600, 0x00000700, 0x00000000, // 84	T
	0x00000000, 0x00040002, 0x00050003, 0x00060004, 0x00070005, 0x00080706, 0x00000000, // 85	U
	0x00000000, 0x00040002, 0x00050003, 0x00060004, 0x00000600, 0x00000700, 0x00000000, // 86	V
	0x00000000, 0x00040002, 0x00050003, 0x00060004, 0x00070605, 0x00080006, 0x00000000, // 87	W
	0x00000000, 0x00040002, 0x00050003, 0x00000500, 0x00070005, 0x00080006, 0x00000000, // 88	X
	0x00000000, 0x00040002, 0x00050003, 0x00000500, 0x00000600, 0x00000700, 0x00000000, // 89	Y
	0x00000000, 0x00040302, 0x00050000, 0x00000500, 0x00000005, 0x00080706, 0x00000000, // 90	Z
	0x00000000, 0x00040300, 0x00000400, 0x00000500, 0x00000600, 0x00080700, 0x00000000, // 91	[
	0x00000000, 0x00000002, 0x00000400, 0x00000500, 0x00070000, 0x00080000, 0x00000000, // 92	'\'
	0x00000000, 0x00000302, 0x00000400, 0x00000500, 0x00000600, 0x00000706, 0x00000000, // 93	]
	0x00000000, 0x00000300, 0x00050003, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 94	^
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00080706, 0x00000000, // 95	_
	0x00000000, 0x00000002, 0x00000400, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 96	`
	0x00000000, 0x00000000, 0x00050400, 0x00060004, 0x00070005, 0x00080700, 0x00000000, // 97	a
	0x00000000, 0x00000002, 0x00000003, 0x00000504, 0x00070005, 0x00000706, 0x00000000, // 98	b
	0x00000000, 0x00000000, 0x00050400, 0x00000004, 0x00000005, 0x00080700, 0x00000000, // 99	c
	0x00000000, 0x00040000, 0x00050000, 0x00060500, 0x00070005, 0x00080700, 0x00000000, // 100	d
	0x00000000, 0x00000000, 0x00050400, 0x00060504, 0x00000005, 0x00080700, 0x00000000, // 101	e
	0x00000000, 0x00040300, 0x00000003, 0x00000504, 0x00000005, 0x00000006, 0x00000000, // 102	f
	0x00000000, 0x00000000, 0x00050400, 0x00060004, 0x00070600, 0x00080000, 0x00000807, // 103	g
	0x00000000, 0x00000002, 0x00000003, 0x00000504, 0x00070005, 0x00080006, 0x00000000, // 104	h
	0x00000000, 0x00000300, 0x00000000, 0x00000500, 0x00000600, 0x00000700, 0x00000000, // 105	i
	0x00000000, 0x00000300, 0x00000000, 0x00000500, 0x00000600, 0x00000700, 0x00000007, // 106	j
	0x00000000, 0x00000002, 0x00000003, 0x00060004, 0x00000605, 0x00080006, 0x00000000, // 107	k
	0x00000000, 0x00000300, 0x00000400, 0x00000500, 0x00000600, 0x00080000, 0x00000000, // 108	l
	0x00000000, 0x00000000, 0x00050003, 0x00060504, 0x00070005, 0x00080006, 0x00000000, // 109	m
	0x00000000, 0x00000000, 0x00000403, 0x00060004, 0x00070005, 0x00080006, 0x00000000, // 110	n
	0x00000000, 0x00000000, 0x00000400, 0x00060004, 0x00070005, 0x00000700, 0x00000000, // 111	o
	0x00000000, 0x00000000, 0x00000400, 0x00060004, 0x00000605, 0x00000006, 0x00000007, // 112	p
	0x00000000, 0x00000000, 0x00000400, 0x00060004, 0x00070600, 0x00080000, 0x00090000, // 113	q
	0x00000000, 0x00000000, 0x00050003, 0x00000504, 0x00000005, 0x00000006, 0x00000000, // 114	r
	0x00000000, 0x00000000, 0x00050400, 0x00000004, 0x00070600, 0x00000706, 0x00000000, // 115	s
	0x00000000, 0x00000300, 0x00050403, 0x00000500, 0x00000600, 0x00080000, 0x00000000, // 116	t
	0x00000000, 0x00000000, 0x00050003, 0x00060004, 0x00070005, 0x00080700, 0x00000000, // 117	u
	0x00000000, 0x00000000, 0x00050003, 0x00060004, 0x00070005, 0x00000700, 0x00000000, // 118	v
	0x00000000, 0x00000000, 0x00050003, 0x00060004, 0x00070605, 0x00080006, 0x00000000, // 119	w
	0x00000000, 0x00000000, 0x00050003, 0x00000500, 0x00070005, 0x00080006, 0x00000000, // 120	x
	0x00000000, 0x00000000, 0x00050003, 0x00060004, 0x00000600, 0x00000700, 0x00000007, // 121	y
	0x00000000, 0x00000000, 0x00050403, 0x00000500, 0x00000005, 0x00080706, 0x00000000, // 122	z
	0x00000000, 0x00040300, 0x00000400, 0x00000504, 0x00000600, 0x00080700, 0x00000000, // 123	{
	0x00000000, 0x00000300, 0x00000400, 0x00000000, 0x00000600, 0x00000700, 0x00000000, // 124	|
	0x00000000, 0x00000302, 0x00000400, 0x00060500, 0x00000600, 0x00000706, 0x00000000, // 125	}
	0x00000000, 0x00000302, 0x00050000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 126	~
	0x00000000, 0x00000000, 0x00000400, 0x00060004, 0x00070605, 0x00000000, 0x00000000, // 127	
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

static void PutTextInternal(VbaLuaState *state, const char *str, int len, short x, short y, int color, int backcolor)
{
	int Opac	 = (color >> 24) & 0xFF;
	int backOpac = (backcolor >> 24) & 0xFF;
	int origX	 = x;

	if (!Opac && !backOpac)
		return;

	while (*str && len && y < LUA_SCREEN_HEIGHT)
	{
		int c = *str++;
		while (x > LUA_SCREEN_WIDTH && c != '\n')
		{
			c = *str;
			if (c == '\0')
				break;
			str++;
		}

		if (c == '\n')
		{
			x  = origX;
			y += 8;
			continue;
		}
		else if (c == '\t') // just in case
		{
			const int tabSpace = 8;
			x += (tabSpace - (((x - origX) / 4) % tabSpace)) * 4;
			continue;
		}

		if ((unsigned int)(c - 32) >= 96)
			continue;

		const unsigned char *Cur_Glyph = (const unsigned char *) &Small_Font_Data + (c - 32) * 7 * 4;

		for (int y2 = 0; y2 < 8; y2++)
		{
			unsigned int glyphLine = *((unsigned int *)Cur_Glyph + y2);
			for (int x2 = -1; x2 < 4; x2++)
			{
				int shift	  = x2 << 3;
				int mask	  = 0xFF << shift;
				int intensity = (glyphLine & mask) >> shift;

				if (intensity && x2 >= 0 && y2 < 7)
				{
					//int xdraw = max(0,min(LUA_SCREEN_WIDTH - 1,x+x2));
					//int ydraw = max(0,min(LUA_SCREEN_HEIGHT - 1,y+y2));
					//gui_drawpixel_fast(state, xdraw, ydraw, color);
					gui_drawpixel_internal(state, x + x2, y + y2, color);
				}
				else if (backOpac)
				{
					for (int y3 = max(0, y2 - 1); y3 <= min(6, y2 + 1); y3++)
					{
						unsigned int glyphLine = *((unsigned int *)Cur_Glyph + y3);
						for (int x3 = max(0, x2 - 1); x3 <= min(3, x2 + 1); x3++)
						{
							int shift = x3 << 3;
							int mask  = 0xFF << shift;
							intensity |= (glyphLine & mask) >> shift;
							if (intensity)
								goto draw_outline;  // speedup?
						}
					}

draw_outline:
					if (intensity)
					{
						//int xdraw = max(0,min(LUA_SCREEN_WIDTH - 1,x+x2));
						//int ydraw = max(0,min(LUA_SCREEN_HEIGHT - 1,y+y2));
						//gui_drawpixel_fast(state, xdraw, ydraw, backcolor);
						gui_drawpixel_internal(state, x + x2, y + y2, backcolor);
					}
				}
			}
		}

		x += 4;
		len--;
	}
}

static int strlinelen(const char *string)
{
	const char *s = string;
	while (*s && *s != '\n')
		s++;
	if (*s)
		s++;
	return (int)(s - string);
}

static void LuaDisplayString(VbaLuaState *state, const char *string, int y, int x, uint32 color, uint32 outlineColor)
{
	if (!string)
		return;

	gui_prepare(state);

	PutTextInternal(state, string, (int)strlen(string), x, y, color, outlineColor);

	/*
	const char* ptr = string;
	while(*ptr && y < LUA_SCREEN_HEIGHT)
	{
		int len = strlinelen(ptr);
		int skip = 0;
		if(len < 1) len = 1;

		// break up the line if it's too long to display otherwise
		if(len > 63)
		{
			len = 63;
			const char* ptr2 = ptr + len-1;
			for(int j = len-1; j; j--, ptr2--)
			{
				if(*ptr2 == ' ' || *ptr2 == '\t')
				{
					len = j;
					skip = 1;
					break;
				}
			}
		}

		int xl = 0;
		int yl = 0;
		int xh = (LUA_SCREEN_WIDTH - 1 - 1) - 4*len;
		int yh = LUA_SCREEN_HEIGHT - 1;
		int x2 = min(max(x,xl),xh);
		int y2 = min(max(y,yl),yh);

		PutTextInternal(state, ptr,len,x2,y2,color,outlineColor);

		ptr += len + skip;
		y += 8;
	}
	*/
}

// gui.text(int x, int y, string msg)
//
//  Displays the given text on the screen, using the same font and techniques as the

//  main HUD.
static int gui_text(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	//extern int font_height;
	const char *msg;
	int			x, y;
	uint32		colour, borderColour;

	x = (int)luaL_checkinteger(L, 1);
	y = (int)luaL_checkinteger(L, 2);
	//msg = luaL_checkstring(L, 3);
	msg = toCString(L, 3);

	//	if (x < 0 || x >= LUA_SCREEN_WIDTH || y < 0 || y >= (LUA_SCREEN_HEIGHT - font_height))
	//		luaL_error(L,"bad coordinates");
	colour		 = gui_optcolour(L, 4, LUA_BUILD_PIXEL(255, 255, 255, 255));
	borderColour = gui_optcolour(L, 5, LUA_BUILD_PIXEL(255, 0, 0, 0));

	gui_prepare(state);

	LuaDisplayString(state, msg, y, x, colour, borderColour);

	return 0;
}

// gui.gdoverlay([int dx=0, int dy=0,] string str [, sx=0, sy=0, sw, sh] [, float alphamul=1.0])
//
//  Overlays the given image on the screen.

// example: gui.gdoverlay(gd.createFromPng("myimage.png"):gdStr())
static int gui_gdoverlay(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	int argCount = lua_gettop(L);

	int xStartDst = 0;
	int yStartDst = 0;
	int xStartSrc = 0;
	int yStartSrc = 0;

	int index = 1;
	if (lua_type(L, index) == LUA_TNUMBER)
	{
		xStartDst = (int)lua_tointeger(L, index++);
		if (lua_type(L, index) == LUA_TNUMBER)
			yStartDst = (int)lua_tointeger(L, index++);
	}

	luaL_checktype(L, index, LUA_TSTRING);

	const unsigned char *ptr = (const unsigned char *)lua_tostring(L, index++);

	if (ptr[0] != 255 || (ptr[1] != 254 && ptr[1] != 255))
		luaL_error(L, "bad image data");

	bool trueColor = (ptr[1] == 254);
	ptr += 2;

	int imgwidth = *ptr++ << 8;
	imgwidth |= *ptr++;

	int width	  = imgwidth;
	int imgheight = *ptr++ << 8;
	imgheight |= *ptr++;

	int height = imgheight;
	if ((!trueColor && *ptr) || (trueColor && !*ptr))
		luaL_error(L, "bad image data");
	ptr++;

	int pitch = imgwidth * (trueColor ? 4 : 1);

	if ((argCount - index + 1) >= 4)
	{
		xStartSrc = (int)luaL_checkinteger(L, index++);
		yStartSrc = (int)luaL_checkinteger(L, index++);
		width	  = (int)luaL_checkinteger(L, index++);
		height	  = (int)luaL_checkinteger(L, index++);
	}

	int alphaMul = state->transparency;
	if (lua_isnumber(L, index))
		alphaMul = (int)(alphaMul * lua_tonumber(L, index++));
	if (alphaMul <= 0)
		return 0;

	// since there aren't that many possible opacity levels,
	// do the opacity modification calculations beforehand instead of per pixel
	int opacMap[256];
	for (int i = 0; i < 128; i++)
	{
		int opac = 255 - ((i << 1) | (i & 1)); // gdAlphaMax = 127, not 255
		opac = (opac * alphaMul) / 255;
		if (opac < 0)
			opac = 0;
		if (opac > 255)
			opac = 255;
		opacMap[i] = opac;
	}

	for (int i = 128; i < 256; i++)
		opacMap[i] = 0;  // what should we do for them, actually?
	int colorsTotal = 0;
	if (!trueColor)
	{
		colorsTotal	 = *ptr++ << 8;
		colorsTotal |= *ptr++;
	}

	int transparent = *ptr++ << 24;
	transparent |= *ptr++ << 16;
	transparent |= *ptr++ << 8;
	transparent |= *ptr++;
	struct
	{
		uint8 r, g, b, a;
	} pal[256];
	if (!trueColor)
		for (int i = 0; i < 256; i++)
		{
			pal[i].r = *ptr++;
			pal[i].g = *ptr++;
			pal[i].b = *ptr++;
			pal[i].a = opacMap[*ptr++];
		}

	// some of clippings
	if (xStartSrc < 0)
	{
		width	  += xStartSrc;
		xStartDst -= xStartSrc;
		xStartSrc  = 0;
	}

	if (yStartSrc < 0)
	{
		height	  += yStartSrc;
		yStartDst -= yStartSrc;
		yStartSrc  = 0;
	}

	if (xStartSrc + width >= imgwidth)
		width = imgwidth - xStartSrc;
	if (yStartSrc + height >= imgheight)
		height = imgheight - yStartSrc;
	if (xStartDst < 0)
	{
		width += xStartDst;
		if (width <= 0)
			return 0;
		xStartSrc = -xStartDst;
		xStartDst = 0;
	}

	if (yStartDst < 0)
	{
		height += yStartDst;
		if (height <= 0)
			return 0;
		yStartSrc = -yStartDst;
		yStartDst = 0;
	}

	if (xStartDst + width >= LUA_SCREEN_WIDTH)
		width = LUA_SCREEN_WIDTH - xStartDst;
	if (yStartDst + height >= LUA_SCREEN_HEIGHT)
		height = LUA_SCREEN_HEIGHT - yStartDst;
	if (width <= 0 || height <= 0)
		return 0;  // out of screen or invalid size
	gui_prepare(state);

	const uint8 *pix = (const uint8 *)(&ptr[yStartSrc * pitch + (xStartSrc * (trueColor ? 4 : 1))]);
	int			 bytesToNextLine = pitch - (width * (trueColor ? 4 : 1));
	if (trueColor)
	{
		for (int y = yStartDst; y < height + yStartDst && y < LUA_SCREEN_HEIGHT; y++, pix += bytesToNextLine)
		{
			for (int x = xStartDst; x < width + xStartDst && x < LUA_SCREEN_WIDTH; x++, pix += 4)
			{
				gui_drawpixel_fast(state, x, y, LUA_BUILD_PIXEL(opacMap[pix[0]], pix[1], pix[2], pix[3]));
			}
		}
	}
	else
	{
		for (int y = yStartDst; y < height + yStartDst && y < LUA_SCREEN_HEIGHT; y++, pix += bytesToNextLine)
		{
			for (int x = xStartDst; x < width + xStartDst && x < LUA_SCREEN_WIDTH; x++, pix++)
			{
				gui_drawpixel_fast(state, x, y, LUA_BUILD_PIXEL(pal[*pix].a, pal[*pix].r, pal[*pix].g, pal[*pix].b));
			}
		}
	}

	return 0;
}

// function gui.register(function f)
//
//  This function will be called just before a graphical update.
//  More complicated, but doesn't suffer any frame delays.
//  Nil will be accepted in place of a function to erase
//  a previously registered function, and the previous function

//  (if any) is returned, or nil if none.
static int gui_register(lua_State *L)
{
	// We'll do this straight up.
	// First set up the stack.
	lua_settop(L, 1);

	// Verify the validity of the entry
	if (!lua_isnil(L, 1))
		luaL_checktype(L, 1, LUA_TFUNCTION);

	// Get the old value
	lua_getfield(L, LUA_REGISTRYINDEX, guiCallbackTable);

	// Save the new value
	lua_pushvalue(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, guiCallbackTable);

	// The old value is on top of the stack. Return it.
	return 1;
}

// string gui.popup(string message, [string type = "ok"])
//

//  Popup dialog!
int gui_popup(lua_State *L)
{
	const char *message = luaL_checkstring(L, 1);
	const char *type	= luaL_optstring(L, 2, "ok");

#if (defined(WIN32) && !defined(SDL))
	lua_host::MsgBoxType t;
	bool wantsAnswer = false;
	if (strcmp(type, "ok") == 0)
		t = lua_host::MsgBoxType::Ok;
	else if (strcmp(type, "yesno") == 0)
	{
		t = lua_host::MsgBoxType::YesNo;
		wantsAnswer = true;
	}
	else if (strcmp(type, "yesnocancel") == 0)
	{
		t = lua_host::MsgBoxType::YesNoCancel;
		wantsAnswer = true;
	}
	else
		return luaL_error(L, "invalid popup type \"%s\"", type);

	lua_host::checkFullscreen();
	systemSoundClearBuffer();
	lua_host::MsgBoxResult result = lua_host::messageBox(
		message, "Lua Script Pop-up", t, false, scriptTextEncoding(VbaLuaState::fromLua(L)));

	lua_settop(L, 1);

	if (wantsAnswer)
	{
		if (result == lua_host::MsgBoxResult::Yes)
			lua_pushstring(L, "yes");
		else if (result == lua_host::MsgBoxResult::No)
			lua_pushstring(L, "no");
		else if (result == lua_host::MsgBoxResult::Cancel)
			lua_pushstring(L, "cancel");
		else
			luaL_error(L, "unrecognized message box result");
		return 1;
	}

	// else, we don't care.
	return 0;
#else
	char *t;
#ifdef __linux
	// The Linux backend has a "FromPause" variable.
	// If set to 1, assume some known external event has screwed with the flow of time.
	// Since this pauses the emulator waiting for a response, we set it to 1.
	// FIXME: Well, actually it doesn't
	//	extern int FromPause;
	//	FromPause = 1;

	int pid; // appease compiler

	// Before doing any work, verify the correctness of the parameters.
	if (strcmp(type, "ok") == 0)
		t = "OK:100";
	else if (strcmp(type, "yesno") == 0)
		t = "Yes:100,No:101";
	else if (strcmp(type, "yesnocancel") == 0)
		t = "Yes:100,No:101,Cancel:102";
	else
		return luaL_error(L, "invalid popup type \"%s\"", type);

	// Can we find a copy of xmessage? Search the path.
	char *path = strdup(getenv("PATH"));

	char *current = path;

	char *colon;

	int found = 0;

	while (current)
	{
		colon = strchr(current, ':');

		// Clip off the colon.
		*colon++ = 0;

		int	  len	   = strlen(current);
		char *filename = (char *)malloc(len + 12); // always give excess
		snprintf(filename, len + 12, "%s/xmessage", current);

		if (access(filename, X_OK) == 0)
		{
			free(filename);
			found = 1;
			break;
		}

		// Failed, move on.
		current = colon;
		free(filename);
	}

	free(path);

	// We've found it?
	if (!found)
		goto use_console;

	pid = fork();
	if (pid == 0)
	{ // I'm the virgin sacrifice
		// I'm gonna be dead in a matter of microseconds anyways, so wasted memory doesn't matter to me.
		// Go ahead and abuse strdup.
		char *parameters[] = { "xmessage", "-buttons", t, strdup(message), NULL };

		execvp("xmessage", parameters);

		// Aw shitty
		perror("exec xmessage");
		exit(1);
	}
	else if (pid < 0) // something went wrong!!! Oh hell... use the console
		goto use_console;
	else
	{
		// We're the parent. Watch for the child.
		int r;
		int res = waitpid(pid, &r, 0);
		if (res < 0) // wtf?
			goto use_console;

		// The return value gets copmlicated...
		if (!WIFEXITED(r))
		{
			luaL_error(L, "don't screw with my xmessage process!");
		}

		r = WEXITSTATUS(r);

		// We assume it's worked.
		if (r == 0)
		{
			return 0; // no parameters for an OK
		}

		if (r == 100)
		{
			lua_pushstring(L, "yes");
			return 1;
		}

		if (r == 101)
		{
			lua_pushstring(L, "no");
			return 1;
		}

		if (r == 102)
		{
			lua_pushstring(L, "cancel");
			return 1;
		}

		// Wtf?
		return luaL_error(L, "popup failed due to unknown results involving xmessage (%d)", r);
	}

use_console:
#endif

	// All else has failed
	if (strcmp(type, "ok") == 0)
		t = "";
	else if (strcmp(type, "yesno") == 0)
		t = "yn";
	else if (strcmp(type, "yesnocancel") == 0)
		t = "ync";
	else
		return luaL_error(L, "invalid popup type \"%s\"", type);

	fprintf(stderr, "Lua Message: %s\n", message);

	while (true)
	{
		char buffer[64];

		// We don't want parameters
		if (!t[0])
		{
			fprintf(stderr, "[Press Enter]");
			fgets(buffer, sizeof(buffer), stdin);

			// We're done
			return 0;
		}

		fprintf(stderr, "(%s): ", t);
		fgets(buffer, sizeof(buffer), stdin);

		// Check if the option is in the list
		if (strchr(t, tolower(buffer[0])))
		{
			switch (tolower(buffer[0]))
			{
			case 'y':
				lua_pushstring(L, "yes");
				return 1;
			case 'n':
				lua_pushstring(L, "no");
				return 1;
			case 'c':
				lua_pushstring(L, "cancel");
				return 1;
			default:
				luaL_error(L, "internal logic error in console based prompts for gui.popup");
			}
		}

		// We fell through, so we assume the user answered wrong and prompt again.
	}

	// Nothing here, since the only way out is in the loop.
#endif
}

#if (defined(WIN32) && !defined(SDL))
const char  *s_keyToName[256] =
{
	NULL,
	"leftclick",
	"rightclick",
	NULL,
	"middleclick",
	NULL,
	NULL,
	NULL,
	"backspace",
	"tab",
	NULL,
	NULL,
	NULL,
	"enter",
	NULL,
	NULL,
	"shift",	   // 0x10
	"control",
	"alt",
	"pause",
	"capslock",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"escape",
	NULL,
	NULL,
	NULL,
	NULL,
	"space",	   // 0x20
	"pageup",
	"pagedown",
	"end",
	"home",
	"left",
	"up",
	"right",
	"down",
	NULL,
	NULL,
	NULL,
	NULL,
	"insert",
	"delete",
	NULL,
	"0",		   "1",			   "2",			   "3",			   "4",		   "5",		  "6",		 "7",		"8",	   "9",
	NULL,		   NULL,		   NULL,		   NULL,		   NULL,	   NULL,	  NULL,
	"A",		   "B",			   "C",			   "D",			   "E",		   "F",		  "G",		 "H",		"I",	   "J",
	"K",		   "L",			   "M",			   "N",			   "O",		   "P",		  "Q",		 "R",		"S",	   "T",
	"U",		   "V",			   "W",			   "X",			   "Y",		   "Z",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"numpad0",	   "numpad1",	   "numpad2",	   "numpad3",	   "numpad4",  "numpad5", "numpad6", "numpad7", "numpad8", "numpad9",
	"numpad*",	   "numpad+",
	NULL,
	"numpad-",	   "numpad.",	   "numpad/",
	"F1",		   "F2",		   "F3",		   "F4",		   "F5",	   "F6",	  "F7",		 "F8",		"F9",	   "F10",	  "F11",
	"F12",
	"F13",		   "F14",		   "F15",		   "F16",		   "F17",	   "F18",	  "F19",	 "F20",		"F21",	   "F22",	  "F23",
	"F24",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"numlock",
	"scrolllock",
	NULL,		  // 0x92
	NULL,		   NULL,		   NULL,		   NULL,		   NULL,	   NULL,	  NULL,		 NULL,		NULL,	   NULL,	  NULL,
	NULL,		   NULL,		   NULL,		   NULL,		   NULL,	   NULL,	  NULL,		 NULL,		NULL,	   NULL,
	NULL,		   NULL,		   NULL,		   NULL,		   NULL,	   NULL,	  NULL,		 NULL,		NULL,
	NULL,		   NULL,		   NULL,		   NULL,		   NULL,	   NULL,	  NULL,		 NULL,
	NULL,		  // 0xB9
	"semicolon",
	"plus",
	"comma",
	"minus",
	"period",
	"slash",
	"tilde",
	NULL,		  // 0xC1
	NULL,		   NULL,		   NULL,		   NULL,		   NULL,	   NULL,	  NULL,		 NULL,		NULL,
	NULL,		   NULL,		   NULL,		   NULL,		   NULL,	   NULL,	  NULL,		 NULL,
	NULL,		   NULL,		   NULL,		   NULL,		   NULL,	   NULL,	  NULL,
	NULL,		  // 0xDA
	"leftbracket",
	"backslash",
	"rightbracket",
	"quote",
};
#endif

// input.get()
// takes no input, returns a lua table of entries representing the current input state,
// independent of the joypad buttons the emulated game thinks are pressed
// for example:
//   if the user is holding the W key and the left mouse button
//   and has the mouse at the bottom-right corner of the game screen,

//   then this would return {W=true, leftclick=true, xmouse=255, ymouse=223}
static int input_getcurrentinputstatus(lua_State *L)
{
	lua_newtable(L);

#if (defined(WIN32) && !defined(SDL))
	// keyboard and mouse button status
	{
		unsigned char keys[256];
		if (true /*!GUI.BackgroundInput*/) // TODO: background input
		{
			if (GetKeyboardState(keys))
			{
				for (int i = 1; i < 255; i++)
				{
					int mask = (i == VK_CAPITAL || i == VK_NUMLOCK || i == VK_SCROLL) ? 0x01 : 0x80;
					if (keys[i] & mask)
					{
						const char *name = s_keyToName[i];
						if (name)
						{
							lua_pushboolean(L, true);
							lua_setfield(L, -2, name);
						}
					}
				}
			}
		}
		else // use a slightly different method that will detect background input:
		{
			for (int i = 1; i < 255; i++)
			{
				const char *name = s_keyToName[i];
				if (name)
				{
					int active;
					if (i == VK_CAPITAL || i == VK_NUMLOCK || i == VK_SCROLL)
						active = GetKeyState(i) & 0x01;
					else
						active = GetAsyncKeyState(i) & 0x8000;
					if (active)
					{
						lua_pushboolean(L, true);
						lua_setfield(L, -2, name);
					}
				}
			}
		}
	}

	// mouse position in game screen pixel coordinates
	{
		int mouseClientX = 0, mouseClientY = 0;
		lua_host::getMouseClientPos(mouseClientX, mouseClientY);

		int width, height;
		int xofs, yofs;
		systemGetLCDResolution(width, height);
		systemGetLCDBaseOffset(xofs, yofs);

		// game screen is always fully stretched to window size,
		// with no aspect rate correction, or something like that.
		int wndWidth  = 0;
		int wndHeight = 0;
		lua_host::getClientSize(wndWidth, wndHeight);

		long mouseX = (long) (mouseClientX * ((float)width / wndWidth)) - xofs;
		long mouseY = (long) (mouseClientY * ((float)height / wndHeight)) - yofs;

		lua_pushinteger(L, mouseX);
		lua_setfield(L, -2, "xmouse");
		lua_pushinteger(L, mouseY);
		lua_setfield(L, -2, "ymouse");
	}

#else
	// NYI (well, return an empty table)
#endif
	return 1;
}

static int avi_framecount(lua_State *L)
{
	lua_pushinteger(L, lua_host::aviActive() ? lua_host::aviVideoFrames() : 0);
	return 1;
}

static int avi_pause(lua_State *L)
{
	if (lua_host::aviActive())
		lua_host::aviPause(true);
	return 1;
}

static int avi_resume(lua_State *L)
{
	if (lua_host::aviActive())
		lua_host::aviPause(false);
	return 1;
}

static int sound_get(lua_State *L)
{
	extern int32 soundLevel1;
	extern int32 soundLevel2;
	extern int32 soundBalance;
	extern int32 soundMasterOn;
	extern int32 soundVIN;
	extern int32 sound1On;
	extern int32 sound1EnvelopeVolume;
	extern int32 sound2On;
	extern int32 sound2EnvelopeVolume;
	extern int32 sound3On;
	extern int32 sound3OutputLevel;
	extern int32 sound3Bank;
	extern int32 sound3DataSize;
	extern int32 sound3ForcedOutput;
	extern int32 sound4On;
	extern int32 sound4EnvelopeVolume;
	extern u8 sound3WaveRam[0x20];

	int freqReg;
	double freq;
	double leftvolscale;
	double rightvolscale;
	double panpot;
	bool gba = systemIsRunningGBA();
	u8* gbMem = gba ? ioMem : gbMemory;
	const int rNR10 = gba ? 0x60 : 0xff10;
	const int rNR11 = gba ? 0x62 : 0xff11;
	const int rNR12 = gba ? 0x63 : 0xff12;
	const int rNR13 = gba ? 0x64 : 0xff13;
	const int rNR14 = gba ? 0x65 : 0xff14;
	const int rNR21 = gba ? 0x68 : 0xff16;
	const int rNR22 = gba ? 0x69 : 0xff17;
	const int rNR23 = gba ? 0x6c : 0xff18;
	const int rNR24 = gba ? 0x6d : 0xff19;
	const int rNR30 = gba ? 0x70 : 0xff1a;
	const int rNR31 = gba ? 0x72 : 0xff1b;
	const int rNR32 = gba ? 0x73 : 0xff1c;
	const int rNR33 = gba ? 0x74 : 0xff1d;
	const int rNR34 = gba ? 0x75 : 0xff1e;
	const int rNR41 = gba ? 0x78 : 0xff20;
	const int rNR42 = gba ? 0x79 : 0xff21;
	const int rNR43 = gba ? 0x7c : 0xff22;
	const int rNR44 = gba ? 0x7d : 0xff23;
	const int rNR50 = gba ? 0x80 : 0xff24;
	const int rNR51 = gba ? 0x81 : 0xff25;
	const int rNR52 = gba ? 0x84 : 0xff26;
	const int rWAVE_RAM = gba ? 0x90 : 0xff30;

	const int32 _soundVIN = 0x88; // gba ? 0x88 : soundVIN;
	const bool soundVINLeft = ((_soundVIN & 0x80) != 0);
	const bool soundVINRight = ((_soundVIN & 0x08) != 0);

	lua_newtable(L);

	// square1
	lua_newtable(L);
	if(sound1On == 0 || soundMasterOn == 0)
	{
		lua_pushnumber(L, 0.0);
		panpot = 0.5;
	}
	else
	{
		double envVolume = sound1EnvelopeVolume / 15.0;
		if (soundVINLeft && (soundBalance & 0x10) != 0)
			leftvolscale = ((soundLevel2 / 7.0) * envVolume);
		else
			leftvolscale = 0.0;
		if (soundVINRight && (soundBalance & 0x01) != 0)
			rightvolscale = ((soundLevel1 / 7.0) * envVolume);
		else
			rightvolscale = 0.0;
		if ((leftvolscale + rightvolscale) != 0)
			panpot = rightvolscale / (leftvolscale + rightvolscale);
		else
			panpot = 0.5;
		lua_pushnumber(L, (leftvolscale + rightvolscale) / 2.0);
	}
	lua_setfield(L, -2, "volume");
	lua_pushnumber(L, panpot);
	lua_setfield(L, -2, "panpot");
	freqReg = (((int)(gbMem[rNR14] & 7) << 8) | gbMem[rNR13]);
	freq = 131072.0 / (2048 - freqReg);
	lua_pushnumber(L, freq);
	lua_setfield(L, -2, "frequency");
	lua_pushnumber(L, (log(freq / 440.0) * 12 / log(2.0)) + 69);
	lua_setfield(L, -2, "midikey");
	lua_pushinteger(L, (gbMem[rNR11] & 0xC0) >> 6);
	lua_setfield(L, -2, "duty");
	lua_newtable(L);
	lua_pushinteger(L, freqReg);
	lua_setfield(L, -2, "frequency");
	lua_setfield(L, -2, "regs");
	lua_setfield(L, -2, "square1");
	// square2
	lua_newtable(L);
	if(sound2On == 0 || soundMasterOn == 0)
	{
		lua_pushnumber(L, 0.0);
		panpot = 0.5;
	}
	else
	{
		double envVolume = sound2EnvelopeVolume / 15.0;
		if (soundVINLeft && (soundBalance & 0x20) != 0)
			leftvolscale = ((soundLevel2 / 7.0) * envVolume);
		else
			leftvolscale = 0.0;
		if (soundVINRight && (soundBalance & 0x02) != 0)
			rightvolscale = ((soundLevel1 / 7.0) * envVolume);
		else
			rightvolscale = 0.0;
		if ((leftvolscale + rightvolscale) != 0)
			panpot = rightvolscale / (leftvolscale + rightvolscale);
		else
			panpot = 0.5;
		lua_pushnumber(L, (leftvolscale + rightvolscale) / 2.0);
	}
	lua_setfield(L, -2, "volume");
	lua_pushnumber(L, panpot);
	lua_setfield(L, -2, "panpot");
	freqReg = (((int)(gbMem[rNR24] & 7) << 8) | gbMem[rNR23]);
	freq = 131072.0 / (2048 - freqReg);
	lua_pushnumber(L, freq);
	lua_setfield(L, -2, "frequency");
	lua_pushnumber(L, (log(freq / 440.0) * 12 / log(2.0)) + 69);
	lua_setfield(L, -2, "midikey");
	lua_pushinteger(L, (gbMem[rNR21] & 0xC0) >> 6);
	lua_setfield(L, -2, "duty");
	lua_newtable(L);
	lua_pushinteger(L, freqReg);
	lua_setfield(L, -2, "frequency");
	lua_setfield(L, -2, "regs");
	lua_setfield(L, -2, "square2");
	// wavememory
	lua_newtable(L);
	if(sound3On == 0 || soundMasterOn == 0)
	{
		lua_pushnumber(L, 0.0);
		panpot = 0.5;
	}
	else
	{
		double envVolume;
		if (gba && sound3ForcedOutput != 0)
			envVolume = 0.75;
		else
		{
			double volTable[4] = { 0.0, 1.0, 0.5, 0.25 };
			envVolume = volTable[sound3OutputLevel & 3];
		}

		if (soundVINLeft && (soundBalance & 0x40) != 0)
			leftvolscale = ((soundLevel2 / 7.0) * envVolume);
		else
			leftvolscale = 0.0;
		if (soundVINRight && (soundBalance & 0x04) != 0)
			rightvolscale = ((soundLevel1 / 7.0) * envVolume);
		else
			rightvolscale = 0.0;
		if ((leftvolscale + rightvolscale) != 0)
			panpot = rightvolscale / (leftvolscale + rightvolscale);
		else
			panpot = 0.5;
		lua_pushnumber(L, (leftvolscale + rightvolscale) / 2.0);
	}
	lua_setfield(L, -2, "volume");
	lua_pushnumber(L, panpot);
	lua_setfield(L, -2, "panpot");
	int waveMemSamples = 32;
	if (gba)
	{
		lua_pushlstring(L, (const char *) &sound3WaveRam[sound3Bank * 0x10], sound3DataSize ? 0x20 : 0x10);
		waveMemSamples = sound3DataSize ? 64 : 32;
	}
	else
	{
		lua_pushlstring(L, (const char *) &gbMem[rWAVE_RAM], 0x10);
	}
	lua_setfield(L, -2, "waveform");
	freqReg = (((int)(gbMem[rNR34] & 7) << 8) | gbMem[rNR33]);
	freq = 2097152.0 / (waveMemSamples * (2048 - freqReg));
	lua_pushnumber(L, freq);
	lua_setfield(L, -2, "frequency");
	lua_pushnumber(L, (log(freq / 440.0) * 12 / log(2.0)) + 69);
	lua_setfield(L, -2, "midikey");
	lua_newtable(L);
	lua_pushinteger(L, freqReg);
	lua_setfield(L, -2, "frequency");
	lua_setfield(L, -2, "regs");
	lua_setfield(L, -2, "wavememory");
	// noise
	lua_newtable(L);
	if(sound4On == 0 || soundMasterOn == 0)
	{
		lua_pushnumber(L, 0.0);
		panpot = 0.5;
	}
	else
	{
		double envVolume = sound4EnvelopeVolume / 15.0;
		if (soundVINLeft && (soundBalance & 0x80) != 0)
			leftvolscale = ((soundLevel2 / 7.0) * envVolume);
		else
			leftvolscale = 0.0;
		if (soundVINRight && (soundBalance & 0x08) != 0)
			rightvolscale = ((soundLevel1 / 7.0) * envVolume);
		else
			rightvolscale = 0.0;
		if ((leftvolscale + rightvolscale) != 0)
			panpot = rightvolscale / (leftvolscale + rightvolscale);
		else
			panpot = 0.5;
		lua_pushnumber(L, (leftvolscale + rightvolscale) / 2.0);
	}
	lua_setfield(L, -2, "volume");
	lua_pushnumber(L, panpot);
	lua_setfield(L, -2, "panpot");
	const int gbNoiseFreqTable[8] = { 1, 2, 4, 6, 8, 10, 12, 14 };
	freqReg = gbNoiseFreqTable[gbMem[rNR43] & 7] << (1 + (gbMem[rNR43] >> 4));
	lua_pushboolean(L, (gbMem[rNR43] & 8) != 0);
	lua_setfield(L, -2, "short");
	freq = 1048576.0 / freqReg;
	lua_pushnumber(L, freq);
	lua_setfield(L, -2, "frequency");
	lua_pushnumber(L, (log(freq / 440.0) * 12 / log(2.0)) + 69);
	lua_setfield(L, -2, "midikey");
	lua_newtable(L);
	lua_pushinteger(L, freqReg);
	lua_setfield(L, -2, "frequency");
	lua_setfield(L, -2, "regs");
	lua_setfield(L, -2, "noise");

	return 1;
}

// math.random()/math.randomseed() are replaced rather than extended, so that a
// script gets the same numbers whichever lua51.dll is in place: stock Lua 5.1
// builds both on the C library's rand(), which hands out 15 bits on MSVC
// (RAND_MAX is 32767) - math.random(1, 100000) can only ever return one of
// 32767 values - while LuaJIT has a good generator of its own, but a different
// one.  SFMT gives 32 bits out of a period of 2^19937-1, and its state lives in
// the script (VbaLuaState::rng), so a math.randomseed() in one script cannot
// shift another script's sequence, nor another script's gui "rand" colour.
static int sfmt_random(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);
	lua_Number r = state ? (lua_Number) sfmt_genrand_real2(&state->rng) : 0.0;
	switch (lua_gettop(L))
	{ // check number of arguments
	case 0:
		{ // no arguments
			lua_pushnumber(L, r); // Number between 0 and 1
			break;
		}

	case 1:
		{ // only upper limit
			int u = luaL_checkint(L, 1);
			luaL_argcheck(L, 1 <= u, 1, "interval is empty");
			lua_pushnumber(L, floor(r * u) + 1); // int between 1 and `u'
			break;
		}

	case 2:
		{ // lower and upper limits
			int l = luaL_checkint(L, 1);
			int u = luaL_checkint(L, 2);
			luaL_argcheck(L, l <= u, 2, "interval is empty");
			lua_pushnumber(L, floor(r * (u - l + 1)) + l); // int between `l' and `u'
			break;
		}

	default:
		return luaL_error(L, "wrong number of arguments");
	}

	return 1;
}

// same as math.randomseed, but seeds SFMT instead of the C library's generator
static int sfmt_randomseed(lua_State *L)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);
	if (state)
		sfmt_init_gen_rand(&state->rng, (uint32_t)luaL_checkint(L, 1));
	return 0;
}

// The old global bit functions.  AND, OR, XOR, SHIFT and BIT are what the VBA
// documentation lists and what existing scripts call, so they stay; the ported
// LuaBitOp that used to live here does not, because the LuaJIT this fork ships
// brings the `bit` library itself.  These five are plain C now, so they work
// with a stock Lua 5.1 lua51.dll as well, which has no `bit` at all.
//
// The conversion rule is the one LuaBitOp used: a number is truncated and
// wrapped to 32 bits (-1 and 0xFFFFFFFF mean the same thing), and a result with
// bit 31 set comes back as a negative number.
static uint32 luaToU32(lua_State *L, int idx)
{
	lua_Number n	   = luaL_checknumber(L, idx);
	lua_Number wrapped = fmod(trunc(n), 4294967296.0);
	if (wrapped < 0)
		wrapped += 4294967296.0;
	return (uint32)wrapped;
}

static int pushU32(lua_State *L, uint32 value)
{
	lua_pushnumber(L, value <= 0x7FFFFFFFU ? (lua_Number)value
										   : (lua_Number)value - 4294967296.0);
	return 1;
}

static int bits_and(lua_State *L)
{
	uint32 bits = luaToU32(L, 1);
	for (int i = lua_gettop(L); i > 1; --i)
		bits &= luaToU32(L, i);
	return pushU32(L, bits);
}

static int bits_or(lua_State *L)
{
	uint32 bits = luaToU32(L, 1);
	for (int i = lua_gettop(L); i > 1; --i)
		bits |= luaToU32(L, i);
	return pushU32(L, bits);
}

static int bits_xor(lua_State *L)
{
	uint32 bits = luaToU32(L, 1);
	for (int i = lua_gettop(L); i > 1; --i)
		bits ^= luaToU32(L, i);
	return pushU32(L, bits);
}

// SHIFT(value, count): left for a positive count, right for a negative one.
static int bit_bshift_emulua(lua_State *L)
{
	uint32 value = luaToU32(L, 1);
	int	   shift = (int)luaL_checkinteger(L, 2);

	if (shift < 0)
		return pushU32(L, value << ((-shift) & 31));
	return pushU32(L, value >> (shift & 31));
}

// BIT(bit, ...): a number with the given bits set; 0-31, anything else ignored.
static int bitbit(lua_State *L)
{
	uint32 bits		= 0;
	int	   numArgs	= lua_gettop(L);
	for (int i = 1; i <= numArgs; i++)
	{
		int where = (int)luaL_checkinteger(L, i);
		if (where >= 0 && where < 32)
			bits |= 1u << where;
	}
	lua_settop(L, 0);
	return pushU32(L, bits);
}

// The function called periodically to ensure Lua doesn't run amok.
static void VBALuaHookFunction(lua_State *L, lua_Debug *dbg)
{
	VbaLuaState *state = VbaLuaState::fromLua(L);

	if (!state)
		return;

	if (state->numTries-- == 0)
	{
		int kill = 0;

#if (defined(WIN32) && !defined(SDL))
		// Uh oh
		lua_host::checkFullscreen();
		systemSoundClearBuffer();
		lua_host::MsgBoxResult ret = lua_host::messageBox(
			"The Lua script running has been running a long time. It may have gone crazy. Keep it running?\n\n"
			"(Abort = Kill; Retry = Wait for a moment; Ignore = Keep running and don't prompt anymore)",
			"Lua Script Gone Nuts?",
			lua_host::MsgBoxType::AbortRetryIgnore);

		if (ret == lua_host::MsgBoxResult::Abort)
		{
			kill = 1;
		}
		else if (ret == lua_host::MsgBoxResult::Retry)
		{
			state->numTries = 3000;
		}

#else
		fprintf(
			stderr,
			"The Lua script running has been running a long time.\nIt may have gone crazy. Kill it?\n");

		char buffer[64];
		while (true)
		{
			fprintf(stderr, "('Y' = Yes, 'L' = Ask again later, 'N' = No, no more asking): ");
			fgets(buffer, sizeof(buffer), stdin);
			if (buffer[0] == 'y' || buffer[0] == 'Y')
			{
				kill = 1;
				break;
			}

			if (buffer[0] == 'l' || buffer[0] == 'L')
			{
				state->numTries = 3000;
				break;
			}

			if (buffer[0] == 'n' || buffer[0] == 'N')
				break;
		}
#endif
		if (kill == 1)
		{
			luaL_error(L, "Killed by user request.");
			state->resetAfterStop();
		}

		// kill the debug hook if answered no.
		if (state->numTries <= 0)
			lua_sethook(L, NULL, 0, 0);
	}
}

static const struct luaL_Reg vbalib[] = {
	//	{"speedmode", vba_speedmode},	// TODO: NYI
	{ "frameadvance",	vba_frameadvance	 },
	{ "pause",			vba_pause			 },
	{ "framecount",		vba_framecount		 },
	{ "lagcount",		vba_getlagcount		 },
	{ "lagged",			vba_lagged			 },
	{ "emulating",		vba_emulating		 },
	{ "registerbefore", vba_registerbefore	 },
	{ "registerafter",	vba_registerafter	 },
	{ "registerexit",	vba_registerexit	 },
	{ "registerrun",	vba_registerpoweron	 },
	{ "registerclose",	vba_registerpoweroff },
	{ "registerloading",vba_registerloading	 },
	{ "registerloaded",	vba_registerloaded	 },
	{ "registersaving",	vba_registersaving	 },
	{ "registersaved",	vba_registersaved	 },
	{ "message",		vba_message			 },
	{ "remapinputdisplay",	vba_remapinputdisplay	},
	{ "screenshot",     vba_screenshot       },
	{ "print",			print				 }, // sure, why not
	{ NULL,				NULL				 }
};

static const struct luaL_Reg memorylib[] = {
	{ "readbyte",				memory_readbyte				},
	{ "readbytesigned",			memory_readbytesigned		},
	{ "readword",				memory_readword				},
	{ "readwordsigned",			memory_readwordsigned		},
	{ "readdword",				memory_readdword			},
	{ "readdwordsigned",		memory_readdwordsigned		},
	{ "readbyterange",			memory_readbyterange		},
	{ "writebyte",				memory_writebyte			},
	{ "writeword",				memory_writeword			},
	{ "writedword",				memory_writedword			},
	{ "getregister",			memory_getregister			},
	{ "setregister",			memory_setregister			},
	{ "gbromreadbyte",			memory_gbromreadbyte		},
	{ "gbromreadbytesigned",	memory_gbromreadbytesigned	},
	{ "gbromreadword",			memory_gbromreadword		},
	{ "gbromreadwordsigned",	memory_gbromreadwordsigned	},
	{ "gbromreaddword",			memory_gbromreaddword		},
	{ "gbromreaddwordsigned",	memory_gbromreaddwordsigned	},
	{ "gbromreadbyterange",		memory_gbromreadbyterange	},

	// alternate naming scheme for word and double-word and unsigned
	{ "readbyteunsigned",		memory_readbyte				},
	{ "readwordunsigned",		memory_readword				},
	{ "readdwordunsigned",		memory_readdword			},
	{ "readshort",				memory_readword				},
	{ "readshortunsigned",		memory_readword				},
	{ "readshortsigned",		memory_readwordsigned		},
	{ "readlong",				memory_readdword			},
	{ "readlongunsigned",		memory_readdword			},
	{ "readlongsigned",			memory_readdwordsigned		},
	{ "writeshort",				memory_writeword			},
	{ "writelong",				memory_writedword			},
	{ "gbromreadbyteunsigned",	memory_gbromreadbyte		},
	{ "gbromreadwordunsigned",	memory_gbromreadword		},
	{ "gbromreaddwordunsigned",	memory_gbromreaddword		},
	{ "gbromreadshort",			memory_gbromreadword		},
	{ "gbromreadshortunsigned",	memory_gbromreadword		},
	{ "gbromreadshortsigned",	memory_gbromreadwordsigned	},
	{ "gbromreadlong",			memory_gbromreaddword		},
	{ "gbromreadlongunsigned",	memory_gbromreaddword		},
	{ "gbromreadlongsigned",	memory_gbromreaddwordsigned	},

	// memory hooks
	{ "registerwrite",	   memory_registerwrite			 },
	{ "registerread",	   memory_registerread			 },
	{ "registerexec",	   memory_registerexec			 },
	// alternate names
	{ "register",		   memory_registerwrite			 },
	{ "registerrun",	   memory_registerexec			 },
	{ "registerexecute",   memory_registerexec			 },

	{ NULL,				   NULL							 }
};

static const struct luaL_Reg joypadlib[] = {
	{ "get",	  joypad_get	  },
	{ "getdown",  joypad_getdown  },
	{ "getup",	  joypad_getup	  },
	{ "set",	  joypad_set	  },

	// alternative names
	{ "read",	  joypad_get	  },
	{ "write",	  joypad_set	  },
	{ "readdown", joypad_getdown  },
	{ "readup",	  joypad_getup	  },
	{ NULL,		  NULL			  }
};

static const struct luaL_Reg savestatelib[] = {
	{ "create", savestate_create },
	{ "save",	savestate_save	 },
	{ "load",	savestate_load	 },

	{ NULL,		NULL			 }
};

static const struct luaL_Reg movielib[] = {
	{ "active",			  movie_isactive				},
	{ "recording",		  movie_isrecording				},
	{ "playing",		  movie_isplaying				},
	{ "mode",			  movie_getmode					},

	{ "length",			  movie_getlength				},
	{ "author",			  movie_getauthor				},
	{ "name",			  movie_getfilename				},
	{ "rerecordcount",	  movie_rerecordcount			},
	{ "setrerecordcount", movie_setrerecordcount		},

	{ "rerecordcounting", movie_rerecordcounting		},
	{ "framecount",		  vba_framecount				}, // for those familiar with
															// other emulators that have
															// movie.framecount()
															// instead of
															// emulatorname.framecount()

	{ "stop",			  movie_stop					},

	// alternative names
	{ "close",			  movie_stop					},
	{ "getauthor",		  movie_getauthor				},
	{ "getname",		  movie_getfilename				},
	{ NULL,				  NULL							}
};

static const struct luaL_Reg guilib[] = {
	{ "register",	  gui_register		   },
	{ "text",		  gui_text			   },
	{ "box",		  gui_drawbox		   },
	{ "line",		  gui_drawline		   },
	{ "pixel",		  gui_drawpixel		   },
	{ "opacity",	  gui_setopacity	   },
	{ "transparency", gui_transparency	   },
	{ "popup",		  gui_popup			   },
	{ "parsecolor",	  gui_parsecolor	   },
	{ "gdscreenshot", gui_gdscreenshot	   },
	{ "gdoverlay",	  gui_gdoverlay		   },
	{ "getpixel",	  gui_getpixel		   },

	// alternative names
	{ "drawtext",	  gui_text			   },
	{ "drawbox",	  gui_drawbox		   },
	{ "drawline",	  gui_drawline		   },
	{ "drawpixel",	  gui_drawpixel		   },
	{ "setpixel",	  gui_drawpixel		   },
	{ "writepixel",	  gui_drawpixel		   },
	{ "rect",		  gui_drawbox		   },
	{ "drawrect",	  gui_drawbox		   },
	{ "drawimage",	  gui_gdoverlay		   },
	{ "image",		  gui_gdoverlay		   },
	{ "readpixel",	  gui_getpixel		   },
	{ NULL,			  NULL				   }
};

static const struct luaL_Reg inputlib[] = {
	{ "get",  input_getcurrentinputstatus  },

	// alternative names
	{ "read", input_getcurrentinputstatus  },
	{ NULL,	  NULL						   }
};

static const struct luaL_Reg soundlib[] = {
	{ "get",  sound_get					   },

	// alternative names
	{ NULL,	  NULL						   }
};

// gocha: since vba dumps avi so badly,
// I add avilib as a workaround for enhanced video encoding.
static const struct luaL_Reg avilib[] = {
	{ "framecount", avi_framecount },
	{ "pause",		avi_pause	   },
	{ "resume",		avi_resume	   },
	{ NULL,			NULL		   }
};

void VbaLuaState::callExitFunction(void)
{
	if (!L)
		return;

	lua_settop(L, 0);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEXIT]);

	int errorcode = 0;
	if (lua_isfunction(L, -1))
	{
		errorcode = lua_pcall(L, 0, 0, 0);
	}

	if (errorcode)
		HandleCallbackError(L);
}

void CallExitFunction(void)
{
	for (size_t i = 0; i < g_luaStates.size(); ++i)
		g_luaStates[i]->callExitFunction();
}

void VbaLuaState::frameBoundaryStep(void)
{
	//	printf("Lua Frame\n");

	if (!L || !running)
		return;

	// Our function needs calling
	lua_settop(L, 0);
	lua_getfield(L, LUA_REGISTRYINDEX, frameAdvanceThread);

	lua_State *thread = lua_tothread(L, 1);

	if (!thread)
	{
		// No coroutine left (the script died earlier); nothing to resume.
		lua_settop(L, 0);
		resetAfterStop();
		return;
	}

	// Lua calling C must know that we're busy inside a frame boundary
	frameBoundary		= true;
	frameAdvanceWaiting = false;

	numTries = 1000;

	int result = lua_resume(thread, 0);

	if (result == LUA_YIELD)
	{
		// Okay, we're fine with that.
	}
	else if (result != 0)
	{
		// Done execution by bad causes
		resetAfterStop();
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, frameAdvanceThread);
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, guiCallbackTable);

		// Error?
		printerror(thread, -1);
	}
	else
	{
		resetAfterStop();
		printf("Script died of natural causes.\n");
	}

	// Past here, VBA actually runs, so any Lua code is called mid-frame. We must
	// not do anything too stupid, so let ourselves know.
	frameBoundary = false;

	if (!frameAdvanceWaiting)
	{
		resetAfterStop();
	}
}

void VBALuaFrameBoundary(void)
{
	// A new frame: nobody has claimed a joypad yet.  This has to happen for
	// every script, including the ones that are stopped, or a script that died
	// mid-frame would keep holding buttons down.
	for (size_t i = 0; i < g_luaStates.size(); ++i)
		g_luaStates[i]->joypadUsed = 0;

	for (size_t i = 0; i < g_luaStates.size(); ++i)
		g_luaStates[i]->frameBoundaryStep();
}

// ---------------------------------------------------------------------------
// Script text encoding
//
// A script's strings reach a Unicode control as bytes, so the console can only
// decode them if it knows what encoding the .lua file was written in.  A BOM is
// the one place that answer is stated: UTF-8 with a BOM is read as UTF-8, and
// everything else is read in the process code page, which is what this program
// assumes for every other narrow string as well.
//
// Guessing is deliberately not attempted.  A .lua file has no other marker and
// the bytes alone cannot decide the question - "中文" in GBK is a legal UTF-8
// sequence too - so a heuristic would turn some scripts into garbage instead of
// leaving them exactly as they behaved before UTF-8 was supported.
// ---------------------------------------------------------------------------

static bool readWholeFile(const char *filename, std::string &bytes)
{
	FILE *f = fopen(filename, "rb");
	if (!f)
		return false;

	char   buffer[8192];
	size_t got;
	while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0)
		bytes.append(buffer, got);

	bool ok = (ferror(f) == 0);
	fclose(f);
	return ok;
}

// The only declaration of a script's encoding there is: a UTF-8 BOM.  The
// loader drops it, because Lua 5.1 does not skip one and would try to parse it
// as source text.
static bool hasUtf8Bom(const std::string &bytes)
{
	return bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
		   (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF;
}

// luaL_loadfile() is Lua 5.1's: it skips a leading '#' line but has no idea
// what a BOM is, so a UTF-8 file written by an editor that adds one fails with
// "unexpected symbol near '\239'".  Such a file is loaded from the bytes read
// above, with the BOM dropped.
static int loadScriptFile(lua_State *L, const char *filename, const std::string *bytesWithBom)
{
	if (!bytesWithBom)
		return luaL_loadfile(L, filename);

	const char *text = bytesWithBom->c_str() + 3;
	size_t	    size = bytesWithBom->size() - 3;

	// Same as luaL_loadfile: a first line starting with '#' is a comment.
	if (size > 0 && text[0] == '#')
	{
		while (size > 0 && *text != '\n')
		{
			++text;
			--size;
		}
	}

	// '@' makes Lua report errors against the file name, which is exactly what
	// luaL_loadfile() passes to luaL_loadbuffer() as well.
	lua_pushfstring(L, "@%s", filename);
	std::string chunkName = lua_tostring(L, -1);
	lua_pop(L, 1);

	return luaL_loadbuffer(L, text, size, chunkName.c_str());
}

/**
* Loads and runs the given Lua script into this state.
* The emulator MUST be paused for this function to be
* called. Otherwise, all frame boundary assumptions go out the window.
*
* Returns true on success, false on failure.  The reason travels through the
* console sinks, so a broken file is reported in the window that asked for it.
*/
bool VbaLuaState::load(const char *filename)
{
	if (!DemandLua())
	{
		return false;
	}

	// Restarting a script: close the old engine first.
	if (L)
		stop();

	scriptName = filename;

	// Set current directory from filename (for dofile)
	char  dir[_MAX_PATH];
	char *slash, *backslash;
	strcpy(dir, filename);
	slash = strrchr(dir, '/');
	backslash = strrchr(dir, '\\');
	if (!slash || (backslash && backslash < slash))
		slash = backslash;
	if (slash)
	{
		slash[1] = '\0'; // keep slash itself for some reasons
		chdir(dir);
	}

	if (!L)
	{
		L = lua_open();
		luaL_openlibs(L);

		luaL_register(L, "emu", vbalib); // added for better cross-emulator compatibility
		luaL_register(L, "vba", vbalib); // kept for backward compatibility
		luaL_register(L, "memory", memorylib);
		luaL_register(L, "joypad", joypadlib);
		luaL_register(L, "savestate", savestatelib);
		luaL_register(L, "movie", movielib);
		luaL_register(L, "gui", guilib);
		luaL_register(L, "input", inputlib);
		luaL_register(L, "sound", soundlib);
		luaL_register(L, "avi", avilib); // workaround for enhanced video encoding
		lua_settop(L, 0); // clean the stack, because each call to luaL_register leaves a table on top

		// register a few utility functions outside of libraries (in the global namespace)
		lua_register(L, "print", print);
		lua_register(L, "tostring", tostring);
		lua_register(L, "addressof", addressof);
		lua_register(L, "copytable", copytable);

		// The old spellings of the bit operations.  `bit` itself comes from the
		// engine: LuaJIT opens it with the standard libraries, and a stock Lua
		// 5.1 lua51.dll simply has none.
		lua_register(L, "AND", bits_and);
		lua_register(L, "OR", bits_or);
		lua_register(L, "XOR", bits_xor);
		lua_register(L, "SHIFT", bit_bshift_emulua);
		lua_register(L, "BIT", bitbit);

		lua_pushstring(L, "math");
		lua_gettable(L, LUA_GLOBALSINDEX);
		lua_pushcfunction(L, sfmt_random);
		lua_setfield(L, -2, "random");
		lua_pushcfunction(L, sfmt_randomseed);
		lua_setfield(L, -2, "randomseed");
		lua_settop(L, 0);

		// push arrays for storing hook functions in
		for (int i = 0; i < LUAMEMHOOK_COUNT; i++)
		{
			lua_newtable(L);
			lua_setfield(L, LUA_REGISTRYINDEX, luaMemHookTypeStrings[i]);
		}

		// The registry knows which instance this engine belongs to; that is how
		// the C functions above find "their" script again (VbaLuaState::fromLua).
		lua_pushlightuserdata(L, &s_luaStateKey);
		lua_pushlightuserdata(L, this);
		lua_rawset(L, LUA_REGISTRYINDEX);
	}

	// The console exists before the file is read, not after: a file that cannot
	// be opened or does not compile is reported through info_print, and without
	// a console that report goes to stderr - which a windowed program has
	// nowhere to put, so a broken script looked exactly like a script that does
	// nothing.  The console must also survive the failure, so info_onstart()
	// (which clears it) is still only called once the script has loaded.
	ensureConsoleSinks();

	// Read the file once, before anything can be printed: its BOM decides both
	// how the script's strings have to be decoded and whether the loader can
	// use luaL_loadfile() at all.  A file that cannot be read is left to
	// luaL_loadfile(), which reports it the usual way.
	std::string scriptBytes;
	bool		hasBom = false;
	encoding = lua_host::TextEncoding::Ansi;
	if (readWholeFile(filename, scriptBytes) && hasUtf8Bom(scriptBytes))
	{
		hasBom	 = true;
		encoding = lua_host::TextEncoding::Utf8;
	}

	// We make our thread NOW because we want it at the bottom of the stack.
	// If all goes wrong, we let the garbage collector remove it.
	lua_State *thread = lua_newthread(L);

	// Load the data
	int result = loadScriptFile(L, filename, hasBom ? &scriptBytes : NULL);

	if (result)
	{
		// The message names the file and the line, or says that the file
		// could not be opened at all.
		printerror(L, -1);

		// Wipe the stack. Our thread
		lua_settop(L, 0);
		return false; // Oh shit.
	}

	// Get our function into it
	lua_xmove(L, thread, 1);

	// Save the thread to the registry. This is why I make the thread FIRST.
	lua_setfield(L, LUA_REGISTRYINDEX, frameAdvanceThread);

	// Initialize settings
	sfmt_init_gen_rand(&rng, (uint32_t)time(NULL));
	running			 = true;
	skipRerecords	 = false;
	memHooks		 = 0;
	transparency	 = 255; // opaque
	joypadUsed		 = 0; // not used
	for (int i = 0; i < 4; ++i)
	{
		inputRemap[i].from	   = 0;
		nextInputRemap[i].from = 0;
	}
	//wasPaused = systemIsPaused();
	//systemSetPause(false);

	// Set up our protection hook to be executed once every 10,000 bytecode instructions.
	lua_sethook(thread, VBALuaHookFunction, LUA_MASKCOUNT, 10000);

	if (info_onstart)
		info_onstart(console);

	// And run it right now. :)
	frameBoundaryStep();
	extern int textMethod;
	if (textMethod == 0)
	{
		int pitch = lua_host::filterWidth() * (systemColorDepth / 8) + (systemColorDepth == 24 ? 0 : 4);
		systemRenderLua(&pix[pitch], pitch);
	}

	// We're done.
	return true;
}

void VbaLuaState::stop(void)
{
	if (!L)
		return;

	//execute the user's shutdown callbacks
	callExitFunction();

	memHooks = 0;
	for (int i = 0; i < LUAMEMHOOK_COUNT; i++)
		CalculateMemHookRegions((LuaMemHookType)i);

	//sometimes iup uninitializes com
	//MBG TODO - test whether this is really necessary. i dont think it is
#if (defined(WIN32) && !defined(SDL))
	CoInitialize(0);
#endif

	if (info_onstop)
		info_onstop(console);

	//lua_gc(L,LUA_GCCOLLECT,0);
	lua_close(L); // this invokes our garbage collectors for us
	L = NULL;
	resetAfterStop();
}

// ---------------------------------------------------------------------------
// The public, per-script entry points the frontend uses.  One script window
// owns one VbaLuaState; nothing here touches any other script.
// ---------------------------------------------------------------------------
VbaLuaState *VBALuaOpenScript(const char *filename, std::intptr_t console)
{
	ensureConsoleSinks();

	VbaLuaState *state = new VbaLuaState();
	state->console	   = console;

	// In the list before it runs: anything the script registers on its first
	// frame (memory hooks, callbacks) must find it there.
	g_luaStates.push_back(state);

	if (!state->load(filename))
	{
		VBALuaCloseScript(state);
		return NULL;
	}

	return state;
}

void VBALuaStopScript(VbaLuaState *state)
{
	if (state)
		state->stop();
}

void VBALuaCloseScript(VbaLuaState *state)
{
	if (!state)
		return;

	state->stop();

	for (size_t i = 0; i < g_luaStates.size(); ++i)
	{
		if (g_luaStates[i] == state)
		{
			g_luaStates.erase(g_luaStates.begin() + i);
			break;
		}
	}

	delete state;
}

const char *VBALuaScriptName(VbaLuaState *state)
{
	return state ? state->scriptName.c_str() : NULL;
}

int VBALuaScriptRunning(VbaLuaState *state)
{
	return (state && state->L) ? 1 : 0;
}

/**
* Equivalent to repeating the last VBALoadLuaCode() call, for every script that
* has a name.  Scripts that fail to load keep their console message and are
* closed, exactly like a failed load from the window.
*/
int VBAReloadLuaCode(void)
{
	int reloaded = 0;

	// Copy the list: a failed reload closes its state, which edits g_luaStates.
	std::vector<VbaLuaState *> states = g_luaStates;
	for (size_t i = 0; i < states.size(); ++i)
	{
		VbaLuaState *state = states[i];
		if (state->scriptName.empty())
			continue;

		std::string name = state->scriptName;
		if (state->load(name.c_str()))
			reloaded++;
	}

	if (!reloaded)
		systemScreenMessage("There's no script to reload.");

	return reloaded;
}

/**
* Terminates every running Lua script.  The windows stay open; their Run button
* goes back to "Run" so the script can be started again.
*
* Always safe to call, except from within a lua call itself (duh).
*/
void VBALuaStop(void)
{
	// Copy: a stop can drop callbacks that close another window in theory.
	std::vector<VbaLuaState *> states = g_luaStates;
	for (size_t i = 0; i < states.size(); ++i)
		states[i]->stop();
}

/**
* Returns true if any script is loaded.
*
*/
int VBALuaRunning(void)
{
	for (size_t i = 0; i < g_luaStates.size(); ++i)
	{
		if (g_luaStates[i]->L)
			return 1;
	}

	return 0;
}

/**
* Returns true if Lua would like to steal the given joypad control.
*
* Range is 0 through 3
*/
int VBALuaUsingJoypad(int which)
{
	if (which < 0 || which > 3)
		which = systemGetDefaultJoypad();

	return mergedLuaJoypadMask() & (1 << which);
}

/**
* Reads the buttons Lua is feeding for the given joypad, in the same
* format as the OS-specific code.
*
* <del>This function must not be called more than once per frame. </del>Ideally exactly once
* per frame (if VBALuaUsingJoypad says it's safe to do so)
*
* With several scripts the pads are or-ed together: joypad.set() replaces the
* whole pad, so a script that does not mention a button must not clear it for
* another script that does.
*/
int VBALuaReadJoypad(int which)
{
	if (which < 0 || which > 3)
		which = systemGetDefaultJoypad();

	//lua_joypads_used &= ~(1 << which);
	return mergedLuaJoypad(which);
}

/**
* Returns true if Lua would like to remap input display.
*
* Range is 0 through 3
*/
int VBALuaRemappingInputDisplay(int which)
{
	if (which < 0 || which > 3)
		which = systemGetDefaultJoypad();

	for (size_t i = 0; i < g_luaStates.size(); ++i)
	{
		VbaLuaState *state = g_luaStates[i];
		if (state->L && (state->inputRemap[which].from || state->nextInputRemap[which].from))
			return 1;
	}

	return 0;
}

/**
* Return remapped input display.
*
* Range is 0 through 3
* Input/returned values are in the platform-specific raw format
*
* The first script that remaps the pad wins; two scripts remapping the same pad
* into different layouts has no sensible answer.
*/
int VBALuaRemapInputDisplay(int which, int input, enum LuaJoypadType padtype)
{
	if (which < 0 || which > 3)
		which = systemGetDefaultJoypad();

	for (size_t s = 0; s < g_luaStates.size(); ++s)
	{
		VbaLuaState *state = g_luaStates[s];
		if (!state->L)
			continue;

		LuaInputDisplayRemap *remap = (padtype == LuaJoypadType::LUAJOYPAD_PLAYBACK)
			? state->nextInputRemap : state->inputRemap;
		if (!remap[which].from)
			continue;

		int output = 0;
		for (int i = 0; i < 10; ++i)
		{
			if ((input & remap[which].from & (1 << i)) != 0)
				output |= remap[which].to[i];
			else
				output |= input & (1 << i);
		}

		return output;
	}

	return input;
}

/**
* If this function returns true, the movie code should NOT increment
* the rerecord count for a load-state.
*
* This function will not return true if no script is running.
*/
bool8 VBALuaRerecordCountSkip(void)
{
	// FIXME: return true if (there are any active callback functions && skipRerecords)
	for (size_t i = 0; i < g_luaStates.size(); ++i)
	{
		VbaLuaState *state = g_luaStates[i];
		if (state->L && state->running && state->skipRerecords)
			return true;
	}

	return false;
}

/**
* Given a screen with the indicated resolution,
* draw the current GUI of every script onto it.
*/
void VbaLuaState::guiFrame(uint8 *screen, int pitch, int width, int height)
{
	if (!L)
		return;

	// First, check if we're being called by anybody
	lua_settop(L, 0);
	lua_getfield(L, LUA_REGISTRYINDEX, guiCallbackTable);

	if (lua_isfunction(L, -1))
	{
		// We call it now
		numTries = 1000;

		int ret = lua_pcall(L, 0, 0, 0);
		if (ret != 0)
		{
			// This is grounds for trashing the function
			// Note: This must be done before the messagebox pops up,
			//		 otherwise the messagebox will cause a paint event which causes a weird
			//		 infinite call sequence that makes Snes9x silently exit with error code 3,
			//		 if a Lua GUI function crashes. (nitsuja)
			lua_pushnil(L);
			lua_setfield(L, LUA_REGISTRYINDEX, guiCallbackTable);

			printerror(L, -1);
		}
	}

	// And wreak the stack
	lua_settop(L, 0);

	if (!guiUsed)
		return;

	guiUsed = false;

	//if (width > LUA_SCREEN_WIDTH)
	//	width = LUA_SCREEN_WIDTH;
	//if (height > LUA_SCREEN_HEIGHT)
	//	height = LUA_SCREEN_HEIGHT;

	GetColorFunc getColor = NULL;
	SetColorFunc setColor = NULL;
	if (!getColorIOFunc(systemColorDepth, &getColor, &setColor) || getColor == NULL || setColor == NULL)
		return;

	for (int y = 0; y < height; y++)
	{
		uint8 *scr = &screen[y * pitch];
		for (int x = 0; x < width; x++, scr += systemColorDepth / 8)
		{
			const uint8 gui_alpha = guiData[(y * LUA_SCREEN_WIDTH + x) * 4 + 3];
			if (gui_alpha == 0)
			{
				// do nothing
				continue;
			}

			const uint8 gui_red	  = guiData[(y * LUA_SCREEN_WIDTH + x) * 4 + 2];
			const uint8 gui_green = guiData[(y * LUA_SCREEN_WIDTH + x) * 4 + 1];
			const uint8 gui_blue  = guiData[(y * LUA_SCREEN_WIDTH + x) * 4];
			int			red, green, blue;

			if (gui_alpha == 255)
			{
				// direct copy
				red	  = gui_red;
				green = gui_green;
				blue  = gui_blue;
			}
			else
			{
				// alpha-blending
				uint8 scr_red, scr_green, scr_blue;
				getColor(scr, &scr_red, &scr_green, &scr_blue);
				red	  = (((int)gui_red - scr_red) * gui_alpha / 255 + scr_red) & 255;
				green = (((int)gui_green - scr_green) * gui_alpha / 255 + scr_green) & 255;
				blue  = (((int)gui_blue - scr_blue) * gui_alpha / 255 + scr_blue) & 255;
			}

			setColor(scr, (uint8) red, (uint8) green, (uint8) blue);
		}
	}

	return;
}

void VBALuaGui(uint8 *screen, int pitch, int width, int height)
{
	// load order: a script started later draws on top of the ones before it
	for (size_t i = 0; i < g_luaStates.size(); ++i)
		g_luaStates[i]->guiFrame(screen, pitch, width, height);
}

void VBALuaClearGui(void)
{
	// Every canvas is consumed and cleared by guiFrame(); this only matters for
	// a frame in which nobody drew.
	for (size_t i = 0; i < g_luaStates.size(); ++i)
		g_luaStates[i]->guiUsed = false;
}

lua_State *VBAGetLuaState()
{
	return g_luaStates.empty() ? NULL : g_luaStates[0]->L;
}

const char *VBAGetLuaScriptName()
{
	return g_luaStates.empty() ? NULL : g_luaStates[0]->scriptName.c_str();
}

// The engine the scripts run on is the lua51.dll next to the exe, delay loaded,
// so it is loaded here when no script has needed it yet.  That way what is
// named is the engine a script would get, not merely the one that happens to be
// mapped already.  Asking the engine for its version is a Windows matter, and so
// is the scripting window that asks, the same way DemandLua above is.
#if (defined(WIN32) && !defined(SDL))
static HMODULE luaEngineModule()
{
	HMODULE module = GetModuleHandleA("lua51.dll");
	if (module == NULL)
		module = LoadLibraryA("lua51.dll");

	return module;
}

// Names the engine out of the engine itself rather than out of the headers the
// emulator was built against: _VERSION is what the loaded DLL reports, and the
// jit table, which only LuaJIT has, carries the build of the engine that is
// really running.  A state of its own is opened for the reading and closed
// again, because the state the scripts use may be in the middle of a script on
// the emulator's thread while this is asked for.
char *VBALuaEngineVersion()
{
	static char version[128];

	version[0] = '\0';

	if (luaEngineModule() == NULL)
	{
		sprintf(version, "lua51.dll could not be loaded");
		return version;
	}

	lua_State *L = lua_open();
	if (L == NULL)
	{
		sprintf(version, "lua51.dll did not start");
		return version;
	}

	luaL_openlibs(L);

	char engine[64]   = "";
	char language[64] = "";

	lua_getglobal(L, "_VERSION");
	if (lua_isstring(L, -1))
		strncpy(language, lua_tostring(L, -1), sizeof(language) - 1);
	lua_pop(L, 1);

	lua_getglobal(L, "jit");
	if (lua_istable(L, -1))
	{
		lua_getfield(L, -1, "version");
		if (lua_isstring(L, -1))
			strncpy(engine, lua_tostring(L, -1), sizeof(engine) - 1);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	lua_close(L);

	if (engine[0] != '\0')
		sprintf(version, "%s (%s)", engine, (language[0] != '\0') ? language : "Lua 5.1");
	else if (language[0] != '\0')
		sprintf(version, "%s (no LuaJIT)", language);
	else
		sprintf(version, "no version reported");

	return version;
}

// Where that engine was loaded from, so a stray lua51.dll is named as such.
char *VBALuaEnginePath()
{
	static char path[MAX_PATH];

	path[0] = '\0';

	HMODULE module = luaEngineModule();
	if (module != NULL)
		GetModuleFileNameA(module, path, sizeof(path));

	return path;
}

// The version resource the engine was built with.  That is where a stock Lua
// build states its patch level, 5.1.5 for instance, because Lua 5.1 keeps the
// patch out of _VERSION, which only says "Lua 5.1" (the patch made it into the
// language's own version string from 5.2 on).  LuaJIT has no such patch level
// and carries no resource, so for it this stays empty and the build in
// jit.version is what identifies the engine.
static void luaEngineFileVersion(char *out, size_t size)
{
	out[0] = '\0';

	HMODULE module = luaEngineModule();
	if (module == NULL)
		return;

	char path[MAX_PATH];
	if (GetModuleFileNameA(module, path, sizeof(path)) == 0)
		return;

	DWORD ignored = 0;
	DWORD length = GetFileVersionInfoSizeA(path, &ignored);
	if (length == 0)
		return;

	BYTE *data = new BYTE[length];
	if (data == NULL)
		return;

	if (!GetFileVersionInfoA(path, 0, length, data))
	{
		delete [] data;
		return;
	}

	// The string the build wrote, the one Explorer prints as "File version",
	// reads better than the numeric form, so try it first.
	WORD *translation = NULL;
	UINT translationLength = 0;
	if (VerQueryValueA(data, "\\VarFileInfo\\Translation", (LPVOID *)&translation, &translationLength) &&
	    translationLength >= sizeof(WORD) * 2)
	{
		char key[64];
		sprintf(key, "\\StringFileInfo\\%04x%04x\\FileVersion", translation[0], translation[1]);

		char *value = NULL;
		UINT valueLength = 0;
		if (VerQueryValueA(data, key, (LPVOID *)&value, &valueLength) && valueLength > 1)
			strncpy(out, value, size - 1);
	}

	if (out[0] == '\0')
	{
		VS_FIXEDFILEINFO *info = NULL;
		UINT infoLength = 0;
		if (VerQueryValueA(data, "\\", (LPVOID *)&info, &infoLength) &&
		    infoLength >= sizeof(VS_FIXEDFILEINFO) && info->dwSignature == VS_FFI_SIGNATURE)
		{
			unsigned major	 = HIWORD(info->dwFileVersionMS);
			unsigned minor	 = LOWORD(info->dwFileVersionMS);
			unsigned build	 = HIWORD(info->dwFileVersionLS);
			unsigned revision = LOWORD(info->dwFileVersionLS);

			if (major != 0 || minor != 0 || build != 0)
			{
				if (revision != 0)
					sprintf(out, "%u.%u.%u.%u", major, minor, build, revision);
				else
					sprintf(out, "%u.%u.%u", major, minor, build);
			}
		}
	}

	delete [] data;
}

// That string as the scripting window prints it, empty when the DLL has no
// version resource to read.
char *VBALuaEngineFileVersion()
{
	static char version[128];

	version[0] = '\0';
	luaEngineFileVersion(version, sizeof(version));

	return version;
}
#endif
