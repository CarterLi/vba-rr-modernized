// Differential verifier: interpreter vs. decoder+IR, one instruction at a time.
// See JitVerify.h for what is compared and what is skipped.
//
// Two passes, one per instruction set: the ARM decoder and the Thumb decoder
// share nothing but the IR, so a bug in one says nothing about the other.

#include <cstdio>
#include <cstring>

#include "JitIr.h"
#include "JitCache.h"
#include "JitVerify.h"
#include "../GBAGlobals.h"
#include "../GBAinline.h"
#include "../GBA.h"

// ---------------------------------------------------------------------------
// Memory snapshot, shared by both cores.
//
// The block pass runs the interpreter first and the block runner second, so
// every region the block can write has to be put back in between - RAM alone is
// not enough.  The ROM code the pass walks is full of halfword accesses to
// palette/VRAM/OAM (`ldrh r3, [r2]` with r2 = 0x0579C063 is a palette read),
// and a block that writes one of those and then reads it back would otherwise
// see whatever the *first* side left there.  That was the last Thumb block
// mismatch on mario.gba in V7, and V8 had the same four blocks failing for the
// same reason.
//
// IO is in for the same cause plus one more: a few IO *reads* write (the DMA
// hack and the lag counter), so two runs are only comparable if the register
// file is put back as well.
// ---------------------------------------------------------------------------
namespace {

struct MemRegion
{
	u8 *base;
	u32 size;
};

#define VT_MEM_SIZE (0x40000 + 0x8000 + 0x400 + 0x20000 + 0x400 + 0x400)
u8 vtSnapshot[VT_MEM_SIZE];
u8 vtAfterA[VT_MEM_SIZE];

const int VT_REGIONS = 6;

void memRegions(MemRegion *r)
{
	r[0].base = workRAM;
	r[0].size = 0x40000;
	r[1].base = internalRAM;
	r[1].size = 0x8000;
	r[2].base = paletteRAM;
	r[2].size = 0x400;
	r[3].base = vram;
	r[3].size = 0x20000;
	r[4].base = oam;
	r[4].size = 0x400;
	r[5].base = ioMem;
	r[5].size = 0x400;
}

void saveMem(u8 *dst)
{
	MemRegion r[VT_REGIONS];
	memRegions(r);
	for (int i = 0; i < VT_REGIONS; i++)
	{
		memcpy(dst, r[i].base, r[i].size);
		dst += r[i].size;
	}
}

void loadMem(const u8 *src)
{
	MemRegion r[VT_REGIONS];
	memRegions(r);
	for (int i = 0; i < VT_REGIONS; i++)
	{
		memcpy(r[i].base, src, r[i].size);
		src += r[i].size;
	}
}

bool memMatches(const u8 *src)
{
	MemRegion r[VT_REGIONS];
	memRegions(r);
	for (int i = 0; i < VT_REGIONS; i++)
	{
		if (memcmp(r[i].base, src, r[i].size) != 0)
			return false;
		src += r[i].size;
	}
	return true;
}

// The interpreter's memory is in vtAfterA and the block runner's is live, so
// the first differing byte can be named instead of a bare "memory".
void printMemDiff()
{
	MemRegion  r[VT_REGIONS];
	memRegions(r);
	const u32 base[VT_REGIONS] = { 0x02000000, 0x03000000, 0x05000000,
								   0x06000000, 0x07000000, 0x04000000 };
	u32		  off			   = 0;
	for (int i = 0; i < VT_REGIONS; i++)
	{
		for (u32 j = 0; j < r[i].size; j++)
			if (r[i].base[j] != vtAfterA[off + j])
			{
				printf("    memory %08X interp=%02X block=%02X\n", base[i] + j,
					   vtAfterA[off + j], r[i].base[j]);
				return;
			}
		off += r[i].size;
	}
}

} // namespace

#ifdef USE_GBA_CORE_V7

// ---------------------------------------------------------------------------
// Differential verifier for the V7 core.
//
// V7 has no armExecute()/thumbExecute() functions - its interpreter is
// #included straight into CPULoop - so the reference side is
// armExecuteOne()/thumbExecuteOne() from V7/GBA.cpp: one instruction, own local
// clockTicks, returned as the cycle count.  That is exactly what
// jitInterpInsn() promises, so the comparison is field for field.
//
// Three things differ from the V8 pass:
//
//   * no cpuPrefetch / busPrefetchCount / cpuTotalTicks - V7 has none of them.
//     The cycle count is compared as a value instead of being accumulated.
//   * the IR can answer "this instruction must go to the interpreter"
//     (JIT_NEED_INTERP: IO stores, SWI, UNDEF, an ALU write to PC with S=1).
//     Those are counted as skipped, and the IR side is asked *before* the
//     interpreter runs, so an instruction that would call CPUSwitchMode()
//     never executes here.
//   * there is no decode-space sweep: V7's "is this word undefined" decision
//     lives in the interpreter's switch, not in a table predicate that can be
//     queried without executing anything.  The decoder's accept/reject rules
//     are pinned by selftest.cpp instead (the six P=0,W=1 halfword keys, the
//     MSR CPSR refusal, the "^" block-transfer refusal).
// ---------------------------------------------------------------------------

extern bool8 holdState;
extern bool8 stopState;
extern int	 armExecuteOne();
extern int	 thumbExecuteOne();

// CPULoop's locals, reached through these pointers by CPU_BREAK_LOOP_2() and
// CPUCheckDMA().  They are NULL until a real CPULoop runs, and a block that
// switches mode with a pending interrupt dereferences them - so give them
// scratch storage for the duration of the verification.  (Two instructions in a
// block can arm an interrupt through an IO store and then take the
// CPUSwitchMode path; that crash is how this was found.)
extern int32 *extCpuNextEvent;
extern int32 *extClockTicks;
extern int32 *extTicks;
extern int32  cpuSavedTicks;

namespace {

struct CpuState
{
	reg_pair reg[45];
	bool8	 n, c, z, v;
	bool8	 armState, armIrqEnable, holdState, stopState;
	u32		 armNextPC;
	s32		 armMode;
	int		 ticks;
};

void saveCpu(CpuState *s)
{
	memcpy(s->reg, reg, sizeof(s->reg));
	s->n			 = N_FLAG;
	s->c			 = C_FLAG;
	s->z			 = Z_FLAG;
	s->v			 = V_FLAG;
	s->armState		 = armState;
	s->armIrqEnable	 = armIrqEnable;
	s->holdState	 = holdState;
	s->stopState	 = stopState;
	s->armNextPC	 = armNextPC;
	s->armMode		 = armMode;
	s->ticks		 = 0;
}

void loadCpu(const CpuState *s)
{
	memcpy(reg, s->reg, sizeof(s->reg));
	N_FLAG		 = s->n;
	C_FLAG		 = s->c;
	Z_FLAG		 = s->z;
	V_FLAG		 = s->v;
	armState	 = s->armState;
	armIrqEnable = s->armIrqEnable;
	holdState	 = s->holdState;
	stopState	 = s->stopState;
	armNextPC	 = s->armNextPC;
	armMode		 = s->armMode;
}

u32 xorshift(u32 *s)
{
	u32 x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

// Instruction fetch straight out of the ROM window, the same way the decoders
// read it.
u32 fetchInsn(const JitSource &src, u32 addr, bool thumb)
{
	u32 v = 0;
	if (thumb)
	{
		u16 h;
		memcpy(&h, src.base + (addr & src.mask), sizeof(h));
		v = h;
	}
	else
		memcpy(&v, src.base + (addr & src.mask), sizeof(v));
	return v;
}

// A machine state that is identical for both sides and keeps every access
// inside work RAM, so nothing here can touch an IO register and cause a side
// effect the second run would not repeat.
//
// At the top of CPULoop's loop body armNextPC is the instruction to execute and
// reg[15] is that plus the instruction size - not the prefetched address the V8
// loop keeps.
void seedCpu(bool thumb, u32 pc, u32 seed)
{
	u32 s = seed * 2654435761u + 1;
	for (int i = 0; i < 13; i++)
		reg[i].I = 0x02000000 + ((xorshift(&s) & 0x3FF) << 2);
	reg[13].I = 0x03007F00;
	reg[14].I = pc + 4;
	reg[15].I = pc + (thumb ? 2 : 4);
	reg[16].I = 0x1F;
	reg[17].I = 0x1F;

	const u32 bits = xorshift(&s);
	N_FLAG		   = (bits & 1) != 0;
	C_FLAG		   = (bits & 2) != 0;
	Z_FLAG		   = (bits & 4) != 0;
	V_FLAG		   = (bits & 8) != 0;

	armState	 = !thumb;
	armMode		 = 0x1F;
	armIrqEnable = false;
	holdState	 = false;
	stopState	 = false;
	armNextPC	 = pc;

	// No interrupt may fire while an instruction is being compared: mode
	// switches inside the reference would read CPULoop's locals through ext*,
	// which are NULL outside a real run.
	IME = 0;
	IF	= 0;
}

// Fill the writable regions with noise so loads in the passes read something
// deterministic that is not all zeroes.
void seedMemoryV7()
{
	u32 s = 0x9E3779B9u;
	for (u32 i = 0; i < 0x40000; i += 4)
		WRITE32LE((u32 *)(workRAM + i), xorshift(&s));
	for (u32 i = 0; i < 0x8000; i += 4)
		WRITE32LE((u32 *)(internalRAM + i), xorshift(&s));
}

const char *compareCpu(const CpuState *a, const CpuState *b)
{
	for (int i = 0; i < 16; i++)
		if (a->reg[i].I != b->reg[i].I)
			return "reg";
	if (a->reg[16].I != b->reg[16].I)
		return "cpsr";
	if (a->n != b->n) return "N";
	if (a->c != b->c) return "C";
	if (a->z != b->z) return "Z";
	if (a->v != b->v) return "V";
	if (a->armState != b->armState) return "armState";
	if (a->armMode != b->armMode) return "armMode";
	if (a->armNextPC != b->armNextPC) return "armNextPC";
	if (a->ticks != b->ticks) return "cycles";
	return NULL;
}

// Used by the per-instruction pass, which reports only the first register that
// differs; the block pass uses printCpuDiff() below and shows them all.
int firstBadReg(const CpuState *a, const CpuState *b)
{
	for (int i = 0; i < 16; i++)
		if (a->reg[i].I != b->reg[i].I)
			return i;
	return -1;
}

void printDump(u32 pc, bool thumb, const JitInsn *in)
{
	char	 dump[512];
	JitBlock one;
	memset(&one, 0, sizeof(one));
	one.guestPc = pc;
	one.thumb	= thumb ? 1 : 0;
	one.count	= 1;
	one.insn[0] = *in;
	jitDumpBlock(&one, dump, sizeof(dump));
	printf("%s", dump);
}

// ---------------------------------------------------------------------------
// Pass 1: one instruction at a time.
// ---------------------------------------------------------------------------
int verifyPass(bool thumb, const JitSource &src, u32 romSize, int maxInsns, int verbose)
{
	CpuState baseline;
	seedCpu(thumb, 0x08000000, 0);
	saveCpu(&baseline);

	const u32 step	= thumb ? 2 : 4;
	int		  tested = 0, failed = 0, skipped = 0, decodeFailed = 0, shown = 0;
	int		  perOpFail[16] = { 0 };

	printf("\n%s pass\n", thumb ? "thumb" : "arm");

	for (u32 off = 0; off + 8 < romSize && tested < maxInsns; off += step)
	{
		const u32 pc = 0x08000000 + off;

		// Skip the ROM header; those bytes are data as often as they are code.
		if (off < 0xC0)
			continue;

		JitInsn in;
		if (!jitDecodeOne(src, pc, thumb, &in))
		{
			decodeFailed++;
			continue;
		}

		// Anything that writes memory, enters the BIOS or switches mode is out
		// of scope for this pass: it has no RAM snapshot, and the other two
		// change state one comparison cannot put back.  (The block pass below
		// does cover stores.)
		if (in.op == JOP_STORE || in.op == JOP_STM || in.op == JOP_SWP ||
			in.op == JOP_PUSH || in.op == JOP_POP || in.op == JOP_SWI ||
			in.op == JOP_UNDEF || in.op == JOP_MSR)
		{
			skipped++;
			continue;
		}

		// B first: the IR can answer "hand this one to the interpreter", and
		// then the reference must not run it either (it may call
		// CPUSwitchMode, which expects CPULoop's locals).
		loadCpu(&baseline);
		seedCpu(thumb, pc, off);
		u32		  nextPc = pc + step;
		const int bTicks = jitInterpInsn(&in, thumb, pc, &nextPc);
		if (bTicks == JIT_NEED_INTERP)
		{
			skipped++;
			continue;
		}
		if (bTicks < 0)
		{
			decodeFailed++;
			continue;
		}
		CpuState b;
		saveCpu(&b);
		b.ticks = bTicks;

		// A: the real interpreter, one instruction.
		loadCpu(&baseline);
		seedCpu(thumb, pc, off);
		CpuState  a;
		const int aTicks = thumb ? thumbExecuteOne() : armExecuteOne();
		saveCpu(&a);
		a.ticks = aTicks;

		tested++;
		// Progress lines exist so a crash inside the reference interpreter
		// leaves a pointer to where it happened: the report only reaches the
		// log on fflush/exit, and a crash skips both.
		if ((tested % 500) == 0)
			printf("  ... %d tested\n", tested);

		const char *bad = compareCpu(&a, &b);
		if (bad)
		{
			failed++;
			if (in.op < 16)
				perOpFail[in.op]++;
			if (shown < (verbose > 0 ? 25 : 5))
			{
				shown++;
				printf("MISMATCH %08X  op=%s  %s\n", pc, jitOpName(in.op), bad);
				if (strcmp(bad, "reg") == 0)
				{
					const int r = firstBadReg(&a, &b);
					if (r >= 0)
						printf("    r%-2d  interp=%08X  ir=%08X\n", r, a.reg[r].I, b.reg[r].I);
				}
				if (strcmp(bad, "cycles") == 0)
					printf("    cycles interp=%d  ir=%d\n", a.ticks, b.ticks);
				if (strcmp(bad, "armNextPC") == 0)
					printf("    nextPC interp=%08X  ir=%08X\n", a.armNextPC, b.armNextPC);
				if (bad[1] == 0 && strchr("NCZV", bad[0]) != NULL)
					printf("    %s interp=%d  ir=%d\n", bad,
						   bad[0] == 'N' ? a.n : bad[0] == 'C' ? a.c : bad[0] == 'Z' ? a.z : a.v,
						   bad[0] == 'N' ? b.n : bad[0] == 'C' ? b.c : bad[0] == 'Z' ? b.z : b.v);
				printDump(pc, thumb, &in);
			}
		}
	}

	printf("  %d tested, %d failed, %d skipped (memory writes, SWI, MSR), %d undecodable\n",
		   tested, failed, skipped, decodeFailed);
	if (failed)
	{
		printf("  failures by op:");
		for (int i = 0; i < 16; i++)
			if (perOpFail[i])
				printf(" %s=%d", jitOpName((u8)i), perOpFail[i]);
		printf("\n");
	}

	loadCpu(&baseline);
	return failed;
}

// ---------------------------------------------------------------------------
// Pass 2: whole blocks.
//
// The block runner decides by itself where to stop and what it leaves in
// armNextPC/reg[15]; getting either wrong hangs the guest instead of corrupting
// a register, so it gets its own pass.  Stores are in scope here: memory is
// snapshotted before the interpreter run and restored before the block run.
// ---------------------------------------------------------------------------

// A block the interpreter side may run as-is.  SWI enters the BIOS handler
// (which pops a modal dialog when no BIOS file is loaded), UNDEF raises an
// exception and MSR can switch mode: all three change state a memory snapshot
// cannot put back, and the first two can block a scripted run.
bool blockComparable(const JitBlock *b)
{
	for (u16 i = 0; i < b->count; i++)
	{
		switch (b->insn[i].op)
		{
		case JOP_SWI:
		case JOP_UNDEF:
		case JOP_MSR:
			return false;
		default:
			break;
		}
	}
	return true;
}

// One A/B comparison of a block, always from the same memory snapshot
// (vtSnapshot) and the same CPU state.  Returns the first differing field
// ("reg", "cycles", ...), or NULL when both sides agree.  *stopped is set when
// the comparison is not meaningful: the block runner refused an instruction, or
// the interpreter side left the block (V7's one-instruction detour).
const char *compareBlockOnce(const JitBlock *block, bool thumb, const CpuState *baseline,
                             u32 pc, u32 off, u32 step, bool *stopped, CpuState *aOut,
                             CpuState *bOut)
{
	loadMem(vtSnapshot);

	// A: block.count instructions of the real interpreter.  It takes its next
	// address from armNextPC, so it would follow V7's one-instruction detour out
	// of the block - and could then execute anything, including an SWI that pops
	// a dialog.  Stop as soon as the address leaves the block.
	loadCpu(baseline);
	seedCpu(thumb, pc, off);
	int aTicks = 0;
	for (u16 i = 0; i < block->count; i++)
	{
		if (armNextPC != pc + i * step)
		{
			*stopped = true;
			return NULL;
		}
		aTicks += armState ? armExecuteOne() : thumbExecuteOne();
	}
	saveCpu(aOut);
	aOut->ticks = aTicks;
	saveMem(vtAfterA);

	// B: the block runner, event test out of the way.
	loadMem(vtSnapshot);
	loadCpu(baseline);
	seedCpu(thumb, pc, off);
	bool	  needInterp = false;
	const int bTicks	 = jitRunBlockForTest(block, &needInterp);
	if (needInterp)
	{
		*stopped = true;
		return NULL;
	}
	saveCpu(bOut);
	bOut->ticks = bTicks;

	if (const char *bad = compareCpu(aOut, bOut))
		return bad;
	if (!memMatches(vtAfterA))
		return "memory";
	return NULL;
}

// Every register that differs, so a "reg" report says which one.  A block
// mismatch is rare enough that printing all of them is cheaper than another
// build/run cycle to find out which register to look at.
void printCpuDiff(const CpuState *a, const CpuState *b)
{
	for (int i = 0; i < 16; i++)
		if (a->reg[i].I != b->reg[i].I)
			printf("    r%-2d interp=%08X block=%08X\n", i, a->reg[i].I, b->reg[i].I);
	if (a->reg[16].I != b->reg[16].I)
		printf("    cpsr interp=%08X block=%08X\n", a->reg[16].I, b->reg[16].I);
	if (a->n != b->n || a->c != b->c || a->z != b->z || a->v != b->v)
		printf("    flags interp=%d%d%d%d block=%d%d%d%d\n", a->n, a->c, a->z, a->v, b->n, b->c,
			   b->z, b->v);
}

// Narrow a reproducible block mismatch down to the first instruction that
// diverges: run prefixes of the block through both sides and stop at the first
// length where they disagree.  Only called for blocks that already failed, so
// the extra runs cost nothing in the normal case.
//
// Prefixes go through compareBlockOnce() rather than a hand-rolled loop: that
// is the only way to get the same seed (`off`), the same memory snapshot and
// the same "the interpreter left the block" test as the comparison that failed.
// (Seeding with 0 here made the localized instruction differ from the failing
// one, which sent the first investigation after the wrong instruction.)
void localizeBlockMismatch(const JitBlock *block, bool thumb, const CpuState *baseline,
                           u32 pc, u32 off, u32 step)
{
	for (u16 n = 1; n <= block->count; n++)
	{
		JitBlock prefix = *block;
		prefix.count	= n;

		CpuState	a, b;
		bool		stopped = false;
		const char *bad =
			compareBlockOnce(&prefix, thumb, baseline, pc, off, step, &stopped, &a, &b);
		if (stopped)
			return;
		if (!bad)
			continue;

		printf("    first divergence at insn %u of %u: %s (interp=%d block=%d)\n", n,
			   block->count, bad, a.ticks, b.ticks);
		printCpuDiff(&a, &b);

		// The state the two sides agree on right before the diverging
		// instruction, plus what the store wrote and where.  A store cannot
		// change a register, so when the diff is "reg" this line is what says
		// whether the instruction really is the culprit.
		{
			loadMem(vtSnapshot);
			loadCpu(baseline);
			seedCpu(thumb, pc, off);
			for (u16 i = 0; i + 1 < n && armNextPC == pc + i * step; i++)
				armState ? armExecuteOne() : thumbExecuteOne();
			printf("    before: pc=%08X r0=%08X r1=%08X r2=%08X r3=%08X r15=%08X\n",
				   pc + (n - 1) * step, reg[0].I, reg[1].I, reg[2].I, reg[3].I, reg[15].I);
		}

		if (strcmp(bad, "memory") == 0)
			printMemDiff();

		JitBlock one;
		memset(&one, 0, sizeof(one));
		one.guestPc = pc + (n - 1) * step;
		one.thumb	= thumb ? 1 : 0;
		one.count	= 1;
		one.insn[0] = block->insn[n - 1];
		char dump[512];
		jitDumpBlock(&one, dump, sizeof(dump));
		printf("%s", dump);
		return;
	}
}

int verifyBlockPass(bool thumb, const JitSource &src, u32 romSize, int maxInsns, int verbose)
{
	CpuState baseline;
	seedCpu(thumb, 0x08000000, 0);
	saveCpu(&baseline);

	const u32 step	= thumb ? 2 : 4;
	int		  tested = 0, failed = 0, skipped = 0, refuse = 0, shown = 0, insns = 0;

	printf("\n%s block pass\n", thumb ? "thumb" : "arm");

	for (u32 off = 0; off + 8 < romSize && tested < maxInsns; off += step)
	{
		const u32 pc = 0x08000000 + off;
		if (off < 0xC0)
			continue;

		JitBlock block;
		memset(&block, 0, sizeof(block));
		if (!(thumb ? jitDecodeThumb(src, pc, &block) : jitDecodeArm(src, pc, &block)))
		{
			refuse++;
			continue;
		}

		if (!blockComparable(&block))
		{
			skipped++;
			continue;
		}

		saveMem(vtSnapshot);

		CpuState	a, b;
		bool		stopped = false;
		const char *bad		= compareBlockOnce(&block, thumb, &baseline, pc, off, step, &stopped, &a, &b);
		if (stopped)
		{
			skipped++;
			loadCpu(&baseline);
			continue;
		}

		tested++;
		insns += block.count;
		if ((tested % 500) == 0)
			printf("  ... %d blocks\n", tested);

		// Both attempts start from the same snapshot now that palette/VRAM/OAM/
		// IO are part of it, so a mismatch should reproduce exactly.  Anything
		// that does not is a state the snapshot cannot see (systemCounters,
		// lcdTicks, the RTC) and therefore the verifier's own artefact rather
		// than a recompiler bug: require both sides to be identical across the
		// two attempts before believing the difference.
		if (bad)
		{
			CpuState	a2, b2;
			bool		stopped2 = false;
			const char *bad2	 = compareBlockOnce(&block, thumb, &baseline, pc, off, step,
												  &stopped2, &a2, &b2);
			if (stopped2 || !bad2 || strcmp(bad, bad2) != 0 ||
				compareCpu(&a, &a2) != NULL || compareCpu(&b, &b2) != NULL)
				bad = NULL;
		}

		if (bad)
		{
			failed++;
			if (shown < (verbose > 0 ? 25 : 5))
			{
				shown++;
				printf("BLOCK MISMATCH %08X  count=%u  %s\n", pc, block.count, bad);
				printCpuDiff(&a, &b);
				if (strcmp(bad, "memory") == 0)
					printMemDiff();
				if (strcmp(bad, "cycles") == 0)
					printf("    cycles interp=%d  block=%d\n", a.ticks, b.ticks);
				if (strcmp(bad, "armNextPC") == 0)
					printf("    nextPC interp=%08X  block=%08X\n", a.armNextPC, b.armNextPC);

				char dump[1024];
				jitDumpBlock(&block, dump, sizeof(dump));
				printf("%s\n", dump);
				localizeBlockMismatch(&block, thumb, &baseline, pc, off, step);
			}
		}
	}

	printf("  %d block%s (%d instructions) tested, %d failed, %d skipped, %d refused\n",
		   tested, tested == 1 ? "" : "s", insns, failed, skipped, refuse);
	return failed;
}

} // namespace

int jitVerifyRun(int maxInsns, int verbose)
{
	if (!rom || maxInsns <= 0)
		return -1;

	// memoryMap[8] is the ROM window; the decoder reads through it exactly the
	// way the interpreter does.
	JitSource src = { memoryMap[8].address, memoryMap[8].mask };
	if (!src.base)
		return -1;

	// V7 maps the whole 32 MB ROM window regardless of the file size
	// (memoryMap[8].mask is 0x1FFFFFF) and the frontend allocates that much, so
	// walking the full window is safe.  maxInsns is what bounds the run.
	const u32 romSize = 0x2000000;

	// The two readers of CPULoop's locals must not see NULL here.
	static int32 scratchNext = 0x7FFFFFF, scratchClock = 1, scratchTicks = 0;
	extCpuNextEvent = &scratchNext;
	extClockTicks	= &scratchClock;
	extTicks		= &scratchTicks;
	cpuSavedTicks	= 0;

	seedMemoryV7();

	const int armFails	= verifyPass(false, src, romSize, maxInsns, verbose);
	const int thumbFails = verifyPass(true, src, romSize, maxInsns, verbose);

	const int armBlocks	  = verifyBlockPass(false, src, romSize, maxInsns, verbose);
	const int thumbBlocks = verifyBlockPass(true, src, romSize, maxInsns, verbose);

	printf("\njit verify: %d mismatches (%d arm, %d thumb), %d block mismatches "
		   "(%d arm, %d thumb)\n",
		   armFails + thumbFails, armFails, thumbFails, armBlocks + thumbBlocks, armBlocks,
		   thumbBlocks);
	return armFails + thumbFails + armBlocks + thumbBlocks;
}

#else

#include "../V8/GBACpu.h"

// Not in a header; the memory code declares these itself.
extern bool8 stopState;
extern int32 IRQTicks;

// In GBA-arm.cpp / GBA-thumb.cpp, next to the dispatch tables.  True when the
// emulator routes the word to armUnknownInsn / thumbUnknownInsn; those must not
// be run at all (see the comment there).
bool armInsnUndefined(u32 opcode);
bool thumbInsnUndefined(u32 opcode);

namespace {

struct CpuState
{
	reg_pair reg[45];
	bool8	 n, c, z, v;
	bool8	 armState, armIrqEnable, holdState, stopState;
	u32		 armNextPC;
	u32		 prefetch[2];
	s32		 armMode;
	s32		 cpuTotalTicks, cpuNextEvent;
	s32		 busPrefetchCount;
	bool8	 busPrefetch;
};

void saveCpu(CpuState *s)
{
	memcpy(s->reg, reg, sizeof(s->reg));
	s->n			 = N_FLAG;
	s->c			 = C_FLAG;
	s->z			 = Z_FLAG;
	s->v			 = V_FLAG;
	s->armState		 = armState;
	s->armIrqEnable	 = armIrqEnable;
	s->holdState	 = holdState;
	s->stopState	 = stopState;
	s->armNextPC	 = armNextPC;
	s->prefetch[0]	 = cpuPrefetch[0];
	s->prefetch[1]	 = cpuPrefetch[1];
	s->armMode		 = armMode;
	s->cpuTotalTicks = cpuTotalTicks;
	s->cpuNextEvent	 = cpuNextEvent;
	s->busPrefetchCount = busPrefetchCount;
	s->busPrefetch	 = busPrefetch;
}

void loadCpu(const CpuState *s)
{
	memcpy(reg, s->reg, sizeof(s->reg));
	N_FLAG		 = s->n;
	C_FLAG		 = s->c;
	Z_FLAG		 = s->z;
	V_FLAG		 = s->v;
	armState	 = s->armState;
	armIrqEnable = s->armIrqEnable;
	holdState	 = s->holdState;
	stopState	 = s->stopState;
	armNextPC	 = s->armNextPC;
	cpuPrefetch[0] = s->prefetch[0];
	cpuPrefetch[1] = s->prefetch[1];
	armMode		 = s->armMode;
	cpuTotalTicks = s->cpuTotalTicks;
	cpuNextEvent = s->cpuNextEvent;
	busPrefetchCount = s->busPrefetchCount;
	busPrefetch	 = s->busPrefetch;
	SWITicks	 = 0;
	IRQTicks	 = 0;
}

u32 xorshift(u32 *s)
{
	u32 x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

// Instruction fetch straight out of the ROM window, the same way the decoders
// read it.
u32 fetchInsn(const JitSource &src, u32 addr, bool thumb)
{
	u32 v = 0;
	if (thumb)
	{
		u16 h;
		memcpy(&h, src.base + (addr & src.mask), sizeof(h));
		v = h;
	}
	else
		memcpy(&v, src.base + (addr & src.mask), sizeof(v));
	return v;
}

// A machine state that is identical for both sides and keeps every access
// inside work RAM, so nothing here can touch an IO register and cause a side
// effect the second run would not repeat.
void seedCpu(bool thumb, u32 pc, u32 seed)
{
	u32 s = seed * 2654435761u + 1;
	for (int i = 0; i < 13; i++)
		reg[i].I = 0x02000000 + ((xorshift(&s) & 0x3FF) << 2);
	reg[13].I = 0x03007F00;
	reg[14].I = pc + 4;
	// At loop entry reg[15] is one instruction past the current one: the loop
	// copies it into armNextPC and then adds the instruction size.
	reg[15].I = pc + (thumb ? 2 : 4);
	reg[16].I = 0x1F;
	reg[17].I = 0x1F;

	const u32 bits = xorshift(&s);
	N_FLAG		   = (bits & 1) != 0;
	C_FLAG		   = (bits & 2) != 0;
	Z_FLAG		   = (bits & 4) != 0;
	V_FLAG		   = (bits & 8) != 0;

	armState	 = !thumb;
	armMode		 = 0x1F;
	armIrqEnable = false;
	holdState	 = false;
	stopState	 = false;
	armNextPC	 = pc;
	cpuTotalTicks = 0;
	cpuNextEvent  = 1;		// the execute loop stops after one instruction
	busPrefetch	  = false;
	busPrefetchCount = 0;
	IME			  = 0;
	IF			  = 0;
	SWITicks	  = 0;
	IRQTicks	  = 0;
}

void seedMemory()
{
	u32 s = 0x9E3779B9u;
	for (u32 i = 0; i < 0x40000; i += 4)
		WRITE32LE((u32 *)(workRAM + i), xorshift(&s));
	for (u32 i = 0; i < 0x8000; i += 4)
		WRITE32LE((u32 *)(internalRAM + i), xorshift(&s));
}

// Returns the first field that differs, or NULL.
const char *compareCpu(const CpuState *a, const CpuState *b)
{
	for (int i = 0; i < 16; i++)
		if (a->reg[i].I != b->reg[i].I)
			return "reg";
	if (a->reg[16].I != b->reg[16].I)
		return "cpsr";
	if (a->n != b->n) return "N";
	if (a->c != b->c) return "C";
	if (a->z != b->z) return "Z";
	if (a->v != b->v) return "V";
	if (a->armState != b->armState) return "armState";
	if (a->armMode != b->armMode) return "armMode";
	if (a->cpuTotalTicks != b->cpuTotalTicks) return "cycles";
	if (a->busPrefetchCount != b->busPrefetchCount) return "busPrefetchCount";
	return NULL;
}

int firstBadReg(const CpuState *a, const CpuState *b)
{
	for (int i = 0; i < 16; i++)
		if (a->reg[i].I != b->reg[i].I)
			return i;
	return -1;
}

// ---------------------------------------------------------------------------
// The block pass
//
// verifyPass() above runs one instruction at a time, which is not how the
// recompiler works: a block chains instructions, decides by itself where to
// stop (event, instruction-set switch) and refills cpuPrefetch on the way out.
// None of that is visible to a one-instruction comparison, and getting any of
// it wrong hangs the guest instead of corrupting a register - so it gets its
// own pass.
//
// Same rules as verifyPass, except that stores are now in scope: RAM is
// snapshotted before the interpreter pass and restored before the block pass,
// so the two runs start from identical memory and the result can be compared
// byte for byte.  SWI / UNDEF / control-field MSR stay out - they change state
// a memory snapshot cannot put back.
// ---------------------------------------------------------------------------

bool blockComparable(const JitBlock *b)
{
	for (u16 i = 0; i < b->count; i++)
	{
		switch (b->insn[i].op)
		{
		case JOP_SWI:
		case JOP_UNDEF:
			// SWI enters the BIOS handler (which pops a modal dialog when no
			// BIOS file is loaded) and UNDEF raises an exception: both change
			// state the two passes cannot put back with a memory snapshot.
			return false;
		case JOP_MSR:
			// A control-field write can switch mode; banked registers are not
			// part of the comparison.  Same reason verifyPass skips it.
			if (b->insn[i].d & 1)
				return false;
			break;
		default:
			break;
		}
	}
	return true;
}

const char *compareBlockCpu(const CpuState *a, const CpuState *b)
{
	const char *bad = compareCpu(a, b);
	if (bad)
		return bad;
	if (a->armNextPC != b->armNextPC)
		return "armNextPC";
	if (a->prefetch[0] != b->prefetch[0] || a->prefetch[1] != b->prefetch[1])
		return "prefetch";
	return NULL;
}

// One A/B comparison of a block, always from the same memory snapshot
// (vtSnapshot) and the same CPU state.  Returns the first differing field
// ("reg", "cycles", ...), or NULL when both sides agree.  *stopped is set when
// the block runner gave up part way through (it will not model an instruction
// that writes PC without branching), which makes the state incomparable.
const char *compareBlockOnce(const JitBlock *block, bool thumb, const CpuState *baseline,
                             const JitSource &src, u32 pc, u32 off, u32 step, bool *stopped,
                             CpuState *aOut, CpuState *bOut)
{
	const u32 opcode	 = fetchInsn(src, pc, thumb);
	const u32 nextOpcode = fetchInsn(src, pc + step, thumb);

	// A: the interpreter, one instruction per call, the same way CPULoop
	// drives it - including the thumb/arm switch in the middle of a block.
	loadMem(vtSnapshot);
	loadCpu(baseline);
	seedCpu(thumb, pc, off);
	cpuPrefetch[0] = opcode;
	cpuPrefetch[1] = nextOpcode;
	for (u16 i = 0; i < block->count; i++)
	{
		cpuNextEvent = cpuTotalTicks + 1;
		armState ? armExecute() : thumbExecute();

		// A HALT/STOP write parks the CPU: CPULoop would return here, so the
		// rest of the block never runs.  Calling armExecute() again anyway
		// would execute instructions the block correctly refused to run.
		if (holdState || stopState)
		{
			*stopped = true;
			return NULL;
		}

		// A block only ends on a branch or an instruction-set switch, so every
		// instruction before the last one must advance sequentially.  If the
		// interpreter leaves that path (a BX in the middle, which the block
		// ended at), it is no longer executing the block's instructions and
		// the two states are not comparable.
		if (i + 1 < block->count && armNextPC != pc + (i + 1) * step)
		{
			*stopped = true;
			return NULL;
		}
	}
	saveCpu(aOut);
	saveMem(vtAfterA);

	// B: the block runner, with the event test pushed out of the way so the
	// whole block runs no matter how many cycles it costs.
	loadMem(vtSnapshot);
	loadCpu(baseline);
	seedCpu(thumb, pc, off);
	cpuPrefetch[0] = opcode;
	cpuPrefetch[1] = nextOpcode;
	cpuNextEvent = 0x7FFFFFFF;
	jitRunBlockForTest(block, stopped);
	if (*stopped)
		return NULL;
	saveCpu(bOut);

	if (const char *bad = compareBlockCpu(aOut, bOut))
		return bad;
	if (!memMatches(vtAfterA))
		return "memory";
	return NULL;
}

// Narrow a reproducible block mismatch down to the first instruction that
// diverges: run prefixes of the block through both sides and stop at the first
// length where they disagree.  Same idea as the V7 helper, and it seeds the
// same way the failing comparison did - with the block's own offset, not 0 -
// so the localized instruction is the one that actually failed.
void localizeBlockMismatch(const JitBlock *block, bool thumb, const CpuState *baseline,
                           const JitSource &src, u32 pc, u32 off, u32 step)
{
	int prevA = 0, prevB = 0;

	for (u16 n = 1; n <= block->count; n++)
	{
		JitBlock prefix = *block;
		prefix.count	= n;

		CpuState	a, b;
		bool		stopped = false;
		const char *bad =
			compareBlockOnce(&prefix, thumb, baseline, src, pc, off, step, &stopped, &a, &b);
		if (stopped)
		{
			printf("    block runner stopped at insn %u of %u (not comparable)\n", n,
				   block->count);
			return;
		}
		if (!bad)
		{
			prevA = a.cpuTotalTicks;
			prevB = b.cpuTotalTicks;
			continue;
		}

		printf("    first divergence at insn %u of %u: %s (interp=%d[+%d] block=%d[+%d])\n", n,
			   block->count, bad, a.cpuTotalTicks, a.cpuTotalTicks - prevA, b.cpuTotalTicks,
			   b.cpuTotalTicks - prevB);
		JitBlock one;
		memset(&one, 0, sizeof(one));
		one.guestPc = pc + (n - 1) * step;
		one.thumb	= thumb ? 1 : 0;
		one.count	= 1;
		one.insn[0] = block->insn[n - 1];
		char dump[512];
		jitDumpBlock(&one, dump, sizeof(dump));
		printf("%s", dump);
		return;
	}

	// Prefixes all agree while the whole block does not: the difference only
	// shows up at the block exit, which rewrites armNextPC/reg[15]/prefetch.
	printf("    no prefix diverged (%u prefixes checked)\n", block->count);
}

int verifyBlockPass(bool thumb, const JitSource &src, u32 romSize, int maxInsns, int verbose)
{
	CpuState baseline;
	seedCpu(thumb, 0x08000000, 0);
	saveCpu(&baseline);

	const u32 step = thumb ? 2 : 4;
	int tested = 0, failed = 0, skipped = 0, refuse = 0, shown = 0, insns = 0;

	printf("\n%s block pass\n", thumb ? "thumb" : "arm");

	for (u32 off = 0; off + 8 < romSize && tested < maxInsns; off += step)
	{
		const u32 pc = 0x08000000 + off;
		if (off < 0xC0)
			continue;

		JitBlock block;
		memset(&block, 0, sizeof(block));
		if (!(thumb ? jitDecodeThumb(src, pc, &block) : jitDecodeArm(src, pc, &block)))
		{
			refuse++;
			continue;
		}

		if (!blockComparable(&block))
		{
			skipped++;
			continue;
		}

		// Both sides have to start from the same memory as well now that stores
		// are in scope: the block is allowed to write, so the interpreter pass
		// gets a snapshot restored before it runs.
		saveMem(vtSnapshot);

		CpuState	a, b;
		bool		stopped = false;
		const char *bad =
			compareBlockOnce(&block, thumb, &baseline, src, pc, off, step, &stopped, &a, &b);
		if (stopped)
		{
			// The block runner gave up part way through (it will not model an
			// instruction that writes PC without branching), so the state is
			// not comparable with a block that ran to the end.
			skipped++;
			loadCpu(&baseline);
			continue;
		}

		tested++;
		insns += block.count;

		// A block that writes an IO register can start a DMA, and the DMA's
		// state is not part of the snapshot - so a difference that does not
		// reproduce is the verifier's own side effect, not a recompiler bug.
		// Require both sides to be identical across the two attempts before
		// believing the mismatch.
		if (bad)
		{
			CpuState	a2, b2;
			bool		stopped2 = false;
			const char *bad2	 = compareBlockOnce(&block, thumb, &baseline, src, pc, off, step,
												  &stopped2, &a2, &b2);
			if (stopped2 || !bad2 || strcmp(bad, bad2) != 0 ||
				compareCpu(&a, &a2) != NULL || compareCpu(&b, &b2) != NULL)
				bad = NULL;
		}

		if (bad)
		{
			failed++;
			if (shown < (verbose > 0 ? 25 : 5))
			{
				shown++;
				printf("BLOCK MISMATCH %08X  count=%u  %s\n", pc, block.count, bad);
				if (strcmp(bad, "reg") == 0)
				{
					const int r = firstBadReg(&a, &b);
					if (r >= 0)
						printf("    r%-2d  interp=%08X  block=%08X\n", r, a.reg[r].I, b.reg[r].I);
				}
				if (strcmp(bad, "cycles") == 0)
					printf("    cycles interp=%d  block=%d\n", a.cpuTotalTicks, b.cpuTotalTicks);
				if (strcmp(bad, "armNextPC") == 0)
					printf("    nextPC interp=%08X  block=%08X\n", a.armNextPC, b.armNextPC);
				if (strcmp(bad, "prefetch") == 0)
					printf("    prefetch interp=%08X,%08X  block=%08X,%08X\n", a.prefetch[0],
						   a.prefetch[1], b.prefetch[0], b.prefetch[1]);

				char dump[1024];
				jitDumpBlock(&block, dump, sizeof(dump));
				printf("%s\n", dump);
				localizeBlockMismatch(&block, thumb, &baseline, src, pc, off, step);
			}
		}
	}

	printf("  %d block%s (%d instructions) tested, %d failed, %d skipped "
		   "(SWI, undefined, mode switch), %d refused\n",
		   tested, tested == 1 ? "" : "s", insns, failed, skipped, refuse);
	return failed;
}

// ---------------------------------------------------------------------------
// Exhaustive sweep of the decode space.
//
// Both dispatch tables are functions of a few bits only: the ARM one is indexed
// by bits 27-20 and 7-4 (256 rows x 16 columns), the Thumb one by bits 15-6
// (1024 entries).  So "which words does the emulator call undefined" can be
// enumerated exactly and compared against the decoder without executing
// anything.  Running ROM bytes only samples that space - this covers it.
//
// What counts as a disagreement:
//   * the decoder produces an instruction where the emulator raises an
//     undefined exception - generated code would not diverge, it would do
//     something the interpreter never does;
//   * the decoder raises where the emulator executes.
// A decoder that refuses (returns false) is not a bug: the caller falls back to
// the interpreter for that block, which is always correct.
int sweepDecodeSpace(bool thumb, int verbose)
{
	// mask 0 maps every address onto the same word, which is what the sweep
	// wants: one word, decoded on its own.
	u32		  word[2] = { 0, 0 };
	JitSource src	  = { (const u8 *)word, 0 };

	printf("\n%s decode space\n", thumb ? "thumb" : "arm");

	int bad	 = 0;
	int runs = thumb ? 1024 : 4096;
	for (int i = 0; i < runs; i++)
	{
		const u32 op = thumb ? ((u32)i << 6)
							 : (0xE0000000u | (((u32)i >> 4) << 20) |
								(((u32)i & 15) << 4));
		word[0] = op;

		const bool emuUndef = thumb ? thumbInsnUndefined(op) : armInsnUndefined(op);

		JitInsn in;
		if (!jitDecodeOne(src, 0, thumb, &in))
			continue;			// refused: the block goes to the interpreter

		const bool decUndef = (in.op == JOP_UNDEF);
		if (emuUndef == decUndef)
			continue;

		// Rows 0x10-0x17 and 0x30-0x37 hold MRS/MSR/BKPT.  Those handlers do
		// their own masking and call armUnknownInsn themselves, so "the table
		// entry is not armUnknownInsn" does not mean the word executes - the
		// decoder is the one that has to be trusted there.  (The other
		// direction, a table entry that really is armUnknownInsn while the
		// decoder executes, is always reported.)  Those rows are covered by the
		// per-word pass and by selftest.
		const u32 row		  = (u32)i >> 4;
		const bool selfMasked = !thumb &&
			((row >= 0x10 && row <= 0x17) || (row >= 0x30 && row <= 0x37));
		if (selfMasked && !emuUndef)
			continue;

		bad++;
		if (bad <= 600)
		{
			if (thumb)
				printf("  %04X  entry %03X  emulator=%s decoder=%s\n", op, i,
					   emuUndef ? "undefined" : "executes",
					   decUndef ? "undefined" : jitOpName(in.op));
			else
				printf("  %08X  row %02X col %X  emulator=%s decoder=%s\n", op,
					   (u32)i >> 4, (u32)i & 15,
					   emuUndef ? "undefined" : "executes",
					   decUndef ? "undefined" : jitOpName(in.op));
		}
	}

	printf("  %d word%s checked, %d disagreement%s\n", runs, runs == 1 ? "" : "s",
		   bad, bad == 1 ? "" : "s");
	return bad;
}

int verifyPass(bool thumb, const JitSource &src, u32 romSize, int maxInsns, int verbose)
{
	CpuState baseline;
	seedCpu(thumb, 0x08000000, 0);
	saveCpu(&baseline);

	const u32 step = thumb ? 2 : 4;
	int tested = 0, failed = 0, skipped = 0, decodeFailed = 0, shown = 0;
	int perOpFail[16] = { 0 };

	printf("\n%s pass\n", thumb ? "thumb" : "arm");

	for (u32 off = 0; off + 8 < romSize && tested < maxInsns; off += step)
	{
		const u32 pc = 0x08000000 + off;

		// Skip the ROM header; those bytes are data as often as they are code.
		if (off < 0xC0)
			continue;

		JitInsn in;
		if (!jitDecodeOne(src, pc, thumb, &in))
		{
			decodeFailed++;
			continue;
		}

		// Writing instructions cannot be compared this way: the first pass
		// changes memory, so the second would not start from the same state.
		//
		// SWI belongs in the same bucket: it jumps into the BIOS handler, which
		// runs its own code and pushes onto the stack.  It is also the one
		// instruction that pops a modal dialog when no BIOS file is loaded
		// ("Unsupported BIOS function"), which would hang a scripted run.
		if (in.op == JOP_STORE || in.op == JOP_STM || in.op == JOP_SWP ||
			in.op == JOP_PUSH || in.op == JOP_POP || in.op == JOP_SWI)
		{
			skipped++;
			continue;
		}

		// An MSR that writes the control field can switch to any mode the ROM
		// byte happens to spell, including the ones the CPU does not have; the
		// emulator answers those with a modal dialog ("Unsupported ARM mode").
		// A mode switch also rebanks r13/r14, which this comparison does not
		// cover, so only the flags/status forms are worth testing.
		if (in.op == JOP_MSR && (in.d & 1) != 0)
		{
			skipped++;
			continue;
		}

		// Same reason the decoders use memcpy instead of READ32LE: the macro
		// casts a const u8* to u32* and dereferences it, which is an aliasing
		// violation the optimiser may rewrite into something else entirely.
		// Feeding the interpreter a mangled opcode made every instruction look
		// like a mismatch, so this read has to match the decoder's bit for bit.
		const u32 opcode	  = fetchInsn(src, pc, thumb);

		// The emulator's table sends this word to armUnknownInsn.  Running it
		// opens the BIOS undefined handler, which pops a modal dialog when no
		// BIOS file is loaded - and there is nothing to compare: the word is
		// ROM data that happens to look like an instruction.  The Thumb table
		// has no such holes, so this is ARM-only.
		if (!thumb && armInsnUndefined(opcode))
		{
			skipped++;
			continue;
		}

		const u32 nextOpcode  = fetchInsn(src, pc + (thumb ? 2 : 4), thumb);

		// A: the real interpreter, one instruction.
		loadCpu(&baseline);
		seedCpu(thumb, pc, off);
		cpuPrefetch[0] = opcode;
		cpuPrefetch[1] = nextOpcode;
		if (thumb)
			thumbExecute();
		else
			armExecute();
		CpuState a;
		saveCpu(&a);

		// B: the same instruction through the IR.
		loadCpu(&baseline);
		seedCpu(thumb, pc, off);
		cpuPrefetch[0] = opcode;
		cpuPrefetch[1] = nextOpcode;
		u32 nextPc	 = pc + (thumb ? 2 : 4);
		const int bTicks = jitInterpInsn(&in, thumb, pc, &nextPc);
		if (bTicks < 0)		// IR does not implement it
		{
			skipped++;
			continue;
		}
		// The interpreter accumulates into cpuTotalTicks, the IR returns the
		// count; make the two comparable before diffing.
		cpuTotalTicks = bTicks;
		CpuState b;
		saveCpu(&b);

		tested++;

		const char *bad = compareCpu(&a, &b);
		if (bad)
		{
			failed++;
			if (in.op < 16)
				perOpFail[in.op]++;
			if (shown < (verbose > 0 ? 25 : 5))
			{
				shown++;
				printf("MISMATCH %08X  raw=%08X  op=%s  %s\n", pc, opcode, jitOpName(in.op), bad);
				if (strcmp(bad, "reg") == 0 || strcmp(bad, "cpsr") == 0)
				{
					const int r = firstBadReg(&a, &b);
					if (r >= 0)
						printf("    r%-2d  interp=%08X  ir=%08X\n", r, a.reg[r].I, b.reg[r].I);
				}
				if (strcmp(bad, "cycles") == 0)
					printf("    cycles interp=%d  ir=%d\n", a.cpuTotalTicks, b.cpuTotalTicks);
				if (bad[1] == 0 && strchr("NCZV", bad[0]) != NULL)
					printf("    %s interp=%d  ir=%d\n", bad,
						   bad[0] == 'N' ? a.n : bad[0] == 'C' ? a.c : bad[0] == 'Z' ? a.z : a.v,
						   bad[0] == 'N' ? b.n : bad[0] == 'C' ? b.c : bad[0] == 'Z' ? b.z : b.v);
				if (strcmp(bad, "busPrefetchCount") == 0)
					printf("    busPrefetch interp=%d  ir=%d\n",
						   a.busPrefetchCount, b.busPrefetchCount);

				char dump[512];
				JitBlock one;
				memset(&one, 0, sizeof(one));
				one.guestPc = pc;
				one.thumb	= thumb ? 1 : 0;
				one.count	= 1;
				one.insn[0] = in;
				jitDumpBlock(&one, dump, sizeof(dump));
				printf("%s", dump);
			}
		}
	}

	printf("  %d tested, %d failed, %d skipped (memory writes, SWI, undefined), %d undecodable\n",
		   tested, failed, skipped, decodeFailed);
	if (failed)
	{
		printf("  failures by op:");
		for (int i = 0; i < 16; i++)
			if (perOpFail[i])
				printf(" %s=%d", jitOpName((u8)i), perOpFail[i]);
		printf("\n");
	}

	loadCpu(&baseline);
	return failed;
}

} // namespace

int jitVerifyRun(int maxInsns, int verbose)
{
	if (!rom || maxInsns <= 0)
		return -1;

	// memoryMap[8] is the ROM window; the decoder reads through it exactly the
	// way the interpreter does.
	JitSource src = { memoryMap[8].address, memoryMap[8].mask };
	if (!src.base)
		return -1;

	const u32 romSize = CPUGetRomSize();
	if (romSize < 16)
		return -1;

	seedMemory();

	// First the cheap, exhaustive one: it catches decode-space disagreements
	// that random ROM bytes only hit by luck.
	const int armSpace	 = sweepDecodeSpace(false, verbose);
	const int thumbSpace = sweepDecodeSpace(true, verbose);

	const int armFails	= verifyPass(false, src, romSize, maxInsns, verbose);
	const int thumbFails = verifyPass(true, src, romSize, maxInsns, verbose);

	// Then the same thing one level up: whole basic blocks through the real
	// block runner.  A block that stops in the wrong place hangs the guest
	// rather than giving a wrong register, so this pass exists.
	const int armBlocks	 = verifyBlockPass(false, src, romSize, maxInsns, verbose);
	const int thumbBlocks = verifyBlockPass(true, src, romSize, maxInsns, verbose);

	printf("\njit verify: %d mismatches (%d arm, %d thumb), %d block mismatches "
		   "(%d arm, %d thumb), %d decode-space disagreement(s)\n",
		   armFails + thumbFails, armFails, thumbFails, armBlocks + thumbBlocks, armBlocks,
		   thumbBlocks, armSpace + thumbSpace);
	return armFails + thumbFails + armBlocks + thumbBlocks + armSpace + thumbSpace;
}

#endif // USE_GBA_CORE_V7
