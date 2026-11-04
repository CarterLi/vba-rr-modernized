#ifndef VBA_JIT_EMIT_H
#define VBA_JIT_EMIT_H

// ---------------------------------------------------------------------------
// Machine-code backends for the IR.
//
// The decoder (JitDecode*.cpp) and the block cache (JitCache.cpp) do not care
// how a block is executed.  This header is the seam: a block is compiled into a
// native function once, and the block runner calls it instead of stepping the
// IR interpreter.  JitEmit.cpp holds the only implementation (x64, V7 core);
// everywhere else jitNativeAvailable() answers false and the caller keeps using
// JitInterp.cpp.
//
// The generated code's contract is deliberately the same as the IR block
// runner's, so the two are interchangeable and the differential verifier can
// compare them:
//
//   int fn(s32 budget, JitExecResult *out);
//
//   * executes guest instructions until the cycle total reaches `budget`, until
//     `holdState` is (or was) set, until an instruction only the interpreter may
//     run is reached, or until the compiled prefix ends;
//   * returns the cycles it executed (never negative);
//   * writes the address the *next* instruction would come from to out->nextPc
//     and one of the JitExitStatus bits to out->status.
//
// The caller (JitCache.cpp's runBlock) then fixes up armNextPC/reg[15] exactly
// the way it does for the IR path.
// ---------------------------------------------------------------------------

#include "../../Port.h"

struct JitBlock;

struct JitExecResult
{
	u32 nextPc;
	u32 status;
};

enum JitExitStatus
{
	// The next instruction has to be executed by CPULoop's inline interpreter
	// (a store into the IO block: CPUWriteMemoryWrapped() may call
	// CPUCheckDMA(), which rewrites CPULoop's own event counter through ext*).
	// Nothing of that instruction ran, so nextPc is the instruction itself.
	JITX_NEED_INTERP = 1,

	// Same, but reg[15] must be left exactly as the last executed instruction
	// left it (V7's one-instruction detour after a write to PC that is not a
	// branch).  The block compiler currently avoids emitting those instructions
	// at all - it ends the prefix in front of them - so nothing sets this yet.
	JITX_KEEP_PC = 2,

	// The cycle budget ran out (or holdState went high) right after the
	// instruction at nextPc: the caller stops the block here.
	JITX_BUDGET = 4,

	// The whole compiled prefix ran; nextPc is the first instruction that was
	// not part of it (either the next instruction of the block, or the first
	// one the backend refused to compile).
	JITX_DONE = 8
};

typedef int (*JitNativeFn)(s32 budget, JitExecResult *out);

// A chainable exit carries an indirect jump through one of these slots.  The
// slot lives in JitCodeEntry (not in the executable page), so linking is a
// plain 8-byte store and unlinking is a single zero - no self-modifying code.
// 0 means "return to the dispatcher"; anything else is the absolute address of
// a successor's chain entry.  Only ROM->ROM links are created, because ROM
// pages are never invalidated; see documentation/jit-recompiler.md 3.2.1.18.
#define JIT_MAX_CHAIN 4

struct JitChainSlot
{
	u64 target;
	u32 tag;			// (nextPc | thumb) that this exit leads to
	u16 linkIndex;		// node in the link pool, 0xFFFF when unlinked
	u16 reserved;
};

struct JitCompiledBlock
{
	JitNativeFn  fn;						// null when the backend refused the block
	u8			*chainEntry;				// address just past the prologue
	JitChainSlot chain[JIT_MAX_CHAIN];	// 0 = linked to nothing yet
	u16			 insnCount;				// JitInsn entries the native code covers (>= 1)
	u16			 chainCount;			// slots in use
	u16			 selfSlot;				// owner's index in jitCodeCache, 0xFFFF = scratch
	u8			 frameKind;				// usesMem; must match across a chain
	u8			 reserved2;
};

// True when this build has a machine-code backend for the current core.
bool jitNativeAvailable();

// Compile the longest prefix of `block` the backend supports.  Returns false
// when nothing could be compiled (no backend, or the first instruction is one
// the backend refuses), in which case *out is untouched.
bool jitCompileBlock(const JitBlock *block, JitCompiledBlock *out);

// Release the executable memory behind a compiled block.  Safe on a zeroed
// struct.
void jitReleaseBlock(JitCompiledBlock *block);

#endif // VBA_JIT_EMIT_H
