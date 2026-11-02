// Human-readable IR dump.  Debugging aid for the decoder and for the
// differential tests: when a test fails, the block dump is the fastest way to
// see which instruction the two sides disagree on.

#include <cstdio>
#include <cstring>

#include "JitIr.h"

static const char *const kAluName[16] = {
	"and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
	"tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"
};

static const char *const kMulName[6] = { "mul", "mla", "umull", "umlal", "smull", "smlal" };
// Indexed by JitShift, including SHIFT_IMM_ROT (never printed) and V7's extra
// ROR-by-register form at the end.  A short table here made the V7 ARM ROR
// dump read past the end of the array.
static const char *const kShiftName[12] = {
	"", "lsl", "lsr", "asr", "ror", "lsl", "lsr", "asr", "ror", "ror", "", "ror"
};

const char *jitOpName(u8 op)
{
	switch (op)
	{
	case JOP_END:	 return "end";
	case JOP_ALU:	 return "alu";
	case JOP_MUL:	 return "mul";
	case JOP_LOAD:	 return "ldr";
	case JOP_STORE:	 return "str";
	case JOP_LDM:	 return "ldm";
	case JOP_STM:	 return "stm";
	case JOP_PUSH:	 return "push";
	case JOP_POP:	 return "pop";
	case JOP_SWP:	 return "swp";
	case JOP_BRANCH: return "b";
	case JOP_BX:	 return "bx";
	case JOP_MRS:	 return "mrs";
	case JOP_MSR:	 return "msr";
	case JOP_SWI:	 return "swi";
	case JOP_UNDEF:	 return "undef";
	default:		 return "?";
	}
}

static void appendRegList(const JitInsn *in, char *out, int outSize, int *n)
{
	for (int r = 0; r < 16; r++)
		if (in->imm & (1u << r))
		{
			if (*n < outSize - 1)
			{
				const char *name = (r == 14) ? "lr" : (r == 15) ? "pc" : NULL;
				int w = name ? snprintf(out + *n, (size_t)(outSize - *n), "%s ", name)
							 : snprintf(out + *n, (size_t)(outSize - *n), "r%d ", r);
				if (w > 0)
					*n += w;
			}
		}
}

int jitDumpBlock(const JitBlock *block, char *out, int outSize)
{
	if (outSize <= 0)
		return 0;

	int n = 0;
#define APPEND(...) \
	do { \
		if (n < outSize - 1) \
		{ \
			int w = snprintf(out + n, (size_t)(outSize - n), __VA_ARGS__); \
			if (w > 0) \
				n += w; \
		} \
	} while (0)

	APPEND("%08X %s (%u insn%s)\n", block->guestPc, block->thumb ? "thumb" : "arm",
		   block->count, block->count == 1 ? "" : "s");

	for (int i = 0; i < block->count; i++)
	{
		const JitInsn *in = &block->insn[i];

		APPEND("  %08X  %-5s", block->guestPc + (block->thumb ? i * 2 : i * 4), jitOpName(in->op));

		switch (in->op)
		{
		case JOP_ALU:
			APPEND(" %s%s r%u, r%u", kAluName[in->sub & 15],
				   in->flagMask ? "s" : "", in->d, in->n);
			if (in->flags & JF_IMM)
			{
				if (in->shift == SHIFT_IMM_ROT)
				{
					const u32 rot	 = (in->imm >> 8) & 0x0F;
					const u32 imm8	 = in->imm & 0xFF;
					APPEND(", #%08X", rot ? ((imm8 >> (rot * 2)) | (imm8 << (32 - rot * 2)))
										  : imm8);
				}
				else
					APPEND(", #%u", in->imm);
			}
			else if (in->shift == SHIFT_NONE)
				APPEND(", r%u", in->m);
			else if (in->shift >= SHIFT_LSL_REG)
				APPEND(", r%u %s r%u", in->m, kShiftName[in->shift], in->imm);
			else
				APPEND(", r%u %s #%u", in->m, kShiftName[in->shift], in->imm);
			break;

		case JOP_MUL:
			APPEND(" %s%s r%u, r%u, r%u", kMulName[in->sub % 6],
				   in->flagMask ? "s" : "", in->d, in->n, in->m);
			break;

		case JOP_LOAD:
		case JOP_STORE:
			APPEND(" r%u, [r%u", in->d, in->n);
			if (in->flags & JF_IMM)
				APPEND(", #%s%u", (in->flags & JF_UP) ? "" : "-", in->imm);
			else if (in->shift != SHIFT_NONE)
				APPEND(", %sr%u %s #%u", (in->flags & JF_UP) ? "" : "-", in->m,
					   kShiftName[in->shift], in->imm);
			else
				APPEND(", %sr%u", (in->flags & JF_UP) ? "" : "-", in->m);
			APPEND("]%s", (in->flags & JF_WRITEBACK) ? "!" : "");
			if (!(in->flags & JF_PRE))
				APPEND(" (post)");
			if (in->sub == MEM_HALF) APPEND("h");
			if (in->sub == MEM_BYTE) APPEND("b");
			if (in->flags & JF_SIGNED) APPEND("s");
			break;

		case JOP_LDM:
		case JOP_STM:
		case JOP_PUSH:
		case JOP_POP:
			if (in->op == JOP_LDM || in->op == JOP_STM)
				APPEND(" r%u%s, {", in->n, (in->flags & JF_WRITEBACK) ? "!" : "");
			else
				APPEND(" {");
			appendRegList(in, out, outSize, &n);
			APPEND("}");
			if (in->op == JOP_LDM || in->op == JOP_STM)
				APPEND(" (%s)", in->sub == BLOCK_IA ? "ia" : in->sub == BLOCK_IB ? "ib"
							  : in->sub == BLOCK_DA ? "da" : "db");
			break;

		case JOP_BRANCH:
			if (block->thumb && (in->flags & JF_LINK))
				APPEND("bl lr+%d", (s32)in->imm);		// Thumb BL low: target is LR-based
			else
				APPEND("%s %08X", (in->flags & JF_LINK) ? "bl" : "b",
					   block->guestPc + (block->thumb ? i * 2 : i * 4) +
						   (block->thumb ? 4 : 8) + in->imm);
			break;

		case JOP_BX:	APPEND(" r%u", in->m); break;
		case JOP_MRS:	APPEND(" r%u, %s", in->d, in->sub ? "spsr" : "cpsr"); break;
		case JOP_MSR:	APPEND(" %s_%x, %s%u", in->sub ? "spsr" : "cpsr", in->d,
							   (in->flags & JF_IMM) ? "#" : "r", (in->flags & JF_IMM) ? in->imm : in->m);
						break;
		case JOP_SWI:	APPEND(" #%u", in->imm); break;
		case JOP_SWP:	APPEND(" r%u, [r%u], r%u", in->d, in->n, in->m); break;
		default:		break;
		}

		if (in->cond != 0xE && in->op != JOP_BRANCH)
			APPEND("  [cond %x]", in->cond);
		// V7 Thumb: the base cost taken from thumbCycles[opcode >> 8].  Zero
		// for every ARM instruction and for the whole V8 build, so it is only
		// printed when it carries information.
		if (in->fetchCycles)
			APPEND("  [fc=%u]", in->fetchCycles);
		APPEND("\n");
	}
#undef APPEND
	return n;
}
