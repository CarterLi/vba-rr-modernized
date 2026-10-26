# Lua scripts: where the output goes, and how a script fails

For whoever has a script that "does nothing".  The files this describes are
`src/common/lua-engine.cpp`, `src/win32/WinSystem.cpp`, `src/win32/MainWnd.cpp`
and `src/win32/VBA.cpp`; the function semantics themselves are the ones
documented upstream (see the references at the end).

## Three output channels, three destinations

| Call | Lands in | Notes |
| --- | --- | --- |
| `print()` / `vba.print()` | the Lua console (the edit box in File -> Lua Script) | the same function registered under the global name and under `vba`; the only channel the display mode cannot swallow, so it is the one to debug with |
| `vba.message(s)` | the left part of the status bar, or the bottom left of the picture | see below |
| `gui.drawText()` and the rest of `gui` | the picture | picture only, never the status bar |

`vba.message()` is `systemScreenMessage(msg)` with **slot 0** and a default
duration of 3000 ms (the default arguments live in `System.h`).  Calling it
every frame keeps it alive, so it can serve as a permanent HUD.

Upstream describes it as "Displays the message on the screen".  This branch
turns that into an **either/or** - never both at once - which is what the next
two points are about.

- **While the status bar is visible (and the emulator is not full screen) the
  message only goes to the left part of the bar; nothing is drawn on the
  picture.**  `DrawTextMessages()` skips every slot with a `continue` when
  `theApp.statusBarActive()` is true (`VBA.cpp`), so "no text on the picture"
  is by design rather than a lost message.
- With the status bar off, or in full screen, the message is drawn in the
  bottom left of the picture instead, with the built-in bitmap font - that font
  is ASCII only, and a line containing anything else is skipped whole.
- The left part of the bar joins slot 0 with the frame counter (slot 1) using
  `"  |  "`, and `storeScreenMessage()` truncates the text to 40 characters.
- `Options -> Video -> Disable status messages` hides slot 0 **in both places**.

## A failed load says why now

In `VBALoadLuaCode()` the console is created **before** `luaL_loadfile()` (it
used to be after, until 2026-10-24).  A file that could not be opened or did not
compile used to be reported through `printerror()` into `info_print`, which was
still NULL at that point, so it went to `fputs(stderr)` - and a windowed program
has nowhere to put stderr.  What the user saw was "Run does nothing at all".

Now:

- a wrong path prints `cannot open xxx.lua`
- a syntax or encoding error (a file saved as UTF-16, say) prints
  `xxx.lua:12: '=' expected near ...`
- in both cases the Run button does not change to "Restart" and the console is
  not cleared, so the message stays on screen
- pressing Run with no ROM running writes `No game is running - load a ROM
  first.` into the console

The cheapest way to tell whether a script started is the Run button's caption:
`WinLuaOnStart()` turns it into "Restart", `WinLuaOnStop()` turns it back.

## Which encoding a script's text is in

`print()`, `gui.popup()` and every error message reach a Unicode control as
bytes, so the console has to know how to read them.  The rule is the one the
rest of the program follows, plus a single exception that the file itself
declares:

- **A UTF-8 BOM at the start of the .lua file means UTF-8.**  Every string the
  script produces is decoded as UTF-8, so Chinese literals print as Chinese.
- **Anything else is the process ANSI code page**, exactly like before
  2026-10-26: a script that printed correctly then prints the same now.

Nothing is sniffed.  A .lua file has no other marker and the bytes cannot decide
the question - `"中文"` in GBK is `D6 D0 CE C4`, which is a legal UTF-8 sequence
as well - so guessing would turn some files into garbage instead of leaving them
alone.  Saving the file as "UTF-8 with BOM" is how a script opts in.

The BOM is also what makes such a file load at all.  Lua 5.1 does not skip a
BOM (`luaL_loadfile()` only skips a leading `#` line), so a file saved that way
used to fail with `unexpected symbol near '\239'`.  A file that starts with a
BOM is read into memory and handed to `luaL_loadbuffer()` with the BOM dropped;
every other file still goes through `luaL_loadfile()`.

One line can still mix the two: an error message names the file in the code page
and quotes the source line out of the file.  The console therefore falls back to
the code page for a string that is not valid UTF-8, rather than printing
replacement characters.

## Hook order: why a write lands one frame late

- `vba.registerbefore` runs inside `systemUpdateJoypads()`, before the game
  reads its input this frame (upstream: "just after the GBA input has updated
  at the start of the next frame").
- `vba.registerafter` runs at the end of `systemFrameBoundaryWork()`: **after
  this frame has been emulated and drawn** (`System.cpp`), and only then does
  `VBALuaFrameBoundary()` resume the coroutine behind `while true`.  So a value
  written at the end of a frame matters only if the game reads that address on
  the next frame - and if the game rewrites the address itself every frame, the
  write may as well not have happened.
- The script has to keep looping on `vba.frameadvance()`; a script whose top
  level returns is treated as finished.
- `vba.frameadvance()` **can only be called from the body of the script**.  From
  a `registerbefore` / `registerafter` callback it raises `can't call
  vba.frameadvance() from here` (`frameAdvanceWaiting` is still true there), and
  from a coroutine of your own - `coroutine.create()` - it cannot yield at all
  (Lua 5.1 does not allow a yield across a C call boundary).  Waiting for a
  frame inside a callback is meaningless anyway: the callback is called once
  per frame by definition.

## Memory addresses

- GBA: `0x02000000`-`0x0203FFFF` is EWRAM and can be read and written directly;
  a half-word access has to be even.
- Anything that is not a GBA (a GB/GBC game, or no ROM at all):
  `memory.readword` and `memory.writeword` go through
  `gbReadMemoryQuick16(addr & 0xFFFF)`, so the high half of the address is
  dropped and the access lands somewhere else.  Upstream also asks that GB
  addresses at `01:Cxxx` be passed as `Dxxx`; this implementation masks them
  again on top of that.  Lua support for GB games is still on the list in
  `documentation/TODO.txt`.

## A script that proves it is running

```lua
local SRC, DST = 0x02037368, 0x0203736A
print("script started")            -- if this shows up, the file compiled and ran

local n = 0
local function copy_data()
  local val    = memory.readword(SRC)
  local before = memory.readword(DST)
  memory.writeword(DST, val)
  n = n + 1
  if n % 60 == 1 then              -- do not flood the console
    print(string.format("f%d src=%04X dst=%04X -> wrote %04X, readback=%04X",
          vba.framecount(), val, before, val, memory.readword(DST)))
  end
end

vba.registerafter(copy_data)
while true do vba.frameadvance() end
```

## References

- TASVideos, VBA Lua Scripting Functions -
  <https://tasvideos.org/EmulatorResources/VBA/LuaScriptingFunctions>
  the function semantics are upstream's; this file only records what this
  branch does differently or what upstream does not say: where `vba.message`
  ends up (status bar or picture, never both), what a failed load reports, and
  how `memory.*` truncates an address in a GB game.
- `documentation/TODO.txt` - Lua support for GB games is still open.
