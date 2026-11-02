#ifndef VBA_JIT_VERIFY_H
#define VBA_JIT_VERIFY_H

// ---------------------------------------------------------------------------
// Differential test for the recompiler.
//
// Four things are checked, in this order:
//
//   1. the decode space: every word the emulator's dispatch table treats as
//      undefined must be decoded as JOP_UNDEF, and no word it executes may be.
//      A decoder that refuses a word outright is fine - the caller hands it
//      back to the interpreter;
//   2. per instruction, one pass per instruction set: the same instruction
//      from an identical machine state through
//        A - the real interpreter (armExecute / thumbExecute, stopped after
//            one instruction by setting cpuNextEvent to 1)
//        B - the decoder plus the IR interpreter
//      comparing every register, the four flags, the cycle count and
//      busPrefetchCount;
//   3. per block: a whole decoded block through runBlock() against the
//      interpreter running the same instructions, including the memory the
//      block wrote (RAM is snapshotted before A and restored before B);
//   4. nothing here covers SWI, undefined instructions or a control-field MSR
//      write: those change state a memory snapshot cannot put back, and
//      without a BIOS file the exception handler pops a modal dialog.
//
// Must be called with a ROM already loaded and the CPU reset.
// ---------------------------------------------------------------------------

// Returns the number of failures (all passes summed, block mismatches
// included), or -1 when the verifier is not available (V7 core, or no ROM
// loaded).
int jitVerifyRun(int maxInsns, int verbose);

#endif // VBA_JIT_VERIFY_H
