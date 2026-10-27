# Type definitions for the Lua API (Lua Language Server)

`vba.lua` is a `---@meta` definition file for the
[Lua Language Server](https://luals.github.io/): once installed, scripts written
for VBA-RR get completion, parameter names and return types for
`vba.`/`memory.`/`gui.`, and a wrong parameter type or a call to a function that
does not exist is flagged on the spot.

It is not part of the emulator build and is never loaded by the emulator. Its
only job is to tell the editor what the Lua state in
`src/common/lua-engine.cpp` actually contains.

```lua
---@meta
```

The script engine in this branch is **LuaJIT 2.1**: `lua51.dll` lives in
`win32/lib/{x86,x64,arm64}/` and is copied unchanged next to the exe at build
time. The language is Lua 5.1 plus LuaJIT's extensions
(<https://luajit.org/extensions.html>): some syntax backported from Lua 5.2 and
5.3, some of LuaJIT's own, and the `bit`, `jit` and `ffi` modules. LuaJIT 2.1
supports `goto` but not `//`. Dropping a stock Lua 5.1 `lua51.dll` in its place
also works (the 5.1 ABI is the one LuaJIT implements); you just lose all of
that.

## Installation

The editor knows two locations: a **library directory** (`workspace.library`,
applies to every open file) and **any .lua file in the workspace** (applies to
that workspace only). The library directory is recommended, because `vba.lua`
itself does not need to be treated as a script.

**A. Alongside the scripts (recommended)** — put `vba.lua` in a subdirectory of
the script directory and add a `.luarc.json` there:

```
C:\path\to\lua\
    macro.lua
    .luarc.json
    vba-api\
        vba.lua
```

```json
{
  "runtime.version": "LuaJIT",
  "workspace.library": ["./vba-api"]
}
```

**B. Install once globally** — give an absolute path in VS Code's
`settings.json` and scripts in any directory get completions:

```json
{
  "Lua.runtime.version": "LuaJIT",
  "Lua.workspace.library": ["C:/path/to/vba-rerecording/documentation/lua-api"]
}
```

**C. Copy it next to the script** — put `vba.lua` in the same directory as the
script and add `{"runtime.version": "LuaJIT"}` there. It works, but every .lua
file in that directory is treated as one workspace, so the scripts can see each
other.

> Pick **LuaJIT** for `runtime.version`, matching the bundled `lua51.dll`, so
> that `bit.` and `jit.` get completions. If you swapped in a stock Lua 5.1 DLL,
> use **Lua 5.1** instead (`bit` does not exist there, and the editor will know
> that too). Avoid Lua 5.3+: it would allow `//`, which LuaJIT does not
> implement. (`goto` is fine — LuaJIT 2.1 supports it.)

## Coverage

`vba.lua` describes **what this branch actually registers**, one-to-one with the
`luaL_Reg` tables at the end of `lua-engine.cpp`:

| Table | Entries | Notes |
| --- | --- | --- |
| `emu` / `vba` | 20 | the same table, `vba` is the old name |
| `memory` | 45 | includes the read-only `gbrom*` group and all aliases |
| `gui` | 26 | canvas is fixed at 256x224 |
| `movie` | 15 | |
| `joypad` | 8 | |
| `savestate` | 3 | |
| `avi` | 3 | |
| `input` | 2 | |
| `sound` | 1 | |
| Global functions | 7 | `AND` `OR` `XOR` `SHIFT` `BIT` `addressof` `copytable` |

`bit` is not in this file: the engine provides it (LuaJIT ships it; a stock
Lua 5.1 does not), and `vba.lua` does not declare other people's libraries.
`print` and `tostring` are the same story: the Lua 5.1 standard library
definitions already declare them (same signature, different behaviour, see the
difference table below), so only their behaviour is written in the comments at
the end of the file.

## Where it differs from the TASVideos documentation

The upstream documentation
(<https://tasvideos.org/EmulatorResources/VBA/LuaScriptingFunctions>) describes
the general semantics; the following is what this branch actually does, and
`vba.lua` follows the actual behaviour:

| Item | Documentation | This branch |
| --- | --- | --- |
| `memory.readbyterange` | returns a string | returns an **array table**, `t[1]` is the byte at `address` (`lua_createtable` + `lua_rawseti`); a negative `length` starts reading at `address+length` and goes forward |
| `vba.speedmode` | absent | present (`normal`/`nothrottle`/`turbo`/`maximum`), wired into frame skipping and throttling, see the paragraph below |
| `gui.fillbox` `gui.fillcircle` `gui.drawcircle` | absent | registered, see the paragraph below |
| port of `joypad.get/set` | optional, defaults to 0 | **required** (`luaL_checkinteger(L, 1)`); 0 is the default pad, 1-4 are the other ports |
| callback of `memory.register*` | `function()` | receives `(address, size)`, so a hook covering a range can tell which byte changed |
| `movie.author()` / `movie.name()` | nil when there is no movie | `""` when there is no movie (`movie.mode()` is the one that returns nil) |
| numeric form of a colour | — | `0xRRGGBBAA`, **alpha in the low byte**, same order as the `"#RRGGBBAA"` string |
| alpha of `gui.parsecolor` | — | scaled by the current `gui.opacity`/`gui.transparency` before it is returned |
| `gui` canvas | 239 pixels high | 256x224 (`LUA_SCREEN_WIDTH/HEIGHT`), anything outside is dropped |
| `gui.getpixel` | — | reads the **picture**, not the gui canvas; out of bounds returns `0,0,0` |
| `vba.message` | "displayed on screen" | when the status bar is visible it goes **only** to the status bar and nothing is drawn on the picture; the built-in bitmap font on the picture is ASCII only, so a whole line containing non-ASCII is skipped |
| `input.get()` | — | only implemented in the Windows frontend; other platforms return an empty table |
| `math.random` / `math.randomseed` | C `rand()` | SFMT, **one generator per script**. `rand()` in a stock Lua 5.1 built with MSVC has only 15 bits (`RAND_MAX` 32767), so `math.random(1, 100000)` can return at most 32767 values; the signature is unchanged, so it is not redeclared here |
| `bit` library | "all functions of the bitop module" | it is LuaJIT's `bit`; the engine no longer carries its own LuaBitOp, and a stock Lua 5.1 DLL has no `bit` |

Present but undocumented: the `emu` alias, `memory.registerread`,
`memory.gbrom*`, `memory.gbromreadbyterange`,
`vba.registerrun/registerclose/registerloading/registerloaded/registersaving/registersaved`,
`vba.screenshot`, `vba.remapinputdisplay`, `movie.framecount`, `sound.get`,
`avi.pause/resume`.

The three extra `gui` drawing functions:

```lua
gui.drawcircle(x, y, radius, color)        -- hollow circle, midpoint algorithm
gui.fillbox(x1, y1, x2, y2, color)         -- solid rectangle, no outline
gui.fillcircle(x, y, radius, color)        -- solid circle
```

Their C implementations have always existed
(`gui_drawcircle`/`gui_fillbox`/`gui_fillcircle`), but before 2026-10-26 they
were not registered in `guilib`, so they could not be called; now they are. The
names come from Snes9x's `gui` library (VBA's set originated there), and the
TASVideos VBA page never listed them, so they do not count as a "difference
from the documentation".

`vba.speedmode(mode)` is now **registered** (`normal` / `nothrottle` / `turbo` /
`maximum`). It used to be a no-op: `vba_speedmode()` was written correctly, but
nothing read `state->speedmode`, `VBALuaSpeed()` was called from nowhere in the
project and still contained `assert(false)`. Now:

- `VBALuaSpeed()` returns "how many frames to skip" (-1 = no script asked;
  0 = skip none; 9 = the fast-forward key's ratio; `0x7FFFFFFF` = draw no frames
  at all), and with several scripts the fastest one wins.
- `systemFramesToSkip()` consumes it: frames that are recording / must be drawn
  still take priority, and otherwise it takes the `max` with the user's setting,
  so holding the fast-forward key is still faster.
- `systemFrame()` and both audio backends (DirectSound / XAudio2) no longer
  throttle against the wall clock while a script has taken the speed — **this
  is required**: with sync on, the sound buffer is the throttle, and only
  changing the frame-skip count does not speed anything up.
- When several scripts ask for a speed at once, **the fastest one wins**
  (`normal` < `nothrottle` < `turbo` < `maximum`), the same rule as bitwise-ORing
  the pads and stacking the canvases in load order; when a script stops, ends on
  its own, or is Run again, its own request is withdrawn (Run again resets
  `speedmode` to `normal`), so it cannot get stuck at high speed.
- Known limitation: with vsync on, `nothrottle` is still capped by the refresh
  rate (`Present(1)`); `turbo`/`maximum` are unaffected (they never produced
  frames at the refresh rate in the first place).

## What it looks like once installed

```lua
local function hud()
  -- gui.text's third argument is any (internally goes through tostring);
  -- the colour can be a name/number/table
  gui.text(4, 4, "frame " .. vba.framecount(), "yellow", { r = 0, g = 0, b = 0 })

  local pad = joypad.get(0)        -- VbaButtonTable, pad.A / pad.B / pad.start ...
  if pad.A then
    memory.writeword(0x02037368, 0x1234)   -- the parameter type is integer; a string is flagged
  end
end

gui.register(hud)

vba.registerbefore(function()
  local bytes = memory.readbyterange(0x02000000, 4)  -- integer[], not string
  print("first byte", bytes[1])
end)

while true do vba.frameadvance() end
```

## Maintenance

After changing a `luaL_Reg` table in `lua-engine.cpp` (adding, removing or
renaming a function), you must update `vba.lua` to match, then run:

```
python documentation/lua-api/check_api.py
```

It extracts the function names of each `luaL_Reg` table from `lua-engine.cpp`
(skipping commented-out entries) and the global names from `lua_register`,
compares them table by table with the names declared in `vba.lua`, and exits
non-zero printing the difference if either side has one too many or too few. It
is the same approach as `src/win32/lang/make_rc.py --check`.

`vba.lua` is itself valid Lua (`luajit -e "assert(loadfile('vba.lua'))"` passes),
but whether the annotations are correct can only be checked with
`lua-language-server --check`: put `vba.lua` in a workspace, write a script that
deliberately uses the wrong parameter types, and confirm that it reports
`param-type-mismatch` / `undefined-field`; a `---@meta` file does not
participate in diagnostics itself, so "no warnings in the file" is not evidence
that the annotations are correct.
