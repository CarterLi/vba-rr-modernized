# Modernize VBA-rr: a current Windows emulator for the movies that were recorded with it

![build](https://github.com/CarterLi/vba-rerecording/actions/workflows/build.yml/badge.svg)

## What this is

**VBA-ReRecording** is the re-recording branch of the stable 1.7.2 version of
VisualBoyAdvance, a Game Boy / Super Game Boy / Game Boy Color / Game Boy
Advance emulator.  Its primary function is to expand the features related to
the creation of *tool-assisted movies*: the savestate, the movie recording and
the replay, the re-recording, the Lua scripting and the debugger.

This branch is that project, continued.  The upstream repository is
<https://github.com/TASEmulators/vba-rerecording>, and the TASVideos emulator
resource page for it - the canonical entry point for anybody looking for this
emulator - is <https://tasvideos.org/EmulatorResources/VBA>.

The builds of this branch are in the releases of this repository; the download
links are at the end of this file.

## Which core should I use?

The emulator ships two cores, and they are not interchangeable:

* **`VBA-RR-v7.exe` - the V7 core** (reports itself as v23.7).  This is the
  original VBA core, the one the emulator itself grew out of.  **The vast
  majority of the existing VBM recordings were made with it**, and it is the
  one to use unless you have a reason not to.
* **`VBA-RR-v8.exe` - the V8 core** (reports itself as v24.1).  This core came
  from VBA-M.  It fixes a number of emulation bugs - a good part of them in
  the GB core, and a few in the GBA core as well, such as the well known
  crash of the tree boss in the Blue Moon edition of Rockman EXE 4 (Mega Man)
  - but it also brings some lag along with it.

**If you are recording a GBA game, use the V7 core.**  The V8 core runs the
old, excessively laggy GBA timing: it does not have the prefetch hack the V7
core has, so the game lags more than it should, and a movie recorded with it
does not match one recorded with the V7 core.

The Play Movie dialog shows the core the selected movie was recorded with and
marks a mismatch in red as "desync expected", so you can tell before you
start.  A movie from the other core can still be played - it is just not
guaranteed to stay in sync.

### Warnings

* **The cheat-related vulnerabilities of the old releases are fixed here.**  The
  GameShark and cheat-list loading overflows were fixed upstream in 2016 and
  2017 and are long gone from this tree, so loading a malicious cheat file is
  no longer a way into the machine.  The upstream code has not been maintained
  since 2019, though, so treat anything else you find in it the way you would
  treat any unmaintained code.
* **Never use revisions 418 and 420.**  They erase the movie input while
  recording.  They are listed under `deprecated/` on the TASVideos page, and
  they are far older than anything in this branch.
* **The Linux port is unmaintained**, and this branch removes it: the SDL and
  GTK frontends and the autotools build are gone, and the Windows front end is
  the only one left.  The Win32 build under WINE was reported as working.

## Summary

VBA-rr has not seen an upstream commit since September 2019. The Windows
project still asks for Visual Studio 2005, the CPU cores were written in x86
assembly so the emulator could not be built 64-bit at all, the renderers were
fixed function Direct3D 9 and OpenGL 1.x, and every dialog asked for an 8 point
bitmap font from 2004.

This branch - 90 commits on top of `4526341`, the September 2019 revision -
rebuilds all of that for a current Windows, and adds the things a modern
emulator is expected to have.

**The short version:** a Direct3D 9Ex / Direct3D 11 rendering stack, with an
OpenGL 3.3 and a WebGPU renderer behind compile time switches, GDI as the
fallback that needs nothing, GPU upscaling
with NVIDIA Image Scaling, AMD FidelityFX and
the whole libretro xBR family, a Windows 10 era shell with dark mode and
per-monitor DPI, XAudio2 as a lower latency sound backend, screenshots in
whatever format the machine can actually write, recordings in the container the
chosen encoder calls for with the filter and the magnification already applied,
a 64-bit and an ARM64 build,
Switch controller support, cheat management, and a movie dialog that tells you
which core recorded the movie - with the emulation itself left bit for bit
alone.

**The goal is deliberately *not* to make the emulation more accurate.**  Changing
the timing or any other timing-sensitive behaviour of the core would
desynchronize every existing recording, and the recordings are the reason this
fork exists.  So the emulated game behaves exactly as it did: the savestate and
movie formats are unchanged, the RTC behaviour is unchanged, the random stream
Lua sees is unchanged, the re-recording semantics are unchanged, and the frame
timing is unchanged.  Everything that changed is the shell *around* the core:
how it is built, how it is drawn, how it sounds, how it looks and how it feels
to use.

## Why

Because the emulator still works.  Nobody threw it away - people kept using it
to make and to replay movies, and they kept having to fight the same handful of
problems: a build that would not open on a modern Visual Studio, a 32-bit only
binary, a picture that had to be stretched by the sampler because there was no
upscaler, a sound device with 150 ms of latency and no way to see how much, and
a UI that looked like it had been abandoned since 2004.  Fixing those without
touching the core was possible; that is what this branch does.

## Who this is for

There is a fair chance that people will migrate to BizHawk, where the actively
developed TAS tooling lives today.  That is not what this branch is for.

VBA-rr still has a lot of long time users: people with a library of
recordings they want to keep replaying, with Lua scripts they have written
over the years, with hotkeys and workflows they have muscle memory for.  For
them the emulator has to keep working, and it has to keep working on the
Windows they now have.  So the target here is the existing user base - not
converting anyone to a different emulator, but making the one they already use
something they would rather keep using.

## What is new

### A modern rendering stack

* **Direct3D 9Ex** - `Direct3DCreate9Ex`, `CreateDeviceEx` and `PresentEx`, so
  the frame is handed to the desktop window manager instead of being copied
  through a redirection surface.  The flip model is used where the system has
  it, and the bitblt model stays as the fallback.
* **GDI** - the renderer the tree was born with, and the slowest of them: it
  blits the frame into a DIB and hands it to `StretchDIBits`, so the picture
  is scaled by the CPU.  It is the last renderer of the fallback chain, so it
  is what the emulator falls back to when no other one starts, and it is on by
  default: it needs no DirectX at all, so it works over a remote desktop, in
  safe mode and on a virtual machine that only has the basic display adapter,
  which is exactly where the other renderers give up.  A build without it
  (`VBA_ENABLE_GDI` in `GdiConfig.h`) leaves the entry out of the Video menu
  and falls back to DirectDraw, when that one is built.
* **DirectDraw** - the older accelerated renderer, which runs on `DDRAW.DLL`
  and is off by default: on Windows 8 and later `DDRAW.DLL` is itself a
  compatibility layer over DXGI, so it fails wherever Direct3D fails and pays
  for the layer wherever it does not.  With its compile time switch
  (`VBA_ENABLE_DDRAW` in `DDrawConfig.h`) on, the entry and its options come
  back to the Video menu; the fallback chain reaches it only in a build that
  leaves GDI out.
* **Direct3D 11** - a new backend: device, swap chain, a dynamic BGRA frame
  texture (BGR565 at 16 bpp) drawn as a single quad, the on screen display
  blended on top, the flip model where the system supports it and bitblt where
  it does not, and VSync as a video option rather than a fixed wait.
* **Screen scaling that keeps the picture's shape.**  **Video -> Screen
  Scaling** offers stretch, aspect fit and aspect fit with an integer factor,
  and both the window and full screen follow it, so a 3:2 picture is no longer
  stretched into whatever shape the window happens to have.  The picture is
  drawn into a child window that is sized and centred for the mode, and the
  bars around it are the window's own background - in full screen too, where
  the picture also stays inside the monitor when the menu bar is shown.
* **The defaults are the modern ones**: a build that does not target Windows
  XP starts on Direct3D 11, XAudio2, a 2x window and 44 kHz audio instead of
  DirectDraw, DirectSound, 1x and 22 kHz.  A WINXP build starts on Direct3D 9
  and keeps the old sound and window size.
* **OpenGL 3.3** - a core profile renderer beside the fixed function one, with
  double buffered frame textures so the upload no longer waits on the last
  draw, and `WGL_EXT_swap_control_tear` so a frame that misses the blanking
  interval is shown at once instead of being held back a whole interval.
  Both OpenGL renderers sit behind a compile time switch
  (`VBA_ENABLE_OPENGL` in `OpenGLConfig.h`), so a build without them leaves
  their entries out of the Video menu and keeps the GLSL upscaler shaders out
  of the executable.  Disabled by default.
* **The legacy OpenGL renderer now draws the on screen display through the
  renderer**, the way every other backend does: GDI rasterises the messages
  into the same `TextOverlay` the Direct3D renderers use, and the renderer
  composites it over the picture as a texture.  Painting the text onto the
  window with GDI cannot work beside a flip swap chain - what the desktop
  composes is the chain, not the surface GDI would write to - which is why the
  messages used to show for a single frame and then disappear.  The glyphs,
  colours, outline and margins are still GDI's and unchanged.
* **WebGPU** - a renderer through `wgpu-native`, which lets the same build run
  on Direct3D 12, Vulkan, Metal or OpenGL.  The backend can be picked from the
  Video menu, the entry names the one that is really in use, and the library
  is loaded at run time, so a build without it simply leaves the entry out.
  Disabled by default.
* **The render method menu now tells the truth.**  It names the device the
  renderer actually got ("Direct3D 9Ex", "Direct3D 9.0c"), the presentation
  model it is actually using ("Direct3D 11 (Flip)", "Direct3D 11 (Bitblt)")
  and, where it runs, the WebGPU backend behind it.  No more guessing from
  the trace log.

The renderers sit behind compile time switches, so a build can leave them out
or put them back.  DirectDraw, OpenGL and WebGPU are off by default; GDI, the
one that runs everywhere, is on:

* `VBA_ENABLE_GDI` in `src/win32/GdiConfig.h` builds the GDI renderer.  With
  it off, the "GDI" entry leaves the Video menu, and the fallback chain ends
  at DirectDraw, when that backend is built.
* `VBA_ENABLE_DDRAW` in `src/win32/DDrawConfig.h` builds the DirectDraw
  renderer, which is off by default.  With it on, the "DirectDraw" entry and
  its options come back to the Video menu, and the chain tries it only in a
  build that leaves GDI out.
* `VBA_ENABLE_OPENGL` in `src/win32/OpenGLConfig.h` builds the two OpenGL
  renderers - the fixed function one and the OpenGL 3.3 one.  With it off,
  the "OpenGL 3.3" and "OpenGL (Legacy)" entries and the two legacy GL
  rendering options leave the Video menu, and the GLSL sources of the
  upscalers - which only that renderer compiles - are not put into the
  executable.
* `VBA_ENABLE_WGPU` in `src/win32/WgpuConfig.h` builds the WebGPU renderer.
  With it off, the "WebGPU" entry and its backend submenu leave the Video
  menu, and the WGSL ports of the upscalers are not put into the executable.

A build that leaves a renderer out still reads the settings of an earlier
build: a stored render method that names a renderer the build does not have
falls back to the one the build starts on rather than failing, and the render
method that finally starts is the one that is saved.

### GPU upscaling that does not cost the emulation thread

* **NVIDIA Image Scaling** and **AMD FidelityFX Super Resolution 1.0** run on
  the GPU, with a shared sharpening amount (0 / 25 / 50 / 75 / 100 percent).
  They compose with the software pixel filters, which keep running first.
* **The libretro shader family** is now available as real upscalers: xBR lv2,
  xBR lv2 without the blend, xBR lv3, xBR lv3 multipass, 2xBR, xBR-mlv4,
  super-xBR, ScaleFX, PT SkyWalker541 and xBRZ-Freescale, plus **Lanczos** as
  the smooth enlargement.  The chains are compiled at run time; only the one
  the user picked is compiled at startup, so the program starts immediately
  instead of building every shader first.
* **Max Scale** caps the ratio the upscalers render at (2x to 8x), so an 8x
  window does not smear a GBA pixel into a block of noise.
* The upscalers run on the GPU *while the frame is being stretched*, which is
  what keeps the audio device fed: the emulation thread no longer runs a
  filter.

### More filters, and filters that behave

* **xBRZ 2x to 5x**, **HQ4x** and **ScaleFX 3x** are new, and the scanline,
  bilinear, AdMame, 2xSaI, pixelate and Simple filters have been replaced by
  faster SSE2 implementations with the same output.
* **GB/GBC LCD colour correction** is built in, with the AGB and CGB profiles
  (per-channel gamma plus a colour matrix) baked into the palette tables, so
  every renderer picks it up.  The GBA and the GB/GBC variants are separate
  menu entries and are remembered.
* **Tools -> "Record at Window Size and Filter"** writes the video at the size
  the emulator is drawn and runs every frame through the active pixel filter
  and the window magnification, instead of dumping the raw unfiltered LCD
  buffer.  A VBM recording can therefore be exported straight to a video that
  looks the way the emulator looked while it was being made - there is no need
  to export the raw frames first and enlarge them with a third party tool
  afterwards.  The same option applies to the screenshots, so a capture is
  exactly the frame the video would hold.

### Audio that says what it is doing

* **XAudio2 2.9** is the default sound backend of a modern build, loaded at
  run time and degrading to DirectSound on any failure.  It holds four blocks
  instead of the DirectSound ring's ten, which is about 67 ms of latency
  instead of 150.
* **The sound API menu names the buffering**: "DirectSound - 9 blocks, 150 ms"
  next to "XAudio2 (2.9) - 4 blocks, 67 ms", with the dropouts appended when
  there are any.  For once the latency is visible where the backend is chosen.

### Screenshots and images

* **Screenshots go through the Windows Imaging Component**, so the format menu
  is built from the encoders the machine actually has - PNG, JPEG, GIF, TIFF,
  DDS, HEIF, JPEG XL and whatever else is installed - instead of PNG or BMP and
  nothing else.  The encoder's own pixel format is honoured (indexed formats
  get an optimal palette), and a missing HEIF or JPEG XL codec reports itself
  instead of a generic failure.
* Every step of a failed write is now logged with its HRESULT, so "Error
  creating file" is a thing that no longer happens.
* **Lua**: `vba.screenshot()` takes a capture from a script.

The Windows Imaging Component is the one piece of this that is not loaded at
run time and does not degrade gracefully - it needs the codecs Windows 7 and
later ship with.  Everything else in this branch degrades.

### Recording that uses the machine's encoders

* **Tools -> "Start/Stop Audio/Video File Recording..."** asks for the encoder
  before it asks for a file name.  The list is built with `MFTEnumEx` from the
  Media Foundation encoders this machine actually has, hardware ones first and
  labelled with the vendor, so a machine with NVENC offers "NVIDIA H.264
  Encoder MFT / Hardware" and a machine without it simply does not.  The audio
  box works the same way: AAC, MP3, FLAC, ALAC, Opus and Dolby Digital each
  appear only when an encoder for them is installed.
* **The container is not a choice, it is a consequence.**  H.264, HEVC or AV1
  with any audio codec goes into `.mp4`; choosing "None (audio only)" gives
  `.m4a`, `.mp3` or `.flac` depending on the codec.  The save dialog's filter
  and default extension follow, so there is no way to end up with an extension
  that does not match the stream.
* **The bit rate is a number, not a word**, with Low/Medium/High presets next
  to it (2000/4000/8000 kbps for the picture, 64/128/192 for the sound);
  typing a value by hand puts the preset box back on "Custom".  H.264 gets a
  profile box (Auto/Baseline/Main/High), and the frame rate box records below
  the emulator's own rate by **dropping** frames instead of duplicating them,
  so the duration of the file stays right.
* **The encoding runs on its own thread, and that is not about the GPU.**  A
  hardware encoder still makes `WriteSample` block: its surface pool is small
  and the call returns only once the encoder takes the sample, so the wait
  lands on the emulation thread.  With the box ticked the emulation thread
  only converts the frame into one of three slots and signals the worker.
  Frames that arrive while all three are busy are dropped and the timestamps
  keep moving, so the duration of the file stays right; the sound is never
  dropped, because a missing block would shorten the track and pull the
  picture out of sync.  At 240x160 this buys almost nothing - 0.10 ms to
  0.08 ms per frame, since the encoder keeps up anyway.  At 1440x960 with the
  window filter applied it is 3.54 ms to 2.19 ms on a hardware encoder and
  5.17 ms to 4.02 ms on the software one.
* **The old flow is still there**, under Tools -> "Legacy AVI Recording":
  Video for Windows with the `AVISaveOptions` codec picker, and the WAV
  recorder.  Neither has changed.  They live in a submenu because that flow
  runs the other way round - save the file first, choose the codec second -
  and folding the two into one list would have meant a list where one row
  behaves unlike the rest.

### A shell that looks like it belongs on this decade's desktop

* **Dark mode**: the title bar and the menu bar follow the system setting.
* **The system UI font** instead of the 2004 bitmap face, so the text follows
  the system language, and a **true type monospaced face** for the machine
  generated text in the disassembly, memory, I/O and cheat views.  An option
  brings the original bitmap look back for layouts that do not fit.
* **Per-monitor DPI awareness (V2)**.  The dialogs scale their children and
  rebuild their fonts when the window moves to a monitor with another scale,
  and the main window keeps the game at its pixel size.  The text is sharp
  instead of a blurred 96 DPI bitmap stretched across the screen.
* **The Vista+ common item dialogs** for every file and folder picker, so
  Windows stops showing the XP-era dialog on a current desktop.
* **Window sizes up to 12x** (1x - 8x and 9x - 12x), and the `-videoScale`
  command line argument takes 1 to 12 as well.
* The **memory viewer's address** can be read and copied, **RAM Watch and RAM
  Search** divide their columns evenly, the **file naming** and **GBA special
  effects** entries say what they do, the **About box** names the build as the
  Modernized Edition and points at the GitHub repository, and the **title bar
  shows the Git revision** instead of `svn0`.
* **Nothing new is a hard requirement.**  Every optional library is loaded at
  run time and every new feature degrades gracefully when it is missing, so an
  old system does not crash - it just does not get the new renderer.  Windows 7
  and later is what we recommend, though.

### Input that actually works

* **Nintendo Switch controllers are supported.**  The Switch pad declares its
  buttons under the simple report only, so it used to enumerate, be acquired
  and then report the defaults for ever: the setup dialog captured nothing and
  the pad did not work in game either.  The pads are now asked for the simple
  report, and they work.
* **DirectInput 8**, and the device list can be rebuilt while the emulator
  runs: a controller that woke up, was plugged in or was paired after the
  start now shows up.  A controller that arrives or goes away says so, on the
  game screen and in the trace.
* **A dead zone of our own**, so a stick that rests a little off centre does
  not hold a direction down.
* **The pad setup dialog** gains a **Clear** button (a key bound by accident
  could not be taken off again before) and a **Gamepads** page that lists what
  each pad answers to.
* **Hold-to-autofire**: an auto-fire key can now be bound to a button, and
  holding it toggles that button's auto-fire on and off - the same thing the
  Alt+0-9 menu commands do, but with a key.

### Cheats that can be managed

* GBA cheats are **grouped** by description and type, with expand and collapse
  headers, **Type and Status** columns, native checkboxes, and grouped edit and
  remove.  Batch input is validated before it is applied.
* The grouping is **optional**, and both checkboxes have tooltips that explain
  what they do.
* **A pasted code can explain itself.**  With one row selected, **Explain**
  opens a window that says what the code does, line by line: which address gets
  which value, which key has to be held, and which condition has to hold for
  the lines below it to run.  A selected group is explained line by line as a
  whole, with the family and the line count at the top and the cheat's own
  description under it, when it has one.  Nothing is decrypted a second time -
  the engine already decoded the code when it was added, and the wording
  follows its own interpreter, so all 116 instruction types are covered.  A
  line that carries the *data* of the instruction above it - the second line of
  a slide, a ROM patch, a group write, a super code - is shown as data instead
  of being read as an instruction of its own.  The window is translated like
  the rest of the shell.

### Movies

* The **Play Movie dialog shows the core that recorded the selected movie**.  A
  movie from the other core is shown in red as "desync expected", so the
  mistake is visible before the recording starts - the movie can still be
  played.  See [Which core should I use?](#which-core-should-i-use) above for
  what the two cores actually are.

## Fixes

These are the things that were broken in the original VBA-rr and are broken no
longer.  Development-time slips in the new code are not listed here - they were
never in a release and no user ever saw them.

**Video**

* The GDI renderer blitted an uninitialised buffer, so with the pixel filter
  set to None the picture was whatever `malloc` happened to return - a black
  window with the on screen text on it.
* Closing a ROM left the last frame on screen.
* Switching the render method rebuilt the main window and flashed the whole
  frame white.
* The title bar collapsed to a single character as soon as the speed indicator
  set it.
* The trace log was truncated to its last line, so it never showed a run.  It
  now stays open for the whole session, is named after the core that wrote it
  (`vba-trace-v7.log`, `vba-trace-v8.log`), opens with the build, the
  architecture and the revision that produced it, and records what the
  renderer, the shaders and the on screen display actually did.

**Filters**

* The 16 bpp interpolation was written for 32 bit channel layouts and gave the
  wrong colours.

**Audio and input**

* The accelerator configuration was parsed out of a buffer far past its end,
  so hundreds of phantom accelerators shadowed the real shortcuts and hotkeys
  such as F1, F3 and F4 intermittently stopped loading save states.
* The current, last and next input button sets were 16 bits wide while the
  speed, capture and GameShark buttons live above bit 15, so those three
  buttons were truncated away.

**Windows shell**

* The `joyDebug` switch was read from the Windows directory instead of beside
  the executable.
* The RAM Watch and RAM Search columns did not fill their control.

**Lua scripting**

* A script that did not load said nothing at all.  The console window belongs
  to the loader, and it was created *after* the file had been read, so the
  error `luaL_loadfile()` returned was handed to a print sink that did not
  exist yet and went to `stderr` - which a windowed program has nowhere to put.
  A file that was missing, one that did not parse and one that had been saved
  as UTF-16 all looked exactly like a script that does nothing: no window, no
  message, the Run button still saying "Run".  The console now exists before
  the file is opened, so the error names the file and the line - `cannot open
  my.lua`, or `my.lua:12: '=' expected near ...`.
* Pressing **Run** with no game loaded was the other silent click: it did
  nothing, and said nothing.  It now says so in the console.

**Emulation correctness**

These five touch the emulation itself, so they are worth spelling out: what
each one changes, and what it means for the recordings that already exist.
None of them touches the frame timing, the movie format, the savestate layout
or the re-recording semantics.

* **The GB 32-bit memory write dropped its top byte.**
  `gbWriteMemoryQuick32()` writes a 32-bit value into the GB memory map.  It
  wrote the top byte to `addr+1` - overwriting the second byte with it - and
  never wrote `addr+3` at all.

  *What it affects:* exactly one caller in the whole tree, the Lua binding of
  `memory.writedword()` / `memory.writelong()`, and only for a GB, GBC or SGB
  game - the GBA path does not go through it.  The four bytes that land in
  memory are now the four bytes of the value, in order.

  *Why a recording is safe:* the emulated cores never call it, so a write like
  this only ever happens when a *script* asks for one.  Neither the movie
  format nor the savestate layout nor the timing is touched.  A script that
  used `memory.writedword()` was writing something other than what it asked
  for before, and `memory.readdword()` could not read back what
  `writedword()` had written; it can now.

* **The Lua colour conversion function pointers were called uninitialised.**
  `getColorIOFunc()` picks the pixel getter and setter for the current colour
  depth and returns false for a depth it does not know.  `gui.gdscreenshot()`
  declared the pointer without initialising it and called it without looking
  at the return value.

  *What it affects:* `gui.gdscreenshot()`, the Lua function that captures the
  Lua GUI layer.  At 16, 24 or 32 bits per pixel - the only depths the
  emulator has - nothing changes; at any other depth the old code could call
  a garbage pointer, and the new code returns without doing anything.

  *Why a recording is safe:* it is a screenshot path.  It reads the frame
  buffer and writes an image file; it does not touch the emulated state, the
  movie or the timing.  The other user of the same helper, the Lua GUI
  overlay, already checked the return value.

* **The V7 MBC3 battery timestamp was read before it was written.**
  Reading the battery file of an MBC3 cartridge with a real time clock stores
  the time of the last save in `mapperLastTime`, the reference the clock
  delta is computed from.  The V7 core declared the `time_t` it displayed and
  stored without initialising it.

  *What it affects:* the value of `mapperLastTime` after a battery file has
  been read, and the timestamp that is put on screen.  It is a defined value
  (zero) now instead of whatever happened to be on the stack - `ctime()` was
  being called on an undefined `time_t`, which can return nothing at all and
  take the message box down with it.

  *Why a recording is safe:* a recording that starts from a **savestate** is
  not affected at all - the whole mapper structure, `mapperLastTime`
  included, is part of the state and is restored after the battery file has
  been read.  A recording that starts from **SRAM** is affected in one narrow
  case: the SRAM block of an MBC3 cartridge carries the clock fields,
  `mapperLastTime` among them, and the V7 core overwrites that value with the
  one it computed itself.  Before the fix that value was undefined, so the
  delta - and with it the date an MBC3 game reads - was whatever the stack
  happened to hold; on a freshly started emulator that stack page is usually
  still zero, which is what it is now.  This is the one place in the branch
  where an old recording can end up with a different date than it had, and
  the reason is that the old date was never a defined one to begin with.

* **The GB real time clock no longer depends on the size of `time_t`.**
  `_USE_32BIT_TIME_T` is not supported on x64, and with it gone `time_t` is
  64 bits everywhere.  The MBC3 clock computes `now - mapperLastTime`, and
  `mapperLastTime` is a `u32` whose "unset" value is 0xFFFFFFFF.  With a
  32-bit `time_t` the subtraction wrapped, so an unset clock meant "now + 1",
  one second.  With a 64-bit `time_t` it does not wrap, and an unset clock
  meant a difference of about fifty-six years, which left a fresh cartridge
  at 00:00:00 instead of advancing it.  The fix spells the 32-bit wrap out:
  `(time_t)(int32)(u32(now) - mapperLastTime)`.

  *What it affects:* the MBC3 and TAMA5 clock deltas, and nothing else.

  *Why a recording is safe:* it restores the exact arithmetic of the 32-bit
  builds.  `u32(now)` is the same 32 bits the old `time_t` held, and the
  `(int32)` is the same signed reading the old `int` arithmetic produced, so
  the difference - and with it the clock a game reads - is bit for bit the one
  the old Win32 build computed.  Without this the x64 build would have
  desynchronized every recording that was made with the 32-bit one.

* **The flash device IDs are derived from the restored save size.**
  `flashReadGame()` restores the flash save out of a state.  The size comes
  out of the stream, but the device and the manufacturer ID are only derived
  inside `flashSetSize()`, which the restore path did not call - so they
  kept whatever the session had, normally the 64K pair.

  *What it affects:* the chip ID a GBA game can read from the flash (`0x00`
  returns the manufacturer ID, `0x01` the device ID): `0x32`/`0x1b` for a
  64K chip, `0x62`/`0x13` for a 128K one.

  *Why a recording is safe:* for a 64K flash - the default, and what most
  games use - `flashSetSize(0x10000)` derives exactly the pair that was
  there before and changes nothing else, so the result is bit for bit the
  same.  For a 128K flash in a session that also has a 128K flash the IDs
  were already right, because loading the ROM calls `flashSetSize()`.  What
  changes is the mismatched case, a 128K state loaded into a session that was
  set up as 64K: there the chip used to report itself as a 64K one while the
  state said 128K and the upper bank was unreachable, and now it reports what
  the state says.  The savestate format older than the current one already
  worked that way; only the current one did not.  The "make a 64K save work
  in a 128K chip" copy inside `flashSetSize()` is not reached from here: the
  size is restored before the call, so the condition it tests cannot hold.

## Under the hood

Briefly, because none of it changes what you see:

* **The CPU cores no longer contain assembly.**  The hand written x86 assembly
  is gone and the C bodies that already sat behind `C_CORE` are the only
  implementation.  They were checked against the assembly they replace with a
  per macro A/B harness - 360 (V8 ARM), 22 (V8 Thumb), 43 (V7 ARM) and 19 (V7
  Thumb) macro combinations, about 480,000 input states, zero differences.  One
  real divergence turned up and was fixed to match the instruction.
* **The core no longer depends on MFC.**  `src/{common,gba,gb,filters}` builds
  with no MFC and no Windows header at all; the host services the core needs
  sit behind a small interface instead of `CVBAApp`.
* **A per-frame regression trace** (`-hashtrace`) writes a hash of the
  emulator state to a CSV, one row per frame, so a change can be *proved* not
  to have moved the emulation.  Two V7 movies over 4001 frames come out
  byte-identical in both columns.
* **The project builds with the current toolset**: Visual Studio 2010 and the
  Windows SDK first, then `VBA_Latest.vcxproj` / `VBA_Latest.sln` - C++17,
  SSE2, `/W3` clean on Win32, x64 **and ARM64**, twelve configurations
  (V7/V8 x Debug/Release x three architectures).  The 2005, 2008 and 2010
  project files and the VC6 era compiler guards are gone; `VBA_Latest` is
  their replacement, and the sources still carry the `WINXP` switch so an
  XP-flavoured configuration can be added to it.
* **The three release packages are built by CI**, one runner per
  architecture - x86 and x64 on a Windows runner, ARM64 on an ARM64 one, so
  the ARM64 build is native and not a cross compile.  A tag publishes the
  three archives as the assets of the release.
* **The tree is warning free.**  C4244 and C4800 are enabled again and every
  64 to 32 bit narrowing is written out.
* **Dead weight is gone**: the autotools build, the SDL and GTK frontends, the
  bundled 7-Zip tree, the vendored libpng, the VC6 era DirectX 9 headers and
  an unused `nasm.exe`.
* **The vendored code is up to date and smaller**: 7-Zip 26.04, zlib 1.3.2,
  SFMT 1.5.1, LuaJIT 2.1 in place of Lua 5.1.  7-Zip and `lua51.dll` are
  loaded at run time instead of being linked in, so the executable starts on a
  system that has neither.

## Compatibility

This is the part that matters for a re-recording fork, so it is stated
plainly:

* **Existing savestates and movies load and save exactly as before.**  The
  savestate container is the same gzip stream it always was, the [VBM movie
  header](https://tasvideos.org/EmulatorResources/VBA/VBM) is the same 64
  bytes, and no flag was renumbered.
* **The emulation timing, the RTC behaviour, the Lua random stream and the
  re-recording semantics are unchanged.**  The SFMT upgrade was verified to
  produce a bit-identical stream for 200 seeds x 1000 draws, with and without
  SSE2, so no replay can change because of it.
* **Registry values keep their meaning.**  New video sizes are appended to the
  enum, never inserted, so the sizes already stored in the registry still mean
  what they meant.
* **The executable still starts on old Windows.**  Every optional library -
  `D3D11.DLL`, `D3DCOMPILER_47.DLL`, `XAUDIO2_9.DLL`, `DXGI.DLL`, `7z.dll`,
  `wgpu_native.dll`, `mfplat.dll`, `mfreadwrite.dll` - is resolved at run time
  with `LoadLibrary`, and each one
  degrades gracefully: a missing DLL leaves its menu entry out, the renderer
  falls back to the next one, and the emulator carries on.  The last step of
  that chain is the GDI renderer, which imports nothing but GDI and USER and
  therefore always starts.  Nothing is imported statically, so the program
  still starts where the library is not installed.
* The one exception is the **Windows Imaging Component** the screenshots go
  through, which needs the codecs that Windows 7 and later ship with.  The
  Media Foundation recorder is in the same position: it needs the sink writer
  of Windows 7 and later, and where Media Foundation is absent the encoder
  list comes up empty and starting a recording says why.
* **Windows 7 or later is what we recommend**, and Windows 10 or 11 if you want
  the dark mode and the per-monitor DPI handling.  The old systems still run
  the emulator - they just do not get everything.
* **The binaries we ship do not run on Windows XP.**  The *sources* still
  target the XP API level (`_WIN32_WINNT` is still `0x0501`) and still carry
  the `WINXP` compile time switch that compiles the dark mode, the modern file
  dialogs and the SSE2 filter paths out - but the toolchain has moved on, and
  the import libraries and the static CRT of a current Visual Studio are
  Windows 7 era.  A package that still runs on XP therefore has to be built
  from these sources by the user, with a toolset that can still target XP.

## Verification

* Win32, x64 and ARM64 build with MSVC 14.52 (VS 18) at `/W3` with no
  warnings, in all twelve V7/V8 x Debug/Release x architecture combinations.
* The CPU core rewrite was checked against the assembly with per macro A/B
  harnesses: 480,000 input states, zero differences.
* The SFMT upgrade was checked the same way: bit-identical between 1.3.3 and
  1.5.1, with and without SSE2.
* The core decoupling was checked with the per-frame hash trace over two V7
  movies, byte-identical in both columns.
* The WebGPU shader ports were checked against the same shaders put through
  glslang and naga: 0/255 in every colour channel, pass by pass and as a whole
  chain, at 1:1 and at 3x.
* The WIC writer was exercised by saving GIF, DDS and HEIF captures from a
  running build.
* The Media Foundation recorder was checked by driving `MFVideoWriter` with
  synthetic frames and reading the result back with ffprobe: H.264 through the
  hardware and the software encoder into MP4, AAC into `.m4a`, MP3 into
  `.mp3`, FLAC into `.flac`, ALAC and Dolby Digital into `.m4a` - each with
  the stream, the frame rate and the duration the settings asked for.
* The frame rate box was checked the same way: at a 30 fps target the 120
  emulator frames became 60 written frames at 179/6 fps and the duration did
  not change, which is what dropping frames instead of duplicating them is
  supposed to look like.
* The H.264 profile box was checked by reading the profile back out of the
  file: Auto and Baseline report `Constrained Baseline`, High reports `High`.
* The background thread was measured against the synchronous path with the
  same synthetic frames: identical output byte for byte, and the time the
  emulation thread spends in the call went from 3.54 ms to 2.19 ms per frame
  at 1440x960.  At 240x160 both are under 0.15 ms, which is why the box
  matters for window-sized recordings and not for raw LCD ones.

## Downloads and resources

### Downloads

**The builds of this branch are in the releases of this repository:**

* <https://github.com/CarterLi/vba-rerecording/releases>
  * `vba-rerecording-win32.zip` - the 32-bit (x86) build
  * `vba-rerecording-x64.zip` - the 64-bit build
  * `vba-rerecording-arm64.zip` - the ARM64 build

Every package carries **both** cores of the emulator - the V7 core
(`VBA-RR-v7.exe`) and the V8 core (`VBA-RR-v8.exe`), which is the one movies
recorded with the V7 core need - together with the libraries they load at run
time, and a `readme.txt` that says which is which.  See
[Which core should I use?](#which-core-should-i-use) above before you pick
one.

**The official releases of VBA-ReRecording, upstream of this branch:**

* <https://github.com/TASEmulators/vba-rerecording/releases>
* Mirrored at <https://files.tasvideos.org/emulatordownloads/vba-rerecording/>
  * `vba-rerecording-svn421-win32.7z` (v23.5)
  * `vba-rerecording-svn438-win32.7z` (v23.5)
  * `vba-rerecording-svn480-win32.7z` (v23.6)
  * `vba-v24m-svn-r422.7z`
  * `vba-v24m-svn-r440a.7z`
  * `vba-v24m-svn-r480.7z`
  * Older builds are in the `old/` folder and the deprecated ones in
    `deprecated/`, next to them.

### Links

* [Lua Scripting Functions](https://tasvideos.org/EmulatorResources/VBA/LuaScriptingFunctions)
* [Lua scripting notes for this branch](documentation/lua-scripting.md)
  - where a script's output actually goes, why it has to advance frames, and
  what a load error looks like
* [VBM movie file format](https://tasvideos.org/EmulatorResources/VBA/VBM)
* [Sync History](https://tasvideos.org/EmulatorResources/VBA/SyncHistory)

---

# The original README

The text below is the README of VisualBoyAdvance 1.7.2 as it was upstream.  It
is kept here for reference: much of it describes the SDL and GTK frontends and
the autotools build, which this branch has removed, and everything that still
applies is covered above.  The Windows specific notes are in
[README-win.txt](README-win.txt), and the release notes of this branch are in
[documentation/VBArr-modernized-changelog.txt](documentation/VBArr-modernized-changelog.txt).

Welcome to version 1.7.2 of VisualBoyAdvance [SDL].
This is a GB/GBC/GBA emulator for Windows, Linux, MacOS X and BeOS.

Features
--------

- configurable GB/GBA keys, including joystick support
- option to use BIOS file
- zip/gzip file support
- directory selection for save state, battery and screen capture
- fullscreen mode (selectable resolution)
- video sizes 1x, 2x, 3x and 4x
- graphic filters Normal, TV Mode, 2xSaI, Super 2xSaI and more
- interframe blending support
- same emulation core as VisualBoyAdvance: GB and GBA emulation
- built-in ARM/THUMB assembly debugger
- 10 save states accesible through keyboard
- automatic battery file load/save
- auto-fire support
- pause, reset through keyboard
- 16, 24 and 32 bit desktop support
- GDB remote debugging (see below for information)
- auto frameskipping and throttle
- AGBPrint support for development
- RTC support

Compiling the sources
---------------------

See the INSTALL file for compiling instructions. Please note the following
requisites to compile:

- GCC must be 3.x or greater in order to compile GBA.cpp with -O2. Earlier
  versions have a problem during optimization that requires an absurd
  ammount of memory and usually ends up crashing the compiler/computer
- On Windows, Microsoft Visual C++ 6 or later is needed. Please note that
  some of the source code will not compile with the shipped header files.
  You will need to install the most recent Platform SDK from Microsoft.

Installing
----------

The easiest installation is to place all files in the distribution in the
same directory.

Per game settings
-----------------

Version 1.5 introduced the support for per game settings for GBA games. You
can defined the following settings on a per game basis by using an INI file
called vba-over.ini in the same directory as the emulator:

rtcEnabled=<0 for false, anything else for true>
flashSize=<65536 or 131072>
saveType=<0 for automatic, 1 for EEPROM, 2 for SRAM, 3 for Flash or 4 for
EEPROM+Sensor>

Use the 4 letter game code to separate settings for each game. Example:

[ABCD]
rtcEnabled=0
flashSize=65536
saveType=0

[ABC2]
rtcEnabled=1
flashSize=131072
saveType=0

Support
-------

Please support VisualBoyAdvance by making a donation. You can donate money
using PayPal (www.paypal.com). Use the contact form to find how you can
send donations. Also, it is recommended that you use the VisualBoyAdvance
forum on www.ngemu.com message board.

FAQ
---

See online FAQ for more information: http://vba.ngemu.com/faq.shtml

Please don't email about what you think it is problem before consulting
the FAQ.

System requirements
-------------------

Windows: PIII 500Mhz machine for GBA emulation. GB emulation requires far less.

Linux, MacOS X, BeOS:
  SDL (>= 1.2.2) runtime library must be installed prior to running the
  program. You can download it from http://www.libsdl.org

Key combinations during emulation
---------------------------------

- F1...F10: load save state 1...10
- Shift+F1...F10: save state 1...10
- Alt+1...4: auto-fire A,B,L,R
- Ctrl+1...8: enable/disable graphical layers (BG0, BG1, BG2, BG3, OBJ, WIN0,
  WIN1, OBJWIN)
- Ctrl+N: pause on next frame
- Ctrl+R: reset
- Ctrl+P: pause
- Ctrl+B: rewind (if enabled and available)
- F11: enter built-in debugger
- ESC: exit emulator

Emulation key settings
----------------------

- Movement: arrow keys
- Button A: Z
- Button B: X
- Button L: A
- Button R: S
- Button Start: ENTER
- Button Select: BACKSPACE
- Speed up: SPACE
- Screen capture: F12
- Motion Left: NUMPAD 4
- Motion Right: NUMPAD 6
- Motion Up: NUMPAD 8
- Motion Down: NUMPAD 2


GDB support
-----------

VisualBoyAdvance now provides GDB remote debugging support. This gives
developers the full power of GDB to debug GBA applications.

In order to use this, you will need a cross-compiled GDB for either the arm-elf
or arm-thumb-elf target (used for the --target= option of GDB configure
script).

You can also use GDB frontends like DDD, CodeMedic, etc... or even GDB/Insight
for GUI debugger (if using anything other than GDB/Insight, please make sure
to point to the right GDB executable).

The emulator provides two transport protocols for remote debugging:

- TCP: allows debugging through TCP using port 55555 (or any specified) port.
  The advantage of using TCP is that it allows true remote debugging but it is
  slower compared to the pipe method (pipe method does not work on Windows -
  probably a restriction imposed by the CYGWIN port of GDB).
- PIPE: allows debugging through a PIPE between the emulator and GDB. This is a
  lot faster than TCP and recommened on Unix systems.

Using TCP transport
-------------------

To use the TCP transport, use the flag -Gtcp[:portnum] where portnum is an
optional port number to be used instead of 55555. VBA will wait for a GDB
connection on the specified port (printed on screen). Start GDB by passing the
.elf file, then connect to the emulator by using the command:

target remote <hostname>:<port number>

where hostname is the host where the emulator is running and port number is the
printed port number.

Using PIPE transport
--------------------

To use the PIPE transport, start GDB with the .elf file to be debugged. Connect
to the emulator by using the command:

target remote |<full path to VBA>/VisualBoyAdvance -Gpipe

Debugging with GDB
------------------

Once you connected to the emulator, you can set breakpoints and debug the
application. But before doing that, you will need to issue the loda command on
GDB to load the code into the emulator. Optionally, you can pass the ELF file
on the emulator's command line (along with the -N option to not parse the debug
information in the emulator) instead of issuing the load command.

After connecting and optionally loading the file into the debugger, you can
start debugging: add breakpoints, step, etc...

While using GDB, any console output (see below) will show up in GDB's console.

If you want to break into the GDB, press F11 and it will give you full command
in GDB again. Pressing ESC will terminate emulation.

You can also detach GDB from the emulator.


Console Output
--------------

There are two forms of console output in this version:

- Mappy style dprint: use the following code (from Mappy's documentation)
  to get output:

- VBA style: use the following code to get output:
  // THUMB code
  void print(char *s)
  {
    asm volatile("mov r0, %0;"
                 "swi 0xff;"
                 : // no ouput
                 : "r" (s)
                 : "r0");
  }
  // ARM code
  void print(char *s)
  {
    asm volatile("mov r0, %0;"
                 "swi 0xff0000;"
                 : // no ouput
                 : "r" (s)
                 : "r0");
  }

When using GDB, the output will show up in GDB's console. When using the
built-in debugger, output will go to standard out.

Built-in debugger enhancements
------------------------------

The built-in debugger has the following enhancements (need debug enabled ELF
file):
- ELF file support: both multiple and regular. Please report any messages or
  problems reading ELF files. C++ classes and some miscellaneous features are
  not supported yet. Also, method names may be mangled in C++ code.
- break command: add a breakpoint on a function, line number of file:line
  number
- locals command: print the local variables on the current function
- print command: prints the value of a given expression. Valid expression
  include *this, ptr->member, var.member, array[0], sizeof(expression), etc...
- symbols command: list information known about a symbol (or symbols that start
  with the given name)
- radix command: sets the output radix to eithe decimal, octal or hex.
- file and line number when stopped: the debugger will show the file and line
  number (if available) for the current address
- fixes to some breakpoint handling problems
- fixes to break on write function

Profiling
---------

VisualBoyAdvance has profiling support. It produces output in the format
supported by GPROF. You need a cross-compiled binary of GPROF (binutils
GNU package) compiled with --target=arm-thumb-elf (or similar).

The output will always contain the histogram of PC positions at the specified
frequency even if the code is not compiled with call graph support.

In order to enable profiling, you need to add a call inside your code
to start it (download vba.s for a ready to use file for GCC):

void monstartup(u32 lowPC, 32 highPC)

This will start profiling for the given address region. If the code was also
compiled with -pg, call graphs will be recorded.

Other calls for fine grain control:

// control profiling
// 0 - stops profiling
// anyother value starts it
void moncontrol(int mode)

// stop profiling and output gmon.out file
void moncleanup()

// record call graph (used by GCC)
// r12 contains previous caller
// r14 contains function that was called
void mcount()

GCC bugs
--------

All versions of GCC previous to the working version 3.3 have bugs when
generating profiling for thumb functions.

The errors are:
- mov\tip,lr not a valid instruction (typo in gcc/config/arm/arm.h)
- LPxx undefined when linking (missing . before LP causes the problem)

You can fix these problems by either recompiling GCC (recommended) or by
modifying cc1.exe to fix the problems.

In the first option, find the given code in gcc/config/arm/arm.h and fix
the typos (remove the extra blackslash from the mov ip,lr instruction and
add the missing dot before LP%d) or change THUMB_FUNCTION_PROFILER to call
ARM_FUNCTION_PROFILER (the recent changes that were performed in CVS).

If you don't want to compile your own GCC, then you can use an hex editor
to fix the problems. Locate cc1.exe under <dir>/lib/gcc-lib/<config>/<version>
and find the mov\tip,lr instruction. Change the extra backslash to a space.
Locate the .LP string just before the mov instruction and change it to
.P instead (make sure to place 0 after the P). Then find the text .word
LP%d and change it to .word .P%d.

Histograms will be output even if the code is not compiled with call graph
support.

Options configuration
---------------------

All configurable options are accessible from the configuration file
VisualBoyAdvance.cfg.

This file is searched in this order:

- current directory
- user home directory (defined by HOME environment variable)
- user profile directory (Windows only defined by USERPROFILE directory)
- executable directory (either relative or defined by PATH variable)

All options are documented in the file supplied with this distribution.

Command line options (override settings in configuration file)
--------------------------------------------------------------

  -1, --video-1x               1x
  -2, --video-2x               2x
  -3, --video-3x               3x
  -4, --video-4x               4x
  -F, --fullscreen             Full screen
  -G, --gdb=PROTOCOL           GNU Remote Stub mode:
                                tcp      - use TCP at port 55555
                                tcp:PORT - use TCP at port PORT
                                pipe     - use pipe transport
  -N, --no-debug               Don't parse debug information
  -S, --flash-size=SIZE        Set the Flash size
      --flash-64k               0 -  64K Flash
      --flash-128k              1 - 128K Flash
  -T, --throttle=THROTTLE      Set the desired throttle (5...1000)
  -Y, --yuv=TYPE               Use YUV overlay for drawing:
                                0 - YV12
                                1 - UYVY
                                2 - YVYU
                                3 - YUY2
                                4 - IYUV
  -b, --bios=BIOS              Use given bios file
  -c, --config=FILE            Read the given configuration file
  -d, --debug                  Enter debugger
  -f, --filter=FILTER          Select filter:
      --filter-normal            0 - normal mode
      --filter-tv-mode           1 - TV Mode
      --filter-2xsai             2 - 2xSaI
      --filter-super-2xsai       3 - Super 2xSaI
      --filter-super-eagle       4 - Super Eagle
      --filter-pixelate          5 - Pixelate
      --filter-motion-blur       6 - Motion Blur
      --filter-advmame           7 - AdvanceMAME Scale2x
      --filter-simple2x          8 - Simple2x
      --filter-bilinear          9 - Bilinear
      --filter-bilinear+        10 - Bilinear Plus
      --filter-scanlines        11 - Scanlines
      --filter-hq2x             12 - hq2x
      --filter-lq2x             13 - lq2x
  -h, --help                   Print this help
  -i, --ips=PATCH              Apply given IPS patch
  -p, --profile=[HERTZ]        Enable profiling
  -s, --frameskip=FRAMESKIP    Set frame skip (0...9)
  -t, --save-type=TYPE         Set the available save type
      --save-auto               0 - Automatic (EEPROM, SRAM, FLASH)
      --save-eeprom             1 - EEPROM
      --save-sram               2 - SRAM
      --save-flash              3 - FLASH
      --save-sensor             4 - EEPROM+Sensor
      --save-none               5 - NONE
  -v, --verbose=VERBOSE        Set verbose logging (trace.log)
                                  1 - SWI
                                  2 - Unaligned memory access
                                  4 - Illegal memory write
                                  8 - Illegal memory read
                                 16 - DMA 0
                                 32 - DMA 1
                                 64 - DMA 2
                                128 - DMA 3
                                256 - Undefined instruction
                                512 - AGBPrint messages

Long options only:
      --agb-print              Enable AGBPrint support
      --auto-frameskip         Enable auto frameskipping
      --ifb-none               No interframe blending
      --ifb-motion-blur        Interframe motion blur
      --ifb-smart              Smart interframe blending
      --no-agb-print           Disable AGBPrint support
      --no-auto-frameskip      Disable auto frameskipping
      --no-ips                 Do not apply IPS patch
      --no-mmx                 Disable MMX support
      --no-pause-when-inactive Don't pause when inactive
      --no-rtc                 Disable RTC support
      --no-show-speed          Don't show emulation speed
      --no-throttle            Disable thrrotle
      --pause-when-inactive    Pause when inactive
      --rtc                    Enable RTC support
      --show-speed-normal      Show emulation speed
      --show-speed-detailed    Show detailed speed data

Known bugs
----------

- loading a save state that uses a different sound quality may hang the
  emulator. Please only use the 22Khz sound quality save states from the
  Windows version with this release
- built-in debugger still has a few bugs
- disassembler contains a few errors. Please report anything wrong you find

LICENSE
-------

    VisualBoyAdvance - a Gameboy and GameboyAdvance emulator
    Copyright (C) 1999-2003 Forgotten
    Copyright (C) 2004 Forgotten and the VBA development team

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

This program also contains code developed by the University of
California and is subject to the extra conditions:

Copyright (c) 1991, 1998 The Regents of the University of California.
All rights reserved.
 
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. [rescinded 22 July 1999]
4. Neither the name of the University nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.
        
Contact
-------

Please don't email unless you found some bug. Requests will be ignored and
deleted. Also, be descriptive when emailing. You have to tell me what version
of the emulator you are writing about and a good description of the problem.
Remember, there are several interfaces (Windows, SDL and GTK+) and
several systems (Windows, Linux, MacOS X and BeOS).

Also, there are still people writing about the old VisualBoy which is no longer
supported. Also remember I am not paid to work on VisualBoyAdvance. This is
just a hobby.

Forgotten (http://vba.ngemu.com/contact.shtml)
kxu <kxu@users.sourceforge.net>

http://vba.ngemu.com
http://sourceforge.net/projects/vba

Special Thanks
--------------

PokemonHacker for all his help improving the emulator.
Costis for his help fixing some of the graphics bugs.
Snes9x developers for the great emulator and source code.
Kreed for his great graphic filters.
SDL team for this amazing library.
And all users who kindly reported problems.
