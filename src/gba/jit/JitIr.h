#ifndef VBA_JIT_IR_H
#define VBA_JIT_IR_H

// ---------------------------------------------------------------------------
// A tiny IR for the GBA CPU, and the three things built on top of it:
//
//   decode  - ARM/Thumb opcode -> JitInsn[]
//   interp  - JitInsn[] executed against the emulator's globals
//   emit    - JitInsn[] -> machine code (x64/ARM64) or C
//
// The IR exists because the semantics have to be written down exactly once.
// Three consumers read it: the C backend (which is also the AOT export path),
// the machine-code backends, and the IR interpreter that the differential
// tests drive.  If each consumer decoded opcodes itself they would drift.
//
// Levels of abstraction: one JitInsn is one guest instruction, not one host
// operation.  The backends expand it.  That keeps the decoder honest - it is a
// transcription of the interpreter's handlers - and keeps the expansion in one
// place per host ISA.
//
// What is deliberately NOT in the IR:
//
//   * cycles, except for the one table lookup that is part of the instruction
//     shape: V7 Thumb charges thumbCycles[opcode >> 8] as its base cost, which
//     does not depend on any runtime value, so the decoder stores it in
//     JitInsn::fetchCycles.  Everything else (bus waits, prefetch effects) still
//     depends on runtime values and stays in the backend.  Do not "optimise"
//     the rest into a per-block constant: IRQs fire on instruction boundaries
//     and a block-level cycle count moves them.
//   * register liveness or allocation.  The backends keep the 16 GBA registers
//     in host registers and spill the rest; that decision is not the IR's.
//
// Two things a backend must not forget, because they are not in any JitInsn:
//
//   * cpuPrefetch[0]/[1].  armExecute() executes the *cached* opcode, and this
//     IR does not reproduce the prefetch reads.  A block that leaves without
//     refreshing the prefetch word will be handed a stale opcode.  Either keep
//     both words up to date the way ARM_PREFETCH_NEXT/THUMB_PREFETCH_NEXT do,
//     or force a refetch when the block returns to CPULoop.
//   * busPrefetch/busPrefetchCount start-of-instruction masking.  Both loops do
//     it before dispatching; jitInterpInsn() below models it because it runs
//     one instruction at a time, a block backend has to hoist it per insn.
// ---------------------------------------------------------------------------

#include "../../Port.h"

// One JitInsn per guest instruction.  `cond` is the ARM condition field (Thumb
// instructions are all AL except conditional branches, which the decoder turns
// into a branch with a condition).
enum JitOp : u8
{
	JOP_END = 0,	// end of block; nothing to execute
	JOP_ALU,		// d = n <sub> m/imm, optionally setting flags
	JOP_MUL,		// multiply family
	JOP_LOAD,		// LDR / LDRB / LDRH / LDRSB / LDRSH
	JOP_STORE,		// STR / STRB / STRH
	JOP_LDM,		// LDMxx (ARM)
	JOP_STM,		// STMxx (ARM)
	JOP_PUSH,		// Thumb PUSH {rlist[, lr]}; imm is the list, bit 14 is lr
	JOP_POP,		// Thumb POP  {rlist[, pc]}; imm is the list, bit 15 is pc
	JOP_SWP,		// SWP / SWPB
	JOP_BRANCH,		// B / BL (Thumb: conditional B, and the second BL half)
	JOP_BX,			// BX (and BLX in Thumb)
	JOP_MRS,		// MRS
	JOP_MSR,		// MSR (register or immediate)
	JOP_SWI,		// SWI
	JOP_UNDEF		// undefined instruction -> CPUUndefinedException()
};

enum JitAluOp : u8
{
	ALU_AND, ALU_EOR, ALU_SUB, ALU_RSB, ALU_ADD, ALU_ADC, ALU_SBC, ALU_RSC,
	ALU_TST, ALU_TEQ, ALU_CMP, ALU_CMN, ALU_ORR, ALU_MOV, ALU_BIC, ALU_MVN
};

enum JitMulOp : u8
{
	MUL_MUL, MUL_MLA, MUL_UMULL, MUL_UMLAL, MUL_SMULL, MUL_SMLAL
};

// How the second operand of a data-processing instruction is produced.
//
// JOP_LOAD / JOP_STORE reuse this field for the offset with the interpreter's
// OFFSET_* semantics, which differ from the VALUE_* ones for LSR/ASR #0 and
// ROR #0 (see evalOffset() in JitInterp.cpp).  Same enum, two meanings - the
// op tells them apart.
enum JitShift : u8
{
	SHIFT_NONE = 0,	// use m as-is (or imm)
	SHIFT_LSL_IMM,	// m << imm
	SHIFT_LSR_IMM,	// m >> imm (imm == 0 means 32 for ALU, 0 for offsets)
	SHIFT_ASR_IMM,	// (s32)m >> imm (imm == 0 means 32 for ALU, sign fill for offsets)
	SHIFT_ROR_IMM,	// ror(m, imm) (imm == 0 means RRX)
	SHIFT_LSL_REG,	// m << Rs
	SHIFT_LSR_REG,
	SHIFT_ASR_REG,
	SHIFT_ROR_REG,
	SHIFT_ROR_REG_T,	// Thumb "ror Rd, Rs": a count of 0/32 still writes the carry
	SHIFT_IMM_ROT,	// rotate the 8-bit immediate right by 2*imm
	// V7's ARM "ror Rd, Rm, Rs" writes the carry even when the raw count is 0
	// (SHIFT_ROR_REG_T only does so for a raw count that masks to 0).  Appended
	// last so the existing enum values keep their numbers.
	SHIFT_ROR_REG_V7
};

enum JitMemSize : u8
{
	MEM_WORD, MEM_BYTE, MEM_HALF
};

enum JitBlockMode : u8
{
	BLOCK_IA = 0, BLOCK_IB, BLOCK_DA, BLOCK_DB
};

enum JitFlags : u16
{
	JF_WRITEBACK	= 1 << 0,	// Rn! (or LDM/STM W bit)
	JF_PRE			= 1 << 1,	// indexed address is base +/- offset (not post-indexed)
	JF_UP			= 1 << 2,	// add the offset instead of subtracting
	JF_SIGNED		= 1 << 3,	// LDRSB / LDRSH
	JF_LINK			= 1 << 4,	// BL / BLX - and, in Thumb, "this is the BL low half"
	JF_IMM			= 1 << 5,	// operand2 / offset / MSR value is `imm`, m is unused
	JF_PC_ALIGN4	= 1 << 6,	// base is reg & ~3 (Thumb "add Rd, pc, ..." and "ldr Rd, [pc, ...]")
	JF_CLEAR_PREFETCH = 1 << 7,	// instruction leaves busPrefetchCount == 0 (Thumb ldr Rd,[pc,#imm])
	JF_USER			= 1 << 8,	// LDM/STM with the ^ suffix (user bank); not decoded yet
	JF_DATA32		= 1 << 9,	// charge the 32-bit data wait even for a halfword (thumb5A)
	JF_QUICK		= 1 << 10	// V7: read with CPUReadMemoryQuick (Thumb ldr [pc]/[sp])
};

// Which of N/Z/C/V an instruction writes.  Replaces a plain "sets flags" bit
// because the two instruction sets disagree: ARM's logical ops write N,Z,C and
// leave V alone, while most Thumb ALU ops write N and Z only.
enum JitFlagMask : u8
{
	JITF_N	 = 1 << 0,
	JITF_Z	 = 1 << 1,
	JITF_C	 = 1 << 2,
	JITF_V	 = 1 << 3,
	JITF_NZ	 = JITF_N | JITF_Z,
	JITF_NZC = JITF_N | JITF_Z | JITF_C,
	JITF_ALL = JITF_N | JITF_Z | JITF_C | JITF_V
};

struct JitInsn
{
	u32 imm;		// immediate, shift amount, branch target, register list, SWI number
	u16 flags;		// JitFlags
	u8	op;			// JitOp
	u8	sub;		// JitAluOp / JitMulOp / JitMemSize / JitBlockMode
	u8	cond;		// ARM condition field, 0..15
	u8	d;			// Rd (or Rt for transfers, RdHi/RdLo for multiplies)
	u8	n;			// Rn (base, or Rm for multiplies)
	u8	m;			// Rm (or Rs)
	u8	shift;		// JitShift
	u8	flagMask;	// JitFlagMask: which flags this instruction writes, 0 = none
	u8	fetchCycles;	// V7 Thumb: thumbCycles[opcode >> 8]; 0 for ARM
};

// The shared consumers (JitDump, JitVerify, selftest) assume one layout for both
// cores.  15 bytes of fields pad to 16; keep it that way when adding fields.
static_assert(sizeof(JitInsn) == 16, "JitInsn layout is part of the shared IR");

#define JIT_MAX_INSNS 64

// A block never crosses a 1 KB page: the invalidation scheme (documentation/
// jit-recompiler.md 3.6) keeps one version counter per page and a block records
// only the page it started on.
#define JIT_PAGE_SIZE 1024

struct JitBlock
{
	u32		guestPc;	// address of insn[0]
	u16		count;		// number of valid JitInsn entries
	u8		thumb;		// decoded in Thumb state
	u8		ended;		// the last instruction ends the block (branch, PC write, ...)
	JitInsn insn[JIT_MAX_INSNS];
};

// Where the decoder reads guest code from.  `base[addr & mask]` mirrors the
// emulator's memoryMap[], so a decoder call is one masked load per halfword.
struct JitSource
{
	const u8 *base;
	u32		  mask;
};

// Decode a basic block starting at pc.  Returns false when the instruction is
// one the decoder does not handle yet; the caller then falls back to the
// interpreter for that block.  On success `block->count` is at least 1 and the
// block stops at the first instruction that ends it (branch, PC write, SWI,
// undefined) or at the last instruction of a 1 KB page.
bool jitDecodeArm(const JitSource &src, u32 pc, JitBlock *block);
bool jitDecodeThumb(const JitSource &src, u32 pc, JitBlock *block);

// Decode exactly one instruction.  Same rules, but stops after the first one;
// used by the verifier, which compares instruction by instruction.
bool jitDecodeOne(const JitSource &src, u32 pc, bool thumb, JitInsn *insn);

// Execute one IR instruction.  `pc` is the address of the instruction itself
// (not PC+4/PC+8).  Writes the address to continue at to *nextPc and returns
// the instruction's cycle count, exactly as the interpreter would have
// computed it - including codeTicksAccess* and the data-access penalties.
// Returns -1 when the op is not implemented (or the insn is skipped).
//
// JIT_NEED_INTERP means "nothing was executed, run this one in the inline
// interpreter".  The V7 core needs it because some instructions call
// CPUSwitchMode()/CPUUpdateFlags(), which read and rewrite CPULoop's own
// locals through extCpuNextEvent/extClockTicks; those are only correct when
// the interpreter drives the instruction itself.  A block runner that sees it
// must stop the block, leave armNextPC pointing at this instruction, and ask
// CPULoop for one interpreted instruction.
#define JIT_NEED_INTERP (-2)

int jitInterpInsn(const JitInsn *insn, bool thumb, u32 pc, u32 *nextPc);

// Human-readable dump of a block, one instruction per line.  Returns the number
// of characters written (never more than outSize-1).
int jitDumpBlock(const JitBlock *block, char *out, int outSize);

// Name of an opcode for the dump and for test failure messages.
const char *jitOpName(u8 op);

#endif // VBA_JIT_IR_H
