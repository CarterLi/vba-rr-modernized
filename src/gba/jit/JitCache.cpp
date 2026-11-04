#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>

#include "JitCache.h"
#include "JitEmit.h"
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
FILE *jitPcTrace	  = NULL;
u32	  jitPcTraceLeft  = 1000000;
// VBA_JIT_PCTRACE_SKIP: instructions to pass over before the window starts, so
// the same 16 MB window can be slid over a later part of a long recording.
u32	  jitPcTraceSkip  = 0;
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

#define JIT_CACHE_BLOCKS 4096

struct JitCacheEntry
{
	u32		 tag;		// guestPc | thumb; 0xFFFFFFFF means "nothing here"
	u16		 version;	// page version the block was decoded at
	u16		 codeSlot;	// entry in jitCodeCache holding this block's machine code
	u32		 codeStamp;	// jitCodeCache[codeSlot].stamp when it was attached
	JitBlock block;
};

JitCacheEntry jitCache[JIT_CACHE_BLOCKS];

// ---------------------------------------------------------------------------
// Compiled-code cache
//
// Decoding a block is cheap (~50 ns); compiling one is three orders of
// magnitude more expensive.  The decoded-block cache above is small and
// direct-mapped, and a page invalidation drops every block on that page, so the
// same guest code is re-decoded over and over - 1.3 million times for 1200
// frames of Super Mario Advance.  Compiling on every one of those made the
// recompiler 12x slower than the interpreter.
//
// So machine code lives in its own cache, keyed by address *and by the contents
// of the decoded block*: a re-decode that produces the same instructions reuses
// the code that is already there, and only a genuinely different instruction
// stream pays for a compilation.  Entries are owned here, never by the block
// cache; a block entry only remembers (slot, stamp) and checks the stamp before
// using the code, which stays valid as long as that slot has not been reused.
// ---------------------------------------------------------------------------

#define JIT_CODE_ENTRIES 16384

struct JitCodeEntry
{
	u32			  tag;		// guestPc | thumb; 0xFFFFFFFF means empty
	u32			  hash;		// of the decoded instructions
	u32			  stamp;	// unique per insert
	JitCompiledBlock code;
};

JitCodeEntry jitCodeCache[JIT_CODE_ENTRIES];
u32			 jitCodeStamp = 1;

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
		"%llu IR instructions, %u bad, %u native blocks, %u compiles, %u code hits, "
		"%llu native instructions, %u chain links, %llu ms compiling\n";

	fprintf(stderr, line, jitStats.blocksDecoded, jitStats.blocksRun, jitStats.cacheHits,
			jitStats.pageInvalidations, (unsigned long long)jitStats.insns, jitStats.badInsns,
			jitStats.nativeBlocks, jitStats.nativeCompiles, jitStats.codeHits,
			(unsigned long long)jitStats.nativeInsns, jitStats.chainLinks,
			(unsigned long long)(jitStats.compileTicks / 3000000));

	if (FILE *log = fopen("jit.log", "w"))
	{
		fprintf(log, line, jitStats.blocksDecoded, jitStats.blocksRun, jitStats.cacheHits,
				jitStats.pageInvalidations, (unsigned long long)jitStats.insns,
				jitStats.badInsns, jitStats.nativeBlocks, jitStats.nativeCompiles,
			jitStats.codeHits, (unsigned long long)jitStats.nativeInsns,
			jitStats.chainLinks, (unsigned long long)(jitStats.compileTicks / 3000000));
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

inline u32 codeIndex(u32 pc)
{
	// A different mix from cacheIndex() on purpose: the two tables should not
	// collide in lockstep.
	return ((pc * 2654435761u) >> 12) & (JIT_CODE_ENTRIES - 1);
}

// Hash of the decoded instructions.  Field by field rather than over the raw
// bytes: JitInsn has one padding byte that the decoders do not initialise, and
// an unstable hash would make every re-decode look like a new block.
u32 hashBlock(const JitBlock *b)
{
	u32 h = 2166136261u;
	h = (h ^ (u32)b->count) * 16777619u;
	h = (h ^ (u32)b->thumb) * 16777619u;
	for (u16 i = 0; i < b->count; i++)
	{
		const JitInsn &in = b->insn[i];
		h = (h ^ in.imm) * 16777619u;
		h = (h ^ (u32)in.flags) * 16777619u;
		h = (h ^ in.op) * 16777619u;
		h = (h ^ in.sub) * 16777619u;
		h = (h ^ in.cond) * 16777619u;
		h = (h ^ ((u32)in.d | ((u32)in.n << 8) | ((u32)in.m << 16) | ((u32)in.shift << 24))) *
			16777619u;
		h = (h ^ ((u32)in.flagMask | ((u32)in.fetchCycles << 8))) * 16777619u;
	}
	return h;
}

// Machine code for `block`, reusing what the code cache already holds for the
// same address and the same instructions.  Returns NULL when the backend
// refuses the block, and also while the block is still cold.
//
// Compiling costs ~10 us; running the same six instructions through the IR
// interpreter costs ~100 ns.  Half the blocks in a game are entered a handful
// of times and never again, so compiling on first sight is a 100x loss on them
// - hence the second-hit threshold below.
#define JIT_COMPILE_AFTER 2

struct JitHotEntry
{
	u32 tag;
	u32 hash;
	u8	count;
	u8	pad[3];
};

JitHotEntry jitHot[512];

// Defined below, after the code-cache helpers they use; nativeFor() needs it
// before releasing an entry whose code might be the target of a link.
void unlinkCodeSlot(u16 s);

const JitCompiledBlock *nativeFor(const JitBlock *block, u32 tag, u16 *slotOut, u32 *stampOut)
{
	const u32	   hash	 = hashBlock(block);
	JitCodeEntry *ce	 = &jitCodeCache[codeIndex(block->guestPc)];

	if (ce->tag == tag && ce->hash == hash && ce->stamp != 0 && ce->code.fn)
	{
		jitStats.codeHits++;
		*slotOut  = (u16)(ce - jitCodeCache);
		*stampOut = ce->stamp;
		return &ce->code;
	}

	// Cold?  Let the IR interpreter have it until it has been seen often
	// enough to pay for a compilation.
	{
		JitHotEntry *hot = &jitHot[codeIndex(block->guestPc) & 511];
		if (hot->tag != tag || hot->hash != hash)
		{
			hot->tag  = tag;
			hot->hash = hash;
			hot->count = 1;
		}
		else if (hot->count < 255)
			hot->count++;

		if (hot->count < JIT_COMPILE_AFTER)
		{
			*slotOut  = 0;
			*stampOut = 0;
			return NULL;
		}
	}

	// Any link may point into the code we are about to free.
	unlinkCodeSlot((u16)(ce - jitCodeCache));
	jitReleaseBlock(&ce->code);
	ce->tag	  = 0xFFFFFFFFu;
	ce->stamp = 0;

	if (!jitCompileBlock(block, &ce->code))
	{
		*slotOut  = 0;
		*stampOut = 0;
		return NULL;
	}

	ce->tag	  = tag;
	ce->hash  = hash;
	ce->stamp = jitCodeStamp++;
	ce->code.selfSlot = (u16)(ce - jitCodeCache);
	jitStats.nativeBlocks++;
	jitStats.nativeCompiles++;

	*slotOut  = (u16)(ce - jitCodeCache);
	*stampOut = ce->stamp;
	return &ce->code;
}

// ---------------------------------------------------------------------------
// Block linking
//
// A compiled block whose JITX_DONE exit leads to an already-compiled successor
// jumps straight into that successor's chain entry (just past its prologue),
// keeping rbx/r12/r13/r14 and the stack frame live across the whole chain.
// One native call then covers many guest blocks; the dispatcher only sees the
// final exit.
//
// The link is a plain 8-byte pointer in the source's JitChainSlot - no code is
// patched.  Links are only made to ROM addresses (0x08..0x0D): EWRAM/IWRAM
// pages get version-bumped on writes, and a link into stale code would be very
// hard to unwind cheaply.  ROM is read-only, so no per-link version check and
// no per-page unlink bookkeeping are needed.
//
// Unlinking has to be O(links touching the released slot), not O(all links).
// The first version walked every live link on each compile - 29 million
// iterations over 600 frames - and cost more than the chaining saved.  Each
// code entry now heads a doubly linked list of the links that point into it,
// and each chain slot remembers its own node, so both directions are O(1).
// ---------------------------------------------------------------------------

#define JIT_MAX_LINKS 8192
#define JIT_LINK_NONE 0xFFFF

struct JitLink
{
	u16	 srcSlot;
	u16	 srcIndex;
	u16	 tgtSlot;
	u16	 prev;		// neighbours in tgtSlot's list
	u16	 next;
	u16	 freeNext;
	bool used;
};

JitLink jitLinks[JIT_MAX_LINKS];
u16		jitLinkHead[JIT_CODE_ENTRIES];
u16		jitLinkFree = JIT_LINK_NONE;
bool	jitLinksReady = false;
// A/B switch, resolved once at startup so the hot path stays a byte test.
bool jitChainDisabled = getenv("VBA_JIT_NOCHAIN") != nullptr;

void linksReset()
{
	for (int i = 0; i < JIT_CODE_ENTRIES; i++)
	{
		jitLinkHead[i] = JIT_LINK_NONE;
		for (int k = 0; k < JIT_MAX_CHAIN; k++)
		{
			jitCodeCache[i].code.chain[k].target	= 0;
			jitCodeCache[i].code.chain[k].linkIndex = JIT_LINK_NONE;
		}
	}
	for (int i = 0; i < JIT_MAX_LINKS; i++)
	{
		jitLinks[i].used	  = false;
		jitLinks[i].freeNext = (u16)(i + 1);
	}
	jitLinks[JIT_MAX_LINKS - 1].freeNext = JIT_LINK_NONE;
	jitLinkFree	 = 0;
	jitLinksReady = true;
}

inline void linksInit()
{
	if (!jitLinksReady)
		linksReset();
}

u16 linkAlloc()
{
	if (jitLinkFree == JIT_LINK_NONE)
		return JIT_LINK_NONE;
	const u16 n = jitLinkFree;
	jitLinkFree = jitLinks[n].freeNext;
	jitLinks[n].used = true;
	return n;
}

void linkFree(u16 n)
{
	jitLinks[n].used = false;
	jitLinks[n].freeNext = jitLinkFree;
	jitLinkFree = n;
}

// Detach node n from its target list, clear the source slot and free the node.
void removeLink(u16 n)
{
	JitLink *l = &jitLinks[n];
	if (l->prev != JIT_LINK_NONE)
		jitLinks[l->prev].next = l->next;
	else
		jitLinkHead[l->tgtSlot] = l->next;
	if (l->next != JIT_LINK_NONE)
		jitLinks[l->next].prev = l->prev;

	JitChainSlot *s = &jitCodeCache[l->srcSlot].code.chain[l->srcIndex];
	if (s->linkIndex == n)
	{
		s->linkIndex = JIT_LINK_NONE;
		s->target	 = 0;
	}
	linkFree(n);
}

bool registerLink(u16 srcSlot, int k, u16 tgtSlot)
{
	linksInit();
	const u16 n = linkAlloc();
	if (n == JIT_LINK_NONE)
		return false;

	JitLink *l = &jitLinks[n];
	l->srcSlot	= srcSlot;
	l->srcIndex = (u16)k;
	l->tgtSlot	= tgtSlot;
	l->prev		= JIT_LINK_NONE;
	l->next		= jitLinkHead[tgtSlot];
	if (l->next != JIT_LINK_NONE)
		jitLinks[l->next].prev = n;
	jitLinkHead[tgtSlot] = n;
	jitCodeCache[srcSlot].code.chain[k].linkIndex = n;
	return true;
}

// Drop the links that touch code entry `s` - as a target (its code is about to
// be freed) or as a source (its slots are about to be reused).
void unlinkCodeSlot(u16 s)
{
	linksInit();
	u16 n = jitLinkHead[s];
	while (n != JIT_LINK_NONE)
	{
		const u16 next = jitLinks[n].next;
		removeLink(n);
		n = next;
	}
	jitLinkHead[s] = JIT_LINK_NONE;

	JitCompiledBlock *cb = &jitCodeCache[s].code;
	for (int k = 0; k < JIT_MAX_CHAIN; k++)
	{
		if (cb->chain[k].target == 0)
			continue;
		const u16 idx = cb->chain[k].linkIndex;
		if (idx != JIT_LINK_NONE && jitLinks[idx].used)
			removeLink(idx);
		cb->chain[k].target	= 0;
		cb->chain[k].linkIndex = JIT_LINK_NONE;
	}
}

// Called from runBlock() right after a native call returned JITX_DONE.
void tryLink(const JitCompiledBlock *code, const JitBlock *b, u32 nextPc)
{
	if (jitChainDisabled || !code || code->selfSlot == 0xFFFF || code->chainCount == 0)
		return;
	if (code->insnCount != b->count)	// an IR tail has to run in the runner
		return;

	const u32 region = nextPc >> 24;
	if (region < 0x08 || region > 0x0D)	// ROM only
		return;

	const u32 tag = (nextPc & 0xFFFFFFFEu) | (b->thumb ? 1u : 0u);
	int		  k	  = -1;
	for (int i = 0; i < code->chainCount; i++)
	{
		if (code->chain[i].tag == tag)
		{
			k = i;
			break;
		}
	}
	if (k < 0 || code->chain[k].target != 0)
		return;

	JitCacheEntry *succ = &jitCache[cacheIndex(nextPc)];
	const int	  page = pageIndex(nextPc);
	const u16	  ver  = (page >= 0) ? jitPageVersion[page] : 0;
	if (succ->tag != tag || succ->version != ver || succ->codeStamp == 0)
		return;
	if (jitCodeCache[succ->codeSlot].stamp != succ->codeStamp)
		return;

	JitCompiledBlock *tgt = &jitCodeCache[succ->codeSlot].code;
	if (!tgt->fn || !tgt->chainEntry || tgt->frameKind != code->frameKind)
		return;
	if (tgt->insnCount != succ->block.count)
		return;

	JitCompiledBlock *src = const_cast<JitCompiledBlock *>(code);
	if (!registerLink(src->selfSlot, k, succ->codeSlot))
		return;
	src->chain[k].target = (u64)(uintptr_t)tgt->chainEntry;
	jitStats.chainLinks++;
}

JitCacheEntry *getEntry(u32 pc, bool thumb, const JitCompiledBlock **codeOut)
{
	*codeOut		  = NULL;
	const u32 tag	  = (pc & 0xFFFFFFFEu) | (thumb ? 1u : 0u);
	JitCacheEntry *e  = &jitCache[cacheIndex(pc)];
	const int	  page = pageIndex(pc);
	const u16	  ver  = (page >= 0) ? jitPageVersion[page] : 0;

	if (!(e->tag == tag && e->version == ver))
	{
		const JitSource src = sourceFor(pc);
		if (!src.base)
		{
			e->tag = 0xFFFFFFFFu;
			e->version = ver;
			e->codeStamp = 0;
			return NULL;
		}

		const bool ok = thumb ? jitDecodeThumb(src, pc, &e->block)
							  : jitDecodeArm(src, pc, &e->block);
		if (!ok)
		{
			e->tag = 0xFFFFFFFFu;	// do not hand out the previous block either
			e->version = ver;
			e->codeStamp = 0;
			return NULL;
		}

		e->tag	   = tag;
		e->version = ver;
		e->codeSlot = 0;
		e->codeStamp = 0;
		if (page >= 0)
			jitPageHasCode[page] = 1;
		jitStats.blocksDecoded++;
	}
	else
		jitStats.cacheHits++;

#ifdef USE_GBA_CORE_V7
	// Attach the machine code, compiling it only when this address has not been
	// compiled with exactly these instructions before.
	if (e->codeStamp != 0 && jitCodeCache[e->codeSlot].stamp == e->codeStamp &&
		jitCodeCache[e->codeSlot].code.fn)
	{
		*codeOut = &jitCodeCache[e->codeSlot].code;
		return e;
	}

	if (jitNativeAvailable())
	{
		const u64 t0 = __rdtsc();
		*codeOut	 = nativeFor(&e->block, tag, &e->codeSlot, &e->codeStamp);
		jitStats.compileTicks += __rdtsc() - t0;
	}
#endif
	return e;
}

#ifdef USE_GBA_CORE_V7

#ifdef USE_GBA_CORE_V7

// ---------------------------------------------------------------------------
// Shadow check (VBA_JIT_SHADOW=1)
//
// Runs each store-free block twice from the same CPU state - once through the
// machine code, once through the IR interpreter - and stops at the first block
// whose result differs.  Blocks that write memory are skipped, because their
// side effects cannot be undone between the two runs.  This is the tool that
// caught the two bugs the block-level verifier cannot see: an exit stub that
// the normal path fell into, and a Thumb PUSH that was applied twice.
// ---------------------------------------------------------------------------

struct ShadowCpu
{
	reg_pair r[45];
	bool8	 n, c, z, v, armSt, hold;
	u32		 nextPc;
	s32		 mode;
};

int	jitShadowMode	= -1;
u32 jitShadowBlocks = 0;

bool shadowEnabled()
{
	if (jitShadowMode < 0)
	{
		const char *s = getenv("VBA_JIT_SHADOW");
		jitShadowMode = (s && s[0] && s[0] != '0') ? 1 : 0;
	}
	return jitShadowMode != 0;
}

bool blockWritesMemory(const JitBlock *b)
{
	for (u16 i = 0; i < b->count; i++)
	{
		const u8 op = b->insn[i].op;
		if (op == JOP_STORE || op == JOP_STM || op == JOP_PUSH || op == JOP_SWP)
			return true;
	}
	return false;
}

void shadowSave(ShadowCpu *s)
{
	memcpy(s->r, reg, sizeof(s->r));
	s->n	  = N_FLAG;
	s->c	  = C_FLAG;
	s->z	  = Z_FLAG;
	s->v	  = V_FLAG;
	s->armSt  = armState;
	s->hold	  = holdState;
	s->nextPc = armNextPC;
	s->mode	  = armMode;
}

void shadowLoad(const ShadowCpu *s)
{
	memcpy(reg, s->r, sizeof(s->r));
	N_FLAG	  = s->n;
	C_FLAG	  = s->c;
	Z_FLAG	  = s->z;
	V_FLAG	  = s->v;
	armState  = s->armSt;
	holdState = s->hold;
	armNextPC = s->nextPc;
	armMode	  = s->mode;
}

// Returns a description of the first difference, or NULL.
const char *shadowCompare(const ShadowCpu *a, const ShadowCpu *b, int ta, int tb)
{
	for (int i = 0; i < 18; i++)
		if (a->r[i].I != b->r[i].I)
			return "reg";
	if (a->n != b->n) return "N";
	if (a->c != b->c) return "C";
	if (a->z != b->z) return "Z";
	if (a->v != b->v) return "V";
	if (a->armSt != b->armSt) return "armState";
	if (a->nextPc != b->nextPc) return "armNextPC";
	if (ta != tb) return "cycles";
	return NULL;
}

void shadowReport(const JitBlock *b, const JitCompiledBlock *code, const ShadowCpu *want,
				  const ShadowCpu *got, int wantTicks, int gotTicks, const char *what)
{
	FILE *f = fopen("jit-shadow.txt", "w");
	if (!f)
		return;

	char dump[4096];
	dump[0] = 0;
	jitDumpBlock(b, dump, (int)sizeof(dump) - 1);

	fprintf(f, "shadow mismatch at block %08X (%s, native covers %u of %u): %s\n%s\n",
			b->guestPc, b->thumb ? "thumb" : "arm", code ? code->insnCount : 0, b->count,
			what, dump);
	fprintf(f, "  cycles native=%d ir=%d\n", gotTicks, wantTicks);
	for (int i = 0; i < 18; i++)
		if (want->r[i].I != got->r[i].I)
			fprintf(f, "  r%-2d ir=%08X native=%08X\n", i, want->r[i].I, got->r[i].I);
	fprintf(f, "  flags ir N%d C%d Z%d V%d  native N%d C%d Z%d V%d\n",
			want->n, want->c, want->z, want->v, got->n, got->c, got->z, got->v);
	fprintf(f, "  armNextPC ir=%08X native=%08X  armState ir=%d native=%d\n",
			want->nextPc, got->nextPc, want->armSt, got->armSt);
	fclose(f);
}

#endif // USE_GBA_CORE_V7

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
int runBlock(const JitCompiledBlock *code, const JitBlock *b, s32 limit, bool *needInterp)
{
	*needInterp = false;

	const bool thumb = b->thumb != 0;
	u32		   pc	 = b->guestPc;
	s32		   total = 0;
	u16		   start = 0;

	// Machine code first, for as many instructions as the backend accepted.
	if (code && code->fn)
	{
#ifdef USE_GBA_CORE_V7
		if (shadowEnabled() && code->insnCount == b->count && !blockWritesMemory(b))
		{
			ShadowCpu before, nativeSt, irSt;

			shadowSave(&before);
			{
				JitExecResult r1;
				r1.nextPc = b->guestPc;
				r1.status = 0;
				const int nativeTicks = code->fn(limit, &r1);
				armNextPC			  = r1.nextPc;
				reg[15].I			  = r1.nextPc + (armState ? 4u : 2u);
				shadowSave(&nativeSt);

				shadowLoad(&before);
				bool	  irNeed = false;
				const int irTicks = runBlock(NULL, b, limit, &irNeed);
				shadowSave(&irSt);

				if (const char *what = shadowCompare(&irSt, &nativeSt, irTicks, nativeTicks))
				{
					shadowReport(b, code, &irSt, &nativeSt, irTicks, nativeTicks, what);
					jitShadowMode = 0;	// one report is enough
				}
				jitShadowBlocks++;

				// Continue from the IR result so a mismatch cannot derail the
				// rest of the run.
				shadowLoad(&irSt);
				*needInterp = irNeed;
				jitStats.nativeInsns += code->insnCount;
				return irTicks;
			}
		}
#endif

		JitExecResult r;
		r.nextPc = b->guestPc;
		r.status = 0;

		total = code->fn(limit, &r);

#ifdef USE_GBA_CORE_V7
		// Lazy linking: the first time this block reaches a given successor we
		// come back here, so patch the exit for the next visit.  Skip it while
		// the shadow checker is running - it wants the two paths independent.
		if ((r.status & JITX_DONE) && jitShadowMode != 1)
			tryLink(code, b, r.nextPc);
#endif

		if (r.status & JITX_NEED_INTERP)
		{
			// The block stopped *in front of* nextPc (an IO store the
			// interpreter has to run, because CPUCheckDMA() rewrites CPULoop's
			// own event counter).  Leave the state the way the IR path's
			// break-before-the-instruction does.
			*needInterp = true;
			if (!(r.status & JITX_KEEP_PC))
			{
				armNextPC = r.nextPc;
				reg[15].I = r.nextPc + (armState ? 4u : 2u);
			}
			jitStats.nativeInsns += code->insnCount;
			return total;
		}

		if (r.status & JITX_BUDGET)
		{
			armNextPC = r.nextPc;
			reg[15].I = r.nextPc + (armState ? 4u : 2u);
			jitStats.nativeInsns += code->insnCount;
			return total;
		}

		// JITX_DONE: the compiled prefix ran to its end.
		pc	  = r.nextPc;
		start = code->insnCount;
		jitStats.nativeInsns += start;

		if (start >= b->count)
		{
			armNextPC = pc;
			reg[15].I = pc + (armState ? 4u : 2u);
			return total;
		}
	}

	for (u16 i = start; i < b->count; i++)
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
int runBlock(const JitCompiledBlock *code, const JitBlock *b, s32 limit, bool *needInterp)
{
	const bool thumb = b->thumb != 0;
	u32		   pc	 = b->guestPc;

	(void)code;		// no machine-code backend for V8 yet

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
	// Lua memory hooks have to see every read/write.  The generated fast path
	// for EWRAM/IWRAM reads bypasses CPURead*/CPUWrite* entirely, so while any
	// hook is installed the whole JIT stands down and the inline interpreter
	// (which calls the hook-aware helpers) takes over.  Tested per call because
	// scripts can install hooks at any time.
	for (int i = 0; i < LUAMEMHOOK_COUNT; i++)
		if (luaMemHookActive[i] != 0)
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
	linksReset();
	for (int i = 0; i < JIT_CACHE_BLOCKS; i++)
	{
		jitCache[i].tag		  = 0xFFFFFFFFu;
		jitCache[i].codeStamp = 0;
	}

	// Every compiled block belongs to the instruction stream that is being
	// thrown away (a reset or a state load changes memory contents).
	for (int i = 0; i < JIT_CODE_ENTRIES; i++)
	{
		jitReleaseBlock(&jitCodeCache[i].code);
		jitCodeCache[i].tag	  = 0xFFFFFFFFu;
		jitCodeCache[i].hash  = 0;
		jitCodeCache[i].stamp = 0;
		jitCodeCache[i].code.selfSlot = 0xFFFF;
	}

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
		const char *skip = getenv("VBA_JIT_PCTRACE_SKIP");
		if (skip && skip[0])
			jitPcTraceSkip = (u32)strtoul(skip, NULL, 10);
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
	if (jitPcTraceSkip)
	{
		--jitPcTraceSkip;
		return;
	}
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

#ifdef USE_GBA_CORE_V7
	// The verifier has to exercise the machine code, not just the IR
	// interpreter, so the block is compiled here for real.  The previous
	// scratch block is released first; the code is only ever entered from this
	// function, so nothing else can be pointing at it.
	static JitCompiledBlock scratch;
	(void)jitCompileBlock(block, &scratch);
	const int ticks = runBlock(&scratch, block, 0x7FFFFFF, needInterp ? needInterp : &dummy);
	jitReleaseBlock(&scratch);
	return ticks;
#else
	return runBlock(NULL, block, 0x7FFFFFF, needInterp ? needInterp : &dummy);
#endif
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

	s32 budget = *extCpuNextEvent;
	s32	 total	= 0;
	bool any	= false;

	for (;;)
	{
		const bool thumb = (armState == false);
		const JitCompiledBlock *native = NULL;
		JitCacheEntry *e = getEntry(armNextPC, thumb, &native);

		if (!e)
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
		total += runBlock(native, &e->block, budget - total, &needInterp);
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
		const JitCompiledBlock *native = NULL;
		JitCacheEntry *e = getEntry(armNextPC, thumb, &native);

		if (!e)
		{
			// The block could not be decoded (BKPT and a few encodings the
			// decoder refuses on purpose).  Let the interpreter run this
			// stretch; it stops at the next event, which is what this loop
			// would have done too.
			return interpStretch();
		}

		jitStats.blocksRun++;
		runBlock(native, &e->block, 0, NULL);

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
