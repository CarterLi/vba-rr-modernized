// ARM (ARMv4T) opcode -> JitInsn.
//
// This is a transcription of GBA-arm.cpp's handlers, not of the ARM ARM.  Where
// the emulator deviates from the architecture (the shift-by-32 cases, ROR #0
// being RRX, the way a write to PC suppresses the flag update) the deviation is
// what the IR records, because the differential tests compare against the
// emulator.
//
// The dispatch mirrors the shape of armInsnTable[]: the 4096-entry table is
// indexed by bits 27-20 and 7-4, so the same two fields drive the tree here.
//
// Deviations from the ARM ARM reproduced on purpose (this is what the
// differential test compares against, and what every recorded movie was made
// with):
//
//   * single data transfer with bit 4 set: the ARM ARM's shift-by-register
//     form (`ldr r0, [r1, r2, lsl r3]`) is not implemented - the emulator's
//     table sends those words to armUnknownInsn.  See decodeSingleTransfer().
//
// Places where the interpreter looks odd but is *right* (do not "fix" them):
//
//   * data processing: TST/TEQ/CMP/CMN with S=0 are reserved encodings, so the
//     undefined-instruction exception is correct - see decodeDataProcessing().
//   * TST/TEQ/CMP/CMN take no destination register.  The Rd field of those
//     encodings is ignored by the emulator, even when a data word has 15 in it,
//     so it must not be treated as a write to PC - see doAlu().
//   * condition 0xF executes nothing at all (armExecute's default branch),
//     which matches the ARM ARM's "never".
//   * the shift-by-register operand forms keep the emulator's shift counts:
//     LSR/ASR with a count of 0 mean "no shift" rather than "shift by 32", and
//     ROR #0 is RRX.  These are timing/flag quirks that the differential test
//     pins down; treat the emulator as the specification.

#include "JitIr.h"

#include <cstring>

namespace {

inline u32 read32(const JitSource &src, u32 addr)
{
	// memcpy rather than READ32LE: the macro casts a const u8* to u32* and
	// dereferences it, which is a const-cast aliasing violation the optimiser is
	// allowed to miscompile.  The compiler turns this back into one load.
	u32 v;
	memcpy(&v, src.base + (addr & src.mask), sizeof(v));
	return v;
}

// Which flags a data-processing instruction writes.  The emulator separates the
// two families exactly like the ARM ARM does: logical ops write N,Z,C and leave
// V alone, arithmetic ones write all four.  A write to PC suppresses both.
inline u8 aluFlagMask(u8 sub)
{
	const bool arith = (sub >= ALU_SUB && sub <= ALU_RSC) ||
					   sub == ALU_CMP || sub == ALU_CMN;
	return arith ? JITF_ALL : JITF_NZC;
}

inline void setAluOp(JitInsn *in, u32 opcode)
{
	// bits 24-21 map one to one onto JitAluOp.
	in->sub = (u8)((opcode >> 21) & 0x0F);
}

// Data processing.
bool decodeDataProcessing(u32 opcode, JitInsn *in)
{
	// TST/TEQ/CMP/CMN (1000..1011) exist only with S=1.  With S=0 the word
	// falls into the MRS/MSR corner of the opcode space, which the dispatcher
	// has already tried; what is left is the hole the emulator's table fills
	// with armUnknownInsn (index 0x30x) or with arm32x/34x/36x, whose "not
	// really MSR" branch is armUnknownInsn too.  Decoding it as an ALU op made
	// the differential test disagree on every one of them.  Note that ORR/MOV/
	// BIC/MVN (1100..1111) keep S=0 in ordinary code, so the range matters.
	const u32 aluOp = (opcode >> 21) & 0x0F;
	if (aluOp >= 8 && aluOp <= 11 && !(opcode & 0x00100000))
		return false;

	in->op = JOP_ALU;
	setAluOp(in, opcode);
	in->d = (u8)((opcode >> 12) & 0x0F);
	in->n = (u8)((opcode >> 16) & 0x0F);
	if (opcode & 0x02000000)		// I: 8-bit immediate rotated right
	{
		in->shift = SHIFT_IMM_ROT;
		in->imm	  = ((opcode >> 8) & 0x0F) << 8 | (opcode & 0xFF);
		in->flags |= JF_IMM;
	}
	// Bit 4 says the shift amount comes from Rs (bits 11-8) instead of the
	// immediate in bits 11-7; the shift type stays in bits 6-5.  Testing the
	// whole nibble 7-4 == 1 only caught LSL-by-register and sent LSR/ASR/ROR
	// by register down the immediate path.
	else if (opcode & 0x10)
	{
		in->m	  = (u8)(opcode & 0x0F);
		in->imm	  = (opcode >> 8) & 0x0F;	// Rs
		switch ((opcode >> 5) & 3)
		{
		case 0: in->shift = SHIFT_LSL_REG; break;
		case 1: in->shift = SHIFT_LSR_REG; break;
		case 2: in->shift = SHIFT_ASR_REG; break;
		default:
			// V7's ARM ROR writes the carry even when the raw count is 0, unlike
			// V8's SHIFT_ROR_REG (and unlike Thumb's SHIFT_ROR_REG_T).
#ifdef USE_GBA_CORE_V7
			in->shift = SHIFT_ROR_REG_V7;
#else
			in->shift = SHIFT_ROR_REG;
#endif
			break;
		}
	}
	else
	{
		in->m	  = (u8)(opcode & 0x0F);
		in->imm	  = (opcode >> 7) & 0x1F;
		switch ((opcode >> 5) & 3)
		{
		case 0: in->shift = SHIFT_LSL_IMM; break;
		case 1: in->shift = SHIFT_LSR_IMM; break;
		case 2: in->shift = SHIFT_ASR_IMM; break;
		default: in->shift = SHIFT_ROR_IMM; break;
		}
	}
	if (opcode & 0x00100000)		// S
		in->flagMask = aluFlagMask(in->sub);
	return true;
}

bool decodeMultiply(u32 opcode, JitInsn *in)
{
	// 0000 1 U A S: bit 23 selects the long forms, bit 22 is U (0 = unsigned),
	// bit 21 accumulates and bit 20 is the S bit.  UMULL is 0x08xxxxxx and
	// SMULL is 0x0Cxxxxxx, the same way arm089/arm0C9 split them in the
	// interpreter's hand-written table.
	const bool accum	  = (opcode & 0x00200000) != 0;
	const bool isLong	  = (opcode & 0x00800000) != 0;
	const bool isUnsigned = (opcode & 0x00400000) == 0;

	in->op = JOP_MUL;
	if (isLong)
		in->sub = accum ? (isUnsigned ? MUL_UMLAL : MUL_SMLAL)
						: (isUnsigned ? MUL_UMULL : MUL_SMULL);
	else
		in->sub = accum ? MUL_MLA : MUL_MUL;

	in->d	= (u8)((opcode >> 16) & 0x0F);	// Rd / RdHi
	in->n	= (u8)(opcode & 0x0F);			// Rm
	in->m	= (u8)((opcode >> 8) & 0x0F);	// Rs
	in->imm	= (opcode >> 12) & 0x0F;		// Rn (accumulate) / RdLo
	if (opcode & 0x00100000)				// S
		in->flagMask = JITF_NZ;				// SETCOND_MUL / SETCOND_MULL
	return true;
}

bool decodeHalfword(u32 opcode, JitInsn *in)
{
	const bool pre	= (opcode & 0x01000000) != 0;
	const bool up	= (opcode & 0x00800000) != 0;
	const bool immF	= (opcode & 0x00400000) != 0;
	const bool wb	= (opcode & 0x00200000) != 0;
	const bool load	= (opcode & 0x00100000) != 0;
	const bool sign	= (opcode & 0x00000040) != 0;
	const bool half	= (opcode & 0x00000020) != 0;

	if (!sign && !half)		// P U I W L Rn Rd 0000 1001 Rm is SWP, handled by the caller
		return false;

	// Holes in the emulator's table.  Both are encodings the ARM ARM calls
	// unpredictable, and armInsnTable[] answers them with arm_UI; the sweep in
	// JitVerify.cpp (sweepDecodeSpace) is what pins these two down:
	//   * S=1 with L=0 - a "signed store" - is undefined in every row;
	//   * H=1,S=0 (LDRH/STRH) with P=0 is undefined unless U and I are both 1.
	//     That is not a rule about the architecture, it is what the hand-written
	//     V8 table happens to contain (rows 0x0E/0x0F are filled in, 0x02/0x03,
	//     0x06/0x07 and 0x0A/0x0B are not); reproduce it, the differential
	//     tests compare against the table.
	// V7's arm-new.h implements all 16 P/U/I/W combinations for LDRH/STRH
	// (including the six keys V8 leaves out: 0x02B/0x03B/0x06B/0x07B/0x0AB/
	// 0x0BB), so the second hole does not exist there.
	if (sign && !load)
		return false;
#ifndef USE_GBA_CORE_V7
	if (half && !sign && !pre && wb && !(up && immF))
		return false;
#endif

	in->op = load ? JOP_LOAD : JOP_STORE;
	in->sub = half ? MEM_HALF : MEM_BYTE;
	in->d	= (u8)((opcode >> 12) & 0x0F);
	in->n	= (u8)((opcode >> 16) & 0x0F);
	if (sign)
		in->flags |= JF_SIGNED;
	if (pre)
		in->flags |= JF_PRE;
	if (up)
		in->flags |= JF_UP;
	// Same rule as the word transfers: arm0x0B/0x2B and friends are
	// STR_POSTINC/STR_POSTDEC and always write the base back.
	if (wb || !pre)
		in->flags |= JF_WRITEBACK;
	if (immF)
	{
		in->imm = ((opcode >> 4) & 0xF0) | (opcode & 0x0F);
		in->flags |= JF_IMM;
	}
	else
	{
		in->m = (u8)(opcode & 0x0F);
	}
	return true;
}

bool decodeSingleTransfer(u32 opcode, JitInsn *in)
{
	const bool immF	= (opcode & 0x02000000) == 0;
	const bool pre	= (opcode & 0x01000000) != 0;
	const bool up	= (opcode & 0x00800000) != 0;
	const bool byte	= (opcode & 0x00400000) != 0;
	const bool wb	= (opcode & 0x00200000) != 0;
	const bool load	= (opcode & 0x00100000) != 0;

	// Bit 4 has to be clear for the register-offset form.  The ARM ARM defines
	// bit4=1 as the shift-by-register shape (`ldr r0, [r1, r2, lsl r3]`), but
	// the emulator's table puts arm_UI at every odd index of the transfer rows
	// (see the "// 7D0" row: arm7D0, arm_UI, arm7D2, arm_UI, ...), so those
	// words are undefined instructions, not transfers.  Returning false makes
	// the caller emit JOP_UNDEF, which ends the block and raises the same
	// exception the interpreter would.  Tested before any field is written so
	// that an undefined word leaves `in` untouched.
	if (!immF && (opcode & 0x10))
		return false;

	in->op	= load ? JOP_LOAD : JOP_STORE;
	in->sub = byte ? MEM_BYTE : MEM_WORD;
	in->d	= (u8)((opcode >> 12) & 0x0F);
	in->n	= (u8)((opcode >> 16) & 0x0F);
	if (pre)
		in->flags |= JF_PRE;
	if (up)
		in->flags |= JF_UP;
	// Post-indexed transfers always write the base back.  Bit 21 only separates
	// the writeback form from the "unprivileged" LDRT/STRT on real ARM, and the
	// emulator ignores that: arm6x0/arm6x1 are all LDR_POSTINC/LDR_POSTDEC.
	if (wb || !pre)
		in->flags |= JF_WRITEBACK;

	if (immF)
	{
		in->imm = opcode & 0x0FFF;
		in->flags |= JF_IMM;
	}
	else
	{
		in->m	  = (u8)(opcode & 0x0F);
		switch ((opcode >> 5) & 3)
		{
		case 0: in->shift = SHIFT_LSL_IMM; break;
		case 1: in->shift = SHIFT_LSR_IMM; break;
		case 2: in->shift = SHIFT_ASR_IMM; break;
		default: in->shift = SHIFT_ROR_IMM; break;
		}
		in->imm = (opcode >> 7) & 0x1F;
	}
	return true;
}

bool decodeBlockTransfer(u32 opcode, JitInsn *in)
{
	const bool pre	= (opcode & 0x01000000) != 0;
	const bool up	= (opcode & 0x00800000) != 0;
	const bool user	= (opcode & 0x00400000) != 0;
	const bool wb	= (opcode & 0x00200000) != 0;
	const bool load	= (opcode & 0x00100000) != 0;

	// The "^" forms touch the banked registers (STM_HIGH_2 / LDM_ALL_2B switch
	// mode mid-instruction).  They are rare, and the doc's rule for anything
	// that calls CPUSwitchMode is to leave it to the interpreter, so the whole
	// block is refused instead of half-modelled.
	if (user)
		return false;

	in->op = load ? JOP_LDM : JOP_STM;
	if (pre)
		in->sub = up ? BLOCK_IB : BLOCK_DB;
	else
		in->sub = up ? BLOCK_IA : BLOCK_DA;
	in->n	= (u8)((opcode >> 16) & 0x0F);
	in->imm = opcode & 0xFFFF;
	if (wb)
		in->flags |= JF_WRITEBACK;
	return true;
}

bool decodeMrsMsr(u32 opcode, JitInsn *in)
{
	// MRS:  cond 00010 R 00 1111 Rd 0000 0000 0000
	// MSR:  cond 00010 R 10 mask 1111 0000 0000 Rm
	// MSRi: cond 00110 R 10 mask 1111 rot  imm8
	if ((opcode & 0x0FBF0FFF) == 0x010F0000)
	{
		in->op	= JOP_MRS;
		in->sub = (opcode & 0x00400000) ? 1 : 0;	// R: SPSR
		in->d	= (u8)((opcode >> 12) & 0x0F);
		return true;
	}
	if ((opcode & 0x0FB0FFF0) == 0x0120F000)
	{
		in->op	= JOP_MSR;
		in->sub = (opcode & 0x00400000) ? 1 : 0;
		in->d	= (u8)((opcode >> 16) & 0x0F);		// mask field
		in->m	= (u8)(opcode & 0x0F);
		return true;
	}
	if ((opcode & 0x0FB0F000) == 0x0320F000)
	{
		in->op	= JOP_MSR;
		in->sub = (opcode & 0x00400000) ? 1 : 0;
		in->d	= (u8)((opcode >> 16) & 0x0F);
		in->imm = (opcode & 0xFF);
		in->n	= (u8)((opcode >> 8) & 0x0F);		// rotate, in units of 2 bits
		in->flags |= JF_IMM;
		return true;
	}
	return false;
}

} // namespace

bool jitDecodeArm(const JitSource &src, u32 pc, JitBlock *block)
{
	block->guestPc = pc;
	block->thumb   = 0;
	block->count   = 0;
	block->ended   = 0;

	u32 addr = pc;
	for (int i = 0; i < JIT_MAX_INSNS; i++)
	{
		const u32 opcode = read32(src, addr);
		JitInsn	  in	 = {};
		in.cond			 = (u8)(opcode >> 28);
		bool ends		 = false;

		// Words the emulator's table routes to armBreakpoint(): BKPT, plus one
		// hole at index 0x7FF (row 0x7F, column F).  armBreakpoint() winds the
		// PC back, signals the debugger and sets clockTicks = -1, which makes
		// armExecute() return - so the machine stops.  No block can model that;
		// refuse the block and let the interpreter stop in its own way.
		if ((opcode & 0x0FF000F0) == 0x01200070 ||	// BKPT
			(opcode & 0x0FF000F0) == 0x07F000F0)	// the table hole
			return false;

		// Anything the decoder does not recognise becomes JOP_UNDEF, which ends
		// the block: the interpreter raises an exception there, so the next
		// address is not pc+4.
		const u32 bits27_26 = (opcode >> 26) & 3;

		if (bits27_26 == 0)
		{
			// MRS/MSR first: their masks are exact, and the 7-4 nibble test below
			// would otherwise swallow "MSR ..., r9" (bits 3-0 are the source
			// register there, not an opcode selector).
			if ((opcode & 0x0FFFFFF0) == 0x012FFF10)
			{
				// BX Rm.  Its bits 7-4 are 0001, the same shape as a register
				// shift, so it has to be recognised before data processing.
				in.op = JOP_BX;
				in.m  = (u8)(opcode & 0x0F);
				ends  = true;
			}
			else if ((opcode & 0x0FF000F0) == 0x01200070)
			{
				// Unreachable: the BKPT test at the top of the loop refuses the
				// block first.  Kept out of the way so the dispatch reads in
				// table order.
				return false;
			}
			else if (decodeMrsMsr(opcode, &in))
			{
				// Neither MRS nor MSR changes the flow of control.
				//
				// V7 runs *every* MSR that writes CPSR - a flags-only write
				// included - through CPUSwitchMode(newValue & 0x1f, false).
				// That two-argument overload means breakLoop = true, so
				// CPUUpdateFlags(true) can fire CPU_BREAK_LOOP_2, which
				// rewrites CPULoop's own cpuNextEvent/clockTicks through ext*.
				// It also rebanks r13/r14 and honours a T-bit change without
				// touching reg[15].  None of that can be modelled inside a
				// block, so refuse the block here and let the inline
				// interpreter run the instruction (3.2.1.5).
#ifdef USE_GBA_CORE_V7
				if (in.op == JOP_MSR && in.sub == 0)
					return false;
#endif
			}
			else if (!(opcode & 0x02000000) && (opcode & 0x90) == 0x90)
			{
				// bits 7-4 in {9,B,D,F}: multiply, swap, or halfword transfer.
				// The I bit (25) has to be clear: with I=1 those four bits are
				// part of the rotated immediate, and `rsbs r2, r0, #0x26`
				// (0x22302198) otherwise lands in the multiply test and comes
				// out as an undefined instruction.
				if ((opcode & 0x60) == 0)
				{
					// The mask stops at bits 11-8: the ARM ARM requires them to
					// be zero for SWP, but the dispatch table sends every
					// "bits 7-4 == 9" word in this row to arm109/arm149, whose
					// handlers ignore them.  A stricter test here made the
					// verifier disagree on ROM data that spells such a word.
					if ((opcode & 0x0FB000F0) == 0x01000090)
					{
						in.op  = JOP_SWP;
						in.sub = (opcode & 0x00400000) ? MEM_BYTE : MEM_WORD;
						in.d   = (u8)((opcode >> 12) & 0x0F);
						in.n   = (u8)((opcode >> 16) & 0x0F);
						in.m   = (u8)(opcode & 0x0F);
						ends   = (in.d == 15);
					}
					else if ((opcode & 0x0FC000F0) == 0x00000090 ||	// MUL / MLA
							 (opcode & 0x0F8000F0) == 0x00800090)	// the long forms
					{
						decodeMultiply(opcode, &in);
						ends = (in.d == 15) ||
							   ((in.sub >= MUL_UMULL) && in.imm == 15);
					}
					else
					{
						in.op = JOP_UNDEF;
						ends  = true;
					}
				}
				else if (!decodeHalfword(opcode, &in))
				{
					in.op = JOP_UNDEF;
					ends  = true;
				}
				else
				{
					ends = (in.d == 15);
				}
			}
			else if (!decodeDataProcessing(opcode, &in))
			{
				in.op = JOP_UNDEF;
				ends  = true;
			}
			else
				ends = (in.d == 15);
		}
		else if (bits27_26 == 1)
		{
			// false here means "the emulator calls this undefined", not
			// "unsupported encoding" - see decodeSingleTransfer().
			if (!decodeSingleTransfer(opcode, &in))
			{
				in.op = JOP_UNDEF;
				ends  = true;
			}
			else
				ends = (in.d == 15);
		}
		else if (bits27_26 == 2)
		{
			if (opcode & 0x02000000)		// B / BL
			{
				in.op  = JOP_BRANCH;
				in.imm = (opcode & 0x00FFFFFF) << 2;
				// Sign-extend the 26-bit word offset.  The value occupies bits
				// 0-25, so the fill starts at bit 26 - filling from bit 24
				// made every negative branch land 0x01000000 too high.
				if (opcode & 0x00800000)
					in.imm |= 0xFC000000;
				if (opcode & 0x01000000)
					in.flags |= JF_LINK;
				ends = true;
			}
			else
			{
				if (!decodeBlockTransfer(opcode, &in))
					return false;			// "^" form: whole block to the interpreter
				ends = (in.imm & 0x8000) != 0;	// PC in the list
			}
		}
		else
		{
			// SWI is bits 27-24 == 1111.  Testing bit 24 alone also swallowed
			// the coprocessor rows 0xDx/0xEx, which the table sends to
			// armUnknownInsn (the GBA has no coprocessor).
			if ((opcode & 0x0F000000) == 0x0F000000)
			{
				in.op  = JOP_SWI;
				in.imm = opcode & 0x00FFFFFF;
			}
			else
			{
				// Coprocessor space.  The GBA has none, and the interpreter
				// routes every one of these to armUnknownInsn().
				in.op = JOP_UNDEF;
			}
			ends = true;
		}

		block->insn[block->count++] = in;
		addr += 4;

		if (ends)
		{
			block->ended = 1;
			break;
		}
		if ((addr & (JIT_PAGE_SIZE - 1)) == 0)
			break;						// next instruction is on a fresh page
	}

	return block->count > 0;
}

bool jitDecodeOne(const JitSource &src, u32 pc, bool thumb, JitInsn *insn)
{
	// A static block: the verifier calls this once per instruction and the
	// stack frame would otherwise be 1 KB deep in a loop.
	static JitBlock block;
	if (!(thumb ? jitDecodeThumb(src, pc, &block) : jitDecodeArm(src, pc, &block)))
		return false;
	*insn = block.insn[0];
	return true;
}
