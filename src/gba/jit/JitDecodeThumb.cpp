// Thumb (ARMv4T) opcode -> JitInsn.
//
// Transcription of GBA-thumb.cpp's handlers, the same way JitDecodeArm.cpp is a
// transcription of GBA-arm.cpp.  The dispatch follows the handler table: the
// 1024-entry thumbInsnTable is indexed by bits 15-6, so the format boundaries
// below land where that table's rows land.
//
// Two things about the Thumb handlers are worth keeping in mind while reading
// this file:
//
//   * most of them do not touch clockTicks at all, so thumbExecute() charges the
//     default "1 + codeTicksAccessSeq16(armNextPC)".  The exceptions (the
//     format-4 register shifts, MUL, and the loads) set it explicitly, and the
//     numbers differ by more than rounding - MUL even clobbers
//     busPrefetchCount with its own formula.
//   * the flags each instruction writes vary.  Format 1 writes N,Z (and C only
//     when it actually shifts), the ALU ops of format 4 write N,Z only where
//     ARM would also write C.  That is what JitInsn::flagMask is for.
//
// Deviations from the ARM ARM that are reproduced on purpose (they are bugs in
// the interpreter, but every recorded movie was made against them, so a
// recompiler that "fixes" them would desynchronise playback):
//
//   * format 7/9 (register-offset loads/stores) never scale the offset
//     register - see decodeRegOffset().  The ARM ARM says the offset is
//     shifted by the access size; the interpreter adds it raw.
//   * ROR by register writes C even when the count is zero
//     (SHIFT_ROR_REG_T), while the other shift-by-register forms treat a count
//     of 0 as "no shift" instead of "shift by 32".
//   * MUL and the format-4 register shifts *assign* clockTicks instead of
//     adding to it, and MUL overwrites busPrefetchCount outright.  Those are
//     timing bugs, but cycle-exactness is what makes a movie replay.

#include "JitIr.h"

#include <cstring>

#ifdef USE_GBA_CORE_V7
// Defined in src/gba/V7/GBA.cpp.  Declared here instead of including
// V7/GBACpu.h, which drags in the emulator's macros (UPDATE_REG, ...) and
// would defeat the JIT files' standalone build.
extern const int32 thumbCycles[256];
#endif

namespace {

inline u16 read16(const JitSource &src, u32 addr)
{
	u16 v;
	memcpy(&v, src.base + (addr & src.mask), sizeof(v));
	return v;
}

// Thumb's conditional branches and its single "always" branch live in the same
// encoding space; cond 14 is the unconditional form and 15 is SWI.
const u8 THUMB_AL = 0xE;

inline void setUndef(JitInsn *in)
{
	in->op = JOP_UNDEF;
}

// LDR/STR with a register offset scaled by 1, 2 or 4 bytes.
void decodeRegOffset(u32 opcode, JitInsn *in)
{
	const u32 kind = (opcode >> 9) & 7;
	const u8  rd   = (u8)(opcode & 7);
	const u8  rb   = (u8)((opcode >> 3) & 7);

	in->n	  = rb;
	in->d	  = rd;
	in->m	  = (u8)((opcode >> 6) & 7);
	in->shift = SHIFT_LSL_IMM;
	in->flags = JF_PRE | JF_UP;

	// The offset register is added as-is.  This is a deviation from the ARM
	// ARM, which says the offset is scaled by the access size (word << 2,
	// halfword << 1).  thumb50/52/58/5A/5C/5E all compute
	//     reg[rb].I + reg[ro].I
	// with no shift at all, i.e. the emulator implements format 7/9 as if the
	// scaling bits did not exist.  It is a bug in the interpreter, but it is
	// the behaviour every recorded movie was made against, so the recompiler
	// has to reproduce it: `imm` (the shift amount evalOffset() would apply)
	// stays zero for every kind.
	in->imm = 0;

	switch (kind)
	{
	case 0: in->op = JOP_STORE; in->sub = MEM_WORD; break;
	case 1: in->op = JOP_STORE; in->sub = MEM_HALF; break;
	case 2: in->op = JOP_STORE; in->sub = MEM_BYTE; break;
	case 3: in->op = JOP_LOAD;  in->sub = MEM_BYTE;
			in->flags |= JF_SIGNED; break;
	case 4: in->op = JOP_LOAD;  in->sub = MEM_WORD; break;
	case 5: in->op = JOP_LOAD;  in->sub = MEM_HALF; break;
	case 6: in->op = JOP_LOAD;  in->sub = MEM_BYTE; break;
	default: in->op = JOP_LOAD; in->sub = MEM_HALF;
			 in->flags |= JF_SIGNED; break;
	}

	// thumb5A (LDRH with a register offset) charges dataTicksAccess32 while the
	// immediate form charges dataTicksAccess16.  Same instruction, different
	// wait table - reproduce it, the verifier compares cycles.  V7's thumb.h
	// has no such quirk: 0x5a uses CPUUpdateTicksAccess16 like every other
	// halfword load, so the flag is V8-only.
#ifndef USE_GBA_CORE_V7
	if (in->op == JOP_LOAD && kind == 5)
		in->flags |= JF_DATA32;
#endif
}

void decodeLoadStoreImm(u32 opcode, JitInsn *in, u32 op, u32 size, u32 scale)
{
	in->op	  = op;
	in->sub	  = (u8)size;
	in->d	  = (u8)(opcode & 7);
	in->n	  = (u8)((opcode >> 3) & 7);
	in->imm	  = ((opcode >> 6) & 0x1F) << scale;
	in->flags = JF_IMM | JF_PRE | JF_UP;
}

} // namespace

bool jitDecodeThumb(const JitSource &src, u32 pc, JitBlock *block)
{
	block->guestPc = pc;
	block->thumb   = 1;
	block->count   = 0;
	block->ended   = 0;

	u32 addr = pc;
	for (int i = 0; i < JIT_MAX_INSNS; i++)
	{
		const u32 opcode = read16(src, addr);
		JitInsn	  ins	 = {};
		JitInsn	  *in	 = &ins;
		bool	  ends	 = false;

		in->cond = THUMB_AL;

		switch (opcode >> 12)
		{
		case 0x0:
		case 0x1:
			// 000xx: LSL/LSR/ASR Rd, Rs, #imm5.  bits 12-11 pick the op and 11
			// is also the "format 2" escape, so both live here.
			if (((opcode >> 11) & 3) == 3)
			{
				// 00011: ADD/SUB Rd, Rs, Rn/#imm3
				in->op		 = JOP_ALU;
				in->sub		 = (opcode & 0x0200) ? ALU_SUB : ALU_ADD;
				in->d		 = (u8)(opcode & 7);
				in->n		 = (u8)((opcode >> 3) & 7);
				in->flagMask = JITF_ALL;
				if (opcode & 0x0400)
				{
					in->imm = (opcode >> 6) & 7;
					in->flags |= JF_IMM;
				}
				else
				{
					in->m = (u8)((opcode >> 6) & 7);
				}
			}
			else
			{
				const u32 amount = (opcode >> 6) & 0x1F;
				const u32 op	 = (opcode >> 11) & 3;

				in->op	  = JOP_ALU;
				in->sub	  = ALU_MOV;
				in->d	  = (u8)(opcode & 7);
				in->m	  = (u8)((opcode >> 3) & 7);
				in->imm	  = amount;
				if (op == 0)
				{
					// "lsl Rd, Rs, #0" is a plain move and leaves the carry alone
					// (IMM5_LSL_0); any other amount writes it (LSL_RD_RM_I5).
					in->shift	 = SHIFT_LSL_IMM;
					in->flagMask = amount ? JITF_NZC : JITF_NZ;
				}
				else
				{
					// LSR #0 means a shift of 32 and ASR #0 a full sign fill; both
					// write the carry.  evalOperand2() already implements those
					// cases, they match VALUE_LSR_IMM / VALUE_ASR_IMM.
					in->shift	 = (op == 1) ? SHIFT_LSR_IMM : SHIFT_ASR_IMM;
					in->flagMask = JITF_NZC;
				}
			}
			break;

		case 0x2:
		case 0x3:
		{
			// MOV/CMP/ADD/SUB Rd, #imm8
			const u8 rd = (u8)((opcode >> 8) & 7);
			in->imm		= opcode & 0xFF;
			in->flags  |= JF_IMM;
			switch ((opcode >> 11) & 3)
			{
			case 0:		// MOV_RN_O8: N is forced false, the result cannot be negative
				in->op		 = JOP_ALU;
				in->sub		 = ALU_MOV;
				in->d		 = rd;
				in->flagMask = JITF_NZ;
				break;
			case 1:		// CMP_RN_O8
				in->op		 = JOP_ALU;
				in->sub		 = ALU_CMP;
				in->n		 = rd;
				in->flagMask = JITF_ALL;
				break;
			case 2:		// ADD_RN_O8
				in->op		 = JOP_ALU;
				in->sub		 = ALU_ADD;
				in->d = in->n = rd;
				in->flagMask = JITF_ALL;
				break;
			default:	// SUB_RN_O8
				in->op		 = JOP_ALU;
				in->sub		 = ALU_SUB;
				in->d = in->n = rd;
				in->flagMask = JITF_ALL;
				break;
			}
			break;
		}

		case 0x4:
			if ((opcode & 0xF800) == 0x4800)
			{
				// LDR Rd, [PC, #imm8*4].  thumb48 reads through
				// CPUReadMemoryQuick and drops the prefetch buffer afterwards.
				in->op	  = JOP_LOAD;
				in->sub	  = MEM_WORD;
				in->d	  = (u8)((opcode >> 8) & 7);
				in->n	  = 15;
				in->imm	  = (opcode & 0xFF) << 2;
				// Both cores read this one with CPUReadMemoryQuick, so the Lua
				// READ hook is bypassed.  V8's IR interpreter honours the flag
				// too (doLoadStore); it used to be V7-only, which made a
				// scripted run see one extra callback per PC-relative load.
				in->flags = JF_IMM | JF_PRE | JF_UP | JF_PC_ALIGN4 | JF_CLEAR_PREFETCH | JF_QUICK;
				break;
			}
			if ((opcode & 0xFC00) == 0x4400)
			{
				// Format 5: hi-register operations and BX.
				const u32  op	  = (opcode >> 8) & 3;		// bits 9-8
				const bool h1	  = (opcode & 0x0080) != 0;	// Rd is a high register
				const bool h2	  = (opcode & 0x0040) != 0;	// Rs is a high register
				const u8   rd	  = (u8)((opcode & 7) + (h1 ? 8 : 0));
				const u8   rs	  = (u8)(((opcode >> 3) & 7) + (h2 ? 8 : 0));

				switch (op)
				{
				case 0:		// ADD: no flags, and it may write PC
					if (!h1 && !h2)		// thumbUI - that encoding is format 2's ADD
					{
						setUndef(in);
						ends = true;
					}
					else
					{
						in->op = JOP_ALU;
						in->sub = ALU_ADD;
						in->d = in->n = rd;
						in->m = rs;
						ends  = (rd == 15);
					}
					break;
				case 1:		// CMP: all four flags, no PC write
					if (!h1 && !h2)
					{
						setUndef(in);
						ends = true;
					}
					else
					{
						in->op		 = JOP_ALU;
						in->sub		 = ALU_CMP;
						in->n		 = rd;
						in->m		 = rs;
						in->flagMask = JITF_ALL;
					}
					break;
				case 2:		// MOV: no flags at all (thumb46_* only copies)
					in->op		 = JOP_ALU;
					in->sub		 = ALU_MOV;
					in->d		 = rd;
					in->m		 = rs;
					ends		 = (rd == 15);
					break;
			default:	// BX
				// The table distinguishes by bit 7 only: thumb47 serves
				// 0x4700-0x47FF with bit 7 clear, and 0x4780/0x47C0 (bit 7
				// set, the ARMv5 BLX shape) are thumbUI.  Bits 6-3 are Rm and
				// are free - testing 0xC0 as well made `bx lr` (0x4770) look
				// like an undefined instruction.
				if (opcode & 0x0080)
				{
					setUndef(in);
					ends = true;
				}
					else
					{
						in->op = JOP_BX;
						in->m  = (u8)((opcode >> 3) & 0x0F);
						ends   = true;
					}
					break;
				}
				break;
			}
			// 0x4000-0x43FF: format 4, ALU operations on the low registers.
			{
				const u8 rd = (u8)(opcode & 7);
				const u8 rs = (u8)((opcode >> 3) & 7);

				in->op = JOP_ALU;
				switch ((opcode >> 6) & 0x0F)
				{
				case 0x0: in->sub = ALU_AND; in->d = in->n = rd; in->m = rs;
						  in->flagMask = JITF_NZ; break;
				case 0x1: in->sub = ALU_EOR; in->d = in->n = rd; in->m = rs;
						  in->flagMask = JITF_NZ; break;
				case 0x2:	// LSL Rd, Rs - 2 cycles plus a non-seq 16-bit fetch
					in->sub = ALU_MOV; in->d = rd; in->m = rd; in->imm = rs;
					in->shift = SHIFT_LSL_REG; in->flagMask = JITF_NZC;
					break;
				case 0x3:
					in->sub = ALU_MOV; in->d = rd; in->m = rd; in->imm = rs;
					in->shift = SHIFT_LSR_REG; in->flagMask = JITF_NZC;
					break;
				case 0x4:
					in->sub = ALU_MOV; in->d = rd; in->m = rd; in->imm = rs;
					in->shift = SHIFT_ASR_REG; in->flagMask = JITF_NZC;
					break;
				case 0x5: in->sub = ALU_ADC; in->d = in->n = rd; in->m = rs;
						  in->flagMask = JITF_ALL; break;
				case 0x6: in->sub = ALU_SBC; in->d = in->n = rd; in->m = rs;
						  in->flagMask = JITF_ALL; break;
				case 0x7:	// ROR Rd, Rs: its count of 0 (or 32) still writes C
					in->sub = ALU_MOV; in->d = rd; in->m = rd; in->imm = rs;
					in->shift = SHIFT_ROR_REG_T; in->flagMask = JITF_NZC;
					break;
				case 0x8: in->sub = ALU_TST; in->n = rd; in->m = rs;
						  in->flagMask = JITF_NZ; break;
				case 0x9:	// NEG Rd, Rs is RSB with a zero operand
					in->sub = ALU_RSB; in->d = rd; in->n = rs; in->imm = 0;
					in->flags |= JF_IMM; in->flagMask = JITF_ALL; break;
				case 0xA: in->sub = ALU_CMP; in->n = rd; in->m = rs;
						  in->flagMask = JITF_ALL; break;
				case 0xB: in->sub = ALU_CMN; in->n = rd; in->m = rs;
						  in->flagMask = JITF_ALL; break;
				case 0xC: in->sub = ALU_ORR; in->d = in->n = rd; in->m = rs;
						  in->flagMask = JITF_NZ; break;
				case 0xD:	// MUL Rd, Rs: Rd = Rs * Rd, timing depends on old Rd
					in->op = JOP_MUL; in->sub = MUL_MUL;
					in->d = rd; in->m = rs; in->flagMask = JITF_NZ; break;
				case 0xE: in->sub = ALU_BIC; in->d = in->n = rd; in->m = rs;
						  in->flagMask = JITF_NZ; break;
				default:  in->sub = ALU_MVN; in->d = rd; in->m = rs;
						  in->flagMask = JITF_NZ; break;
				}
			}
			break;

		case 0x5:
			// Format 7/8: register-offset loads and stores.
			decodeRegOffset(opcode, in);
			break;

		case 0x6:
			// Word load/store with an immediate offset.
			decodeLoadStoreImm(opcode, in, (opcode & 0x0800) ? JOP_LOAD : JOP_STORE,
							   MEM_WORD, 2);
			break;

		case 0x7:
			// Byte load/store with an immediate offset.
			decodeLoadStoreImm(opcode, in, (opcode & 0x0800) ? JOP_LOAD : JOP_STORE,
							   MEM_BYTE, 0);
			break;

		case 0x8:
			// Halfword load/store with an immediate offset.
			decodeLoadStoreImm(opcode, in, (opcode & 0x0800) ? JOP_LOAD : JOP_STORE,
							   MEM_HALF, 1);
			break;

		case 0x9:
			// [SP, #imm8*4]
			in->op	 = (opcode & 0x0800) ? JOP_LOAD : JOP_STORE;
			in->sub	 = MEM_WORD;
			in->d	 = (u8)((opcode >> 8) & 7);
			in->n	 = 13;
			in->imm	 = (opcode & 0xFF) << 2;
			in->flags = JF_IMM | JF_PRE | JF_UP;
			// thumb98 loads with CPUReadMemoryQuick (no Lua READ hook) on both
			// cores; stores still go through the hooking CPUWriteMemory wrapper.
			if (in->op == JOP_LOAD)
				in->flags |= JF_QUICK;
			break;

		case 0xA:
			// ADD Rd, PC/SP, #imm8*4
			in->op	   = JOP_ALU;
			in->sub	   = ALU_ADD;
			in->d	   = (u8)((opcode >> 8) & 7);
			in->n	   = (opcode & 0x0800) ? 13 : 15;
			in->imm	   = (opcode & 0xFF) << 2;
			in->flags |= JF_IMM;
			if (in->n == 15)
				in->flags |= JF_PC_ALIGN4;		// (reg[15] & ~3) + offset
			break;

		case 0xB:
			if ((opcode & 0xFF00) == 0xB000)
			{
				// ADD/SUB SP, #imm7*4
				u32 offset = (opcode & 0x7F) << 2;
				if (opcode & 0x80)
					offset = 0u - offset;
				in->op	   = JOP_ALU;
				in->sub	   = ALU_ADD;
				in->d = in->n = 13;
				in->imm	   = offset;
				in->flags |= JF_IMM;
			}
			else if (opcode >= 0xB400 && opcode <= 0xB5FF)
			{
				// PUSH {rlist}, optionally with LR
				in->op	= JOP_PUSH;
				in->n	= 13;
				in->imm = (opcode & 0xFF) | ((opcode & 0x0100) ? 0x4000 : 0);
			}
			else if (opcode >= 0xBC00 && opcode <= 0xBDFF)
			{
				// POP {rlist}, optionally with PC
				in->op	= JOP_POP;
				in->n	= 13;
				in->imm = (opcode & 0xFF) | ((opcode & 0x0100) ? 0x8000 : 0);
				ends	= (opcode & 0x0100) != 0;
			}
			else if (opcode >= 0xBE00 && opcode <= 0xBEFF)
			{
				// BKPT: thumbBreakpoint() winds the PC back, signals the
				// debugger and sets clockTicks = -1, which stops the machine.
				// Refuse the block instead of modelling that.
				return false;
			}
			else
			{
				setUndef(in);
				ends = true;
			}
			break;

		case 0xC:
			// STMIA/LDMIA Rn!, {rlist} - only the low registers can be moved.
			in->op	 = (opcode & 0x0800) ? JOP_LDM : JOP_STM;
			in->sub	 = BLOCK_IA;
			in->n	 = (u8)((opcode >> 8) & 7);
			in->imm	 = opcode & 0xFF;
			in->flags = JF_WRITEBACK;
			break;

		case 0xD:
			if ((opcode & 0xFF00) == 0xDF00)
			{
				in->op	= JOP_SWI;
				in->imm = opcode & 0xFF;
				ends	= true;
			}
			else if (((opcode >> 8) & 0x0F) < 0x0E)
			{
				// Conditional branch.  cond 14 is undefined here and 15 is SWI,
				// which the 0xDFxx test above already took.
				in->op	= JOP_BRANCH;
				in->cond = (u8)((opcode >> 8) & 0x0F);
				in->imm	= (u32)(s32)(s8)(opcode & 0xFF) << 1;
				ends	= true;
			}
			else
			{
				setUndef(in);
				ends = true;
			}
			break;

		case 0xE:
			if ((opcode & 0xF800) == 0xE000)
			{
				// B: 11-bit signed offset, in halfwords
				u32 offset = (opcode & 0x7FF) << 1;
				if (opcode & 0x0400)
					offset |= 0xFFFFF000;
				in->op	= JOP_BRANCH;
				in->imm	= offset;
				ends	= true;
			}
			else
			{
				setUndef(in);
				ends = true;
			}
			break;

		default:	// 0xF
			if ((opcode & 0xF800) == 0xF000)
			{
				// BL, first half: LR = PC + (offset << 12).  Modelled as an ALU
				// op, which also gives the right 1 + seq16 cycle count.
				u32 offset = (opcode & 0x7FF) << 12;
				if (opcode & 0x0400)
					offset |= 0xFF800000;		// sign-extend the 11-bit field
				in->op	   = JOP_ALU;
				in->sub	   = ALU_ADD;
				in->d	   = 14;
				in->n	   = 15;
				in->imm	   = offset;
				in->flags |= JF_IMM;
			}
			else
			{
				// BL, second half: PC = LR + (offset << 1), LR = return address.
				in->op	 = JOP_BRANCH;
				in->imm	 = (opcode & 0x7FF) << 1;
				in->flags |= JF_LINK;
				ends	 = true;
			}
			break;
		}

#ifdef USE_GBA_CORE_V7
		// V7's base cost for a Thumb instruction.  It is a pure table lookup on
		// the opcode, so it belongs in the IR; memoryWaitFetch is added at run
		// time because WAITCNT can change it.
		ins.fetchCycles = (u8)thumbCycles[opcode >> 8];
#endif
		block->insn[block->count++] = ins;
		addr += 2;

		if (ends)
		{
			block->ended = 1;
			break;
		}
		if ((addr & (JIT_PAGE_SIZE - 1)) == 0)
			break;
	}

	return block->count > 0;
}
