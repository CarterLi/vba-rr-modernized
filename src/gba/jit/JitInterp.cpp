// IR interpreter.
//
// Two jobs:
//   1. reference implementation for the differential tests - it executes the
//      same JitInsn[] the backends will, against the same globals, so a decoder
//      bug shows up as a state difference instead of a mystery crash in
//      generated code;
//   2. executable specification of the cycle accounting.  Every value returned
//      here is what the interpreter's handler would have left in clockTicks.
//
// Semantics are transcribed from GBA-arm.cpp and GBA-thumb.cpp, including the
// parts that are not architecturally correct (shift by 32, ROR #0 meaning RRX,
// flags not updated when Rd is PC, Thumb's LDM/STM throwing away their data
// ticks).  The emulator is the specification.
//
// The two instruction sets differ in more than the instruction width, and every
// one of these differences is load-bearing:
//
//   * armNextPC is pc+4 in ARM and pc+2 in Thumb, and reg[15] is that plus 4 or
//     plus 2 - so an instruction that reads PC sees pc+8 or pc+4.
//   * most Thumb handlers leave clockTicks at zero, which means the loop charges
//     "1 + codeTicksAccessSeq16(pc+2)"; the ARM handlers almost always set it.
//   * the Thumb loop masks busPrefetchCount with 0xFFFFFF00, the ARM loop with
//     0xFFFFFE00.
//   * Thumb's block transfers assign clockTicks instead of adding to it, so
//     their per-register data ticks vanish; the accesses still happen and still
//     move busPrefetchCount.

#include "JitIr.h"

#include "../GBAGlobals.h"
#include "../GBA.h"
#include "../GBAinline.h"

#ifdef USE_GBA_CORE_V7

// ---------------------------------------------------------------------------
// V7 semantics.
//
// Transcribed from src/gba/V7/arm-new.h and thumb.h, not from the V8 code
// above: the two cores disagree on almost every cycle formula, and "the same
// IR with a different constant" would desynchronise every recorded movie.
// What is different here, in one place so it can be checked against the
// source:
//
//   * the base cost is memoryWaitFetch32[pc >> 24] for ARM and
//     thumbCycles[opcode >> 8] + memoryWaitFetch[pc >> 24] for Thumb.  The
//     Thumb half is precomputed by the decoder (JitInsn::fetchCycles); the
//     fetch wait is looked up here because WAITCNT can change it.
//   * there is no prefetch buffer, no busPrefetchCount and no cpuTotalTicks,
//     so nothing here mirrors the V8 loop's three bookkeeping lines.
//   * Thumb handlers mostly add nothing, the ARM ones almost always add
//     something.  A Thumb store charges only its data access; an ARM store
//     charges 2 + access.
//   * the ARM data-processing path reads PC as pc+8 and adds no fetch cycles
//     for a PC write, while Thumb's high-register ADD/MOV to PC adds 1.
//   * shift-by-register counts are masked to five bits by the host on ARM
//     (the handler shifts a u32 by an int without clamping, which is UB in
//     C++ and `shl/shr` with the count masked to 5 bits in the generated
//     code); Thumb's handlers spell the >=32 cases out instead.  See
//     evalOperand2V7().  This only matters for counts >= 32, which real code
//     does not use - but the differential test would flag it if we guessed.
//   * SWI, UNDEF, a store into the IO block, any data-processing write to
//     PC with S=1 and every load into PC whose PC effect V7 does not
//     implement as a jump (byte/halfword, and the post-indexed word form)
//     go back to the interpreter (JIT_NEED_INTERP).  The first group calls
//     CPUSwitchMode()/CPUCheckDMA(), which read and rewrite CPULoop's own
//     locals through extCpuNextEvent/extClockTicks; the loads are the
//     one-instruction detour described at doLoadStore().
// ---------------------------------------------------------------------------

// From the V7 core.  Declared here rather than by including V7/GBACpu.h: that
// header carries UPDATE_REG and the emulator's inline helpers, and every file
// under src/gba/jit has to stay compilable on its own (selftest.cpp builds the
// decoders without linking the emulator).
extern int32 memoryWait[16];
extern int32 memoryWait32[16];
extern int32 memoryWaitSeq[16];
extern int32 memoryWaitSeq32[16];
extern int32 memoryWaitFetch[16];
extern int32 memoryWaitFetch32[16];
extern u8	 cpuBitsSet[256];
extern void	 CPUUpdateCPSR();

namespace {

static inline bool condTrue(u32 cond)
{
	switch (cond)
	{
	case 0x0: return Z_FLAG;
	case 0x1: return !Z_FLAG;
	case 0x2: return C_FLAG;
	case 0x3: return !C_FLAG;
	case 0x4: return N_FLAG;
	case 0x5: return !N_FLAG;
	case 0x6: return V_FLAG;
	case 0x7: return !V_FLAG;
	case 0x8: return C_FLAG && !Z_FLAG;
	case 0x9: return !C_FLAG || Z_FLAG;
	case 0xA: return N_FLAG == V_FLAG;
	case 0xB: return N_FLAG != V_FLAG;
	case 0xC: return !Z_FLAG && (N_FLAG == V_FLAG);
	case 0xD: return Z_FLAG || (N_FLAG != V_FLAG);
	case 0xE: return true;
	default:  return false;		// 0xF: "never"
	}
}

// Shift forms that cost one extra cycle in ARM (the handlers do
// `clockTicks++` before they touch the operand).  SHIFT_IMM_ROT is not one.
static inline bool isRegShift(u8 shift)
{
	return (shift >= SHIFT_LSL_REG && shift <= SHIFT_ROR_REG_T) ||
		   shift == SHIFT_ROR_REG_V7;
}

static inline int bitsSet(u32 v)
{
	return cpuBitsSet[v & 0xFF] + cpuBitsSet[(v >> 8) & 0xFF];
}

static inline int acc32(u32 a) { return memoryWait32[(a >> 24) & 15]; }
static inline int acc16(u32 a) { return memoryWait[(a >> 24) & 15]; }
static inline int seq32(u32 a) { return memoryWaitSeq32[(a >> 24) & 15]; }

static inline u32 ror32(u32 v, u32 n)
{
	n &= 31;
	return n ? ((v >> n) | (v << (32 - n))) : v;
}

static inline u32 negMask(u32 v) { return (u32)((s32)v >> 31); }
static inline u32 posMask(u32 v) { return ~negMask(v); }

static void setLogicalFlags(u32 res, bool carryOut, u8 mask)
{
	if (mask & JITF_N) N_FLAG = ((s32)res < 0);
	if (mask & JITF_Z) Z_FLAG = (res == 0);
	if (mask & JITF_C) C_FLAG = carryOut;
}

static void setAddFlags(u32 a, u32 b, u32 res, u8 mask)
{
	if (mask & JITF_N) N_FLAG = ((s32)res < 0);
	if (mask & JITF_Z) Z_FLAG = (res == 0);
	if (mask & JITF_V)
		V_FLAG = (((negMask(a) & negMask(b) & posMask(res)) |
				   (posMask(a) & posMask(b) & negMask(res))) &
				  0x80000000) != 0;
	if (mask & JITF_C)
		C_FLAG = (((negMask(a) & negMask(b)) |
				   (negMask(a) & posMask(res)) |
				   (negMask(b) & posMask(res))) &
				  0x80000000) != 0;
}

static void setSubFlags(u32 a, u32 b, u32 res, u8 mask)
{
	if (mask & JITF_N) N_FLAG = ((s32)res < 0);
	if (mask & JITF_Z) Z_FLAG = (res == 0);
	if (mask & JITF_V)
		V_FLAG = (((negMask(a) & posMask(b) & posMask(res)) |
				   (posMask(a) & negMask(b) & negMask(res))) &
				  0x80000000) != 0;
	if (mask & JITF_C)
		C_FLAG = (((negMask(a) & posMask(b)) |
				   (negMask(a) & posMask(res)) |
				   (posMask(b) & posMask(res))) &
				  0x80000000) != 0;
}

// Second operand of a data-processing instruction.
//
// ARM's register-shift handlers shift a u32 by the raw byte from Rs with no
// clamp (`v << n`, `v >> (32 - n)`), which is undefined C++ for n >= 32: this
// build of the interpreter produces whatever the host's masked shift happens to
// give.  Those cases never reach this function - shiftNeedsInterp() hands the
// instruction to the interpreter - so the masks below are belt and braces.
//
// Thumb spells the same cases out (a count of 32 is "shift by 32", not "shift
// by 0"), so its branches here are exact.
static u32 evalOperand2V7(const JitInsn *in, bool thumb, bool *carryOut)
{
	const u32 v = (in->flags & JF_IMM) ? in->imm : reg[in->m & 0x0F].I;

	if (in->flags & JF_IMM)
	{
		if (in->shift != SHIFT_IMM_ROT)
			return v;

		const u32 rot	= (in->imm >> 8) & 0x0F;
		const u32 imm8	= in->imm & 0xFF;
		if (rot == 0)
			return imm8;
		const u32 amount = rot * 2;
		*carryOut = ((imm8 >> (amount - 1)) & 1) != 0;
		return (imm8 >> amount) | (imm8 << (32 - amount));
	}

	switch (in->shift)
	{
	case SHIFT_NONE:
		return v;

	case SHIFT_LSL_IMM:
		if (in->imm == 0)
			return v;
		*carryOut = ((v >> (32 - in->imm)) & 1) != 0;
		return v << in->imm;

	case SHIFT_LSR_IMM:
		if (in->imm == 0)
		{
			*carryOut = (v & 0x80000000) != 0;
			return 0;
		}
		*carryOut = ((v >> (in->imm - 1)) & 1) != 0;
		return v >> in->imm;

	case SHIFT_ASR_IMM:
		if (in->imm == 0)
		{
			if (v & 0x80000000)
			{
				*carryOut = true;
				return 0xFFFFFFFF;
			}
			*carryOut = false;
			return 0;
		}
		*carryOut = (((s32)v >> (in->imm - 1)) & 1) != 0;
		return (u32)((s32)v >> in->imm);

	case SHIFT_ROR_IMM:
		if (in->imm == 0)		// RRX
		{
			*carryOut = (v & 1) != 0;
			return (v >> 1) | ((u32)C_FLAG << 31);
		}
		{
			const u32 r = (v >> in->imm) | (v << (32 - in->imm));
			*carryOut = (r & 0x80000000) != 0;
			return r;
		}

	case SHIFT_LSL_REG:
	{
		const u32 raw = reg[in->imm & 0x0F].B.B0;
		if (raw == 0)
			return v;
		if (thumb)
		{
			// thumb40_2: 32 -> value 0 and C = bit0; >32 -> value 0, C = 0.
			if (raw == 32)
			{
				*carryOut = (v & 1) != 0;
				return 0;
			}
			if (raw < 32)
			{
				*carryOut = ((v >> (32 - raw)) & 1) != 0;
				return v << raw;
			}
			*carryOut = false;
			return 0;
		}
		*carryOut = ((v >> ((32 - raw) & 31)) & 1) != 0;
		return v << (raw & 31);
	}

	case SHIFT_LSR_REG:
	{
		const u32 raw = reg[in->imm & 0x0F].B.B0;
		if (raw == 0)
			return v;
		if (thumb)
		{
			if (raw == 32)
			{
				*carryOut = (v & 0x80000000) != 0;
				return 0;
			}
			if (raw < 32)
			{
				*carryOut = ((v >> (raw - 1)) & 1) != 0;
				return v >> raw;
			}
			*carryOut = false;
			return 0;
		}
		*carryOut = ((v >> ((raw - 1) & 31)) & 1) != 0;
		return v >> (raw & 31);
	}

	case SHIFT_ASR_REG:
	{
		const u32 raw = reg[in->imm & 0x0F].B.B0;
		if (raw == 0)
			return v;
		if (thumb)
		{
			if (raw < 32)
			{
				*carryOut = (((s32)v >> (raw - 1)) & 1) != 0;
				return (u32)((s32)v >> raw);
			}
			if (v & 0x80000000)
			{
				*carryOut = true;
				return 0xFFFFFFFF;
			}
			*carryOut = false;
			return 0;
		}
		*carryOut = (((s32)v >> ((raw - 1) & 31)) & 1) != 0;
		return (u32)((s32)v >> (raw & 31));
	}

	case SHIFT_ROR_REG_T:
	{
		// Thumb "ror Rd, Rs" (ROR_RD_RS): a count that masks to zero still
		// writes the carry, unlike the ARM forms.
		const u32 raw = reg[in->imm & 0x0F].B.B0;
		if (raw == 0)
			return v;
		const u32 n = raw & 0x1F;
		if (n == 0)
		{
			*carryOut = (v & 0x80000000) != 0;
			return v;
		}
		const u32 r = (v >> n) | (v << (32 - n));
		*carryOut = (r & 0x80000000) != 0;
		return r;
	}

	case SHIFT_ROR_REG_V7:
	{
		// arm-new.h:616-636 - both a raw count of 0 and one that masks to 0
		// leave the value alone and take the carry from bit 31.
		const u32 raw = reg[in->imm & 0x0F].B.B0;
		const u32 n	  = raw & 0x1F;
		if (n == 0)
		{
			*carryOut = (v & 0x80000000) != 0;
			return v;
		}
		const u32 r = (v >> n) | (v << (32 - n));
		*carryOut = (r & 0x80000000) != 0;
		return r;
	}

	default:
		return v;
	}
}

// Offset of a load/store (the OFFSET_* family).  Same rules as the V8 core:
// lsr #0 means 0, asr #0 means a full sign fill, ror #0 means RRX.
static u32 evalOffsetV7(const JitInsn *in)
{
	const u32 v		= reg[in->m & 0x0F].I;
	const u32 shift = in->imm;

	switch (in->shift)
	{
	case SHIFT_LSL_IMM:
		return v << shift;
	case SHIFT_LSR_IMM:
		return shift ? (v >> shift) : 0;
	case SHIFT_ASR_IMM:
		if (shift)
			return (u32)((s32)v >> shift);
		return (v & 0x80000000) ? 0xFFFFFFFF : 0;
	case SHIFT_ROR_IMM:
		if (shift)
			return (v >> shift) | (v << (32 - shift));
		return (v >> 1) | ((u32)C_FLAG << 31);
	default:
		return v;
	}
}

// A store into 0x04xxxxxx can start a DMA transfer: CPUWriteMemoryWrapped()
// calls CPUCheckDMA(), which reads and rewrites CPULoop's cpuNextEvent through
// extCpuNextEvent.  Nothing else a block can reach does that, so the block
// runners hand exactly these stores back to the interpreter.
static inline bool hitsIo(u32 address, u32 size)
{
	if ((address & 0x0F000000) == 0x04000000)
		return true;
	return size > 1 && ((address + size - 1) & 0x0F000000) == 0x04000000;
}

// ARM's shift-by-register handlers shift a u32 by the raw byte from Rs with no
// clamp (`v << shift`, `v >> (32 - shift)`), which is undefined C++ for counts
// >= 32.  What this build of the interpreter does there is whatever the host
// masks the count to - not something a portable expression can promise, and not
// something a recompiler should guess at.  Those instructions go to the
// interpreter (JIT_NEED_INTERP); real code does not use counts that large.
//
// Thumb is not affected: thumb40_2/40_3/41_0 spell the 32-and-up cases out
// explicitly, so evalOperand2V7() models them exactly.
static inline bool shiftNeedsInterp(const JitInsn *in, bool thumb)
{
	if (thumb || (in->flags & JF_IMM))
		return false;

	switch (in->shift)
	{
	case SHIFT_LSL_REG:
	case SHIFT_LSR_REG:
	case SHIFT_ASR_REG:
		return reg[in->imm & 0x0F].B.B0 >= 32;
	default:
		return false;
	}
}

static int doAlu(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc, int base)
{
	// A data-processing write to PC with S=1 calls CPUSwitchMode(..., false),
	// which reaches the two-argument overload -> breakLoop = true ->
	// CPUUpdateFlags(true) -> CPU_BREAK_LOOP_2, i.e. it rewrites the
	// interpreter's locals.  Compare instructions with 15 in Rd take the same
	// path (their S bit is always set in a valid encoding).  Both go back to
	// the interpreter; only the S=0 form stays here.
	if (in->d == 15 && in->flagMask != 0)
		return JIT_NEED_INTERP;
	if (shiftNeedsInterp(in, thumb))
		return JIT_NEED_INTERP;

	bool	  carryOut = C_FLAG;
	const u32 rhs	   = evalOperand2V7(in, thumb, &carryOut);
	u32		  lhs	   = reg[in->n & 0x0F].I;
	if (in->flags & JF_PC_ALIGN4)
		lhs &= 0xFFFFFFFC;

	const u8   mask		= in->flagMask;
	const bool toPc		= (in->d == 15);
	u32		   res		= 0;

	switch (in->sub)
	{
	case ALU_AND: res = lhs & rhs;	  reg[in->d].I = res; setLogicalFlags(res, carryOut, mask); break;
	case ALU_EOR: res = lhs ^ rhs;	  reg[in->d].I = res; setLogicalFlags(res, carryOut, mask); break;
	case ALU_SUB: res = lhs - rhs;	  reg[in->d].I = res; setSubFlags(lhs, rhs, res, mask); break;
	case ALU_RSB: res = rhs - lhs;	  reg[in->d].I = res; setSubFlags(rhs, lhs, res, mask); break;
	case ALU_ADD: res = lhs + rhs;	  reg[in->d].I = res; setAddFlags(lhs, rhs, res, mask); break;
	case ALU_ADC: res = lhs + rhs + (u32)C_FLAG; reg[in->d].I = res;
				  setAddFlags(lhs, rhs, res, mask); break;
	case ALU_SBC: res = lhs - rhs - !((u32)C_FLAG); reg[in->d].I = res;
				  setSubFlags(lhs, rhs, res, mask); break;
	case ALU_RSC: res = rhs - lhs - !((u32)C_FLAG); reg[in->d].I = res;
				  setSubFlags(rhs, lhs, res, mask); break;

	case ALU_TST: res = lhs & rhs; setLogicalFlags(res, carryOut, mask); break;
	case ALU_TEQ: res = lhs ^ rhs; setLogicalFlags(res, carryOut, mask); break;
	case ALU_CMP: res = lhs - rhs; setSubFlags(lhs, rhs, res, mask); break;
	case ALU_CMN: res = lhs + rhs; setAddFlags(lhs, rhs, res, mask); break;

	case ALU_ORR: res = lhs | rhs;	  reg[in->d].I = res; setLogicalFlags(res, carryOut, mask); break;
	case ALU_MOV: res = rhs;		  reg[in->d].I = res; setLogicalFlags(res, carryOut, mask); break;
	case ALU_BIC: res = lhs & ~rhs;	  reg[in->d].I = res; setLogicalFlags(res, carryOut, mask); break;
	case ALU_MVN: res = ~rhs;		  reg[in->d].I = res; setLogicalFlags(res, carryOut, mask); break;
	default: break;
	}

	if (toPc)
	{
		// Reachable only with S=0 (see the guard above): the dest==15 branch
		// runs OPCODE2, which for a register-writing op is the non-S variant,
		// so the value is stored and the flags are left alone.  Then the PC is
		// aligned for the instruction set *after* the write and armNextPC is
		// set from it.  ARM adds no fetch cycles for this; a Thumb
		// high-register ADD/MOV to PC adds one.
		reg[15].I = res;
		if (armState)
		{
			reg[15].I &= 0xFFFFFFFC;
			armNextPC  = reg[15].I;
			reg[15].I += 4;
		}
		else
		{
			reg[15].I &= 0xFFFFFFFE;
			armNextPC  = reg[15].I;
			reg[15].I += 2;
		}
		*nextPc = armNextPC;
		return base + (isRegShift(in->shift) ? 1 : 0) + (thumb ? 1 : 0);
	}

	*nextPc = pc + (thumb ? 2 : 4);
	return base + (isRegShift(in->shift) ? 1 : 0);
}

static int doMul(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc, int base)
{
	const u32 rs = reg[in->m].I;

	switch (in->sub)
	{
	case MUL_MUL:
		// Thumb multiplies the destination by Rs and ARM multiplies Rm by Rs;
		// the product is the same, the timing source is Rs in *both* cores
		// (thumb43_1 takes it from `rm`, not from the destination).
		reg[in->d].I = thumb ? reg[in->d].I * rs : reg[in->n].I * rs;
		break;
	case MUL_MLA:
		reg[in->d].I = reg[in->n].I * rs + reg[in->imm & 0x0F].I;
		break;
	case MUL_UMULL:
	case MUL_SMULL:
	{
		const u64 a	= (in->sub == MUL_SMULL) ? (u64)(s64)(s32)reg[in->n].I : (u64)reg[in->n].I;
		const u64 b	= (in->sub == MUL_SMULL) ? (u64)(s64)(s32)rs : (u64)rs;
		const u64 r = a * b;
		reg[in->imm & 0x0F].I = (u32)r;
		reg[in->d].I			  = (u32)(r >> 32);
		break;
	}
	case MUL_UMLAL:
	case MUL_SMLAL:
	{
		const u64 acc = ((u64)reg[in->d].I << 32) | reg[in->imm & 0x0F].I;
		const u64 a	  = (in->sub == MUL_SMLAL) ? (u64)(s64)(s32)reg[in->n].I : (u64)reg[in->n].I;
		const u64 b	  = (in->sub == MUL_SMLAL) ? (u64)(s64)(s32)rs : (u64)rs;
		const u64 r	  = acc + a * b;
		reg[in->imm & 0x0F].I = (u32)r;
		reg[in->d].I		  = (u32)(r >> 32);
		break;
	}
	}

	const u8 mask = in->flagMask;
	if (mask)
	{
		if (in->sub >= MUL_UMULL)
		{
			// N is bit 31 of the high word, Z is the whole 64-bit result.
			if (mask & JITF_N) N_FLAG = (reg[in->d].I & 0x80000000) != 0;
			if (mask & JITF_Z) Z_FLAG = (reg[in->d].I == 0) && (reg[in->imm & 0x0F].I == 0);
		}
		else
		{
			if (mask & JITF_N) N_FLAG = ((s32)reg[in->d].I < 0);
			if (mask & JITF_Z) Z_FLAG = reg[in->d].I == 0;
		}
	}

	u32 mag = rs;
	// Only the signed forms take the magnitude of Rs; UMULL/UMLAL bucket the raw
	// value (arm-new.h: they go straight to `usource & 0xFFFFFF00`).  Applying
	// the complement there turns 0xFFFFFF80 into 0x7F and picks a lower bucket.
	const bool signedMul = (in->sub == MUL_MUL || in->sub == MUL_MLA ||
							in->sub == MUL_SMULL || in->sub == MUL_SMLAL);
	if (signedMul && ((s32)mag < 0))
		mag = ~mag;

	int bucket;
	if ((mag & 0xFFFFFF00) == 0)
		bucket = 0;
	else if ((mag & 0xFFFF0000) == 0)
		bucket = 1;
	else if ((mag & 0xFF000000) == 0)
		bucket = 2;
	else
		bucket = 3;

	*nextPc = pc + (thumb ? 2 : 4);

	if (thumb)
		return base + 1 + bucket;		// thumb43_1: +1..+4
	// ARM: mul/umull/smull +2..+5, the accumulating forms one more.  (Testing
	// `sub >= MUL_UMULL` instead of the two non-accumulating forms also caught
	// UMLAL/SMLAL, which cost one cycle more.)
	const int plus = (in->sub == MUL_MUL || in->sub == MUL_UMULL || in->sub == MUL_SMULL) ? 0 : 1;
	return base + 2 + bucket + plus;
}

static int doLoadStore(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc, int base)
{
	const bool pre = (in->flags & JF_PRE) != 0;
	const bool up  = (in->flags & JF_UP) != 0;
	const bool wb  = (in->flags & JF_WRITEBACK) != 0;

	u32 b = reg[in->n & 0x0F].I;
	if (in->flags & JF_PC_ALIGN4)
		b &= 0xFFFFFFFC;		// thumb48 / thumbA0: (reg[15] & ~3) + imm

	const u32 off	 = (in->flags & JF_IMM) ? in->imm : evalOffsetV7(in);
	const u32 address = pre ? (up ? b + off : b - off) : b;
	const u32 wbValue = up ? b + off : b - off;

	if (in->op == JOP_STORE)
	{
		if (hitsIo(address, in->sub == MEM_WORD ? 4u : in->sub == MEM_HALF ? 2u : 1u))
			return JIT_NEED_INTERP;

		// V7 writes the base back *before* the store for the byte and word
		// forms and *after* it for the halfword form (STR/STRB vs STRH in
		// arm-new.h).  Only visible when Rn == Rt, but the differential test
		// compares registers, so it has to be exact.
		switch (in->sub)
		{
		case MEM_BYTE:
			if (pre && wb) reg[in->n].I = address;
			CPUWriteByte(address, reg[in->d].B.B0);
			break;
		case MEM_HALF:
			CPUWriteHalfWord(address, reg[in->d].W.W0);
			if (pre && wb) reg[in->n].I = address;
			break;
		default:
			if (pre && wb) reg[in->n].I = address;
			CPUWriteMemory(address, reg[in->d].I);
			break;
		}

		if (!pre && wb) reg[in->n].I = wbValue;

		*nextPc = pc + (thumb ? 2 : 4);
		int ticks = base + (in->sub == MEM_WORD ? acc32(address) : acc16(address));
		if (!thumb)
		{
			// ARM's store constants are not uniform: STR/STRB charge 2, STRH
			// charges 4 (arm-new.h: all the STRH cases read `+= 4 + Access16`).
			ticks += (in->sub == MEM_HALF) ? 4 : 2;
		}
		return ticks;
	}

	// A byte or halfword load into PC is the one form V7 does *not* special
	// case: reg[15] becomes the loaded value, the *next* instruction still
	// comes from pc+4, and only the one after that jumps.  A block cannot
	// express a one-instruction detour like that, so hand it back.
	//
	// The post-indexed word form is the same story for a different reason: the
	// table entry for `ldr pc, [rn], #imm` is a copy of the LDRT handler
	// (CASE_16(0x430) and 0x4b0 in arm-new.h) that simply lacks the dest==15
	// block, so reg[15] ends up holding the loaded word - no alignment, no +4.
	// Only the W=0 words (0x410 / 0x490) jump.  The decoder does not keep the
	// raw W bit - a post-indexed transfer always writes back - so refuse them
	// all: a post-indexed `ldr pc` is vanishingly rare and one interpreter call
	// is cheaper than guessing which of the two shapes the word is.
	if (in->d == 15 && (thumb || in->sub != MEM_WORD || !pre))
		return JIT_NEED_INTERP;

	u32 value;
	switch (in->sub)
	{
	case MEM_BYTE:
		value = (in->flags & JF_SIGNED) ? (u32)(s32)(s8)CPUReadByte(address)
										: (u32)CPUReadByte(address);
		break;
	case MEM_HALF:
		value = (in->flags & JF_SIGNED) ? (u32)(s32)(s16)CPUReadHalfWordSigned(address)
										: (u32)CPUReadHalfWord(address);
		break;
	default:
		// V7 reads only these two classes of load without the Lua READ hook.
		value = (in->flags & JF_QUICK) ? CPUReadMemoryQuick(address)
									   : CPUReadMemory(address);
		break;
	}
	reg[in->d].I = value;
	if (in->d != in->n && wb)
		reg[in->n].I = pre ? address : wbValue;

	int ticks = base + (in->sub == MEM_WORD ? acc32(address) : acc16(address));
	if (!thumb)
		ticks += 3;

	*nextPc = pc + (thumb ? 2 : 4);

	if (in->d == 15)
	{
		// Only the ARM word form gets here (the others fell back above):
		// `ldr pc, [...]` charges two more cycles, aligns the loaded address to
		// four bytes and jumps there.
		ticks += 2;
		reg[15].I &= 0xFFFFFFFC;
		armNextPC  = reg[15].I;
		reg[15].I += 4;
		*nextPc = armNextPC;
	}

	return ticks;
}

static int doBlockTransfer(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc, int base)
{
	const u32 list = in->imm & 0xFFFF;
	int		  ticks = 0;
	bool	  first = true;

	if (in->op == JOP_PUSH)
	{
		u32 temp = reg[13].I - 4u * (u32)bitsSet(list & 0xFF);
		if (list & 0x4000)
			temp -= 4;
		u32 address = temp & 0xFFFFFFFC;

		const u32 count = (u32)bitsSet(list & 0xFF) + ((list & 0x4000) ? 1u : 0u);
		if (hitsIo(address, count * 4))
			return JIT_NEED_INTERP;

		for (int r = 0; r < 8; r++)
			if (list & (1u << r))
			{
				CPUWriteMemory(address, reg[r].I);
				ticks += 1 + (first ? acc32(address) : seq32(address));
				first	= false;
				address += 4;
			}
		if (list & 0x4000)
		{
			CPUWriteMemory(address, reg[14].I);
			ticks += 1 + (first ? acc32(address) : seq32(address));
		}

		reg[13].I = temp;
		*nextPc	  = pc + 2;
		return base + ticks;
	}

	if (in->op == JOP_POP)
	{
		const bool withPc = (list & 0x8000) != 0;
		u32 address = reg[13].I & 0xFFFFFFFC;
		u32 temp	= reg[13].I + 4u * (u32)bitsSet(list & 0xFF) + (withPc ? 4u : 0u);

		for (int r = 0; r < 8; r++)
			if (list & (1u << r))
			{
				reg[r].I = CPUReadMemory(address);
				ticks += 2 + (first ? acc32(address) : seq32(address));
				first	= false;
				address += 4;
			}

		if (!withPc)
		{
			reg[13].I = temp;
			*nextPc	  = pc + 2;
			return base + ticks;
		}

		// thumb.h:1410 - the PC slot charges a bare data access (no +2) and the
		// loaded address is only masked to halfword alignment; V7 does not
		// switch to ARM here even though bit 0 could be clear.
		reg[15].I = CPUReadMemory(address) & 0xFFFFFFFE;
		ticks += first ? acc32(address) : seq32(address);
		armNextPC  = reg[15].I;
		reg[15].I += 2;
		reg[13].I  = temp;
		*nextPc	   = armNextPC;
		return base + ticks;
	}

	const u8  baseReg = in->n;
	u32		  address, temp;

	switch (in->sub)
	{
	case BLOCK_IA:
		address = reg[baseReg].I & 0xFFFFFFFC;
		temp	= reg[baseReg].I + 4u * (u32)bitsSet(list);
		break;
	case BLOCK_IB:
		address = (reg[baseReg].I + 4) & 0xFFFFFFFC;
		temp	= reg[baseReg].I + 4u * (u32)bitsSet(list);
		break;
	case BLOCK_DA:
		temp	= reg[baseReg].I - 4u * (u32)bitsSet(list);
		address = (temp + 4) & 0xFFFFFFFC;
		break;
	default:	// BLOCK_DB
		temp	= reg[baseReg].I - 4u * (u32)bitsSet(list);
		address = temp & 0xFFFFFFFC;
		break;
	}

	if (in->op == JOP_STM)
	{
		if (hitsIo(address, 4u * ((u32)bitsSet(list) + ((list & 0x8000) ? 1u : 0u))))
			return JIT_NEED_INTERP;

		// ARM charges two cycles before the per-register accesses; Thumb's
		// thumbC0/thumbC8 do not.
		ticks = thumb ? 0 : 2;
		for (int r = 0; r < 15; r++)
			if (list & (1u << r))
			{
				CPUWriteMemory(address, reg[r].I);
				// STMW_*: the base is written back inside the loop, after the
				// first store; STM_* never write it back.
				if (in->flags & JF_WRITEBACK)
					reg[baseReg].I = temp;
				ticks += 1 + (first ? acc32(address) : seq32(address));
				first	= false;
				address += 4;
			}
		if (list & 0x8000)
		{
			CPUWriteMemory(address, reg[15].I + 4);
			if (in->flags & JF_WRITEBACK)
				reg[baseReg].I = temp;
			ticks += 1 + (first ? acc32(address) : seq32(address));
		}

		if (thumb)
		{
			// thumbC0 only moves r0-r7 and has no PC slot; keep the shape.
			*nextPc = pc + 2;
		}
		else
			*nextPc = pc + 4;
		return base + ticks;
	}

	// LDM
	ticks = thumb ? 0 : 2;
	for (int r = 0; r < 15; r++)
		if (list & (1u << r))
		{
			reg[r].I = CPUReadMemory(address);
			// V7's LDM loops are not symmetric: ARM's LDM_REG charges 1 per
			// register, Thumb's THUMB_LDM_REG charges 2 (same as POP).
			ticks += (thumb ? 2 : 1) + (first ? acc32(address) : seq32(address));
			first	= false;
			address += 4;
		}

	if (thumb)
	{
		// thumbC8: only r0-r7, and the base is written back unless it is one of
		// the loaded registers.
		if (!(list & (1u << baseReg)))
			reg[baseReg].I = temp;
		*nextPc = pc + 2;
		return base + ticks;
	}

	if (list & 0x8000)
	{
		reg[15].I = CPUReadMemory(address);
		ticks += 2 + (first ? acc32(address) : seq32(address));
		armNextPC  = reg[15].I;
		reg[15].I += 4;
	}

	if ((in->flags & JF_WRITEBACK) && !(list & (1u << baseReg)))
		reg[baseReg].I = temp;

	*nextPc = (list & 0x8000) ? armNextPC : pc + 4;
	return base + ticks;
}

static int doSwp(const JitInsn *in, u32 pc, u32 *nextPc, int base)
{
	const u32 address = reg[in->n].I;
	u32		  temp;

	// SWP writes as well as reads, so it can start a DMA too.
	if (hitsIo(address, in->sub == MEM_BYTE ? 1u : 4u))
		return JIT_NEED_INTERP;

	if (in->sub == MEM_BYTE)
	{
		temp = CPUReadByte(address);
		CPUWriteByte(address, reg[in->m].B.B0);
	}
	else
	{
		temp = CPUReadMemory(address);
		CPUWriteMemory(address, reg[in->m].I);
	}
	reg[in->d].I = temp;

	*nextPc = pc + 4;
	return base;		// V7 charges no data cycles for SWP
}

static int doBranch(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc, int base)
{
	if (thumb)
	{
		if (in->flags & JF_LINK)		// BL low half: target comes from LR
		{
			const u32 target = (reg[14].I + in->imm) & 0xFFFFFFFE;
			reg[14].I = (pc + 2) | 1;
			armNextPC = target;
			reg[15].I = target + 2;
			*nextPc	  = armNextPC;
			return base;
		}

		// V7 adds nothing for a branch; a taken conditional branch *assigns*
		// 3 to clockTicks instead of adding to the base count.
		reg[15].I += in->imm;
		armNextPC  = reg[15].I;
		reg[15].I += 2;
		*nextPc = armNextPC;
		return (in->cond == 0xE) ? base : 3;
	}

	if (in->flags & JF_LINK)
		reg[14].I = reg[15].I - 4;		// reg[15] is pc+8 -> LR = pc+4

	reg[15].I += in->imm;
	armNextPC  = reg[15].I;
	reg[15].I += 4;
	*nextPc = armNextPC;
	return base + 3;
}

static int doBx(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc, int base)
{
	const u32 target = reg[in->m & 0x0F].I;

	armState = (target & 1) ? false : true;

	if (armState)
	{
		reg[15].I = target & 0xFFFFFFFC;
		armNextPC = reg[15].I;
		reg[15].I += 4;
	}
	else
	{
		reg[15].I = target & 0xFFFFFFFE;
		armNextPC = reg[15].I;
		reg[15].I += 2;
	}
	*nextPc = armNextPC;
	return base + (thumb ? 0 : 3);		// thumb47 does not touch clockTicks
}

static int doMrs(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc, int base)
{
	if (!in->sub)
		CPUUpdateCPSR();		// arm100 resyncs reg[16]; arm140 does not
	reg[in->d].I = in->sub ? reg[17].I : reg[16].I;
	*nextPc		 = pc + (thumb ? 2 : 4);
	return base;
}

static int doMsr(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc, int base)
{
	// Only the SPSR form is decoded for V7; the CPSR form calls
	// CPUSwitchMode(..., breakLoop=true) even for a flags-only write.
	if (!in->sub)
		return JIT_NEED_INTERP;

	const u32 value = (in->flags & JF_IMM)
						  ? ror32(in->imm & 0xFF, (in->n & 0x0F) * 2)
						  : reg[in->m & 0x0F].I;

	// arm160/arm360: no CPUUpdateCPSR, no CPUUpdateFlags, no mode change, and
	// the emulator's test excludes System (0x1F) as well as User.
	if (armMode > 0x10 && armMode < 0x1F)
	{
		if (in->d & 1) reg[17].I = (reg[17].I & 0xFFFFFF00) | (value & 0x000000FF);
		if (in->d & 2) reg[17].I = (reg[17].I & 0xFFFF00FF) | (value & 0x0000FF00);
		if (in->d & 4) reg[17].I = (reg[17].I & 0xFF00FFFF) | (value & 0x00FF0000);
		if (in->d & 8) reg[17].I = (reg[17].I & 0x00FFFFFF) | (value & 0xFF000000);
	}

	*nextPc = pc + (thumb ? 2 : 4);
	return base;
}

} // namespace

int jitInterpInsn(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc)
{
	// Same convention as the V8 implementation: what the interpreter's loop
	// body has done by the time a handler runs.  V7 has no prefetch buffer, so
	// the only state to set up is armNextPC (pc+2/4) and reg[15] (pc+2/4 plus
	// the instruction size).
	const u32 fetch = pc + (thumb ? 2 : 4);
	armNextPC  = fetch;
	reg[15].I  = pc + (thumb ? 4 : 8);
	*nextPc	   = fetch;

	// arm-new.h:1413 / thumb.h:184.  Both take the fetch wait from the address
	// of the instruction being executed, before armNextPC is advanced.
	const int base = thumb
						 ? (int)in->fetchCycles + (int)memoryWaitFetch[(pc >> 24) & 15]
						 : (int)memoryWaitFetch32[(pc >> 24) & 15];

	if (!condTrue(in->cond))
		return base;

	switch (in->op)
	{
	case JOP_ALU:	return doAlu(in, thumb, pc, nextPc, base);
	case JOP_MUL:	return doMul(in, thumb, pc, nextPc, base);
	case JOP_LOAD:
	case JOP_STORE: return doLoadStore(in, thumb, pc, nextPc, base);
	case JOP_LDM:
	case JOP_STM:
	case JOP_PUSH:
	case JOP_POP:	return doBlockTransfer(in, thumb, pc, nextPc, base);
	case JOP_SWP:	return doSwp(in, pc, nextPc, base);
	case JOP_BRANCH: return doBranch(in, thumb, pc, nextPc, base);
	case JOP_BX:	return doBx(in, thumb, pc, nextPc, base);
	case JOP_MRS:	return doMrs(in, thumb, pc, nextPc, base);
	case JOP_MSR:	return doMsr(in, thumb, pc, nextPc, base);

	case JOP_SWI:
	case JOP_UNDEF:
		// CPUSoftwareInterrupt()/CPUUndefinedException() enter a mode switch
		// and the SWI handler runs from the BIOS; the interpreter does both
		// better than a block can.
		return JIT_NEED_INTERP;

	case JOP_END:
	default:
		return -1;		// not an executable instruction: caller bug
	}
}

#else

#include "../V8/GBACpu.h"

namespace {

static inline bool condTrue(u32 cond)
{
	switch (cond)
	{
	case 0x0: return Z_FLAG;
	case 0x1: return !Z_FLAG;
	case 0x2: return C_FLAG;
	case 0x3: return !C_FLAG;
	case 0x4: return N_FLAG;
	case 0x5: return !N_FLAG;
	case 0x6: return V_FLAG;
	case 0x7: return !V_FLAG;
	case 0x8: return C_FLAG && !Z_FLAG;
	case 0x9: return !C_FLAG || Z_FLAG;
	case 0xA: return N_FLAG == V_FLAG;
	case 0xB: return N_FLAG != V_FLAG;
	case 0xC: return !Z_FLAG && (N_FLAG == V_FLAG);
	case 0xD: return Z_FLAG || (N_FLAG != V_FLAG);
	case 0xE: return true;
	default:  return false;		// 0xF: "never" - armExecute() skips it
	}
}

// "shift by register" forms cost one extra cycle in ARM and a different base
// count in Thumb, so both backends want to recognise them.
static inline bool isRegShift(u8 shift)
{
	return shift == SHIFT_LSL_REG || shift == SHIFT_LSR_REG ||
		   shift == SHIFT_ASR_REG || shift == SHIFT_ROR_REG ||
		   shift == SHIFT_ROR_REG_T || shift == SHIFT_ROR_REG_V7;
}

static inline int bitsSet(u32 v)
{
	return cpuBitsSet[v & 0xFF] + cpuBitsSet[(v >> 8) & 0xFF];
}

// The emulator uses _rotr() here, which is an MSVC intrinsic; spell it out so
// the IR interpreter also builds with clang.
static inline u32 ror32(u32 v, u32 n)
{
	n &= 31;
	return n ? ((v >> n) | (v << (32 - n))) : v;
}

// Second operand of a data-processing instruction, with the carry output the
// flag update needs.  Mirrors the VALUE_* macros, which is why the shift-by-zero
// cases look odd: they are the emulator's behaviour, not the architecture's.
static u32 evalOperand2(const JitInsn *in, bool *carryOut)
{
	const u32 v	  = (in->flags & JF_IMM) ? in->imm : reg[in->m & 0x0F].I;
	u32		  shift = in->imm;

	if (in->flags & JF_IMM)
	{
		// The only immediate form that shifts is the rotated 8-bit one, which
		// packs (rotate << 8) | imm8 into `imm`.
		if (in->shift != SHIFT_IMM_ROT)
			return v;

		const u32 rot	= (in->imm >> 8) & 0x0F;
		const u32 imm8	= in->imm & 0xFF;
		if (rot == 0)
			return imm8;
		const u32 amount = rot * 2;
		*carryOut = ((imm8 >> (amount - 1)) & 1) != 0;
		return (imm8 >> amount) | (imm8 << (32 - amount));
	}

	switch (in->shift)
	{
	case SHIFT_NONE:
		return v;

	case SHIFT_LSL_IMM:
		if (shift == 0)
			return v;
		*carryOut = ((v >> (32 - shift)) & 1) != 0;
		return v << shift;

	case SHIFT_LSR_IMM:
		if (shift == 0)
		{
			*carryOut = (v & 0x80000000) != 0;
			return 0;
		}
		*carryOut = ((v >> (shift - 1)) & 1) != 0;
		return v >> shift;

	case SHIFT_ASR_IMM:
		if (shift == 0)
		{
			if (v & 0x80000000)
			{
				*carryOut = true;
				return 0xFFFFFFFF;
			}
			*carryOut = false;
			return 0;
		}
		*carryOut = (((s32)v >> (shift - 1)) & 1) != 0;
		return (u32)((s32)v >> shift);

	case SHIFT_ROR_IMM:
		if (shift == 0)		// RRX
		{
			*carryOut = (v & 1) != 0;
			return (v >> 1) | ((u32)C_FLAG << 31);
		}
		{
			const u32 r = (v >> shift) | (v << (32 - shift));
			*carryOut = (r & 0x80000000) != 0;
			return r;
		}

	case SHIFT_LSL_REG:
		shift = reg[in->imm & 0x0F].B.B0;
		if (shift == 0)
			return v;
		if (shift == 32)
		{
			*carryOut = (v & 1) != 0;
			return 0;
		}
		if (shift < 32)
		{
			*carryOut = ((v >> (32 - shift)) & 1) != 0;
			return v << shift;
		}
		*carryOut = false;
		return 0;

	case SHIFT_LSR_REG:
		shift = reg[in->imm & 0x0F].B.B0;
		if (shift == 0)
			return v;
		if (shift == 32)
		{
			*carryOut = (v & 0x80000000) != 0;
			return 0;
		}
		if (shift < 32)
		{
			*carryOut = ((v >> (shift - 1)) & 1) != 0;
			return v >> shift;
		}
		*carryOut = false;
		return 0;

	case SHIFT_ASR_REG:
		shift = reg[in->imm & 0x0F].B.B0;
		if (shift < 32)
		{
			if (shift == 0)
				return v;
			*carryOut = (((s32)v >> (shift - 1)) & 1) != 0;
			return (u32)((s32)v >> shift);
		}
		if (v & 0x80000000)
		{
			*carryOut = true;
			return 0xFFFFFFFF;
		}
		*carryOut = false;
		return 0;

	case SHIFT_ROR_REG:
	{
		// "ror r32, cl" masks the count to five bits, so only shift & 31 counts.
		const u32 n = reg[in->imm & 0x0F].B.B0 & 0x1F;
		if (n == 0)
			return v;
		const u32 r = (v >> n) | (v << (32 - n));
		*carryOut = (r & 0x80000000) != 0;
		return r;
	}

	case SHIFT_ROR_REG_T:
	{
		// Thumb's version of the same instruction (ROR_RD_RS), which unlike the
		// ARM one writes the carry even when the count masks down to zero.
		const u32 raw = reg[in->imm & 0x0F].B.B0;
		if (raw == 0)
			return v;
		const u32 n = raw & 0x1F;
		if (n == 0)
		{
			*carryOut = (v & 0x80000000) != 0;
			return v;
		}
		const u32 r = (v >> n) | (v << (32 - n));
		*carryOut = (r & 0x80000000) != 0;
		return r;
	}

	case SHIFT_ROR_REG_V7:
	{
		// V7's ARM ROR (arm-new.h:616-636): even a raw count of 0 writes the
		// carry, which neither SHIFT_ROR_REG nor SHIFT_ROR_REG_T does.
		const u32 n = reg[in->imm & 0x0F].B.B0 & 0x1F;
		if (n == 0)
		{
			*carryOut = (v & 0x80000000) != 0;
			return v;
		}
		const u32 r = (v >> n) | (v << (32 - n));
		*carryOut = (r & 0x80000000) != 0;
		return r;
	}

	default:
		return v;
	}
}

// Address offset of a load/store.  This is the OFFSET_* family, not VALUE_*,
// and the two disagree when the shift amount is zero:
//
//   offset lsr #0  -> 0            (value lsr #0 would be a shift of 32)
//   offset asr #0  -> sign fill    (same shift, but no carry bookkeeping)
//   offset ror #0  -> RRX
//
// None of them touch the flags.
static u32 evalOffset(const JitInsn *in)
{
	const u32 v		= reg[in->m & 0x0F].I;
	const u32 shift = in->imm;

	switch (in->shift)
	{
	case SHIFT_LSL_IMM:
		return v << shift;
	case SHIFT_LSR_IMM:
		return shift ? (v >> shift) : 0;
	case SHIFT_ASR_IMM:
		if (shift)
			return (u32)((s32)v >> shift);
		return (v & 0x80000000) ? 0xFFFFFFFF : 0;
	case SHIFT_ROR_IMM:
		if (shift)
			return (v >> shift) | (v << (32 - shift));
		return (v >> 1) | ((u32)C_FLAG << 31);
	default:
		return v;
	}
}

// Only the flags named in the mask are written; the two instruction sets
// disagree about which ones an "add" or an "and" updates.
static void setLogicalFlags(u32 res, bool carryOut, u8 mask)
{
	if (mask & JITF_N) N_FLAG = ((s32)res < 0);
	if (mask & JITF_Z) Z_FLAG = (res == 0);
	if (mask & JITF_C) C_FLAG = carryOut;
}

// NEG(x)/POS(x) exactly as the emulator spells them: the whole word is all
// ones for "negative", zero otherwise, so the three-way AND/OR below can be
// shifted down to a single bit.  Doing it this way matters: 0 - x is *not*
// the same mask as x >> 31, and the difference shows up on every 0-0 and
// 0+0 case.
static inline u32 negMask(u32 v)
{
	return (u32)((s32)v >> 31);
}

static inline u32 posMask(u32 v)
{
	return ~negMask(v);
}

static void setAddFlags(u32 a, u32 b, u32 res, u8 mask)
{
	if (mask & JITF_N) N_FLAG = ((s32)res < 0);
	if (mask & JITF_Z) Z_FLAG = (res == 0);
	if (mask & JITF_V)
		V_FLAG = (((negMask(a) & negMask(b) & posMask(res)) |
				   (posMask(a) & posMask(b) & negMask(res))) &
				  0x80000000) != 0;
	if (mask & JITF_C)
		C_FLAG = (((negMask(a) & negMask(b)) |
				   (negMask(a) & posMask(res)) |
				   (negMask(b) & posMask(res))) &
				  0x80000000) != 0;
}

static void setSubFlags(u32 a, u32 b, u32 res, u8 mask)
{
	if (mask & JITF_N) N_FLAG = ((s32)res < 0);
	if (mask & JITF_Z) Z_FLAG = (res == 0);
	if (mask & JITF_V)
		V_FLAG = (((negMask(a) & posMask(b) & posMask(res)) |
				   (posMask(a) & negMask(b) & negMask(res))) &
				  0x80000000) != 0;
	if (mask & JITF_C)
		C_FLAG = (((negMask(a) & posMask(b)) |
				   (negMask(a) & posMask(res)) |
				   (posMask(b) & posMask(res))) &
				  0x80000000) != 0;
}

static int doAlu(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc)
{
	bool	  carryOut = C_FLAG;
	const u32 rhs	   = evalOperand2(in, &carryOut);
	u32		  lhs	   = reg[in->n & 0x0F].I;
	if (in->flags & JF_PC_ALIGN4)
		lhs &= 0xFFFFFFFC;

	const u8   mask = in->flagMask;
	// Which ALU ops actually write a destination.  The PC-write path below is
	// taken by all of them (the emulator branches on the raw Rd field), but the
	// register write and the mode switch only happen for these.
	const bool writesRd = in->sub < ALU_TST || in->sub > ALU_CMN;
	const bool toPc		= (in->d == 15);
	u32		   res	= 0;

	switch (in->sub)
	{
	case ALU_AND: res = lhs & rhs;	  reg[in->d].I = res; if (!toPc) setLogicalFlags(res, carryOut, mask); break;
	case ALU_EOR: res = lhs ^ rhs;	  reg[in->d].I = res; if (!toPc) setLogicalFlags(res, carryOut, mask); break;
	case ALU_SUB: res = lhs - rhs;	  reg[in->d].I = res; if (!toPc) setSubFlags(lhs, rhs, res, mask); break;
	case ALU_RSB: res = rhs - lhs;	  reg[in->d].I = res; if (!toPc) setSubFlags(rhs, lhs, res, mask); break;
	case ALU_ADD: res = lhs + rhs;	  reg[in->d].I = res; if (!toPc) setAddFlags(lhs, rhs, res, mask); break;
	case ALU_ADC: res = lhs + rhs + (u32)C_FLAG; reg[in->d].I = res;
				  if (!toPc) setAddFlags(lhs, rhs, res, mask); break;
	case ALU_SBC: res = lhs - rhs - !((u32)C_FLAG); reg[in->d].I = res;
				  if (!toPc) setSubFlags(lhs, rhs, res, mask); break;
	case ALU_RSC: res = rhs - lhs - !((u32)C_FLAG); reg[in->d].I = res;
				  if (!toPc) setSubFlags(rhs, lhs, res, mask); break;

	case ALU_TST: res = lhs & rhs; if (mask) setLogicalFlags(res, carryOut, mask); break;
	case ALU_TEQ: res = lhs ^ rhs; if (mask) setLogicalFlags(res, carryOut, mask); break;
	case ALU_CMP: res = lhs - rhs; if (mask) setSubFlags(lhs, rhs, res, mask); break;
	case ALU_CMN: res = lhs + rhs; if (mask) setAddFlags(lhs, rhs, res, mask); break;

	case ALU_ORR: res = lhs | rhs;	  reg[in->d].I = res; if (!toPc) setLogicalFlags(res, carryOut, mask); break;
	case ALU_MOV: res = rhs;		  reg[in->d].I = res; if (!toPc) setLogicalFlags(res, carryOut, mask); break;
	case ALU_BIC: res = lhs & ~rhs;	  reg[in->d].I = res; if (!toPc) setLogicalFlags(res, carryOut, mask); break;
	case ALU_MVN: res = ~rhs;		  reg[in->d].I = res; if (!toPc) setLogicalFlags(res, carryOut, mask); break;
	default: break;
	}

	if (toPc)
	{
		// The emulator's ALU macro tests `dest != 15` for *every* instruction,
		// compare ops included.  TST/TEQ/CMP/CMN never write a register, but a
		// data word that has 15 in Rd still takes this path: the PC is refetched
		// at reg[15] (= pc+8), so the instruction after this one is skipped.
		//
		// Only the forms that really write PC switch mode - C_CHECK_PC is empty
		// for the compare instructions - and only they suppress the flag update.
		if (mask && writesRd)
			CPUSwitchMode(reg[17].I & 0x1F, false);

		// CPUSwitchMode() above may already have changed the instruction set,
		// and the emulator picks both the alignment and the cycle formula from
		// the *new* state: `SUBS pc, lr, #4` is an ARM instruction that returns
		// into Thumb code (0xE25EF004 closes every internal-BIOS call).  Masking
		// to 4 bytes here dropped bit 1 of that return address, so every BIOS
		// call came back two bytes early - the game then ran the wrong half of
		// a Thumb pair and hung.
		if (!armState)
		{
			reg[15].I &= 0xFFFFFFFE;
			armNextPC  = reg[15].I;
			reg[15].I += 2;
			THUMB_PREFETCH;
			*nextPc = armNextPC;

			// A Thumb high-register write (MOV/ADD Hd,pc) uses the Thumb
			// formula, but an *ARM* ALU instruction can also land in Thumb:
			// `SUBS pc, lr, #4` closes every internal-BIOS SWI and IRQ return.
			// The ARM handler's clockTicks expression does not depend on the
			// new state - it always charges 32-bit code accesses.  Using the
			// 16-bit formula here made every exception return six cycles short,
			// which accumulated until an IRQ was delivered one instruction late.
			if (thumb)
			{
				// thumb44/thumb46 spell this as
				// `codeTicksAccessSeq16(armNextPC) * 2` - the function is called
				// once and its result doubled.  Calling it twice (which is what
				// the ARM formula below really does) applies its
				// busPrefetchCount side effect twice and charged up to eight
				// cycles too many on the second instruction of a block.
				const int seq = codeTicksAccessSeq16(armNextPC);
				return 3 + seq + seq + codeTicksAccess16(armNextPC);
			}
			return 3 + (isRegShift(in->shift) ? 1 : 0) +
				   codeTicksBranch32NonSeqFirst(armNextPC);
		}

		reg[15].I &= 0xFFFFFFFC;
		armNextPC  = reg[15].I;
		reg[15].I += 4;
		ARM_PREFETCH;
		*nextPc = armNextPC;
		return 3 + (isRegShift(in->shift) ? 1 : 0) +
			   codeTicksBranch32NonSeqFirst(armNextPC);
	}

	*nextPc = pc + (thumb ? 2 : 4);

	if (thumb)
	{
		// thumb40_2/40_3/41_0/41_3 are the only ALU handlers that set clockTicks.
		if (isRegShift(in->shift))
			return 2 + codeTicksAccess16(armNextPC);
		return 1 + codeTicksAccessSeq16(armNextPC);
	}
	return 1 + (isRegShift(in->shift) ? 1 : 0) + codeTicksAccessSeq32(armNextPC);
}

static int doMul(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc)
{
	const u32 rs = reg[in->m].I;

	// Thumb's MUL multiplies the destination by the source and takes its timing
	// from the destination's old value; ARM's takes it from Rs.
	const u32 magSrc = thumb ? reg[in->d].I : rs;

	switch (in->sub)
	{
	case MUL_MUL:
		reg[in->d].I = thumb ? rs * magSrc : reg[in->n].I * rs;
		break;
	case MUL_MLA:
		reg[in->d].I = reg[in->n].I * rs + reg[in->imm & 0x0F].I;
		break;
	case MUL_UMULL:
	case MUL_SMULL:
	{
		const u64 a   = (in->sub == MUL_SMULL) ? (u64)(s64)(s32)reg[in->n].I : (u64)reg[in->n].I;
		const u64 b   = (in->sub == MUL_SMULL) ? (u64)(s64)(s32)rs : (u64)rs;
		const u64 res = a * b;
		reg[in->imm & 0x0F].I = (u32)res;
		reg[in->d].I			  = (u32)(res >> 32);
		break;
	}
	case MUL_UMLAL:
	case MUL_SMLAL:
	{
		const u64 acc = ((u64)reg[in->d].I << 32) | reg[in->imm & 0x0F].I;
		const u64 a   = (in->sub == MUL_SMLAL) ? (u64)(s64)(s32)reg[in->n].I : (u64)reg[in->n].I;
		const u64 b   = (in->sub == MUL_SMLAL) ? (u64)(s64)(s32)rs : (u64)rs;
		const u64 res = acc + a * b;
		reg[in->imm & 0x0F].I = (u32)res;
		reg[in->d].I		  = (u32)(res >> 32);
		break;
	}
	}

	const u8 mask = in->flagMask;
	if (mask)
	{
		if (in->sub >= MUL_UMULL)
		{
			if (mask & JITF_N) N_FLAG = (reg[in->d].I & 0x80000000) != 0;
			if (mask & JITF_Z) Z_FLAG = (reg[in->d].I == 0) && (reg[in->imm & 0x0F].I == 0);
		}
		else
		{
			if (mask & JITF_N) N_FLAG = ((s32)reg[in->d].I < 0);
			if (mask & JITF_Z) Z_FLAG = reg[in->d].I == 0;
		}
	}

	// 0..3 cycles depending on how many significant bytes the multiplier has.
	u32 mag = magSrc;
	if ((s32)mag < 0)
		mag = ~mag;
	int extra;
	if ((mag & 0xFFFFFF00) == 0)
		extra = 0;
	else if ((mag & 0xFFFF0000) == 0)
		extra = 1;
	else if ((mag & 0xFF000000) == 0)
		extra = 2;
	else
		extra = 3;

	*nextPc = pc + (thumb ? 2 : 4);

	if (thumb)
	{
		// thumb43_1: clockTicks = 1 + extra, then it *replaces* the low bits of
		// busPrefetchCount, then adds 1 + the code access.
		const u32 n = (u32)(1 + extra);
		busPrefetchCount = (busPrefetchCount << n) | (0xFFu >> (8 - n));
		return (int)n + 1 + codeTicksAccess16(armNextPC);
	}

	if (busPrefetchCount == 0)
		busPrefetchCount = ((busPrefetchCount + 1) << extra) - 1;
	return extra + 1 + codeTicksAccess32(armNextPC);
}

static int doLoadStore(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc)
{
	if (busPrefetchCount == 0)
		busPrefetch = busPrefetchEnable;

	const bool pre = (in->flags & JF_PRE) != 0;
	const bool up  = (in->flags & JF_UP) != 0;
	const bool wb  = (in->flags & JF_WRITEBACK) != 0;
	const bool wide = (in->sub == MEM_WORD) || ((in->flags & JF_DATA32) != 0);

	u32 base = reg[in->n & 0x0F].I;
	if (in->flags & JF_PC_ALIGN4)
		base &= 0xFFFFFFFC;

	const u32 off	 = (in->flags & JF_IMM) ? in->imm : evalOffset(in);
	const u32 address = pre ? (up ? base + off : base - off) : base;
	const u32 wbValue = up ? base + off : base - off;

	if (in->op == JOP_STORE)
	{
		// The interpreter writes the base back before the store for the
		// pre-indexed form and after it for the post-indexed form.
		if (pre && wb)
			reg[in->n].I = address;

		switch (in->sub)
		{
		case MEM_BYTE: CPUWriteByte(address, (u8)reg[in->d].I); break;
		case MEM_HALF: CPUWriteHalfWord(address, (u16)reg[in->d].I); break;
		default:	   CPUWriteMemory(address, reg[in->d].I); break;
		}

		if (!pre && wb)
			reg[in->n].I = wbValue;

		*nextPc = pc + (thumb ? 2 : 4);
		// Data access first, then the fetch: both helpers move
		// busPrefetchCount, so the order decides the total (see GBACpu.h).
		const int dticks = wide ? dataTicksAccess32(address) : dataTicksAccess16(address);
		return 2 + dticks +
			   (thumb ? codeTicksAccess16(armNextPC) : codeTicksAccess32(armNextPC));
	}

	// Load.
	u32 value = 0;
	switch (in->sub)
	{
	case MEM_BYTE:
		value = (in->flags & JF_SIGNED) ? (u32)(s32)(s8)CPUReadByte(address)
										: CPUReadByte(address);
		break;
	case MEM_HALF:
		value = (in->flags & JF_SIGNED) ? (u32)(s32)(s16)CPUReadHalfWordSigned(address)
										: CPUReadHalfWord(address);
		break;
	default:
		// thumb48 (ldr Rd, [pc, #imm]) and thumb98 (ldr Rd, [sp, #imm]) both
		// read with CPUReadMemoryQuick in the interpreter, so they do not call
		// the Lua READ hook; every other word load does.
		value = (in->flags & JF_QUICK) ? CPUReadMemoryQuick(address) : CPUReadMemory(address);
		break;
	}

	// thumb48 (ldr Rd, [pc, #imm]) drops the prefetch buffer after the load.
	if (in->flags & JF_CLEAR_PREFETCH)
		busPrefetchCount = 0;

	reg[in->d].I = value;

	// LDR only writes back when the destination is not the base register.
	if (in->d != in->n && wb)
		reg[in->n].I = pre ? address : wbValue;

	if (in->d == 15)
	{
		reg[15].I &= 0xFFFFFFFC;
		armNextPC  = reg[15].I;
		reg[15].I += 4;
		ARM_PREFETCH;
		*nextPc = armNextPC;
		// The prefetch penalty is charged before the data access, and both move
		// busPrefetchCount, so the order here is not cosmetic.
		int ticks = 2 + dataTicksAccessSeq32(address) + dataTicksAccessSeq32(address);
		const int dticks = wide ? dataTicksAccess32(address) : dataTicksAccess16(address);
		ticks += 3 + dticks + codeTicksAccess32(armNextPC);
		return ticks;
	}

	*nextPc = pc + (thumb ? 2 : 4);
	const int dticks = wide ? dataTicksAccess32(address) : dataTicksAccess16(address);
	return 3 + dticks +
		   (thumb ? codeTicksAccess16(armNextPC) : codeTicksAccess32(armNextPC));
}

// Thumb PUSH/POP.  Separate from the ARM block transfers because the emulator
// implements them as their own handlers with their own (odd) accounting.
static int doPushPop(const JitInsn *in, u32 pc, u32 *nextPc)
{
	if (busPrefetchCount == 0)
		busPrefetch = busPrefetchEnable;

	int ticks = 0;
	int count = 0;

	if (in->op == JOP_PUSH)
	{
		const u32 list = in->imm & 0xFFFF;
		u32 temp = reg[13].I - 4u * (u32)bitsSet(list & 0xFF);
		if (list & 0x4000)
			temp -= 4;
		u32 address = temp & 0xFFFFFFFC;

		for (int r = 0; r < 8; r++)
			if (list & (1u << r))
			{
				CPUWriteMemory(address, reg[r].I);
				ticks += 1 + (count ? dataTicksAccessSeq32(address) : dataTicksAccess32(address));
				count++;
				address += 4;
			}
		if (list & 0x4000)
		{
			CPUWriteMemory(address, reg[14].I);
			ticks += 1 + (count ? dataTicksAccessSeq32(address) : dataTicksAccess32(address));
			count++;
			address += 4;
		}

		ticks += 1 + codeTicksAccess16(armNextPC);
		reg[13].I = temp;
		*nextPc = pc + 2;
		return ticks;
	}

	// POP
	const u32 list	  = in->imm & 0xFFFF;
	const bool withPc = (list & 0x8000) != 0;
	u32 address = reg[13].I & 0xFFFFFFFC;
	u32 temp	= reg[13].I + 4u * (u32)bitsSet(list & 0xFF) + (withPc ? 4u : 0u);

	for (int r = 0; r < 8; r++)
		if (list & (1u << r))
		{
			reg[r].I = CPUReadMemory(address);
			ticks += 1 + (count ? dataTicksAccessSeq32(address) : dataTicksAccess32(address));
			count++;
			address += 4;
		}

	if (!withPc)
	{
		reg[13].I = temp;
		// thumbBC assigns, so the accumulated data ticks are dropped.
		return 2 + codeTicksAccess16(armNextPC);
	}

	reg[15].I = CPUReadMemory(address) & 0xFFFFFFFE;
	ticks += 1 + (count ? dataTicksAccessSeq32(address) : dataTicksAccess32(address));
	count++;

	armNextPC  = reg[15].I;
	reg[15].I += 2;
	reg[13].I  = temp;
	THUMB_PREFETCH;
	busPrefetchCount = 0;
	ticks += 3 + codeTicksAccess16(armNextPC) + codeTicksAccess16(armNextPC);
	*nextPc = armNextPC;
	return ticks;
}

static int doBlockTransfer(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc)
{
	if (in->op == JOP_PUSH || in->op == JOP_POP)
		return doPushPop(in, pc, nextPc);

	if (busPrefetchCount == 0)
		busPrefetch = busPrefetchEnable;

	const int count = bitsSet(in->imm & 0xFFFF);
	const u8  base	= in->n;
	u32		  address, temp;

	switch (in->sub)
	{
	case BLOCK_IA:
		address = reg[base].I & 0xFFFFFFFC;
		temp	= reg[base].I + 4u * (u32)count;
		break;
	case BLOCK_IB:
		address = (reg[base].I + 4) & 0xFFFFFFFC;
		temp	= reg[base].I + 4u * (u32)count;
		break;
	case BLOCK_DA:
		temp	= reg[base].I - 4u * (u32)count;
		address = (temp + 4) & 0xFFFFFFFC;
		break;
	default:	// BLOCK_DB
		temp	= reg[base].I - 4u * (u32)count;
		address = temp & 0xFFFFFFFC;
		break;
	}

	int ticks = 0;

	if (in->op == JOP_STM)
	{
		bool first = true;
		for (int r = 0; r < 15; r++)
			if (in->imm & (1u << r))
			{
				CPUWriteMemory(address, reg[r].I);
				// STMW_* write the base back inside the loop, STM_* never do.
				if (in->flags & JF_WRITEBACK)
					reg[base].I = temp;
				ticks += 1 + (first ? dataTicksAccess32(address) : dataTicksAccessSeq32(address));
				first  = false;
				address += 4;
			}
		if (in->imm & 0x8000)
		{
			CPUWriteMemory(address, reg[15].I + 4);
			if (in->flags & JF_WRITEBACK)
				reg[base].I = temp;
			ticks += 1 + (first ? dataTicksAccess32(address) : dataTicksAccessSeq32(address));
		}
		if (thumb)
		{
			// thumbC0 assigns clockTicks at the end, discarding the sum above.
			*nextPc = pc + 2;
			return 1 + codeTicksAccess16(armNextPC);
		}
		*nextPc = pc + 4;
		return ticks + 1 + codeTicksAccess32(armNextPC);
	}

	// LDM
	bool first = true;
	for (int r = 0; r < 15; r++)
		if (in->imm & (1u << r))
		{
			reg[r].I = CPUReadMemory(address);
			ticks += 1 + (first ? dataTicksAccess32(address) : dataTicksAccessSeq32(address));
			first  = false;
			address += 4;
		}

	if (thumb)
	{
		// thumbC8 assigns clockTicks, so the per-register data ticks above are
		// dropped; the accesses themselves still happened.  Only r0-r7 can be in
		// the list, so there is no PC to load.
		if (!(in->imm & (1u << base)))
			reg[base].I = temp;
		*nextPc = pc + 2;
		return 2 + codeTicksAccess16(armNextPC);
	}

	if (in->imm & 0x8000)		// LDM with PC in the list refetches
	{
		reg[15].I = CPUReadMemory(address);
		ticks += 1 + (first ? dataTicksAccess32(address) : dataTicksAccessSeq32(address));
		armNextPC  = reg[15].I;
		reg[15].I += 4;
		ARM_PREFETCH;
		ticks += 1 + codeTicksAccessSeq32(armNextPC);
	}

	if ((in->flags & JF_WRITEBACK) && !(in->imm & (1u << base)))
		reg[base].I = temp;

	*nextPc = (in->imm & 0x8000) ? armNextPC : pc + 4;
	return ticks + 2 + codeTicksAccess32(armNextPC);
}

static int doSwp(const JitInsn *in, u32 pc, u32 *nextPc)
{
	const u32 address = reg[in->n].I;
	u32		  temp;

	if (in->sub == MEM_BYTE)
	{
		temp = CPUReadByte(address);
		CPUWriteByte(address, reg[in->m].B.B0);
	}
	else
	{
		temp = CPUReadMemory(address);
		CPUWriteMemory(address, reg[in->m].I);
	}
	reg[in->d].I = temp;

	*nextPc = pc + 4;
	const int dticks = 4 + dataTicksAccess32(address) + dataTicksAccess32(address);
	return dticks + codeTicksAccess32(armNextPC);
}

static int doBranch(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc)
{
	if (thumb)
	{
		if (in->flags & JF_LINK)		// second half of BL: target comes from LR
		{
			const u32 target = (reg[14].I + in->imm) & 0xFFFFFFFE;
			reg[15].I  = target;
			armNextPC  = target;
			reg[15].I  = target + 2;
			reg[14].I  = (pc + 2) | 1;
			THUMB_PREFETCH;
			*nextPc = armNextPC;
			// Sequential - non-sequential - sequential, the order the
			// interpreter's thumbF8 uses (see GBACpu.h).
			const int blSeq1 = codeTicksAccessSeq16(armNextPC);
			const int blNseq = codeTicksAccess16(armNextPC);
			const int blSeq2 = codeTicksAccessSeq16(armNextPC);
			const int ticks  = 3 + blSeq1 + blNseq + blSeq2;
			busPrefetchCount = 0;
			return ticks;
		}

		reg[15].I += in->imm;
		armNextPC  = reg[15].I;
		reg[15].I += 2;
		THUMB_PREFETCH;
		*nextPc = armNextPC;
		const int ticks = 3 + codeTicksBranch16(armNextPC);
		busPrefetchCount = 0;
		return ticks;
	}

	if (in->flags & JF_LINK)
		reg[14].I = reg[15].I - 4;

	reg[15].I += in->imm;
	armNextPC  = reg[15].I;
	reg[15].I += 4;
	ARM_PREFETCH;
	*nextPc = armNextPC;

	int ticks = 1 + codeTicksAccessSeq32(armNextPC);
	const int bNseq = codeTicksAccess32(armNextPC);
	const int bSeq  = codeTicksAccessSeq32(armNextPC);
	ticks += 2 + bNseq + bSeq;
	busPrefetchCount = 0;
	return ticks;
}

static int doBx(const JitInsn *in, u32 pc, u32 *nextPc)
{
	const u32 target = reg[in->m & 0x0F].I;
	busPrefetchCount = 0;
	armState		 = (target & 1) ? false : true;

	if (armState)
	{
		reg[15].I = target & 0xFFFFFFFC;
		armNextPC = reg[15].I;
		reg[15].I += 4;
		ARM_PREFETCH;
		*nextPc = armNextPC;
		return 3 + codeTicksBranch32(armNextPC);
	}

	reg[15].I = target & 0xFFFFFFFE;
	armNextPC = reg[15].I;
	reg[15].I += 2;
	THUMB_PREFETCH;
	*nextPc = armNextPC;
	return 3 + codeTicksBranch16(armNextPC);
}

static int doMrs(const JitInsn *in, u32 pc, u32 *nextPc)
{
	// arm100 (CPSR) syncs reg[16] from the flag variables first; arm140 (SPSR)
	// does not - it just copies reg[17].  Syncing both made the CPSR word differ
	// after "mrs r0, spsr".
	if (!in->sub)
		CPUUpdateCPSR();
	reg[in->d].I = in->sub ? reg[17].I : reg[16].I;
	*nextPc		 = pc + 4;
	// Neither handler sets clockTicks, so the loop default applies - charged at
	// this instruction's address, like every other default.
	return 1 + codeTicksAccessSeq32(pc);
}

static int doMsr(const JitInsn *in, u32 pc, u32 *nextPc)
{
	const u32 value = (in->flags & JF_IMM)
						  ? ror32(in->imm & 0xFF, (in->n & 0x0F) * 2)
						  : reg[in->m & 0x0F].I;

	if (in->sub)
	{
		// SPSR form (arm160 / arm360).  Completely different from the CPSR
		// form, which is why this used to disagree on ROM data:
		//   * it writes reg[17], not reg[16] - reg[16].I is left stale;
		//   * no CPUUpdateCPSR() at the start and no CPUUpdateFlags() at the
		//     end, so the four flag variables are not resynced from CPSR;
		//   * no CPUSwitchMode() and no "|= 0x10" - the mode cannot change;
		//   * the whole body is skipped unless the CPU is in a privileged
		//     mode, and the emulator's test is `armMode > 0x10 && < 0x1F`,
		//     i.e. it excludes System (0x1F) as well as User (0x10).  That
		//     is not what the ARM ARM says; it is what arm160/arm360 do.
		if (armMode > 0x10 && armMode < 0x1F)
		{
			if (in->d & 1) reg[17].I = (reg[17].I & 0xFFFFFF00) | (value & 0x000000FF);
			if (in->d & 2) reg[17].I = (reg[17].I & 0xFFFF00FF) | (value & 0x0000FF00);
			if (in->d & 4) reg[17].I = (reg[17].I & 0xFF00FFFF) | (value & 0x00FF0000);
			if (in->d & 8) reg[17].I = (reg[17].I & 0x00FFFFFF) | (value & 0xFF000000);
		}
		*nextPc = pc + 4;
		return 1 + codeTicksAccessSeq32(armNextPC);
	}

	// CPSR form (arm120 / arm320).
	CPUUpdateCPSR();
	u32 newValue = reg[16].I;

	if (armMode > 0x10)
	{
		if (in->d & 1) newValue = (newValue & 0xFFFFFF00) | (value & 0x000000FF);
		if (in->d & 2) newValue = (newValue & 0xFFFF00FF) | (value & 0x0000FF00);
		if (in->d & 4) newValue = (newValue & 0xFF00FFFF) | (value & 0x00FF0000);
	}
	if (in->d & 8)
		newValue = (newValue & 0x00FFFFFF) | (value & 0xFF000000);

	newValue |= 0x10;
	CPUSwitchMode(newValue & 0x1F, false);
	reg[16].I = newValue;
	CPUUpdateFlags();

	if (!armState)
	{
		THUMB_PREFETCH;
		reg[15].I = armNextPC + 2;
		*nextPc	  = armNextPC;
	}
	else
	{
		*nextPc = pc + 4;
	}

	return 1 + codeTicksAccessSeq32(armNextPC);
}

static int doSwi(const JitInsn *in, bool thumb, u32 *nextPc)
{
	int ticks;
	if (thumb)
	{
		// thumbDF charges three accesses to address 0 (a quirk, not region 0 of
		// the instruction) before entering the exception.
		ticks = 3 + codeTicksBranch16(0);
		busPrefetchCount = 0;
	}
	else
	{
		ticks  = codeTicksAccessSeq32(armNextPC) + 1;
		const int swiNseq = codeTicksAccess32(armNextPC);
		const int swiSeq  = codeTicksAccessSeq32(armNextPC);
		ticks += 2 + swiNseq + swiSeq;
		busPrefetchCount = 0;
	}

	CPUSoftwareInterrupt((int)(in->imm & 0x00FFFFFF));
	*nextPc = armNextPC;
	return ticks;
}

} // namespace

int jitInterpInsn(const JitInsn *in, bool thumb, u32 pc, u32 *nextPc)
{
	// What armExecute()/thumbExecute() have done by the time a handler runs:
	// armNextPC is the address after this instruction, reg[15] is that plus 4/2.
	// An instruction that reads PC therefore sees pc+8 (ARM) or pc+4 (Thumb),
	// which is why BL stores "reg[15] - 4" and gets pc+4.
	const u32 fetch = pc + (thumb ? 2 : 4);

	// The three prefetch bookkeeping lines the loop runs before dispatching.
	// The first one is a hack for code at 0x08020000 and tests the *current*
	// instruction address (the loop has not touched armNextPC yet).
	if (!thumb && ((pc & 0x0803FFFF) == 0x08020000))
		busPrefetchCount = 0x100;

	busPrefetch = false;
	if (thumb)
	{
		if (busPrefetchCount & 0xFFFFFF00)
			busPrefetchCount = 0x100 | (busPrefetchCount & 0xFF);
	}
	else if (busPrefetchCount & 0xFFFFFE00)
		busPrefetchCount = 0x100 | (busPrefetchCount & 0xFF);

	armNextPC = fetch;
	reg[15].I = pc + (thumb ? 4 : 8);
	*nextPc	  = fetch;

	if (!condTrue(in->cond))
	{
		// The handler is not called at all; the loop's default applies, and that
		// one is computed from oldArmNextPC - the address of this instruction,
		// not the one after it.
		return 1 + (thumb ? codeTicksAccessSeq16(pc) : codeTicksAccessSeq32(pc));
	}

	switch (in->op)
	{
	case JOP_ALU:	return doAlu(in, thumb, pc, nextPc);
	case JOP_MUL:	return doMul(in, thumb, pc, nextPc);
	case JOP_LOAD:
	case JOP_STORE: return doLoadStore(in, thumb, pc, nextPc);
	case JOP_LDM:
	case JOP_STM:
	case JOP_PUSH:
	case JOP_POP:	return doBlockTransfer(in, thumb, pc, nextPc);
	case JOP_SWP:	return doSwp(in, pc, nextPc);
	case JOP_BRANCH: return doBranch(in, thumb, pc, nextPc);
	case JOP_BX:	return doBx(in, pc, nextPc);
	case JOP_MRS:	return doMrs(in, pc, nextPc);
	case JOP_MSR:	return doMsr(in, pc, nextPc);
	case JOP_SWI:	return doSwi(in, thumb, nextPc);

	case JOP_UNDEF:
	{
		// Neither armUnknownInsn() nor thumbUnknownInsn() sets clockTicks, so
		// the loop's default applies: one cycle plus a sequential code fetch at
		// the address of the instruction that faulted.  That has to be taken
		// before CPUUndefinedException() redirects armNextPC to the vector.
		const int ticks = 1 + (thumb ? codeTicksAccessSeq16(pc) : codeTicksAccessSeq32(pc));
		CPUUndefinedException();
		*nextPc = armNextPC;
		return ticks;
	}

	case JOP_END:
	default:
		// Not an executable instruction: the caller must treat it as a bug.
		return -1;
	}
}

#endif // USE_GBA_CORE_V7
