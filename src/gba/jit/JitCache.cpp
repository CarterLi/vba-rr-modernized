#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "JitCache.h"
#include "JitIr.h"

#include "../GBA.h"
#include "../GBAGlobals.h"
#include "../GBAinline.h"
#include "../../common/System.h"
#include "../../common/vbalua.h"

// Off by default: -jit on the command line turns it on.  Defined outside the
// V7 guard because the command line parser in the frontend refers to it for
// both cores.
bool jitEnabled = false;

// Set by the V7 block runner when the next instruction has to be executed by
// CPULoop's inline interpreter (see jitExecute() below).  CPULoop clears it
// right after it has run that one instruction.
bool jitForceInterp = false;

#ifdef USE_GBA_CORE_V7
// V7 has no prefetch buffer and no cpuTotalTicks, so only these two of its
// locals are needed here.  Neither is in a header (V7/GBA.cpp defines them).
extern bool8  holdState;
extern int32 *extCpuNextEvent;
extern int32  memoryWaitFetch[16];
extern int32  memoryWaitFetch32[16];
#else
#include "../V8/GBACpu.h"

extern u32 mastercode;
#endif

namespace {

// --- debug PC trace --------------------------------------------------------
// Both this file and the interpreter loops call jitTracePc(); the file is only
// open when VBA_JIT_PCTRACE named one, so the cost otherwise is one load and a
// not-taken branch.  Compiled out unless JIT_PC_TRACE is on (JitCache.h).
#if JIT_PC_TRACE
FILE *jitPcTrace	 = NULL;
u32	  jitPcTraceLeft = 1000000;
#endif

// --- the cache -------------------------------------------------------------
//
// Direct-mapped and fixed size: a miss simply decodes a new block over the old
// one, so there is no allocator, no free list and no eviction policy to get
// wrong.  A 64-instruction block is 1032 bytes, so 1024 entries cost about
// 1 MB of BSS.  Nothing in here is core specific: the decoder, the page table
// and the block layout are shared, only the cycle accounting in runBlock()
// differs.
// ---------------------------------------------------------------------------

#define JIT_CACHE_BLOCKS 1024

struct JitCacheEntry
{
	u32		 tag;		// guestPc | thumb; 0xFFFFFFFF means "nothing here"
	u16		 version;	// page version the block was decoded at
	u16		 spare;
	JitBlock block;
};

JitCacheEntry jitCache[JIT_CACHE_BLOCKS];

// ---------------------------------------------------------------------------
// Code pages
//
// EWRAM (0x02000000..0x0203FFFF) is 256 pages of 1 KB, IWRAM
// (0x03000000..0x03007FFF) another 32.  ROM and BIOS are read-only, so a block
// that lives there can never go stale and needs no version at all.
//
// A page gets a version bump only once a block has actually been compiled in
// it (`jitPageHasCode`), which keeps the store path down to one byte load in
// the common case of "this is game data, not code".
// ---------------------------------------------------------------------------

#define JIT_PAGES 288

u16 jitPageVersion[JIT_PAGES];
u8	jitPageHasCode[JIT_PAGES];

JitStats jitStats;

// V8 only: how many interpreter stretches still have to run before a block may
// be decoded again.  Set by runBlock() when an instruction wrote PC without
// being a branch (see the comment there) - armNextPC stays one instruction
// behind the prefetch queue until the detour has played out, so nothing may be
// decoded from it in the meantime.
int jitDetour = 0;

void jitReportStats()
{
	if (!jitEnabled)
		return;

	// A GUI process has no console, so stderr goes nowhere: the same line is
	// also written to jit.log next to the emulator.  That is the only way to
	// tell a working block path from one that silently falls back on every
	// instruction, which is exactly what a HashTrace comparison cannot show.
	const char *line =
		"jit: %u blocks decoded, %u runs, %u cache hits, %u page invalidations, "
		"%llu instructions, %u bad\n";

	fprintf(stderr, line, jitStats.blocksDecoded, jitStats.blocksRun, jitStats.cacheHits,
			jitStats.pageInvalidations, (unsigned long long)jitStats.insns, jitStats.badInsns);

	if (FILE *log = fopen("jit.log", "w"))
	{
		fprintf(log, line, jitStats.blocksDecoded, jitStats.blocksRun, jitStats.cacheHits,
				jitStats.pageInvalidations, (unsigned long long)jitStats.insns, jitStats.badInsns);
		fclose(log);
	}
}

inline int pageIndex(u32 address)
{
	if ((address & 0x0F000000) == 0x02000000)
		return (int)((address >> 10) & 0xFF);
	if ((address & 0x0F000000) == 0x03000000)
		return 256 + (int)((address >> 10) & 0x1F);
	return -1;
}

inline u32 cacheIndex(u32 pc)
{
	return ((pc >> 2) ^ (pc >> 12)) & (JIT_CACHE_BLOCKS - 1);
}

// Where the decoder reads guest code from.  Mirrors the interpreter's
// memoryMap[] lookup, which is also what CPUReadMemoryQuick() uses for the
// prefetch words.
JitSource sourceFor(u32 pc)
{
	JitSource src;
	src.base = memoryMap[(pc >> 24) & 0xFF].address;
	src.mask = memoryMap[(pc >> 24) & 0xFF].mask;
	return src;
}

JitBlock *getBlock(u32 pc, bool thumb)
{
	const u32 tag	  = (pc & 0xFFFFFFFEu) | (thumb ? 1u : 0u);
	JitCacheEntry *e  = &jitCache[cacheIndex(pc)];
	const int	  page = pageIndex(pc);
	const u16	  ver  = (page >= 0) ? jitPageVersion[page] : 0;

	if (e->tag == tag && e->version == ver)
	{
		jitStats.cacheHits++;
		return &e->block;
	}

	const JitSource src = sourceFor(pc);
	if (!src.base)
		return NULL;

	const bool ok = thumb ? jitDecodeThumb(src, pc, &e->block)
						  : jitDecodeArm(src, pc, &e->block);
	if (!ok)
	{
		e->tag = 0xFFFFFFFFu;	// do not hand out the previous block either
		return NULL;
	}

	e->tag	   = tag;
	e->version = ver;
	if (page >= 0)
		jitPageHasCode[page] = 1;
	jitStats.blocksDecoded++;
	return &e->block;
}

#ifdef USE_GBA_CORE_V7

// One basic block, instruction by instruction, exactly as one turn of CPULoop's
// inline interpreter would run them.
//
// Returns the cycles executed.  `limit` is the number of cycles left before
// the next event; the block stops after the instruction that reaches it, the
// same place the interpreter's `cpuNextEvent -= clockTicks` test would.
//
// *needInterp is set when an instruction has to be executed by the inline
// interpreter instead (see JIT_NEED_INTERP): the block stops *before* it, so
// armNextPC still points at it and CPULoop can run exactly that one
// instruction with its own clockTicks in place.  That is what keeps
// CPUCheckDMA() and CPU_BREAK_LOOP_2() - the only two readers of the
// interpreter's locals - seeing the values they expect.
int runBlock(const JitBlock *b, s32 limit, bool *needInterp)
{
	*needInterp = false;

	const bool thumb = b->thumb != 0;
	u32		   pc	 = b->guestPc;
	s32		   total = 0;

	for (u16 i = 0; i < b->count; i++)
	{
#if JIT_PC_TRACE
		jitTracePc(pc);
#endif

		u32		  nextPc = 0;
		const int ticks	 = jitInterpInsn(&b->insn[i], thumb, pc, &nextPc);

		if (ticks == JIT_NEED_INTERP)
		{
			*needInterp = true;
			break;
		}

		if (ticks < 0)
		{
			// The decoder only emits ops this interpreter implements, and each
			// of them returns a count >= 0, so this is a bug.  Charge the
			// interpreter's default rather than add a negative number to the
			// clock, and let the counter in the stats expose it.
			//
			// Zero is legitimate and must not be counted: with WAITCNT's
			// prefetch bit set GBA.cpp decrements memoryWaitFetch[8], and
			// thumbCycles[opcode >> 8] is 0 for plenty of Thumb ops, so a
			// Thumb instruction fetched from ROM can cost exactly 0 cycles.
			// Treating that as "bad" charged an extra cycle each time and
			// drifted the clock away from the interpreter's.
			jitStats.badInsns++;
			total += 1 + (thumb ? (s32)memoryWaitFetch[(pc >> 24) & 15]
								: (s32)memoryWaitFetch32[(pc >> 24) & 15]);
		}
		else
			total += ticks;

		jitStats.insns++;
		pc = nextPc;

		// V7's loop fetches from armNextPC but derives the *next* armNextPC from
		// reg[15], so an instruction that writes reg[15] without being one of
		// the PC-write forms (a store whose base is PC and writes back, an MRS
		// to PC) sends the interpreter on a one-instruction detour: the next
		// instruction still comes from pc+4, and only the one after that jumps.
		// A block cannot express that, so stop here, leave the state exactly as
		// the instruction left it, and let the next jitExecute() hand the next
		// instruction to the interpreter (jitForceInterp is set by the caller).
		if (reg[15].I != pc + (armState ? 4 : 2))
		{
			*needInterp = true;
			return total;
		}

		// CPULoop tests these after every instruction, so a block has to stop
		// at exactly the same boundaries: an IRQ that lands one instruction
		// late is a desync.
		if (total >= limit || holdState)
			break;
		if ((!armState) != thumb)
			break;
	}

	// Leave the state the way the inline interpreter's next turn expects to
	// find it: armNextPC is the instruction to run next and reg[15] is that
	// plus 4/2 - which is precisely what the loop body does at its top before
	// it dispatches.  No prefetch refill: V7 has no prefetch buffer.  When the
	// block stopped on JIT_NEED_INTERP, `pc` is still that instruction, so this
	// points the interpreter at the right one.
	//
	// The detour case returned above without reaching this: there reg[15] is
	// deliberately *not* pc+4, and overwriting it would lose the jump the
	// interpreter is about to take.
	armNextPC = pc;
	reg[15].I = pc + (armState ? 4 : 2);
	return total;
}

#else

// One interpreter stretch, plus the bookkeeping for a pending "write to PC"
// detour.  Any stretch is correct here - it *is* the interpreter - this only
// decides when the recompiler may decode from armNextPC again: a stretch that
// executed at least one instruction counts as one of the two the detour needs.
int interpStretch()
{
	const s32 before = cpuTotalTicks;
	const int r		 = armState ? armExecute() : thumbExecute();
	if (jitDetour > 0 && cpuTotalTicks != before)
		jitDetour--;
	return r;
}

// One basic block, instruction by instruction, with the same accounting as one
// turn of armExecute()/thumbExecute().
int runBlock(const JitBlock *b, s32 limit, bool *needInterp)
{
	const bool thumb = b->thumb != 0;
	u32		   pc	 = b->guestPc;

	if (needInterp)
		*needInterp = false;
	(void)limit;

	jitStats.insns += b->count;

	for (u16 i = 0; i < b->count; i++)
	{
#if JIT_PC_TRACE
		jitTracePc(pc);
#endif

		// The first line of the interpreters' loop body.  Everything else it
		// does before dispatching - the 0x08020000 hack, the busPrefetch mask,
		// armNextPC and reg[15] - is inside jitInterpInsn(), which was written
		// to be exactly one iteration of that loop.
		if (UNLIKELY(mastercode && mastercode == pc))
			CPUMasterCodeCheck();

		u32		  nextPc = 0;
		const int ticks	 = jitInterpInsn(&b->insn[i], thumb, pc, &nextPc);

		if (ticks <= 0)
		{
			// Unreachable: the decoder only emits ops the IR interpreter
			// implements, and each of them returns a positive count (the
			// "handler left clockTicks at 0" default is applied inside it).
			// Fall back to that same default rather than add a negative number
			// to the clock.
			jitStats.badInsns++;
			cpuTotalTicks += 1 + (thumb ? codeTicksAccessSeq16(pc)
										: codeTicksAccessSeq32(pc));
		}
		else
			cpuTotalTicks += ticks;

		pc = nextPc;

		// A load/store or block transfer that uses PC as its base *and* writes
		// the base back leaves reg[15] holding something other than
		// "next + 4/2" (`stm r15!, {...}`, `str rX, [r15, #k]!`,
		// `mrs r15, cpsr`).  It is not a branch, so armNextPC still holds the
		// sequential address - but armExecute() rebuilds armNextPC from reg[15]
		// on its next turn and refills the prefetch queue from it, so the
		// interpreter executes two more sequential instructions and only then
		// jumps to (written value + 4).  A block cannot express that, so stop
		// here, leave reg[15] exactly as the instruction left it, and let
		// jitExecute() keep the recompiler out until the detour has played out.
		if (reg[15].I != pc + (armState ? 4u : 2u))
		{
			armNextPC = pc;
			if (armState)
			{
				ARM_PREFETCH;
			}
			else
			{
				THUMB_PREFETCH;
			}
			jitDetour = 2;
			// *needInterp doubles as "the block did not run to the end" for the
			// differential verifier: the state it leaves is mid-block, so it
			// cannot be compared against a fully executed block.
			if (needInterp)
				*needInterp = true;
			return 0;
		}

		// Both loops test these after every instruction, so a block has to
		// stop at exactly the same boundaries - an IRQ that lands one
		// instruction late is a desync.
		if (cpuTotalTicks >= cpuNextEvent || holdState || SWITicks)
		{
			if (i + 1 < b->count && needInterp)
				*needInterp = true;
			break;
		}
		if ((!armState) != thumb)
		{
			if (i + 1 < b->count && needInterp)
				*needInterp = true;
			break;
		}
	}

	// Leave the state the way the next interpreter turn expects to find it:
	// armNextPC is the instruction to run next, reg[15] is that plus 4/2, and
	// cpuPrefetch[] holds the two words ARM_PREFETCH / THUMB_PREFETCH would
	// have loaded.  (A branch handler leaves exactly this, so falling back to
	// the interpreter after a block is indistinguishable from a branch.)
	//
	// Refilling costs two quick reads per block and is what makes the block
	// and instruction paths interoperable; the alternative - trusting whatever
	// the last block left there - breaks the first fallback after a page
	// invalidation.
	armNextPC = pc;
	if (armState)
	{
		reg[15].I = armNextPC + 4;
		ARM_PREFETCH;
	}
	else
	{
		reg[15].I = armNextPC + 2;
		THUMB_PREFETCH;
	}
	return 0;
}

#endif // USE_GBA_CORE_V7

inline bool jitActive()
{
	if (!jitEnabled || systemDebug != 0)
		return false;
	// A Lua EXEC hook has to see every instruction, and it can be installed
	// while the emulator is running, so test it per call rather than deciding
	// at block-compile time.
	if (luaMemHookActive[LUAMEMHOOK_EXEC] != 0)
		return false;
	return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool jitAvailable()
{
	return true;
}

void jitSetEnabled(bool on)
{
	if (on == jitEnabled)
		return;
	jitEnabled = on;
	jitForceInterp = false;

	if (on)
	{
		jitResetCache();

		static bool registered = false;
		if (!registered)
		{
			registered = true;
			atexit(jitReportStats);
		}
	}
}

void jitResetCache()
{
	for (int i = 0; i < JIT_CACHE_BLOCKS; i++)
		jitCache[i].tag = 0xFFFFFFFFu;

	// The version counters are deliberately *not* reset: they only ever have
	// to differ from the version a cached block recorded, and every tag is
	// gone.  Wrapping is harmless for the same reason.
	memset(jitPageHasCode, 0, sizeof(jitPageHasCode));
	memset(&jitStats, 0, sizeof(jitStats));

	// A pending pipeline detour belongs to the instruction stream that is
	// being thrown away.
	jitDetour = 0;
}

void jitCodeWritten(u32 address, u32 size)
{
	int page = pageIndex(address);
	if (page >= 0 && jitPageHasCode[page])
	{
		jitPageVersion[page]++;
		jitStats.pageInvalidations++;
	}

	// A 4-byte store can straddle a page boundary; do not assume it cannot.
	if (size > 1)
	{
		const int last = pageIndex(address + size - 1);
		if (last >= 0 && last != page && jitPageHasCode[last])
		{
			jitPageVersion[last]++;
			jitStats.pageInvalidations++;
		}
	}
}

void jitGetStats(JitStats *out)
{
	*out = jitStats;
}

void jitTracePc(u32 pc)
{
#if JIT_PC_TRACE
	if (!jitPcTrace && jitPcTraceLeft)
	{
		// Opened on first use so the interpreter path traces too, not just
		// -jit runs.
		const char *path = getenv("VBA_JIT_PCTRACE");
		if (path && path[0])
			jitPcTrace = fopen(path, "wb");
		if (!jitPcTrace)
		{
			jitPcTraceLeft = 0;
			return;
		}
	}
	if (!jitPcTrace || !jitPcTraceLeft)
		return;
	--jitPcTraceLeft;
	fwrite(&pc, sizeof(pc), 1, jitPcTrace);
	{
		// V7 has no cpuTotalTicks; extClockTicks is CPULoop's local, which is
		// the nearest equivalent (it holds the previous instruction's count
		// while a block runs, which is enough to spot a divergence).
		const u32 lr = reg[14].I, sp = reg[13].I;
#ifdef USE_GBA_CORE_V7
		const u32 ticks = (u32)0;
#else
		const u32 ticks = (u32)cpuTotalTicks;
#endif
		fwrite(&lr, sizeof(lr), 1, jitPcTrace);
		fwrite(&sp, sizeof(sp), 1, jitPcTrace);
		fwrite(&ticks, sizeof(ticks), 1, jitPcTrace);
	}
#else
	(void)pc;
#endif
}

int jitRunBlockForTest(const JitBlock *block, bool *needInterp)
{
	bool dummy = false;
	return runBlock(block, 0x7FFFFFF, needInterp ? needInterp : &dummy);
}

int jitExecute()
{
#ifdef USE_GBA_CORE_V7
	if (!jitActive())
		return -1;

	// The block runner works off CPULoop's own event countdown.  Called from
	// anywhere else (the verifier does not use this entry point) there is no
	// budget to work with.
	if (!extCpuNextEvent || *extCpuNextEvent <= 0)
		return -1;

	// V7's loop reads the opcode from armNextPC and takes the *following*
	// address from reg[15], so a block is only well defined while
	// reg[15] == armNextPC + 4 (ARM) / + 2 (Thumb).  Anything that broke that
	// (the one-instruction detour after a write to PC through a writeback) is
	// resolved by running one instruction in the inline interpreter first.
	if (reg[15].I != armNextPC + (armState ? 4u : 2u))
		return -1;

	s32	 budget = *extCpuNextEvent;
	s32	 total	= 0;
	bool any	= false;

	for (;;)
	{
		const bool thumb = (armState == false);
		JitBlock *b		 = getBlock(armNextPC, thumb);

		if (!b)
		{
			// The block could not be decoded (BKPT, "^" transfers, an MSR that
			// writes CPSR).  Let the inline interpreter run this stretch; it
			// stops at the next event, which is what this loop would have done
			// too.  Nothing has been executed yet, so report "no ticks" only if
			// nothing was.
			return any ? total : -1;
		}

		jitStats.blocksRun++;

		bool needInterp = false;
		total += runBlock(b, budget - total, &needInterp);
		any = true;

		if (needInterp)
		{
			jitForceInterp = true;
			return total;
		}
		if (total >= budget || holdState)
			return total;

		// BX inside the block switched instruction set: the other mode owns the
		// next instruction.
		if ((!armState) != thumb)
			return total;
	}
#else
	if (!jitActive())
		return armState ? armExecute() : thumbExecute();

	// A block stopped on a "write to PC without a branch" detour: armNextPC is
	// one instruction behind the prefetch queue until the interpreter has run
	// the two instructions the queue still holds, so nothing may be decoded
	// from it yet.
	if (jitDetour > 0)
		return interpStretch();

	for (;;)
	{
		const bool thumb = (armState == false);
		JitBlock *b		 = getBlock(armNextPC, thumb);

		if (!b)
		{
			// The block could not be decoded (BKPT and a few encodings the
			// decoder refuses on purpose).  Let the interpreter run this
			// stretch; it stops at the next event, which is what this loop
			// would have done too.
			return interpStretch();
		}

		jitStats.blocksRun++;
		runBlock(b, 0, NULL);

		// runBlock() bailed out on the detour described above.
		if (jitDetour > 0)
			return interpStretch();

		if (cpuTotalTicks >= cpuNextEvent || holdState || SWITicks)
			return 1;

		// BX inside the block switched instruction set: the other loop owns
		// the next instruction.
		if ((!armState) != thumb)
			return 1;
	}
#endif
}
