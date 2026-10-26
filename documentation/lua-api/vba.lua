---@meta

-- Type definitions for the Lua API of VBA-RR, for the Lua Language Server
-- (<https://luals.github.io/>).  Nothing in this file is executed: it only
-- tells the editor what a script finds in the emulator's Lua state.
--
-- What exists comes from the `luaL_Reg` tables at the end of
-- `src/common/lua-engine.cpp` (vbalib/memorylib/... plus the `lua_register`
-- calls), and what each entry accepts comes from the C function it points at.
-- Where the upstream documentation
-- (<https://tasvideos.org/EmulatorResources/VBA/LuaScriptingFunctions>)
-- disagrees with this build, this file follows the build; the differences are
-- listed in `documentation/lua-api/README.md`.
--
-- The engine is LuaJIT 2.1, shipped as `lua51.dll` next to the exe, so the
-- language is Lua 5.1: no goto, no `//`, no integer division - but LuaJIT's
-- `bit`, `jit` and `ffi` are there too.  A stock Lua 5.1 DLL can be dropped in
-- its place, which is why nothing below is typed against a LuaJIT extension;
-- set the runtime to match the DLL you actually run.

--------------------------------------------------------------------------------
-- Types used by the signatures below
--------------------------------------------------------------------------------

---Where a pad function reads from or writes to.  The strings are compared
---exactly (case-sensitive); anything else raises "Invalid input type".
---
---`JOYPAD_USER` the pad the user is holding, `JOYPAD_PLAYBACK` the movie input
---being replayed, `JOYPAD_RECORD` the mixed input that would be recorded,
---`JOYPAD_LUA` the pad the scripts set, `JOYPAD_GAME` what the game receives.
---@alias VbaPadType "JOYPAD_USER"|"JOYPAD_PLAYBACK"|"JOYPAD_RECORD"|"JOYPAD_LUA"|"JOYPAD_GAME"

---Button names as `joypad` and `emu.remapinputdisplay` spell them.
---@alias VbaButtonName "A"|"B"|"select"|"start"|"right"|"left"|"up"|"down"|"R"|"L"

---GBA registers understood by `memory.getregister`/`memory.setregister`.
---@alias VbaGbaRegister "r0"|"r1"|"r2"|"r3"|"r4"|"r5"|"r6"|"r7"|"r8"|"r9"|"r10"|"r11"|"r12"|"r13"|"r14"|"r15"|"cpsr"|"spsr"

---GB registers understood by `memory.getregister`/`memory.setregister`.
---@alias VbaGbRegister "a"|"f"|"b"|"c"|"d"|"e"|"h"|"l"|"af"|"bc"|"de"|"hl"|"sp"|"pc"

---@alias VbaRegisterName VbaGbaRegister|VbaGbRegister

---The `{ r = , g = , b = , a = }` form of a colour.  A channel that is not
---given is 0, except the alpha, which defaults to 255.
---@class VbaColorTable
---@field r? integer 0-255
---@field g? integer 0-255
---@field b? integer 0-255
---@field a? integer 0 = transparent, 255 = opaque

---Every `gui` function that takes a colour accepts all of these:
---
---* a number in 0xRRGGBBAA order - the same order the string form uses, so the
---  alpha is the lowest byte, not the highest
---* a name: white, black, clear, gray/grey, red, orange, yellow, chartreuse,
---  green, teal, cyan, blue, purple, magenta, or "rand" for a random colour
---* "#RRGGBB" or "#RRGGBBAA"
---* a `VbaColorTable`
---@alias VbaColor integer|string|VbaColorTable

---Opaque handle returned by `savestate.create`.  It carries a file name in its
---(protected) metatable and nothing else; only `savestate.save` and
---`savestate.load` do anything with it.
---@class VbaSavestate

---The table `joypad.get`/`getdown`/`getup` return and `joypad.set` accepts.
---Only the keys that the call reports are present, so every field is optional.
---@class VbaButtonTable
---@field A? boolean
---@field B? boolean
---@field select? boolean
---@field start? boolean
---@field right? boolean
---@field left? boolean
---@field up? boolean
---@field down? boolean
---@field R? boolean
---@field L? boolean

---Value of the `regs` field of a `sound.get` channel.
---@class VbaSoundRegs
---@field frequency integer raw frequency register value

---One of the four channels `sound.get` reports.
---@class VbaSoundChannel
---@field volume number 0.0-1.0, both speakers together (0.0 when the channel is off)
---@field panpot number 0.0 = left, 0.5 = centre, 1.0 = right
---@field frequency number Hz
---@field midikey number fractional MIDI note number, 69.0 = A4
---@field regs VbaSoundRegs

---@class VbaSoundSquare: VbaSoundChannel
---@field duty integer 0-3

---@class VbaSoundWave: VbaSoundChannel
---@field waveform string raw wave RAM, 32 or 16 bytes

---@class VbaSoundNoise: VbaSoundChannel
---@field short boolean 7-bit (true) or 15-bit (false) LFSR

---@class VbaSound
---@field square1 VbaSoundSquare
---@field square2 VbaSoundSquare
---@field wavememory VbaSoundWave
---@field noise VbaSoundNoise

---Keyboard and mouse state as `input.get` sees it, in game screen pixels.
---Only the keys that are held are present, so every field is optional; the
---table has no fixed key set, hence the indexer.
---@class VbaInputState
---@field xmouse integer
---@field ymouse integer
---@field leftclick? boolean
---@field rightclick? boolean
---@field middleclick? boolean
---@field backspace? boolean
---@field tab? boolean
---@field enter? boolean
---@field shift? boolean
---@field control? boolean
---@field alt? boolean
---@field pause? boolean
---@field capslock? boolean
---@field escape? boolean
---@field space? boolean
---@field pageup? boolean
---@field pagedown? boolean
---@field end? boolean
---@field home? boolean
---@field left? boolean
---@field up? boolean
---@field right? boolean
---@field down? boolean
---@field insert? boolean
---@field delete? boolean
---@field numlock? boolean
---@field scrolllock? boolean
---@field semicolon? boolean
---@field plus? boolean
---@field comma? boolean
---@field minus? boolean
---@field period? boolean
---@field slash? boolean
---@field tilde? boolean
---@field leftbracket? boolean
---@field backslash? boolean
---@field rightbracket? boolean
---@field quote? boolean
---@field [string] boolean|integer

--------------------------------------------------------------------------------
-- emu / vba -- the emulator
--------------------------------------------------------------------------------

---@class VbaEmuLib
emu = {}

---The same table as `emu`, kept because scripts written for older builds use
---this name.  Both names exist, and every function below is under both.
---@type VbaEmuLib
vba = emu

---Takes control of the emulation speed until the script asks for another mode
---or is stopped.  Several scripts may disagree; the fastest mode wins.
---
---`normal` gives the emulator its own speed back; `nothrottle` stops waiting
---for the clock and leaves frame skipping at the user's setting; `turbo` draws
---about six frames a second, the ratio the fast-forward key uses; `maximum`
---draws nothing at all.  Recording and a paused frame still draw everything,
---and with vsync on `nothrottle` is capped by the refresh rate.  Any other
---string raises "Invalid mode ... to vba.speedmode".
---@param mode "normal"|"nothrottle"|"turbo"|"maximum"
function emu.speedmode(mode) end

---Waits until the next frame has been emulated.  Call it from the body of the
---script (`while true do emu.frameadvance() end`).  From a `registerbefore` /
---`registerafter` callback it raises "can't call vba.frameadvance() from here",
---and a coroutine of your own cannot yield across the C call that entered it.
function emu.frameadvance() end

---Pauses the emulator and waits until the user unpauses.  Called in the middle
---of a frame (that is, from a callback) it only sets the pause and returns.
function emu.pause() end

---Frame counter of the running movie, or the number of frames since the last
---reset when no movie is active.
---@return integer
function emu.framecount() end

---@return integer
function emu.lagcount() end

---True when the frame that just ran was a lag frame.
---@return boolean
function emu.lagged() end

---True when a ROM is loaded, whether or not the emulator is paused.
---@return boolean
function emu.emulating() end

---Calls `callback` at the start of every frame, after the joypads have been
---polled and before the game reads them.
---@param callback? fun() nil unregisters
function emu.registerbefore(callback) end

---Calls `callback` after a frame has been emulated and drawn.
---@param callback? fun() nil unregisters
function emu.registerafter(callback) end

---Calls `callback` when the script is stopped (File -> Lua Script, Run/Stop,
---or the window closing).
---@param callback? fun() nil unregisters
function emu.registerexit(callback) end

---Calls `callback` after the machine has been powered on (a ROM was loaded).
---@param callback? fun() nil unregisters
function emu.registerrun(callback) end

---Calls `callback` before the machine is powered off.
---@param callback? fun() nil unregisters
function emu.registerclose(callback) end

---@param callback? fun() nil unregisters
function emu.registerloading(callback) end

---@param callback? fun() nil unregisters
function emu.registerloaded(callback) end

---@param callback? fun() nil unregisters
function emu.registersaving(callback) end

---@param callback? fun() nil unregisters
function emu.registersaved(callback) end

---Shows `text` in the left part of the status bar while the bar is visible, or
---in the bottom left of the picture otherwise - never in both.  The built-in
---font used on the picture is ASCII only, so a line containing anything else
---is skipped whole there.
---@param text string
function emu.message(text) end

---Remaps what the input display shows for one port.
---@param port integer 0 = the default pad, 1-4 = the other pads
---@param mapping? table<VbaButtonName, VbaButtonName> nil clears the remap
---@param padtype? VbaPadType "JOYPAD_PLAYBACK" remaps the next-frame display instead
function emu.remapinputdisplay(port, mapping, padtype) end

---Saves a numbered screen capture, exactly like the menu command.
function emu.screenshot() end

---Prints to the Lua console: the same function as the global `print`, which
---also accepts any value and expands tables.
---@param ... any
function emu.print(...) end

--------------------------------------------------------------------------------
-- memory -- the emulated address space
--------------------------------------------------------------------------------

---@class VbaMemoryLib
memory = {}

---@param address integer
---@return integer 0-255
function memory.readbyte(address) end

---Alias of `memory.readbyte`.
memory.readbyteunsigned = memory.readbyte

---@param address integer
---@return integer -128..127
function memory.readbytesigned(address) end

---@param address integer
---@return integer 0-65535
function memory.readword(address) end

---Alias of `memory.readword`.
memory.readwordunsigned = memory.readword
---Alias of `memory.readword`.
memory.readshort = memory.readword
---Alias of `memory.readword`.
memory.readshortunsigned = memory.readword

---@param address integer
---@return integer -32768..32767
function memory.readwordsigned(address) end

---Alias of `memory.readwordsigned`.
memory.readshortsigned = memory.readwordsigned

---@param address integer
---@return integer values >= 0x80000000 come back as a float
function memory.readdword(address) end

---Alias of `memory.readdword`.
memory.readdwordunsigned = memory.readdword
---Alias of `memory.readdword`.
memory.readlong = memory.readdword
---Alias of `memory.readdword`.
memory.readlongunsigned = memory.readdword

---@param address integer
---@return integer -2147483648..2147483647
function memory.readdwordsigned(address) end

---Alias of `memory.readdwordsigned`.
memory.readlongsigned = memory.readdwordsigned

---Reads `length` bytes and returns them as a table: `result[1]` is the byte at
---`address`.  A negative `length` reads the `|length|` bytes immediately before
---`address`; the result is in ascending address order either way.
---
---Upstream documents a string here; this build returns an array.
---@param address integer
---@param length integer
---@return integer[]
function memory.readbyterange(address, length) end

---@param address integer
---@param value integer only the low 8 bits are kept
function memory.writebyte(address, value) end

---@param address integer
---@param value integer only the low 16 bits are kept
function memory.writeword(address, value) end

---Alias of `memory.writeword`.
memory.writeshort = memory.writeword

---@param address integer
---@param value integer
function memory.writedword(address, value) end

---Alias of `memory.writedword`.
memory.writelong = memory.writedword

---@param name VbaRegisterName
---@return integer|nil the register, or nil for a name this build does not know
function memory.getregister(name) end

---@param name VbaRegisterName
---@param value integer truncated to the width of the register
function memory.setregister(name, value) end

---Reads from the GB ROM rather than the address space: the address is not
---banked and no mapper is applied.
---@param address integer
---@return integer|nil nil when a GBA game is running
function memory.gbromreadbyte(address) end

---Alias of `memory.gbromreadbyte`.
memory.gbromreadbyteunsigned = memory.gbromreadbyte

---@param address integer
---@return integer|nil nil when a GBA game is running
function memory.gbromreadbytesigned(address) end

---@param address integer
---@return integer|nil nil when a GBA game is running
function memory.gbromreadword(address) end

---Alias of `memory.gbromreadword`.
memory.gbromreadwordunsigned = memory.gbromreadword
---Alias of `memory.gbromreadword`.
memory.gbromreadshort = memory.gbromreadword
---Alias of `memory.gbromreadword`.
memory.gbromreadshortunsigned = memory.gbromreadword

---@param address integer
---@return integer|nil nil when a GBA game is running
function memory.gbromreadwordsigned(address) end

---Alias of `memory.gbromreadwordsigned`.
memory.gbromreadshortsigned = memory.gbromreadwordsigned

---@param address integer
---@return integer|nil nil when a GBA game is running
function memory.gbromreaddword(address) end

---Alias of `memory.gbromreaddword`.
memory.gbromreaddwordunsigned = memory.gbromreaddword
---Alias of `memory.gbromreaddword`.
memory.gbromreadlong = memory.gbromreaddword
---Alias of `memory.gbromreaddword`.
memory.gbromreadlongunsigned = memory.gbromreaddword

---@param address integer
---@return integer|nil nil when a GBA game is running
function memory.gbromreaddwordsigned(address) end

---Alias of `memory.gbromreaddwordsigned`.
memory.gbromreadlongsigned = memory.gbromreaddwordsigned

---@param address integer
---@param length integer negative reads backwards, as in `memory.readbyterange`
---@return integer[]|nil nil when a GBA game is running
function memory.gbromreadbyterange(address, length) end

---Calls `callback` when the game writes to `address`.  The hook is called with
---the address it fired for and the size of the access, so one hook registered
---over a range can tell which byte changed.
---
---Registering over a range replaces whatever was there; `nil` removes the hook.
---@overload fun(address: integer, size: integer, callback: fun(address: integer, size: integer)|nil)
---@overload fun(address: integer, cpu: "sub", callback: fun(address: integer, size: integer)|nil)
---@overload fun(address: integer, size: integer, cpu: "sub", callback: fun(address: integer, size: integer)|nil)
---@param address integer
---@param callback? fun(address: integer, size: integer)
function memory.registerwrite(address, callback) end

---Alias of `memory.registerwrite`, the name the upstream documentation uses.
memory.register = memory.registerwrite

---Calls `callback` when the game reads from `address`.  Same rules as
---`memory.registerwrite`.
---@overload fun(address: integer, size: integer, callback: fun(address: integer, size: integer)|nil)
---@overload fun(address: integer, cpu: "sub", callback: fun(address: integer, size: integer)|nil)
---@overload fun(address: integer, size: integer, cpu: "sub", callback: fun(address: integer, size: integer)|nil)
---@param address integer
---@param callback? fun(address: integer, size: integer)
function memory.registerread(address, callback) end

---Calls `callback` when the CPU executes `address`.  Same rules as
---`memory.registerwrite`.
---@overload fun(address: integer, size: integer, callback: fun(address: integer, size: integer)|nil)
---@overload fun(address: integer, cpu: "sub", callback: fun(address: integer, size: integer)|nil)
---@overload fun(address: integer, size: integer, cpu: "sub", callback: fun(address: integer, size: integer)|nil)
---@param address integer
---@param callback? fun(address: integer, size: integer)
function memory.registerexec(address, callback) end

---Alias of `memory.registerexec`.
memory.registerrun = memory.registerexec
---Alias of `memory.registerexec`.
memory.registerexecute = memory.registerexec

--------------------------------------------------------------------------------
-- joypad -- what the emulated machine receives
--------------------------------------------------------------------------------

---@class VbaJoypadLib
joypad = {}

---@param port integer 0 = the default pad, 1-4 = the other pads
---@param padtype? VbaPadType defaults to "JOYPAD_GAME"
---@return VbaButtonTable every button, pressed and released
function joypad.get(port, padtype) end

---Alias of `joypad.get`.
joypad.read = joypad.get

---@param port integer 0 = the default pad, 1-4 = the other pads
---@param padtype? VbaPadType defaults to "JOYPAD_GAME"
---@return VbaButtonTable only the buttons that are held
function joypad.getdown(port, padtype) end

---Alias of `joypad.getdown`.
joypad.readdown = joypad.getdown

---@param port integer 0 = the default pad, 1-4 = the other pads
---@param padtype? VbaPadType defaults to "JOYPAD_GAME"
---@return VbaButtonTable only the buttons that are not held, mapped to false
function joypad.getup(port, padtype) end

---Alias of `joypad.getup`.
joypad.readup = joypad.getup

---Sets the pad the emulator uses from the next frame on.  The table replaces
---what this script set before: a button that is not `true` is not pressed by
---this script, and keys it does not know are ignored.  `padtype` decides which
---source is overridden.
---@param port integer 0 = the default pad, 1-4 = the other pads
---@param buttons VbaButtonTable
---@param padtype? VbaPadType defaults to "JOYPAD_GAME"
function joypad.set(port, buttons, padtype) end

---Alias of `joypad.set`.
joypad.write = joypad.set

--------------------------------------------------------------------------------
-- savestate
--------------------------------------------------------------------------------

---@class VbaSavestateLib
savestate = {}

---Creates a state handle.  With a slot number the state is the one the user
---can also load from the menu (1-12); without one it is a private temporary
---file that is deleted when the handle is collected.
---@param slot? integer 1-12, or nil for a private state
---@return VbaSavestate
function savestate.create(slot) end

---@param state VbaSavestate
function savestate.save(state) end

---@param state VbaSavestate
function savestate.load(state) end

--------------------------------------------------------------------------------
-- movie
--------------------------------------------------------------------------------

---@class VbaMovieLib
movie = {}

---@return boolean
function movie.active() end

---@return boolean
function movie.recording() end

---@return boolean
function movie.playing() end

---@return "record"|"playback"|nil nil when no movie is active
function movie.mode() end

---@return integer 0 when no movie is active
function movie.length() end

---@return string empty when no movie is active
function movie.author() end

---Alias of `movie.author`.
movie.getauthor = movie.author

---@return string the file name, empty when no movie is active
function movie.name() end

---Alias of `movie.name`.
movie.getname = movie.name

---@return integer 0 when no movie is active
function movie.rerecordcount() end

---@param count integer
function movie.setrerecordcount(count) end

---Turns the rerecord counter off (and back on) for the save states this script
---loads, so a script that scrubs frames does not inflate the movie's count.
---@param count boolean true = do not count rerecords made by this script
function movie.rerecordcounting(count) end

---@return integer the movie frame counter, or the frame count since reset
function movie.framecount() end

---Stops the movie that is recording or playing.  Raises an error when there is
---none.
function movie.stop() end

---Alias of `movie.stop`.
movie.close = movie.stop

--------------------------------------------------------------------------------
-- gui -- the overlay drawn on top of the picture
--------------------------------------------------------------------------------

---Every script draws on a canvas of its own, 256x224 pixels regardless of the
---machine (GBA 240x160, GB 160x144); a pixel outside it is dropped.  The canvas
---is cleared at the start of every frame and blended onto the picture, in
---script load order, once all `gui.register` callbacks have run.
---@class VbaGuiLib
gui = {}

---Registers the function called just before the canvas is blended onto the
---picture, replacing the previous one.
---@param callback? fun() nil unregisters
---@return fun()|nil the callback that was registered before, if any
function gui.register(callback) end

---Draws `text` with the built-in 8x8 font.  Only bytes 32-127 have glyphs, so
---control characters and anything above ASCII are skipped.
---@param x integer
---@param y integer
---@param text any converted with `tostring`, so numbers and tables work
---@param fillcolor? VbaColor defaults to opaque white
---@param bordercolor? VbaColor defaults to opaque black
function gui.text(x, y, text, fillcolor, bordercolor) end

---Alias of `gui.text`.
gui.drawtext = gui.text

---Draws a filled rectangle with a one pixel outline.
---@param x1 integer
---@param y1 integer
---@param x2 integer
---@param y2 integer
---@param fillcolor? VbaColor defaults to white at 25% opacity
---@param outlinecolor? VbaColor defaults to `fillcolor` at full opacity
function gui.box(x1, y1, x2, y2, fillcolor, outlinecolor) end

---Alias of `gui.box`.
gui.drawbox = gui.box
---Alias of `gui.box`.
gui.rect = gui.box
---Alias of `gui.box`.
gui.drawrect = gui.box

---@param x1 integer
---@param y1 integer
---@param x2 integer
---@param y2 integer
---@param color? VbaColor defaults to opaque white
---@param skipfirst? boolean do not draw the first pixel of the line
function gui.line(x1, y1, x2, y2, color, skipfirst) end

---Alias of `gui.line`.
gui.drawline = gui.line

---@param x integer
---@param y integer
---@param color VbaColor
function gui.pixel(x, y, color) end

---Alias of `gui.pixel`.
gui.drawpixel = gui.pixel
---Alias of `gui.pixel`.
gui.setpixel = gui.pixel
---Alias of `gui.pixel`.
gui.writepixel = gui.pixel

---Draws the outline of a circle of `radius` pixels around (x, y) with the
---midpoint algorithm.  A radius of 0 draws nothing, a negative one is treated
---as its absolute value.
---@param x integer centre
---@param y integer centre
---@param radius integer
---@param color VbaColor
function gui.drawcircle(x, y, radius, color) end

---Fills a rectangle, corners included: what `gui.box` would cover without the
---outline.  Reversed corners are sorted out, and a box that lies completely
---outside the canvas draws nothing.
---@param x1 integer
---@param y1 integer
---@param x2 integer
---@param y2 integer
---@param color VbaColor
function gui.fillbox(x1, y1, x2, y2, color) end

---Fills a circle of `radius` pixels around (x, y).  A radius of 0 draws
---nothing, a radius of 1 only the centre pixel.
---@param x integer centre
---@param y integer centre
---@param radius integer
---@param color VbaColor
function gui.fillcircle(x, y, radius, color) end

---Sets the opacity of every later draw call.  1.0 is opaque, 0.0 invisible, and
---the alpha is clamped when a pixel is blended, so a value above 1.0 does not
---draw anything brighter.  The same as putting an alpha in each colour, but
---global to the script.
---@param alpha number
function gui.opacity(alpha) end

---The older spelling of `gui.opacity`: 0 is solid, 4 is invisible.
---@param strength number
function gui.transparency(strength) end

---Shows a message box and, for the two question types, waits for the answer.
---@param text string
---@param kind? "ok"|"yesno"|"yesnocancel" defaults to "ok"
---@return "yes"|"no"|"cancel"|nil nil for "ok"
function gui.popup(text, kind) end

---Splits a colour into its components, with the current `gui.opacity` applied
---to the alpha channel.
---@param color VbaColor
---@return integer red
---@return integer green
---@return integer blue
---@return integer alpha
function gui.parsecolor(color) end

---@return string the picture as a gd v1 image: an 11 byte header, then
---`r, g, b, a` per pixel, left to right then top to bottom
function gui.gdscreenshot() end

---Draws a gd image string (the output of `gui.gdscreenshot`) on the canvas.
---Accepted forms: `(image)`, `(dx, dy, image)`, `(image, sx, sy, w, h)`,
---`(dx, dy, image, sx, sy, w, h)`, each optionally followed by an alpha
---multiplier.
---@overload fun(image: string): nil
---@overload fun(dx: integer, dy: integer, image: string): nil
---@overload fun(image: string, sx: integer, sy: integer, width: integer, height: integer): nil
---@overload fun(dx: integer, dy: integer, image: string, sx: integer, sy: integer, width: integer, height: integer): nil
---@param dx_or_image integer|string
---@param ... integer|string
function gui.gdoverlay(dx_or_image, ...) end

---Alias of `gui.gdoverlay`.
gui.drawimage = gui.gdoverlay
---Alias of `gui.gdoverlay`.
gui.image = gui.gdoverlay

---Reads a pixel of the picture itself, not of the gui canvas.
---@param x integer
---@param y integer
---@return integer red
---@return integer green
---@return integer blue
function gui.getpixel(x, y) end

---Alias of `gui.getpixel`.
gui.readpixel = gui.getpixel

--------------------------------------------------------------------------------
-- input -- what the user is holding
--------------------------------------------------------------------------------

---@class VbaInputLib
input = {}

---Keyboard and mouse, independent of what the game receives.  Windows only:
---on any other frontend the table comes back empty.
---@return VbaInputState
function input.get() end

---Alias of `input.get`.
input.read = input.get

--------------------------------------------------------------------------------
-- sound -- the four channels of the APU
--------------------------------------------------------------------------------

---@class VbaSoundLib
sound = {}

---@return VbaSound
function sound.get() end

--------------------------------------------------------------------------------
-- avi -- the video file being recorded
--------------------------------------------------------------------------------

---@class VbaAviLib
avi = {}

---@return integer frames written so far, 0 when nothing is being recorded
function avi.framecount() end

---Stops writing frames without ending the file.
function avi.pause() end

---Resumes writing after `avi.pause`.
function avi.resume() end

--------------------------------------------------------------------------------
-- `bit` is not declared here: it belongs to the engine.  LuaJIT opens its own
-- with the standard libraries, and a stock Lua 5.1 lua51.dll has none.  The
-- five older spellings VBA adds are the globals in the next section.
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
-- Global functions registered outside any library
--------------------------------------------------------------------------------

---Same as `bit.band`, under the name old scripts use.
---@param value integer
---@param ... integer
---@return integer
function AND(value, ...) end

---Same as `bit.bor`.
---@param value integer
---@param ... integer
---@return integer
function OR(value, ...) end

---Same as `bit.bxor`.
---@param value integer
---@param ... integer
---@return integer
function XOR(value, ...) end

---Shifts left for a positive `shift`, right for a negative one.
---@param value integer
---@param shift integer
---@return integer
function SHIFT(value, shift) end

---Builds a number with the given bits set.
---@param ... integer bit numbers, 0-31; anything else is ignored
---@return integer
function BIT(...) end

---The address of a table or function.  The global `print` prints contents
---instead, so this is how you get an identity to compare with.
---@param value any
---@return integer
function addressof(value) end

---Shallow copy: a new table with the same keys and values, plus the same
---metatable.  Nested tables are shared, not copied.
---@generic T
---@param value T
---@return T
function copytable(value) end

--------------------------------------------------------------------------------
-- Standard library functions this build replaces.  Their signatures are the
-- ones Lua 5.1 already declares, so they are not redeclared here - only the
-- behaviour differs:
--
--   print(...)              to the Lua console, not stdout; expands tables
--   tostring(...)           same formatting as print, any number of arguments
--   math.random([m[, n]])   SFMT generator, one per script, same contract as
--   math.randomseed(seed)   Lua 5.1 - so a seed in one script cannot shift
--                           another script's sequence
--
-- `bit` comes from LuaJIT (32-bit, no `bit32` shim); the engine no longer
-- carries a ported LuaBitOp of its own.
--------------------------------------------------------------------------------
