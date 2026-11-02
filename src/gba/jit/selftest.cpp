// Decoder self-test.
//
// Not part of VBA_Latest.vcxproj: it is a host-side test that only needs the
// decoders and the dumper, so it builds without MFC, without zlib and without
// the emulator's globals:
//
//   clang++ -std=c++17 -I src/gba/jit src/gba/jit/selftest.cpp \
//           src/gba/jit/JitDecodeArm.cpp src/gba/jit/JitDecodeThumb.cpp \
//           src/gba/jit/JitDump.cpp -o selftest
//
// Every case below is a hand-assembled instruction whose encoding was checked
// against the ARM ARM, with the fields the decoder is expected to produce.  It
// catches misdecodes; the differential test against the emulator (phase 2) is
// what catches wrong semantics.

#include <cstdio>
#include <cstring>

#include "JitIr.h"

static int failures = 0;
static int checks	  = 0;

#ifdef USE_GBA_CORE_V7
// The V7 decoder reads the real table from src/gba/V7/GBA.cpp, which the host
// test does not link.  Define a synthetic table with the same 256-entry shape:
// the check in runCases() then proves the decoder indexed it by opcode >> 8,
// which is the only thing the IR field depends on.
#define TC4(a, b, c, d) a, b, c, d
#define TC16(a, b, c, d) TC4(a, b, c, d), TC4(a, b, c, d), TC4(a, b, c, d), TC4(a, b, c, d)
#define TC64(a, b, c, d) TC16(a, b, c, d), TC16(a, b, c, d), TC16(a, b, c, d), TC16(a, b, c, d)
extern const int32 thumbCycles[256] = {
	TC64(1, 2, 3, 4), TC64(1, 2, 3, 4), TC64(1, 2, 3, 4), TC64(1, 2, 3, 4)
};
#undef TC64
#undef TC16
#undef TC4
// thumb5A (ldrh with a register offset) charges a 32-bit data wait in V8 only:
// V7's handler uses CPUUpdateTicksAccess16 like every other halfword load.
#define JIT_LDRH_REG_DATA32 0
#else
#define JIT_LDRH_REG_DATA32 JF_DATA32
#endif

static_assert(sizeof(JitInsn) == 16, "JitInsn is part of the shared IR layout");

#define CHECK(cond) \
	do { \
		checks++; \
		if (!(cond)) \
		{ \
			failures++; \
			printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

#define CHECK_EQ(a, b) \
	do { \
		checks++; \
		long long _a = (long long)(a), _b = (long long)(b); \
		if (_a != _b) \
		{ \
			failures++; \
			printf("FAIL %s:%d  %s == %s  (%lld vs %lld)\n", \
				   __FILE__, __LINE__, #a, #b, _a, _b); \
		} \
	} while (0)

struct Case
{
	u32		 opcode;
	u8		 op, sub, cond, d, n, m, shift, flagMask;
	u16		 flags;
	u32		 imm;
	const char *name;
};

static void runCases(const Case *cases, int count, bool thumb)
{
	static u32		code32[256];
	static u16		code16[256];
	static JitBlock block;

	const u32 mask = thumb ? (u32)(sizeof(code16) - 2) : (u32)(sizeof(code32) - 4);

	for (int i = 0; i < count; i++)
	{
		memset(code32, 0, sizeof(code32));
		if (thumb)
			code16[0] = (u16)cases[i].opcode;
		else
			code32[0] = cases[i].opcode;

		JitSource src = { thumb ? (const u8 *)code16 : (const u8 *)code32, mask };
		const bool ok = thumb ? jitDecodeThumb(src, 0, &block)
							  : jitDecodeArm(src, 0, &block);

		printf("%s %-26s %04X -> ", thumb ? "thumb" : "arm  ",
			   cases[i].name, cases[i].opcode);
		if (!ok)
		{
			printf("DECODE FAILED\n");
			failures++;
			continue;
		}
		printf("%s\n", jitOpName(block.insn[0].op));

		const JitInsn *in = &block.insn[0];
		CHECK_EQ(in->op, cases[i].op);
		CHECK_EQ(in->sub, cases[i].sub);
		CHECK_EQ(in->cond, cases[i].cond);
		CHECK_EQ(in->d, cases[i].d);
		CHECK_EQ(in->n, cases[i].n);
		CHECK_EQ(in->m, cases[i].m);
		CHECK_EQ(in->shift, cases[i].shift);
		CHECK_EQ(in->flags, cases[i].flags);
		CHECK_EQ(in->flagMask, cases[i].flagMask);
		CHECK_EQ(in->imm, cases[i].imm);
#ifdef USE_GBA_CORE_V7
		if (thumb)
			CHECK_EQ(in->fetchCycles, (int)thumbCycles[cases[i].opcode >> 8]);
		else
			CHECK_EQ(in->fetchCycles, 0);
#endif
	}
}

int main()
{
	// ---------------------------------------------------------------------
	// ARM
	// ---------------------------------------------------------------------
	// op, sub, cond, d, n, m, shift, flagMask, flags, imm
	const Case alu[] = {
		{ 0xE3A00001, JOP_ALU, ALU_MOV, 0xE, 0, 0, 0, SHIFT_IMM_ROT, 0, JF_IMM, 0x0001, "mov r0, #1" },
		{ 0xE0821003, JOP_ALU, ALU_ADD, 0xE, 1, 2, 3, SHIFT_LSL_IMM, 0, 0, 0, "add r1, r2, r3" },
		{ 0xE0821203, JOP_ALU, ALU_ADD, 0xE, 1, 2, 3, SHIFT_LSL_IMM, 0, 0, 4, "add r1, r2, r3, lsl #4" },
		{ 0xE0821013, JOP_ALU, ALU_ADD, 0xE, 1, 2, 3, SHIFT_LSL_REG, 0, 0, 0, "add r1, r2, r3, lsl r0" },
		// V7's ARM ROR-by-register writes the carry even for a raw count of 0;
		// V8 leaves the carry alone in that case (SHIFT_ROR_REG).
#ifdef USE_GBA_CORE_V7
		{ 0xE1A00271, JOP_ALU, ALU_MOV, 0xE, 0, 0, 1, SHIFT_ROR_REG_V7, 0, 0, 2, "mov r0, r1, ror r2 (v7)" },
#else
		{ 0xE1A00271, JOP_ALU, ALU_MOV, 0xE, 0, 0, 1, SHIFT_ROR_REG, 0, 0, 2, "mov r0, r1, ror r2" },
#endif
		{ 0xE3500000, JOP_ALU, ALU_CMP, 0xE, 0, 0, 0, SHIFT_IMM_ROT, JITF_ALL, JF_IMM, 0, "cmp r0, #0" },
		{ 0xE1A00000, JOP_ALU, ALU_MOV, 0xE, 0, 0, 0, SHIFT_LSL_IMM, 0, 0, 0, "mov r0, r0 (nop)" },
		{ 0x03A00000, JOP_ALU, ALU_MOV, 0x0, 0, 0, 0, SHIFT_IMM_ROT, 0, JF_IMM, 0, "moveq r0, #0" },
		{ 0xE0900000, JOP_ALU, ALU_ADD, 0xE, 0, 0, 0, SHIFT_LSL_IMM, JITF_ALL, 0, 0, "adds r0, r0, r0" },
		{ 0xE1100001, JOP_ALU, ALU_TST, 0xE, 0, 0, 1, SHIFT_LSL_IMM, JITF_NZC, 0, 0, "tst r0, r1" },
		{ 0xE1B00000, JOP_ALU, ALU_MOV, 0xE, 0, 0, 0, SHIFT_LSL_IMM, JITF_NZC, 0, 0, "movs r0, r0" },
	};

	const Case mem[] = {
		{ 0xE5910000, JOP_LOAD, MEM_WORD, 0xE, 0, 1, 0, SHIFT_NONE,
		  0, JF_PRE | JF_UP | JF_IMM, 0, "ldr r0, [r1]" },
		{ 0xE5B10004, JOP_LOAD, MEM_WORD, 0xE, 0, 1, 0, SHIFT_NONE,
		  0, JF_PRE | JF_UP | JF_WRITEBACK | JF_IMM, 4, "ldr r0, [r1, #4]!" },
		{ 0xE4810004, JOP_STORE, MEM_WORD, 0xE, 0, 1, 0, SHIFT_NONE,
		  0, JF_UP | JF_WRITEBACK | JF_IMM, 4, "str r0, [r1], #4" },
		{ 0xE1D100B0, JOP_LOAD, MEM_HALF, 0xE, 0, 1, 0, SHIFT_NONE,
		  0, JF_PRE | JF_UP | JF_IMM, 0, "ldrh r0, [r1]" },
		{ 0xE1D100F0, JOP_LOAD, MEM_HALF, 0xE, 0, 1, 0, SHIFT_NONE,
		  0, JF_PRE | JF_UP | JF_SIGNED | JF_IMM, 0, "ldrsh r0, [r1]" },
		{ 0xE1D100D0, JOP_LOAD, MEM_BYTE, 0xE, 0, 1, 0, SHIFT_NONE,
		  0, JF_PRE | JF_UP | JF_SIGNED | JF_IMM, 0, "ldrsb r0, [r1]" },
		{ 0xE5110004, JOP_LOAD, MEM_WORD, 0xE, 0, 1, 0, SHIFT_NONE,
		  0, JF_PRE | JF_IMM, 4, "ldr r0, [r1, #-4]" },
		{ 0xE5C10004, JOP_STORE, MEM_BYTE, 0xE, 0, 1, 0, SHIFT_NONE,
		  0, JF_PRE | JF_UP | JF_IMM, 4, "strb r0, [r1, #4]" },
		{ 0xE7910102, JOP_LOAD, MEM_WORD, 0xE, 0, 1, 2, SHIFT_LSL_IMM,
		  0, JF_PRE | JF_UP, 2, "ldr r0, [r1, r2, lsl #2]" },
		{ 0xE7910062, JOP_LOAD, MEM_WORD, 0xE, 0, 1, 2, SHIFT_ROR_IMM,
		  0, JF_PRE | JF_UP, 0, "ldr r0, [r1, r2, rrx]" },
	};

	const Case mul[] = {
		{ 0xE0000291, JOP_MUL, MUL_MUL, 0xE, 0, 1, 2, 0, 0, 0, 0, "mul r0, r1, r2" },
		{ 0xE0200291, JOP_MUL, MUL_MLA, 0xE, 0, 1, 2, 0, 0, 0, 0, "mla r0, r1, r2, r0" },
		{ 0xE0810392, JOP_MUL, MUL_UMULL, 0xE, 1, 2, 3, 0, 0, 0, 0, "umull r0, r1, r2, r3" },
		{ 0xE0C10392, JOP_MUL, MUL_SMULL, 0xE, 1, 2, 3, 0, 0, 0, 0, "smull r0, r1, r2, r3" },
		{ 0xE0A10392, JOP_MUL, MUL_UMLAL, 0xE, 1, 2, 3, 0, 0, 0, 0, "umlal r0, r1, r2, r3" },
		{ 0xE0E10392, JOP_MUL, MUL_SMLAL, 0xE, 1, 2, 3, 0, 0, 0, 0, "smlal r0, r1, r2, r3" },
		{ 0xE0100291, JOP_MUL, MUL_MUL, 0xE, 0, 1, 2, 0, JITF_NZ, 0, 0, "muls r0, r1, r2" },
	};

	const Case other[] = {
		{ 0xEAFFFFFE, JOP_BRANCH, 0, 0xE, 0, 0, 0, 0, 0, 0, 0xFFFFFFF8, "b ." },
		{ 0xEB000000, JOP_BRANCH, 0, 0xE, 0, 0, 0, 0, 0, JF_LINK, 0, "bl +0" },
		{ 0xE12FFF1E, JOP_BX, 0, 0xE, 0, 0, 14, 0, 0, 0, 0, "bx lr" },
		{ 0xEF000000, JOP_SWI, 0, 0xE, 0, 0, 0, 0, 0, 0, 0, "swi 0" },
		{ 0xE10F0000, JOP_MRS, 0, 0xE, 0, 0, 0, 0, 0, 0, 0, "mrs r0, cpsr" },
		{ 0xE14F0000, JOP_MRS, 1, 0xE, 0, 0, 0, 0, 0, 0, 0, "mrs r0, spsr" },
#ifndef USE_GBA_CORE_V7
		// V7 refuses every MSR that writes CPSR (see JitDecodeArm.cpp): those
		// go through CPUSwitchMode(..., breakLoop = true) and rewrite CPULoop's
		// locals, so the block has to fall back to the interpreter.  The
		// SPSR forms below are still decoded; the register form is checked
		// separately in checkV7MsrSpsr().
		{ 0xE128F000, JOP_MSR, 0, 0xE, 8, 0, 0, 0, 0, 0, 0, "msr cpsr_f, r0" },
		{ 0xE328F0FF, JOP_MSR, 0, 0xE, 8, 0, 0, 0, 0, JF_IMM, 0xFF, "msr cpsr_f, #0xFF" },
#endif
		{ 0xE1020091, JOP_SWP, MEM_WORD, 0xE, 0, 2, 1, 0, 0, 0, 0, "swp r0, r1, [r2]" },
		// Coprocessor space: the GBA has none, and bits 27-24 are 1101/1110
		// here - testing bit 24 alone mistook these for SWI.
		{ 0xED000000, JOP_UNDEF, 0, 0xE, 0, 0, 0, 0, 0, 0, 0, "coprocessor load (no cp)" },
		// S=1 with L=0 is a "signed store": undefined in the emulator's table.
		{ 0xE1C100D0, JOP_UNDEF, 0, 0xE, 0, 0, 0, 0, 0, 0, 0, "ldrsb with L=0" },
		// The ARM ARM wants bits 11-8 clear here; the emulator's table does
		// not look at them, so the decoder must not either.
		{ 0x01092196, JOP_SWP, MEM_WORD, 0x0, 2, 9, 6, 0, 0, 0, 0, "swp r2, r6, [r9], bits 11-8 set" },
		// Single data transfer with bit 4 set: the ARM ARM's shift-by-register
		// form, which the emulator routes to armUnknownInsn.
		{ 0xE7D76039, JOP_UNDEF, 0, 0xE, 0, 0, 0, 0, 0, 0, 0, "ldr with bit 4 set" },
		{ 0xE8B00006, JOP_LDM, BLOCK_IA, 0xE, 0, 0, 0, 0, 0, JF_WRITEBACK, 6, "ldmia r0!, {r1,r2}" },
		{ 0xE8A00006, JOP_STM, BLOCK_IA, 0xE, 0, 0, 0, 0, 0, JF_WRITEBACK, 6, "stmia r0!, {r1,r2}" },
		{ 0xE9000006, JOP_STM, BLOCK_DB, 0xE, 0, 0, 0, 0, 0, 0, 6, "stmdb r0, {r1,r2}" },
		{ 0xEE000000, JOP_UNDEF, 0, 0xE, 0, 0, 0, 0, 0, 0, 0, "cdp (no coprocessor)" },
	};

	runCases(alu, (int)(sizeof(alu) / sizeof(alu[0])), false);
	runCases(mem, (int)(sizeof(mem) / sizeof(mem[0])), false);
	runCases(mul, (int)(sizeof(mul) / sizeof(mul[0])), false);
	runCases(other, (int)(sizeof(other) / sizeof(other[0])), false);

	// A branch ends the block after itself.
	{
		static u32		 code[8] = { 0xE1A00000, 0xE1A00000, 0xEAFFFFFE, 0xE1A00000 };
		static JitBlock	 block;
		JitSource src = { (const u8 *)code, (u32)(sizeof(code) - 4) };
		jitDecodeArm(src, 0, &block);
		CHECK_EQ(block.count, 3);
		CHECK_EQ(block.ended, 1);

		char buf[1024];
		jitDumpBlock(&block, buf, sizeof(buf));
		printf("\n--- dump ---\n%s\n", buf);
	}

	// A PC write ends the block too.
	{
		static u32		 code[8] = { 0xE1A0F000, 0xE1A00000 };		// mov pc, r0
		static JitBlock	 block;
		JitSource src = { (const u8 *)code, (u32)(sizeof(code) - 4) };
		jitDecodeArm(src, 0, &block);
		CHECK_EQ(block.count, 1);
		CHECK_EQ(block.ended, 1);
	}

	// The "^" (user bank) block transfers are left to the interpreter.
	{
		static u32		 code[4] = { 0xE8D00006, 0, 0, 0 };	// ldmia r0, {r1,r2}^
		static JitBlock	 block;
		JitSource src = { (const u8 *)code, (u32)(sizeof(code) - 4) };
		CHECK(!jitDecodeArm(src, 0, &block));
	}

	// ---------------------------------------------------------------------
	// Thumb
	// ---------------------------------------------------------------------
	const Case tshift[] = {
		{ 0x0008, JOP_ALU, ALU_MOV, 0xE, 0, 0, 1, SHIFT_LSL_IMM, JITF_NZ, 0, 0, "lsl r0, r1, #0" },
		{ 0x00C8, JOP_ALU, ALU_MOV, 0xE, 0, 0, 1, SHIFT_LSL_IMM, JITF_NZC, 0, 3, "lsl r0, r1, #3" },
		{ 0x0848, JOP_ALU, ALU_MOV, 0xE, 0, 0, 1, SHIFT_LSR_IMM, JITF_NZC, 0, 1, "lsr r0, r1, #1" },
		{ 0x1000, JOP_ALU, ALU_MOV, 0xE, 0, 0, 0, SHIFT_ASR_IMM, JITF_NZC, 0, 0, "asr r0, r0, #32" },
	};

	const Case taddsub[] = {
		{ 0x1888, JOP_ALU, ALU_ADD, 0xE, 0, 1, 2, SHIFT_NONE, JITF_ALL, 0, 0, "add r0, r1, r2" },
		{ 0x1A88, JOP_ALU, ALU_SUB, 0xE, 0, 1, 2, SHIFT_NONE, JITF_ALL, 0, 0, "sub r0, r1, r2" },
		{ 0x1C48, JOP_ALU, ALU_ADD, 0xE, 0, 1, 0, SHIFT_NONE, JITF_ALL, JF_IMM, 1, "add r0, r1, #1" },
		{ 0x1E48, JOP_ALU, ALU_SUB, 0xE, 0, 1, 0, SHIFT_NONE, JITF_ALL, JF_IMM, 1, "sub r0, r1, #1" },
		{ 0x2005, JOP_ALU, ALU_MOV, 0xE, 0, 0, 0, SHIFT_NONE, JITF_NZ, JF_IMM, 5, "mov r0, #5" },
		{ 0x2805, JOP_ALU, ALU_CMP, 0xE, 0, 0, 0, SHIFT_NONE, JITF_ALL, JF_IMM, 5, "cmp r0, #5" },
		{ 0x3005, JOP_ALU, ALU_ADD, 0xE, 0, 0, 0, SHIFT_NONE, JITF_ALL, JF_IMM, 5, "add r0, #5" },
		{ 0x3805, JOP_ALU, ALU_SUB, 0xE, 0, 0, 0, SHIFT_NONE, JITF_ALL, JF_IMM, 5, "sub r0, #5" },
	};

	const Case talu[] = {
		{ 0x4008, JOP_ALU, ALU_AND, 0xE, 0, 0, 1, SHIFT_NONE, JITF_NZ, 0, 0, "and r0, r1" },
		{ 0x4048, JOP_ALU, ALU_EOR, 0xE, 0, 0, 1, SHIFT_NONE, JITF_NZ, 0, 0, "eor r0, r1" },
		{ 0x4088, JOP_ALU, ALU_MOV, 0xE, 0, 0, 0, SHIFT_LSL_REG, JITF_NZC, 0, 1, "lsl r0, r1" },
		{ 0x40C8, JOP_ALU, ALU_MOV, 0xE, 0, 0, 0, SHIFT_LSR_REG, JITF_NZC, 0, 1, "lsr r0, r1" },
		{ 0x4108, JOP_ALU, ALU_MOV, 0xE, 0, 0, 0, SHIFT_ASR_REG, JITF_NZC, 0, 1, "asr r0, r1" },
		{ 0x4148, JOP_ALU, ALU_ADC, 0xE, 0, 0, 1, SHIFT_NONE, JITF_ALL, 0, 0, "adc r0, r1" },
		{ 0x4188, JOP_ALU, ALU_SBC, 0xE, 0, 0, 1, SHIFT_NONE, JITF_ALL, 0, 0, "sbc r0, r1" },
		{ 0x41C8, JOP_ALU, ALU_MOV, 0xE, 0, 0, 0, SHIFT_ROR_REG_T, JITF_NZC, 0, 1, "ror r0, r1" },
		{ 0x4208, JOP_ALU, ALU_TST, 0xE, 0, 0, 1, SHIFT_NONE, JITF_NZ, 0, 0, "tst r0, r1" },
		{ 0x4248, JOP_ALU, ALU_RSB, 0xE, 0, 1, 0, SHIFT_NONE, JITF_ALL, JF_IMM, 0, "neg r0, r1" },
		{ 0x4288, JOP_ALU, ALU_CMP, 0xE, 0, 0, 1, SHIFT_NONE, JITF_ALL, 0, 0, "cmp r0, r1" },
		{ 0x42C8, JOP_ALU, ALU_CMN, 0xE, 0, 0, 1, SHIFT_NONE, JITF_ALL, 0, 0, "cmn r0, r1" },
		{ 0x4308, JOP_ALU, ALU_ORR, 0xE, 0, 0, 1, SHIFT_NONE, JITF_NZ, 0, 0, "orr r0, r1" },
		{ 0x4348, JOP_MUL, MUL_MUL, 0xE, 0, 0, 1, SHIFT_NONE, JITF_NZ, 0, 0, "mul r0, r1" },
		{ 0x4388, JOP_ALU, ALU_BIC, 0xE, 0, 0, 1, SHIFT_NONE, JITF_NZ, 0, 0, "bic r0, r1" },
		{ 0x43C8, JOP_ALU, ALU_MVN, 0xE, 0, 0, 1, SHIFT_NONE, JITF_NZ, 0, 0, "mvn r0, r1" },
	};

	const Case thi[] = {
		{ 0x4488, JOP_ALU, ALU_ADD, 0xE, 8, 8, 1, SHIFT_NONE, 0, 0, 0, "add r8, r1" },
		{ 0x4588, JOP_ALU, ALU_CMP, 0xE, 0, 8, 1, SHIFT_NONE, JITF_ALL, 0, 0, "cmp r8, r1" },
		{ 0x4688, JOP_ALU, ALU_MOV, 0xE, 8, 0, 1, SHIFT_NONE, 0, 0, 0, "mov r8, r1" },
		{ 0x468F, JOP_ALU, ALU_MOV, 0xE, 15, 0, 1, SHIFT_NONE, 0, 0, 0, "mov pc, r1" },
		{ 0x4708, JOP_BX, 0, 0xE, 0, 0, 1, SHIFT_NONE, 0, 0, 0, "bx r1" },
		// Bits 6-3 are Rm, so bit 6 must not be part of the BLX test.
		{ 0x4770, JOP_BX, 0, 0xE, 0, 0, 14, SHIFT_NONE, 0, 0, 0, "bx lr" },
		{ 0x4744, JOP_BX, 0, 0xE, 0, 0, 8, SHIFT_NONE, 0, 0, 0, "bx r8" },
	};

	const Case tmem[] = {
		{ 0x4804, JOP_LOAD, MEM_WORD, 0xE, 0, 15, 0, SHIFT_NONE, 0,
		  JF_IMM | JF_PRE | JF_UP | JF_PC_ALIGN4 | JF_CLEAR_PREFETCH | JF_QUICK, 16, "ldr r0, [pc, #16]" },
		{ 0x5088, JOP_STORE, MEM_WORD, 0xE, 0, 1, 2, SHIFT_LSL_IMM, 0,
		  JF_PRE | JF_UP, 0, "str r0, [r1, r2]" },
		{ 0x5688, JOP_LOAD, MEM_BYTE, 0xE, 0, 1, 2, SHIFT_LSL_IMM, 0,
		  JF_PRE | JF_UP | JF_SIGNED, 0, "ldrsb r0, [r1, r2]" },
		{ 0x5A88, JOP_LOAD, MEM_HALF, 0xE, 0, 1, 2, SHIFT_LSL_IMM, 0,
		  JF_PRE | JF_UP | JIT_LDRH_REG_DATA32, 0, "ldrh r0, [r1, r2]" },
		{ 0x6048, JOP_STORE, MEM_WORD, 0xE, 0, 1, 0, SHIFT_NONE, 0,
		  JF_IMM | JF_PRE | JF_UP, 4, "str r0, [r1, #4]" },
		{ 0x6848, JOP_LOAD, MEM_WORD, 0xE, 0, 1, 0, SHIFT_NONE, 0,
		  JF_IMM | JF_PRE | JF_UP, 4, "ldr r0, [r1, #4]" },
		{ 0x7048, JOP_STORE, MEM_BYTE, 0xE, 0, 1, 0, SHIFT_NONE, 0,
		  JF_IMM | JF_PRE | JF_UP, 1, "strb r0, [r1, #1]" },
		{ 0x8848, JOP_LOAD, MEM_HALF, 0xE, 0, 1, 0, SHIFT_NONE, 0,
		  JF_IMM | JF_PRE | JF_UP, 2, "ldrh r0, [r1, #2]" },
		{ 0x9004, JOP_STORE, MEM_WORD, 0xE, 0, 13, 0, SHIFT_NONE, 0,
		  JF_IMM | JF_PRE | JF_UP, 16, "str r0, [sp, #16]" },
		{ 0x9804, JOP_LOAD, MEM_WORD, 0xE, 0, 13, 0, SHIFT_NONE, 0,
		  JF_IMM | JF_PRE | JF_UP | JF_QUICK, 16, "ldr r0, [sp, #16]" },
	};

	const Case tsp[] = {
		{ 0xA004, JOP_ALU, ALU_ADD, 0xE, 0, 15, 0, SHIFT_NONE, 0,
		  JF_IMM | JF_PC_ALIGN4, 16, "add r0, pc, #16" },
		{ 0xA804, JOP_ALU, ALU_ADD, 0xE, 0, 13, 0, SHIFT_NONE, 0, JF_IMM, 16, "add r0, sp, #16" },
		{ 0xB081, JOP_ALU, ALU_ADD, 0xE, 13, 13, 0, SHIFT_NONE, 0, JF_IMM, 0xFFFFFFFC, "sub sp, #4" },
		{ 0xB402, JOP_PUSH, 0, 0xE, 0, 13, 0, SHIFT_NONE, 0, 0, 2, "push {r1}" },
		{ 0xB500, JOP_PUSH, 0, 0xE, 0, 13, 0, SHIFT_NONE, 0, 0, 0x4000, "push {lr}" },
		{ 0xBC02, JOP_POP, 0, 0xE, 0, 13, 0, SHIFT_NONE, 0, 0, 2, "pop {r1}" },
		{ 0xBD00, JOP_POP, 0, 0xE, 0, 13, 0, SHIFT_NONE, 0, 0, 0x8000, "pop {pc}" },
		{ 0xC006, JOP_STM, BLOCK_IA, 0xE, 0, 0, 0, SHIFT_NONE, 0, JF_WRITEBACK, 6, "stmia r0!, {r1,r2}" },
		{ 0xC806, JOP_LDM, BLOCK_IA, 0xE, 0, 0, 0, SHIFT_NONE, 0, JF_WRITEBACK, 6, "ldmia r0!, {r1,r2}" },
	};

	const Case tbl[] = {
		{ 0xD0FE, JOP_BRANCH, 0, 0x0, 0, 0, 0, SHIFT_NONE, 0, 0, 0xFFFFFFFC, "beq ." },
		{ 0xDF05, JOP_SWI, 0, 0xE, 0, 0, 0, SHIFT_NONE, 0, 0, 5, "swi 5" },
		{ 0xE7FE, JOP_BRANCH, 0, 0xE, 0, 0, 0, SHIFT_NONE, 0, 0, 0xFFFFFFFC, "b ." },
		{ 0xF000, JOP_ALU, ALU_ADD, 0xE, 14, 15, 0, SHIFT_NONE, 0, JF_IMM, 0, "bl hi #0" },
		{ 0xF400, JOP_ALU, ALU_ADD, 0xE, 14, 15, 0, SHIFT_NONE, 0, JF_IMM, 0xFFC00000, "bl hi #-1024" },
		{ 0xF800, JOP_BRANCH, 0, 0xE, 0, 0, 0, SHIFT_NONE, 0, JF_LINK, 0, "bl lo #0" },
		{ 0xDE00, JOP_UNDEF, 0, 0xE, 0, 0, 0, SHIFT_NONE, 0, 0, 0, "undefined (cond 14)" },
		{ 0xB100, JOP_UNDEF, 0, 0xE, 0, 0, 0, SHIFT_NONE, 0, 0, 0, "undefined (0xB1xx)" },
		{ 0x4780, JOP_UNDEF, 0, 0xE, 0, 0, 0, SHIFT_NONE, 0, 0, 0, "undefined (bx h1)" },
	};

	runCases(tshift, (int)(sizeof(tshift) / sizeof(tshift[0])), true);
	runCases(taddsub, (int)(sizeof(taddsub) / sizeof(taddsub[0])), true);
	runCases(talu, (int)(sizeof(talu) / sizeof(talu[0])), true);
	runCases(thi, (int)(sizeof(thi) / sizeof(thi[0])), true);
	runCases(tmem, (int)(sizeof(tmem) / sizeof(tmem[0])), true);
	runCases(tsp, (int)(sizeof(tsp) / sizeof(tsp[0])), true);
	runCases(tbl, (int)(sizeof(tbl) / sizeof(tbl[0])), true);

	// Thumb block boundaries: a branch ends the block, POP {pc} too.
	{
		static u16		 code[8] = { 0x2000, 0x2001, 0xE000, 0x2002 };
		static JitBlock	 block;
		JitSource src = { (const u8 *)code, (u32)(sizeof(code) - 2) };
		jitDecodeThumb(src, 0, &block);
		CHECK_EQ(block.count, 3);
		CHECK_EQ(block.ended, 1);

		char buf[1024];
		jitDumpBlock(&block, buf, sizeof(buf));
		printf("\n--- thumb dump ---\n%s\n", buf);
	}
	{
		static u16		 code[8] = { 0xBD00, 0x2000 };
		static JitBlock	 block;
		JitSource src = { (const u8 *)code, (u32)(sizeof(code) - 2) };
		jitDecodeThumb(src, 0, &block);
		CHECK_EQ(block.count, 1);
		CHECK_EQ(block.ended, 1);
	}

	// A 1 KB page boundary ends a block even without a branch.
	{
		static u16		 code[700];
		static JitBlock	 block;
		memset(code, 0, sizeof(code));
		for (int i = 0; i < 700; i++)
			code[i] = 0x2000;			// mov r0, #0
		JitSource src = { (const u8 *)code, (u32)(sizeof(code) - 2) };
		jitDecodeThumb(src, 0, &block);
		CHECK_EQ(block.count, 64);		// JIT_MAX_INSNS wins here
		CHECK_EQ(block.ended, 0);

		jitDecodeThumb(src, 0x3FC, &block);		// 2 insns left before 0x400
		CHECK_EQ(block.count, 2);
		CHECK_EQ(block.ended, 0);
	}

	// Words the emulator's table routes to armBreakpoint()/thumbBreakpoint():
	// the handler stops the machine (clockTicks = -1), which a block cannot
	// model, so the decoder must refuse the whole block.
	{
		static u32		code[4] = { 0xE1200070, 0xE1A00000 };	// bkpt 0 / mov r0, r0
		static JitBlock block;
		JitSource src = { (const u8 *)code, (u32)(sizeof(code) - 4) };
		CHECK_EQ(jitDecodeArm(src, 0, &block), false);

		static u16		code16[4] = { 0xBE00, 0x2000 };
		JitSource src16 = { (const u8 *)code16, (u32)(sizeof(code16) - 2) };
		CHECK_EQ(jitDecodeThumb(src16, 0, &block), false);
	}

	// P=0, W=1 halfword transfers: V8's table leaves the U=0/I=0 encodings
	// undefined (keys 0x02B/0x03B/0x06B/0x07B/0x0AB/0x0BB), V7 implements them
	// as ordinary post-indexed writeback.  See 3.2.1.7.
	{
		static u32		code[4] = { 0xE03100B0, 0xE1A00000 };	// ldrh r0, [r1], -r0
		static JitBlock block;
		JitSource src = { (const u8 *)code, (u32)(sizeof(code) - 4) };
#ifdef USE_GBA_CORE_V7
		CHECK_EQ(jitDecodeArm(src, 0, &block), true);
		CHECK_EQ(block.insn[0].op, JOP_LOAD);
		CHECK_EQ(block.insn[0].sub, MEM_HALF);
		CHECK_EQ(block.insn[0].flags, JF_WRITEBACK);
		CHECK_EQ(block.insn[0].d, 0);
		CHECK_EQ(block.insn[0].n, 1);
		CHECK_EQ(block.insn[0].m, 0);

		static u32		code2[4] = { 0xE02100B0, 0xE1A00000 };	// strh r0, [r1], -r0
		JitSource src2 = { (const u8 *)code2, (u32)(sizeof(code2) - 4) };
		CHECK_EQ(jitDecodeArm(src2, 0, &block), true);
		CHECK_EQ(block.insn[0].op, JOP_STORE);
		CHECK_EQ(block.insn[0].sub, MEM_HALF);
		CHECK_EQ(block.insn[0].flags, JF_WRITEBACK);
#else
		// V8's table routes the word to arm_UI, which the decoder represents as
		// JOP_UNDEF (the block still decodes; the undefined op ends it).
		CHECK_EQ(jitDecodeArm(src, 0, &block), true);
		CHECK_EQ(block.insn[0].op, JOP_UNDEF);
#endif
	}

	// Thumb 0x48 / 0x98 loads must ask the IR interpreter for a Quick read
	// (no Lua READ hook) on both cores; stores must not.
	{
		static u16		code[4] = { 0x4804, 0x9004, 0x9804, 0x2000 };
		static JitBlock block;
		JitSource src = { (const u8 *)code, (u32)(sizeof(code) - 2) };
		CHECK_EQ(jitDecodeThumb(src, 0, &block), true);
		CHECK_EQ(block.insn[0].flags & JF_QUICK, JF_QUICK);	// ldr r0, [pc, #16]
		CHECK_EQ(block.insn[1].flags & JF_QUICK, 0);		// str r0, [sp, #16]
		CHECK_EQ(block.insn[2].flags & JF_QUICK, JF_QUICK);	// ldr r0, [sp, #16]
	}

#ifdef USE_GBA_CORE_V7
	// V7 refuses every MSR that writes CPSR (it reaches CPUSwitchMode with
	// breakLoop = true and rewrites CPULoop's locals) but keeps the SPSR forms.
	{
		static u32		code[4];
		static JitBlock block;
		JitSource src = { (const u8 *)code, (u32)(sizeof(code) - 4) };

		code[0] = 0xE128F000;		// msr cpsr_f, r0
		CHECK_EQ(jitDecodeArm(src, 0, &block), false);
		code[0] = 0xE328F0FF;		// msr cpsr_f, #0xFF
		CHECK_EQ(jitDecodeArm(src, 0, &block), false);
		code[0] = 0xE121F000;		// msr cpsr_c, r0
		CHECK_EQ(jitDecodeArm(src, 0, &block), false);

		code[0] = 0xE166F000;		// msr spsr_f, r0
		CHECK_EQ(jitDecodeArm(src, 0, &block), true);
		CHECK_EQ(block.insn[0].op, JOP_MSR);
		CHECK_EQ(block.insn[0].sub, 1);
		CHECK_EQ(block.insn[0].d, 6);
		code[0] = 0xE366F0FF;		// msr spsr_f, #0xFF
		CHECK_EQ(jitDecodeArm(src, 0, &block), true);
		CHECK_EQ(block.insn[0].op, JOP_MSR);
		CHECK_EQ(block.insn[0].sub, 1);
		CHECK_EQ(block.insn[0].flags & JF_IMM, JF_IMM);
	}
#endif

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
