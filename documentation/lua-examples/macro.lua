--[[==========================================================================
  macro.lua -- hold a trigger key and the emulator replays a button sequence

  Behaviour
    press it      : start from step 1, wrap back to step 1 after the last step,
                    loop forever
    keep holding  : keep looping
    release it    : stop on that very frame, joypad 1 is handed straight back
    press again   : start over from step 1 (never resumes where it stopped)

  Usage
    VBA-rr  ->  Tools -> Lua Scripting -> New Lua Script Window
    -> Open... pick this file -> Run

  ---------------------------------------------------------------------------
  Reverse-engineering notes (line numbers are src/common/lua-engine.cpp):

  * Namespaces 4993 / 4994:
       luaL_register(LUA, "emu", vbalib);  // added for better cross-emulator compatibility
       luaL_register(LUA, "vba", vbalib);  // kept for backward compatibility
    => emu and vba are one and the same table, either name works (this script
       probes for both).

  * Button names 147-149 (case sensitive, lua_getfield does an exact match, a
    typo fails silently):
       button_mappings[] = { "A","B","select","start","right","left","up","down","R","L" }
    Bit order: A=0 B=1 select=2 start=3 right=4 left=5 up=6 down=7 R=8 L=9
    => A/B/R/L are upper case, start/select and the four directions are lower.

  * joypad.set 1975-1998: without the third argument it takes the else branch,
    writes lua_joypads[] and sets lua_joypads_used |= 1 << which; it zeroes
    *buttons first and then sets/clears one bit per boolean in the table, so it
    replaces the whole pad -- a false entry clears a button too.
    systemGetJoypad() 203-215: if (VBALuaUsingJoypad(i)) res = VBALuaReadJoypad(i)
    => while a macro is active the player's own presses are fully overridden
       (not added on top of the macro output).

  * Lifetime 4885: VBALuaFrameBoundary() clears lua_joypads_used at the start of
    every frame.
    => a frame that does not call joypad.set loses the override immediately, so
       "release stops the macro" needs no cleanup code and no button can stick.

  * Callback lifetime 5260: VBALuaGui() tests if (!LUA /* || !luaRunning*/),
    the luaRunning check is commented out => the per-frame callback keeps firing
    after the main block returned, so this script does not need a
    while true do emu.frameadvance() end loop to stay alive.

  * Trigger keys 3961: s_keyToName uses upper case "K","L","M","N","O",... and
    input.get() stores boolean true for every key that is down
    => input.get()["M"] is true while M is held.
    (Note: input.get uses GetKeyboardState, the emulator window needs the focus.)

  * Frame order System.cpp 164 / 398-401 plus 5258:
       read player input -> BEFOREEMULATION callback -> merge -> the game calls
       systemGetJoypad -> AFTEREMULATION callback -> VBALuaFrameBoundary
       (clears the pad, resumes the script coroutine) -> render the screen
       -> VBALuaGui (gui callback)
    => prefer emu.registerbefore: it runs before the game latches the input and
       exactly once per *emulated* frame; gui.register hangs off the *rendered*
       frame and can be skipped while fast-forwarding or frame-skipping, which
       makes the macro stutter. Only fall back to gui.register when
       registerbefore is not available.
==========================================================================]]

------------------------------------------------------------------- config --
-- One entry per binding. Every macro keeps its own step counter, so several
-- triggers may be held at once: their buttons are OR-ed into a single
-- joypad.set call. Binding the same key twice is a hard error.
--
--   key      : trigger key, named the way input.get() names it:
--              "A".."Z", "0".."9", "space", "enter", "shift", ...
--   sequence : ordered array of steps.
--       keys   : buttons held during the step. "" means nothing.
--                A B L R face buttons (upper case), < > ^ v directions,
--                S = start, E = select.
--                Characters may be concatenated, e.g. ">vA" = right+down+A.
--                Each character is mapped to a button_mappings name internally
--                (right/left/up/down are lower case there).
--       frames : how many frames the step lasts
local MACROS = {
  {
    key = "M",
    sequence = { -- AoS
      { keys = "A",   frames = 1 },   -- A            1 frame
      {               frames = 1 },   -- nothing      1 frame
      { keys = "A",   frames = 1 },   -- A            1 frame
      {               frames = 1 },   -- nothing      1 frame
      { keys = ">vA", frames = 1 },   -- right+down+A 1 frame
      {               frames = 1 },   -- nothing      1 frame
    },
  },

  {
    key = "N",
    sequence = { -- AoS
      { keys = "A",   frames = 1 },   -- A            1 frame
      {               frames = 1 },   -- nothing      1 frame
      { keys = "A",   frames = 1 },   -- A            1 frame
      {               frames = 1 },   -- nothing      1 frame
      { keys = "<vA", frames = 1 },   -- left+down+A  1 frame
      {               frames = 1 },   -- nothing      1 frame
    },
  },
}

-- lag frames (frames on which the game did not read the input) do not consume
-- macro time
local SKIP_LAG = true

-- print "start / stop" lines to the Lua console
local VERBOSE = true

-- print one line per step change (noisy for very short steps, off by default)
local DEBUG_STEPS = false

------------------------------------------------------------- implementation --
-- emu and vba are the same table (4993/4994), take whichever exists
local ext    = rawget(_G, "emu") or rawget(_G, "vba") or {}
local lagged = ext.lagged          -- 1405: systemCounters.laggedLast

-- character -> button_mappings[] name (147-149, the case must not change)
local CHAR2BUTTON = {
  ["A"] = "A",      ["B"] = "B",      ["L"] = "L",       ["R"] = "R",
  ["<"] = "left",   [">"] = "right",  ["^"] = "up",      ["v"] = "down",
  ["V"] = "down",
  ["S"] = "start",  ["E"] = "select",
}

-- cross-check: these ten names are all the buttons the emulator knows
local KNOWN = {
  A = true, B = true, R = true, L = true,
  start = true, select = true,
  up = true, down = true, left = true, right = true,
}

-- ">vA" -> { A=true, right=true, down=true }
local function parseKeys(spec)
  local pad = {}
  if spec == nil then
    return pad
  end
  for ch in tostring(spec):gmatch(".") do
    local name = CHAR2BUTTON[ch]
    if name then
      pad[name] = true
    elseif ch ~= "+" and ch ~= " " then
      -- "+" and " " are plain separators, so ">+v+A" parses as well
      error(("macro.lua: bad key char '%s' in \"%s\""):format(ch, tostring(spec)), 0)
    end
  end
  return pad
end

-- Pre-process one MACROS entry: compile the keys strings into tables that can
-- be handed to joypad.set as they are, and give the macro its own runtime state.
local function compileMacro(entry, pos)
  if type(entry) ~= "table" then
    error(("macro.lua: MACROS[%d] is not a table"):format(pos), 0)
  end
  local key = entry.key
  if type(key) ~= "string" or key == "" then
    error(("macro.lua: MACROS[%d].key must be a non-empty string"):format(pos), 0)
  end
  local seq = entry.sequence
  if type(seq) ~= "table" then
    error(("macro.lua: MACROS[%d] (key %q) has no sequence table"):format(pos, key), 0)
  end

  local steps = {}
  for i = 1, #seq do
    local src = seq[i]
    if type(src) ~= "table" then
      error(("macro.lua: MACROS[%d].sequence[%d] is not a table"):format(pos, i), 0)
    end
    local n = math.floor(tonumber(src.frames) or 0)
    if n < 1 then
      error(("macro.lua: MACROS[%d].sequence[%d].frames must be >= 1"):format(pos, i), 0)
    end
    local pad = parseKeys(src.keys)
    for name in pairs(pad) do
      if not KNOWN[name] then
        error(("macro.lua: unknown button %q"):format(name), 0)
      end
    end
    steps[i] = {
      pad    = pad,
      frames = n,
      label  = (src.keys ~= "") and src.keys or "-",
    }
  end
  if #steps == 0 then
    error(("macro.lua: MACROS[%d] (key %q) has an empty sequence"):format(pos, key), 0)
  end

  return {
    key    = key,
    steps  = steps,
    active = false,                -- is the trigger currently held?
    index  = 1,                    -- current step
    remain = steps[1].frames,      -- frames left on the current step
  }
end

local macros   = {}
local usedKeys = {}
for i = 1, #MACROS do
  local m = compileMacro(MACROS[i], i)
  if usedKeys[m.key] then
    error(("macro.lua: key %q is bound more than once"):format(m.key), 0)
  end
  usedKeys[m.key] = true
  macros[i] = m
end
if #macros == 0 then error("macro.lua: MACROS is empty", 0) end

local function tick()
  local keys  = input.get()
  local pad   = {}
  local anyOn = false

  for i = 1, #macros do
    local m    = macros[i]
    local held = (keys ~= nil) and (keys[m.key] ~= nil)

    -- ---- trigger not held: this macro outputs nothing ----
    -- Its bits simply never reach the merged pad, and when no macro is active
    -- joypad.set is not called at all, so lua_joypads_used stays 0 and the pad
    -- is back in the player's hands (4885).
    if not held then
      if m.active then
        m.active = false
        if VERBOSE then
          print(("[macro] %s released -> stopped, reset to step 1"):format(m.key))
        end
      end
      -- rewind every idle frame, so the next press always starts at step 1
      m.index, m.remain = 1, m.steps[1].frames
    else
      -- ---- trigger just went down: start over ----
      if not m.active then
        m.active = true
        m.index, m.remain = 1, m.steps[1].frames
        if VERBOSE then
          print(("[macro] %s pressed -> start from step 1"):format(m.key))
        end
      end

      -- contribute the current step to the merged pad
      local step = m.steps[m.index]
      for name in pairs(step.pad) do pad[name] = true end
      anyOn = true

      -- ---- advance the timer; lag frames are free ----
      if not (SKIP_LAG and lagged ~= nil and lagged()) then
        m.remain = m.remain - 1
        if m.remain <= 0 then
          m.index = m.index + 1
          if m.index > #m.steps then m.index = 1 end
          m.remain = m.steps[m.index].frames
          if DEBUG_STEPS then
            print(("[macro] %s step %d/%d  %s x %d"):format(
              m.key, m.index, #m.steps,
              m.steps[m.index].label, m.steps[m.index].frames))
          end
        end
      end
    end
  end

  -- One call for the whole pad: joypad.set replaces every button (1975-1998),
  -- so all active macros have to be merged into a single table first.
  if anyOn then
    joypad.set(0, pad)
  end
end

-- Prefer hooking in before the game latches the input (runs on every emulated
-- frame); only fall back to gui.register when that is unavailable.
local register = ext.registerbefore or (rawget(_G, "gui") and gui.register)
if type(register) ~= "function" then
  error("macro.lua: neither emu/vba.registerbefore nor gui.register is available", 0)
end
register(tick)
