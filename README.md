# VBA-ReRecording — Modernized Edition

![build](https://github.com/CarterLi/vba-rerecording/actions/workflows/build.yml/badge.svg)

**VBA-ReRecording** is the re-recording branch of VisualBoyAdvance 1.7.2, a Game Boy /
Super Game Boy / Game Boy Color / Game Boy Advance emulator for Windows. It exists for one
reason: making and replaying *tool-assisted movies*. Save states, movie record and replay,
re-recording, Lua scripting and the debugger are what that needs.

This branch is that emulator, kept alive. The Windows build no longer asks for Visual
Studio 2005, the picture is no longer stretched to whatever shape the window has, the sound
no longer sits behind a 150 ms buffer by default, and the dialogs no longer wear a 2004
bitmap font. Underneath, **the emulation is deliberately untouched**, because the recordings
are the point: the movie format, the savestate format, the frame timing, the RTC, the random
stream Lua sees and the re-recording semantics are all exactly what they were. A movie that
synced before still syncs.

This is not an attempt to move you to another emulator. It is the one you already use,
still working, on the Windows you actually have.

The short version, for the impatient:

* The picture is no longer stretched, the window goes up to 12x, and the sharpening and
  upscaling run on the GPU instead of costing you frames.
* **Record straight to MP4** with the encoder your machine already has — the graphics card's
  when there is one — at the window size and with the filter you were watching: no raw frames,
  no third-party tool.
* **Edit a movie's key timeline** — a TAStudio-style frame grid, and every change goes
  straight back into the `.vbm`.
* Dark mode, sharp text on any monitor, and file dialogs from this decade.
* Switch controllers work, controllers can be plugged in while the emulator runs, and cheats
  can explain themselves.
* **As many Lua scripts as you want, each in its own window** — a HUD, a macro pad and a
  memory watch no longer replace one another.
* Timings, savestates, movies and Lua behaviour: unchanged, on purpose.

## Start here: which `.exe` do I run?

Every download carries both cores, and they are **not** interchangeable:

* **`VBA-RR-v7.exe` — the V7 core.** The original VBA core. **The vast majority of existing
  VBM movies were recorded with it.** Unless you have a reason not to, this is the one.
* **`VBA-RR-v8.exe` — the V8 core.** Came from VBA-M. It fixes a number of emulation bugs —
  a good part of them in the GB core, plus a few in the GBA core, such as the well known
  crash of the tree boss in the Blue Moon edition of Rockman EXE 4 — at the cost of some
  extra lag.

**If you are recording a GBA movie, use V7.** The V8 core keeps the old, excessively laggy
GBA timing (it has no prefetch hack), so the game lags more than it should and a movie
recorded on it does not match one recorded on V7.

You cannot get this wrong by accident: **File → Play Movie** shows the core the selected
movie was recorded with and marks a mismatch in red as "desync expected", before you start.
The movie still plays — it is just not guaranteed to stay in sync.

## Download and install

The builds are in the releases of this repository:
<https://github.com/CarterLi/vba-rerecording/releases>

| Package | For |
| --- | --- |
| `vba-rerecording-win32.zip` | 32-bit Windows, and anything you are in doubt about |
| `vba-rerecording-x64.zip` | 64-bit Windows — the one to take on a normal PC |
| `vba-rerecording-arm64.zip` | Windows on ARM (Snapdragon laptops, some tablets): a native build, not an emulation of x86 |

Each package carries both cores, the libraries they load at run time, and a `readme.txt`
that says which is which. Unpack one package into a folder and run either exe. There is no
installer and no runtime to install: the emulator is the folder.

## What changed, and what it buys you

### The picture

* **It is no longer stretched.** **Video → Screen Scaling** offers *stretch*, *keep aspect*
  and *keep aspect with an integer factor*; the window and fullscreen both follow it, so a
  3:2 picture stays 3:2 instead of being pulled into whatever shape the window happens to
  have. The bars are the window's own background, and the picture stays inside the monitor
  in fullscreen even with the menu bar shown.
* **Window sizes up to 12x** — 1x to 8x, plus 9x to 12x — and `-videoScale 1` to `12` on the
  command line.
* **Upscaling on the GPU.** NVIDIA Image Scaling and AMD FidelityFX Super Resolution 1.0,
  with a shared sharpening amount (0 / 25 / 50 / 75 / 100 %); the libretro xBR family (xBR
  lv2, xBR lv2 without the blend, xBR lv3, xBR lv3 multipass, 2xBR, xBR-mlv4, super-xBR,
  ScaleFX, PT SkyWalker541, xBRZ-Freescale) and Lanczos for a smooth enlargement. They run
  while the frame is being stretched, so **a sharper picture costs no speed and no audio** —
  the emulation thread no longer runs a filter.
* **Max Scale** caps the ratio the upscalers render at (2x to 8x), so an 8x window does not
  smear a GBA pixel into a block of noise.
* **New software filters**: xBRZ 2x to 5x, HQ4x and ScaleFX 3x. The scanline, bilinear,
  AdMame, 2xSaI, pixelate and Simple filters are still there with the same output, only
  faster.
* **GB/GBC LCD colour correction**, with the AGB and CGB profiles, baked into the palette
  tables — every renderer shows it, and the GBA and GB/GBC variants are separate menu
  entries that are remembered.
* **A renderer you can actually choose.** Direct3D 11 is the default, Direct3D 9Ex the
  alternative, and GDI the one that always works: it needs no DirectX at all, so it is what
  runs over remote desktop, in safe mode and on a VM with the basic display adapter.
* **The defaults are current**: Direct3D 11, XAudio2, a 2x window and 44 kHz audio, instead
  of DirectDraw, DirectSound, 1x and 22 kHz.
* **The Video menu now tells the truth** about what you got — "Direct3D 11 (Flip)",
  "Direct3D 9Ex", the presentation model, the WebGPU backend behind it where there is one.
  No more guessing from the trace log.
* OpenGL 3.3 and WebGPU renderers exist as well; the official builds leave them off, and a
  build that has them shows them in the Video menu.

### The sound

* **XAudio2 is the default backend**, loaded at run time and degrading to DirectSound if it
  is missing.
* **The sound menu names the buffering**: "DirectSound - 9 blocks, 150 ms" next to "XAudio2
  (2.9) - 4 blocks, 67 ms", with any dropouts appended. For once you can see what each
  choice costs *before* you pick it.

### Recording: a movie straight to video, the way you were watching it

This is the part built for people who record:

* **Tools → Start/Stop Audio/Video File Recording…** asks for the encoder first, then for a
  file name. The list is built from the encoders the machine actually has, hardware ones
  first and labelled with the vendor — a machine with NVENC offers "NVIDIA H.264 Encoder
  MFT / Hardware", a machine without it simply does not. The audio box works the same way:
  AAC, MP3, FLAC, ALAC, Opus and Dolby Digital appear only when an encoder for them is
  installed.
* **The container is not a choice, it is a consequence.** H.264, HEVC or AV1 with any audio
  goes into `.mp4`; "None (audio only)" gives `.m4a`, `.mp3` or `.flac` depending on the
  codec. The save dialog's filter and default extension follow, so you cannot end up with an
  extension that does not match the stream.
* **The bit rate is a number, not a word** — Low/Medium/High presets (2000/4000/8000 kbps
  for the picture, 64/128/192 for the sound) next to the box. H.264 gets a profile box
  (Auto/Baseline/Main/High), and recording *below* the emulator's own frame rate **drops**
  frames instead of duplicating them, so the duration of the file stays right.
* **Encoding runs on its own thread.** A hardware encoder still blocks when it takes a
  sample, and that wait used to land on the emulation thread. With the box ticked the
  emulator only hands the frame off and carries on: if the encoder falls behind, video frames
  are dropped and the timestamps keep moving, so the duration stays right — and the sound is
  never dropped, because a missing block would shorten the track and pull the picture out of
  sync. It matters at window size with a filter on; at the raw 240x160 it changes nothing.
* **Tools → "Record at Window Size and Filter"** writes the picture as you were watching it:
  the window magnification and the active pixel filter are already applied, instead of the
  raw LCD buffer being dumped. A VBM recording therefore exports straight to a video that
  looks like the emulator looked, with no third-party enlargement afterwards. The same
  option applies to screenshots, so a capture is exactly the frame the video would hold.
* **The old flow is still there**, under **Tools → Legacy AVI Recording**: Video for Windows
  with the `AVISaveOptions` codec picker, and the WAV recorder, both unchanged. They live in
  a submenu because that flow runs the other way round — save the file first, choose the
  codec second.

### Editing a movie's key timeline

**Tools → Movie → VBM Editor…** shows the open `.vbm` as a frame grid — one row per frame,
one column per pad button, an `HH:mm:ss.SS` stamp, an arrow on the row being played. Click a
cell to press or release that button, right-click to insert, delete or mark a row; every
change is written straight back into the `.vbm`. The buttons at the top are the File menu's
pause and frame advance, and a box jumps to a typed frame number.

It edits the recording, never the emulator — no pause, reset, seek or state load — so a frame
before the playhead can only matter from the next playback on. Movies open read-only; turn
that off first, then untick the editor's own box. The grid is a virtual list, so a three-hour
movie opens as fast as a ten-second one. Notes:
[documentation/vbm-editor.md](documentation/vbm-editor.md).

### Screenshots

* **Screenshots go through the Windows Imaging Component**, so the format menu lists the
  encoders the machine actually has — PNG, JPEG, GIF, TIFF, DDS, HEIF, JPEG XL and whatever
  else is installed — instead of PNG or BMP and nothing else. The encoder's own pixel format
  is honoured, and a missing HEIF or JPEG XL codec says so instead of failing generically.
* A failed write names the step that failed instead of "Error creating file".

### The window and the dialogs

* **Dark mode**: the title bar and the menu bar follow the system setting.
* **The system UI font** instead of the 2004 bitmap face, so the text follows the system
  language; a **true type monospaced face** for the machine generated text in the
  disassembly, memory, I/O and cheat views; and an option that brings the old bitmap look
  back for layouts that do not fit.
* **Per-monitor DPI awareness (V2)**: dialogs scale their children and rebuild their fonts
  when the window moves to a monitor with another scale, and the game keeps its pixel size.
  Text is sharp instead of a 96 DPI bitmap stretched across the screen.
* **The current file dialogs** (the Vista+ common item dialogs) for every file and folder
  picker, instead of the XP-era ones.
* The **memory viewer's address** can be read and copied, **RAM Watch and RAM Search** divide
  their columns evenly, the **About box** names the build and points at the repository, and
  the **title bar shows the Git revision** instead of `svn0`.
* **Nothing new is a hard requirement.** Every optional library is loaded at run time and
  every new feature degrades gracefully when it is missing, so an old system does not crash
  — it just does not get the new renderer.

### Controllers

* **Nintendo Switch controllers work.** The Switch pad declares its buttons under the simple
  report only; it used to enumerate, be acquired and then report the defaults for ever, so
  the setup dialog captured nothing and the pad did not work in game either.
* **DirectInput 8**, and the device list can be rebuilt while the emulator runs: a pad that
  woke up, was plugged in or was paired after the start now shows up. One that arrives or
  goes away says so, on the game screen and in the trace.
* **A dead zone of our own**, so a stick that rests a little off centre does not hold a
  direction down.
* **The pad setup dialog** gains a **Clear** button (a key bound by accident could not be
  taken off again before) and a **Gamepads** page that lists what each pad answers to.
* **Hold-to-autofire**: an auto-fire key can be bound to a button, and holding it toggles
  that button's auto-fire — the Alt+0-9 menu commands, but with a key.

### Cheats

* GBA cheats are **grouped** by description and type, with expand and collapse headers,
  **Type and Status** columns, native checkboxes, grouped edit and remove, and batch input
  that is validated before it is applied. The grouping is optional, and both checkboxes have
  tooltips that explain what they do.
* **A pasted code can explain itself.** With one row selected, **Explain** opens a window
  that says what the code does, line by line: which address gets which value, which key has
  to be held, which condition has to hold for the lines below it to run. A selected group is
  explained as a whole, with the family and the line count at the top and the cheat's own
  description underneath. All 116 instruction types are covered, and a line that carries the
  *data* of the line above it — the second line of a slide, a ROM patch, a group write, a
  super code — is shown as data instead of being read as an instruction of its own.

### Scripts

* **Lua runs on LuaJIT 2.1**, so the scripts you already have keep working and the heavy ones
  run a lot faster. `lua51.dll` is still the file that decides: drop the original Lua 5.1 DLL
  in its place and everything keeps working, only `bit`, `jit` and `ffi` go away with it.
* `vba.screenshot()` takes a capture from a script.
* **As many script windows as you like, side by side.** Each window runs its own script and
  they no longer replace one another: **File → Lua Scripting → New Lua Script Window...**
  opens a new one every time, and dropping a `.lua` file on the emulator opens one with that
  script already running. A script that fails stops itself and leaves the others alone, and
  **Close All Script Windows** closes the lot.
* **A script can print Chinese.** Save the `.lua` file as UTF-8 **with a BOM** and `print()`,
  error messages and pop-ups come out as written. Without a BOM the text is read in the
  system code page, exactly as before — Lua cannot tell the two apart, so the BOM is the only
  way to say it.

## Your existing recordings

The point of the whole branch, so plainly:

* **Save states and movies load and save exactly as before.** The savestate container is the
  same gzip stream it always was, the [VBM movie
  header](https://tasvideos.org/EmulatorResources/VBA/VBM) is the same 64 bytes, and no flag
  was renumbered.
* **Frame timing, RTC behaviour, the Lua random stream and the re-recording semantics are
  unchanged.** The random number generator was updated to a newer version of the same
  library, and that was verified to produce a bit-identical stream, with and without SSE2 —
  no replay can change because of it.
* **Your settings carry over.** The same registry values are read, so hotkeys, filters and
  directories set by an older build still apply, and new video sizes are appended to the
  list rather than inserted — a size stored before still means what it meant.
* **The 64-bit build computes the MBC3 clock the same way the 32-bit one did**, down to the
  32-bit wraparound, because otherwise every recording made on the old build would have
  desynchronized on the new one.
* **One honest exception**, and it is narrow: a recording that starts from **SRAM** on an
  **MBC3 cartridge with a real time clock** can read a different in-game date than it used
  to. The old value was read from uninitialised memory — it was never a defined date to
  begin with. A recording that starts from a **savestate** is not affected at all, because
  the clock state is part of the savestate.
* **It is checked, not just claimed.** A per-frame hash of the emulator state (`-hashtrace`)
  can show that a build has not moved the emulation at all; two V7 movies over 4001 frames
  come out byte for byte identical.

## Things that were broken and are not any more

**Picture**

* With the pixel filter set to None, the GDI renderer blitted an uninitialised buffer: the
  picture was whatever `malloc` happened to return — a black window with the on screen text
  on it.
* Closing a ROM left the last frame on screen.
* Switching the render method rebuilt the main window and flashed the whole frame white.
* The title bar collapsed to a single character as soon as the speed indicator wrote to it.
* The 16 bpp interpolation gave the wrong colours.

**Hotkeys and input**

* The hotkey configuration was parsed out of a buffer far past its end, so hundreds of
  phantom entries shadowed the real ones and keys such as F1, F3 and F4 intermittently
  stopped loading save states.
* The input button sets were 16 bits wide while speed, capture and GameShark live above bit
  15, so those three buttons were truncated away.

**Lua**

* A script that did not load said nothing at all: no window, no message, the Run button
  still saying "Run". A missing file or a syntax error now names the file and the line, and
  pressing **Run** with no game loaded says so instead of doing nothing silently.
* A script saved as UTF-8 **with a BOM** was refused with a syntax error, because Lua 5.1 has
  no idea what a BOM is. Such a file loads now, and its Chinese prints as Chinese.

**Emulation**

* The GB 32-bit memory write dropped its top byte, so Lua's `memory.writedword()` did not
  write what it was asked to, and `memory.readdword()` could not read back what
  `writedword()` had written. Only scripts reach this path; the cores never call it.
* `gui.gdscreenshot()` could call an uninitialised colour conversion pointer at a colour
  depth the emulator does not use. Nothing changes at the depths it does use.
* The flash chip IDs are now derived from the restored save size, so a 128K save loaded into
  a session set up as 64K reports what the state says instead of pretending to be a 64K chip.
  The common 64K case is bit for bit unchanged.

## Requirements

* **Windows 7 or later** is what we recommend; **Windows 10 or 11** if you want the dark mode
  and the per-monitor DPI handling. Older systems still run the emulator — they just do not
  get everything.
* The screenshot path needs the imaging codecs Windows 7 and later ship with, and the
  recorder needs the Media Foundation sink writer of Windows 7 and later. Where Media
  Foundation is absent the encoder list comes up empty and starting a recording says why.
* **The shipped binaries do not run on Windows XP.** The sources still target the XP API
  level and still carry the `WINXP` switch, but the current Visual Studio toolchain does not
  produce XP-compatible binaries. An XP package has to be built from these sources with a
  toolset that can still target XP.

## Controls and default keys

During emulation: `F1`–`F10` load state 1–10, `Shift+F1`–`F10` save them, `Ctrl+F1`–`F10`
reopen a recent ROM, `Alt+0`–`9` toggle auto-fire (A, B, L, R, Start, Select, and the four
directions), `Ctrl+1`–`8` show/hide the graphic layers, `Ctrl+N` pause on the next frame,
`Ctrl+P` pause, `Ctrl+R` reset, `Ctrl+B` rewind (when enabled), `F11` debugger, `F12`
screenshot, `Esc` show/hide the menu bar, `Ctrl+X` exit.

Default pad: arrows move, `Z` A, `X` B, `A` L, `S` R, `Enter` Start, `Backspace` Select,
`Space` speed up, numpad `4`/`6`/`8`/`2` for the motion sensor. Everything is rebindable in
the input options.

Per-game settings still come from `vba-over.ini` next to the emulator, keyed by the four
letter game code:

```ini
[ABCD]
rtcEnabled=0
flashSize=65536
saveType=0
```

## Links

* [Releases](https://github.com/CarterLi/vba-rerecording/releases)
* [TASVideos emulator resource page](https://tasvideos.org/EmulatorResources/VBA) — the
  canonical entry point for this emulator
* [VBM movie file format](https://tasvideos.org/EmulatorResources/VBA/VBM)
* [Sync History](https://tasvideos.org/EmulatorResources/VBA/SyncHistory)
* [Lua Scripting Functions](https://tasvideos.org/EmulatorResources/VBA/LuaScriptingFunctions)
  and [notes for this branch](documentation/lua-scripting.md) — where a script's output
  actually goes, why it has to advance frames, what a load error looks like, and how to get
  Chinese out of `print()`
* [Release notes of this branch](documentation/VBArr-modernized-changelog.txt) and the
  [Windows specific notes](README-win.txt)

## Building it yourself

`VBA_Latest.sln` / `VBA_Latest.vcxproj`, a current Visual Studio (MSVC 14.52 or later),
C++17, SSE2, `/W3` clean on Win32, x64 and ARM64 — twelve configurations, V7/V8 ×
Debug/Release × three architectures. The renderers sit behind compile time switches in
`src/win32/*Config.h`, so a build can leave one out or put it back. CI builds the three
release packages, one runner per architecture. Design notes live in
[documentation/](documentation/).

## License

GPL version 2 or later — the full text is in [COPYING](COPYING). The copyright notices for
VisualBoyAdvance, VBA-M and VBA-rr, the licences of the bundled libraries, and the extra
conditions for the code developed by the University of California are in
[documentation/License.txt](documentation/License.txt).
