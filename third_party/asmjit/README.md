# asmjit (vendored)

Machine-code assembler used by the JIT's native backends (`src/gba/jit/JitEmitX64.cpp`).

- Upstream: <https://github.com/asmjit/asmjit>
- Commit vendored: `f566238` ("[bug] Fixed cross-section fixups ignoring an out of range displacement")
- License: Zlib (`LICENSE.md`), compatible with this project's GPL-2.0.

## What is here

The whole `asmjit/` include tree plus the non-test sources: `core/`, `x86/`, `arm/`,
`axl/`, `ujit/`.  Files ending in `_test.cpp` were stripped, as were the
`.github/`, `tools/`, `db/` and `asmjit-testing/` directories.  Nothing else was
modified, so re-vendoring is a copy:

```
robocopy <asmjit-repo>\asmjit third_party\asmjit\asmjit /S /XF *_test.cpp
copy <asmjit-repo>\LICENSE.md third_party\asmjit\LICENSE.md
```

Then update the commit hash above.

## How the project builds it

- Include path: `third_party/asmjit` (relative, from `win32/`), so upstream's own
  `#include <asmjit/core.h>` spelling works unchanged.
- `ASMJIT_STATIC` is defined project-wide: asmjit's public classes are otherwise
  marked `dllimport` on MSVC.
- The `.cpp` files are compiled only in the x64 configurations — the backends are
  architecture specific and the Win32/ARM64 builds do not need them yet. See the
  `ClCompile` entries with a `'$(Platform)'=='x64'` condition in
  `win32/VBA_Latest.vcxproj`.
- asmjit is built with the project's own flags (`/std:c++17 /MT /GL`). It does not
  need or use MFC.
