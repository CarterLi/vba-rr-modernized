#ifndef VBA_HASH_TRACE_H
#define VBA_HASH_TRACE_H

// ---------------------------------------------------------------------------
// HashTrace - per-frame state hashing, used to prove that a refactor did not
// change emulation behaviour (regression baseline).
//
// Design notes:
//   1. Disabled by default. When disabled it costs one empty call per frame.
//   2. It does not diff symbol tables and does not rely on "remembering which
//      memory regions matter": it hashes the full 400000-byte blob returned by
//      emulator.emuWriteMemState(), which covers CPU registers, timers, DMA,
//      sound channels and all memory. Any behavioural drift shows up here.
//   3. The buffer is zeroed before every write, so the unused tail is always 0
//      and the hash stays reproducible (emuWriteMemState returns success only,
//      not the number of bytes written).
//   4. This file lives in common/ and uses no MFC or Windows types, so the wx
//      frontend can reuse it unchanged.
//
// WARNING: the two builds being compared must use the *same* savestate layout.
// If a refactor changes the savestate format, do that in a separate, separately
// verified pass - never mixed with a behaviour-preserving refactor.
// ---------------------------------------------------------------------------

// Start tracing into the given CSV path. Repeated calls are ignored.
void hashTraceEnable(const char *path);

// True while a trace file is open.
bool hashTraceEnabled();

// Called once per frame (driven by systemFrameBoundaryWork).
void hashTraceFrameBoundary();

// Flush and close. Also runs automatically via atexit().
void hashTraceClose();

// ---------------------------------------------------------------------------
// PC trace - a debugging aid for "the same code behaves differently in Debug
// and Release".  This is what found the 2026-11 busy-prefetch bug: two builds
// agreeing on every instruction but charging different cycle counts.
//
// The hook in the CPU loops is compiled out by default.  To use it:
//
//   1. set VBA_PC_TRACE_HOOK to 1 at the top of GBA-arm.cpp and GBA-thumb.cpp
//      (they do not include this header - common/ must not know about the GBA
//      core internals);
//   2. build a copy of the exe for each build under test;
//   3. run both with the environment below and diff the two binary files.
//
//     VBA_PC_LOG=<file>    binary stream of 24-byte records, one per instruction
//     VBA_PC_START=<n>     first frame boundary to record from (default 0)
//     VBA_PC_END=<n>       frame boundary to stop at (default start + 1)
//
// The range [START, END) is recorded, so START=7 END=8 captures exactly the
// 8th frame.  Both builds start it at the same frame, which makes the two
// binary streams directly comparable: the first differing record is the first
// instruction where behaviour diverged.
//
// Record layout (6 x u32):
//     [0] pc             address of the instruction that just retired
//     [1] ticks          cycles it charged (clockTicks)
//     [2] busPrefetchCount   as left by the instruction
//     [3] cpuTotalTicks      running total after the charge
//     [4] cpuNextEvent       budget the loop is comparing against
//     [5] busPrefetch        the per-instruction prefetch latch
//
// [2]..[5] exist because a cycle-count difference alone does not say which
// piece of hidden CPU state diverged: the prefetch counter is mutated by every
// memory-timing helper and is *not* part of the savestate, so it is the prime
// suspect whenever two builds agree on the instruction stream and disagree on
// the clock.  Identical pc with different ticks pins the fault on one handler;
// identical everything with different instruction counts means the frame
// boundary itself moved.
// ---------------------------------------------------------------------------
void pcTraceRecord(unsigned int pc, int ticks, unsigned int prefetchCount,
                   unsigned int totalTicks, int nextEvent, int prefetch);

#endif // VBA_HASH_TRACE_H
