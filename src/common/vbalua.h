#ifndef VBA_LUA_H
#define VBA_LUA_H

#pragma once

#include <cstdint>

enum LuaCallID
{
	LUACALL_BEFOREEMULATION,
	LUACALL_AFTEREMULATION,
	LUACALL_BEFOREEXIT,
	LUACALL_AFTERPOWERON,
	LUACALL_BEFOREPOWEROFF,
	LUACALL_BEFORESTATELOAD,
	LUACALL_AFTERSTATELOAD,
	LUACALL_BEFORESTATESAVE,
	LUACALL_AFTERSTATESAVE,

	LUACALL_COUNT
};
void CallRegisteredLuaFunctions(LuaCallID calltype);

enum LuaMemHookType
{
	LUAMEMHOOK_WRITE,
	LUAMEMHOOK_READ,
	LUAMEMHOOK_EXEC,
	LUAMEMHOOK_WRITE_SUB,
	LUAMEMHOOK_READ_SUB,
	LUAMEMHOOK_EXEC_SUB,

	LUAMEMHOOK_COUNT
};
void CallRegisteredLuaMemHook(unsigned int address, int size, unsigned int value, LuaMemHookType hookType);

enum LuaJoypadType
{
	LUAJOYPAD_USER,
	LUAJOYPAD_PLAYBACK,
	LUAJOYPAD_RECORD,
	LUAJOYPAD_LUA,
	LUAJOYPAD_GAME,

	LUAJOYPAD_COUNT
};

// Just forward function declarations

// One script window owns one VbaLuaState.  The frontend creates it, runs it and
// closes it; the engine keeps the list of live instances and each of the
// emulator-facing functions below walks that list in load order.  Two scripts
// therefore run side by side without sharing a lua_State, a callback slot, a
// joypad or a canvas.
class VbaLuaState;

// Load and start a script.  console is the frontend handle its output goes to
// (0 when the frontend has no console).  Returns NULL when the engine is
// missing or the file cannot be read or does not compile; the reason has
// already been written to that console.
VbaLuaState *VBALuaOpenScript(const char *filename, std::intptr_t console);
void VBALuaStopScript(VbaLuaState *state);
void VBALuaCloseScript(VbaLuaState *state);   // stop + free
const char *VBALuaScriptName(VbaLuaState *state);
int VBALuaScriptRunning(VbaLuaState *state);

void VBALuaFrameBoundary();
int VBAReloadLuaCode();
void VBALuaStop();
int VBALuaRunning();

int VBALuaUsingJoypad(int);
int VBALuaReadJoypad(int);
int VBALuaRemappingInputDisplay(int);
int VBALuaRemapInputDisplay(int, int, enum LuaJoypadType);

// How many frames the running scripts want the frontend to skip, or
// VBALUA_SPEED_NONE when none of them asked for a speed.  The frontend owns
// the effect: it feeds the value into systemFramesToSkip() and drops its
// throttling while the value is not VBALUA_SPEED_NONE, the same way it does
// for the fast-forward key.
enum
{
	VBALUA_SPEED_NONE	 = -1,			// no script asked for anything
	VBALUA_SPEED_MAXIMUM = 0x7FFFFFFF	// never draw a frame again
};

int VBALuaSpeed();
bool8 VBALuaRerecordCountSkip();

void VBALuaGui(uint8 *screen, int ppl, int width, int height);
void VBALuaClearGui();

const char* VBAGetLuaScriptName();

// The engine scripts run on and the file it was loaded from.  The version is
// read out of the engine itself rather than out of the headers the emulator was
// built against, so a lua51.dll swapped in next to the exe is reported as it is.
char* VBALuaEngineVersion();
char* VBALuaEnginePath();

// And that file's own version resource when it carries one, which is where a
// stock Lua 5.1 build states its patch level (5.1.5): empty otherwise.
char* VBALuaEngineFileVersion();

// And some interesting REVERSE declarations!
char *VBAGetFreezeFilename(int slot);

#endif // VBA_LUA_H
