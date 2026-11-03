#ifndef VBA_JIT_CACHE_H
#define VBA_JIT_CACHE_H

#include "../../Port.h"

struct JitBlock;

// Debug aid, off in normal builds: when set to 1 *and* both this file and the
// two interpreter loops (src/gba/V8/GBA-arm.cpp, GBA-thumb.cpp) are compiled
// with it, every executed guest PC plus lr/sp/cpuTotalTicks is appended to the
// file named by the environment variable VBA_JIT_PCTRACE.  Running the same ROM
// once with and once without -jit and diffing the two files gives the first
// instruction where the block path and the interpreter disagree - that is how
// the 4-vs-2-byte exception-return bug in doAlu() was found.
#define JIT_PC_TRACE 0

// ---------------------------------------------------------------------------
// Block cache and dispatch: the piece that lets CPULoop call the recompiler
// one basic block at a time instead of one instruction at a time.
//
// Everything here is gated on `jitEnabled`, which is off by default.  With it
// off, CPULoop runs exactly the code it always ran; the only added cost is one
// predictable branch per dispatch.
//
// jitExecute() has armExecute()'s contract on V8:
//
//   * run until an event is due (cpuTotalTicks >= cpuNextEvent), until the CPU
//     switches instruction set, or until holdState / SWITicks;
//   * accumulate into cpuTotalTicks itself, leaving clockTicks alone;
//   * return 0 only when the emulator has to stop (the debugger's BKPT path).
//
// V7 has no armExecute() to fall back to (its interpreter is #included straight
// into CPULoop), so there it returns the cycle count instead and CPULoop assigns
// it to clockTicks.  See the declaration below.
//
// The backend is still the IR interpreter (JitInterp.cpp), so turning this on
// today makes the emulator slower, not faster.  What it does buy is the block
// machinery - block cache, page versioning, prefetch refill, per-instruction
// event checks - being exercised for real, so a machine-code backend can be
// dropped in later without touching any of it.
//
// Known deviations from the interpreter, both inherent to running blocks:
//
//   * the interpreter fetches opcode words two instructions ahead, so code that
//     rewrites the instruction it is about to run executes the stale word.  A
//     block is decoded from memory when it is first entered, so the new word is
//     used immediately.  Code that jumps right after patching itself is
//     unaffected (the branch refetches anyway).
//   * an instruction that writes a block's own page bumps that page's version,
//     which invalidates the block for the *next* entry; the rest of the
//     currently running block still comes from the cache, like the
//     interpreter's prefetched words would.
// ---------------------------------------------------------------------------

// Set from -jit on the command line.  Kept as a plain global so CPULoop's test
// is a byte load; jitSetEnabled() exists for the side effects.
extern bool jitEnabled;

struct JitStats
{
	u32 blocksDecoded;		// cache misses that produced a block
	u32 blocksRun;			// block entries that reached the runner
	u32 cacheHits;			// entries served from the cache
	u32 pageInvalidations;	// 1 KB pages bumped because code was written
	u32 badInsns;			// jitInterpInsn() returned <= 0 (must stay 0)
	u64 insns;				// guest instructions retired through the IR interpreter
	u32 nativeBlocks;		// blocks that got machine code (V7 x64 only)
	u64 nativeInsns;		// guest instructions the machine code covered
	u32 nativeCompiles;		// asmjit compilations (the expensive part)
	u32 codeHits;			// re-decodes that reused an existing compilation
	u64 compileTicks;		// rdtsc ticks spent inside jitCompileBlock
};

// True on every platform and for both cores.  Win32 (x86) still has no
// machine-code backend, but the IR interpreter below runs everywhere, so the
// option is available there too.
bool jitAvailable();

// V7 only: set when a block stopped because the next instruction has to be
// executed by CPULoop's inline interpreter (an IO store, SWI, UNDEF, an
// ALU write to PC with S=1, an MSR that writes CPSR).  CPULoop runs exactly one
// interpreted instruction and clears it.
extern bool jitForceInterp;

// Turn the recompiler on or off.  Toggling clears the cache.
void jitSetEnabled(bool on);

// Drop every cached block.  Call on reset and after loading a state; the page
// version counters are left alone (they only have to differ from the versions
// the cached blocks recorded, and their tags are gone anyway).
void jitResetCache();

// A store landed at `address` (`size` bytes, 1/2/4).  Bumps the page version
// when that page ever held a compiled block.  Cheap enough to call per store,
// but the caller checks the 0x02/0x03 region first.
void jitCodeWritten(u32 address, u32 size);

// Replacement for the armExecute()/thumbExecute() call in CPULoop.  Falls back
// to the interpreter when the JIT is off, when the block at armNextPC cannot be
// decoded, and when a debugger or a Lua EXEC hook is active.
//
// V7 contract (the V8 one is unchanged): returns the cycles executed, or -1
// when nothing was executed and CPULoop should run the inline interpreter.
// A return of 0 with jitForceInterp set means "run one interpreted instruction
// next"; CPULoop assigns the value to clockTicks either way.
int jitExecute();

// Test hook: run one decoded block exactly the way jitExecute() does, without
// touching the cache.  Returns the cycles it executed (V8 accumulates into
// cpuTotalTicks instead and returns 0) and, on V7, sets *needInterp when the
// block stopped on an instruction that only the interpreter may run.  The block
// passes in JitVerify.cpp use it.
int jitRunBlockForTest(const JitBlock *block, bool *needInterp);

void jitGetStats(JitStats *out);

// Debug aid: when the environment variable VBA_JIT_PCTRACE names a file, both
// the recompiler's block runner and the interpreter's loops append the guest PC
// of every instruction they execute to it (binary u32, capped).  Comparing the
// two files shows the first instruction where the two paths diverge.  Costs one
// predictable branch per instruction when no trace is open.
void jitTracePc(u32 pc);


#endif // VBA_JIT_CACHE_H
