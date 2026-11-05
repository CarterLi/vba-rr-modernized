// ARM64/AArch64 machine-code backend for the V7 core's IR.
//
// This is the AArch64 counterpart of JitEmit.cpp.  It consumes the same JitBlock
// and honours the same contract (see documentation/jit-a64-backend.md and
// documentation/jit-recompiler.md section 3):
//
//   * one JitBlock -> one native function, covering the longest prefix the
//     backend supports;
//   * every guest instruction charges its own cycles and checks the budget;
//   * reg[15], the flag bytes and armNextPC keep exactly the values the V7
//     inline interpreter would have left behind;
//   * anything this backend cannot express ends the prefix or returns
//     JITX_NEED_INTERP.  The failure mode is "slower", never "different".
//
// Register use (Windows ARM64 ABI):
//
//   x19  &reg[0]                  x25  &N_FLAG
//   w20  cycles executed so far  x26  &memoryWait[0]
//   w21  cycle budget            x27  &memoryMap[0].address
//   x22  JitExecResult*          x28  &gJitShiftCarry
//   w23  current memory address x29/x30  saved FP/LR
//   w24  writeback value
//   w0/w1 helper arguments, w0 helper result, x16 helper address
//   w3..w12 scratch (caller-saved, not live across a helper call)
//
// The frame is a fixed 96 bytes for every block, so a chained successor can
// jump straight past its prologue and reuse the predecessor's frame.

#include "JitEmit.h"

#include "JitCache.h"       // JIT_PC_TRACE
#include "JitIr.h"

#if defined(_M_ARM64) || defined(__aarch64__) || defined(VBA_JIT_A64_FORCE)
#define VBA_JIT_A64 1
#else
#define VBA_JIT_A64 0
#endif

#if VBA_JIT_A64 && defined(USE_GBA_CORE_V7)

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <asmjit/core.h>
#include <asmjit/core/error_handler.h>
#include <asmjit/core/jit_runtime.h>
#include <asmjit/a64.h>

#include "../GBAGlobals.h"     // reg[], N_FLAG..V_FLAG, armState, armNextPC
#include "../GBAinline.h"      // MemoryMap, CPUReadMemoryQuick

// MSVC's arm64_neon.h defines a function-like macro `mvn(src)` which is pulled
// in through <intrin.h> on an AArch64 target.  It collides with the emitter's
// mvn() method, so either undef it or use `orn dst, wzr, src` instead.  This
// backend uses orn and keeps the undef as a belt-and-braces measure.
#ifdef mvn
#undef mvn
#endif

// Declared here rather than by including GBA.h / V7/GBACpu.h: those pull in
// UPDATE_REG and the handler macros, and this file has to stay buildable as
// part of the standalone JIT sources.
extern bool8 holdState;

extern int32 memoryWait[16];
extern int32 memoryWait32[16];
extern int32 memoryWaitSeq[16];
extern int32 memoryWaitSeq32[16];
extern int32 memoryWaitFetch[16];
extern int32 memoryWaitFetch32[16];

extern u32  CPUReadMemory(u32 address);
extern u32  CPUReadHalfWord(u32 address);   // V7 returns it zero-extended
extern u16  CPUReadHalfWordSigned(u32 address);
extern u8   CPUReadByte(u32 address);
extern void CPUWriteMemory(u32 address, u32 value);
extern void CPUWriteHalfWord(u32 address, u16 value);
extern void CPUWriteByte(u32 address, u8 value);
extern void CPUUpdateCPSR();

// JitCache.cpp, JIT_PC_TRACE only.
extern void jitTracePc(u32 pc);

namespace {

using namespace asmjit;
namespace a = asmjit::a64;

static_assert(sizeof(MemoryMap) == 16, "MemoryMap layout is part of the A64 backend");
static_assert(offsetof(MemoryMap, mask) == 8, "memoryMap mask must be at +8");
static_assert(sizeof(reg_pair) == 4, "reg[r] is a 32-bit load at r*4");

// ---------------------------------------------------------------------------
// Register aliases
// ---------------------------------------------------------------------------
const a64::Gp XREG   = a64::x19;   // &reg[0]
const a64::Gp WCYC   = a64::w20;   // cycles
const a64::Gp WBUD   = a64::w21;   // budget
const a64::Gp XOUT   = a64::x22;   // JitExecResult*
const a64::Gp WADDR  = a64::w23;   // memory address
const a64::Gp WWB    = a64::w24;   // writeback value
const a64::Gp XFLAGS = a64::x25;   // &N_FLAG
const a64::Gp XWAIT  = a64::x26;   // &memoryWait[0]
const a64::Gp XMAP   = a64::x27;   // &memoryMap[0].address
const a64::Gp XCARRY = a64::x28;   // &gJitShiftCarry

// ---------------------------------------------------------------------------
// Link-time offsets.  A64 cannot add an arbitrary displacement to a
// register-indexed address, so all globals are reached from one of the four
// base registers above with a small immediate (or a register index).  Using
// real pointer differences instead of hardcoded 0/1/2/3 keeps this correct if
// GBAGlobals.cpp is ever reordered.
// ---------------------------------------------------------------------------
inline int32_t offFlags(const void *p)
{
	return (int32_t)((intptr_t)p - (intptr_t)&N_FLAG);
}

inline int32_t offWait(const void *p)
{
	return (int32_t)((intptr_t)p - (intptr_t)&memoryWait[0]);
}

inline a64::Mem fmem(const void *p)
{
	return a64::ptr(XFLAGS, offFlags(p));
}

inline a64::Mem gr(int r)
{
	return a64::ptr(XREG, r * 4);
}

inline a64::Mem wmem(const void *p)
{
	return a64::ptr(XWAIT, offWait(p));
}

struct Offsets
{
	bool checked;
	bool ok;
};

Offsets gOffsets = { false, false };

bool checkedOffsets()
{
	if (gOffsets.checked)
		return gOffsets.ok;
	gOffsets.checked = true;

	// ldrb/strb accept an unscaled 12-bit unsigned offset; ldr w accepts a
	// scaled one (multiple of four).  Only the globals the emitted code
	// actually addresses from XFLAGS are checked: armNextPC/holdState live in
	// a different translation unit and are owned by the block runner, so
	// requiring them to be within 4 KB would disable the backend for no
	// reason.
	const void *flags[] = { &N_FLAG, &C_FLAG, &Z_FLAG, &V_FLAG,
							&armState, &armMode };
	for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
	{
		const int32_t d = offFlags(flags[i]);
		if (d < 0 || d > 4095 || (flags[i] == &armMode && (d & 3)))
			return gOffsets.ok = false;
	}

	const int32 *tables[] = { memoryWait, memoryWait32, memoryWaitSeq,
							  memoryWaitSeq32, memoryWaitFetch, memoryWaitFetch32 };
	for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); t++)
	{
		const int32_t base = offWait(&tables[t][0]);
		if (base < 0 || (base & 3) || base / 4 > 4095)
			return gOffsets.ok = false;
		for (int i = 0; i < 16; i++)
		{
			const int32_t d = offWait(&tables[t][i]);
			if (d < 0 || d > 16380 || (d & 3))
				return gOffsets.ok = false;
		}
	}
	return gOffsets.ok = true;
}

#if JIT_PC_TRACE
bool jitTraceEnabled = getenv("VBA_JIT_PCTRACE") != nullptr;
#endif

// ---------------------------------------------------------------------------
// Immediate materialisation and helper calls
// ---------------------------------------------------------------------------

// A64 has no "load 64-bit immediate" instruction.  movz/movk is immune to
// ASLR and to the distance between the JIT code page and the static image.
void emitMovImm64(a64::Assembler *as, const a64::Gp &dst, uint64_t v)
{
	const uint32_t w0 = (uint32_t)(v & 0xFFFFu);
	const uint32_t w1 = (uint32_t)((v >> 16) & 0xFFFFu);
	const uint32_t w2 = (uint32_t)((v >> 32) & 0xFFFFu);
	const uint32_t w3 = (uint32_t)((v >> 48) & 0xFFFFu);
	as->movz(dst, Imm(w0));
	if (w1) as->movk(dst, Imm(w1), Imm(16));
	if (w2) as->movk(dst, Imm(w2), Imm(32));
	if (w3) as->movk(dst, Imm(w3), Imm(48));
}

// asmjit's mov(Gp, Imm) expands arbitrary constants into movz/movk/orr on its
// own (verified with the encoder probe).  add/sub/cmp immediates do NOT expand,
// so those go through addConst() below.
void emitMovImm32(a64::Assembler *as, const a64::Gp &dst, u32 v)
{
	as->mov(dst, Imm(v));
}

// Add a small constant to a 32-bit register.  A64 add/sub immediates are 12
// bits (optionally shifted by 12); anything larger is loaded into w3 first.
void addImm(a64::Assembler *as, const a64::Gp &dst, int32_t v)
{
	if (v == 0)
		return;
	const uint32_t abs = (uint32_t)(v < 0 ? -v : v);
	if (abs <= 4095)
	{
		if (v > 0) as->add(dst, dst, Imm(abs));
		else       as->sub(dst, dst, Imm(abs));
		return;
	}
	emitMovImm32(as, a64::w3, abs);
	if (v > 0) as->add(dst, dst, a64::w3);
	else       as->sub(dst, dst, a64::w3);
}

void callHelper(a64::Assembler *as, const void *fn)
{
	emitMovImm64(as, a64::x16, (uint64_t)(uintptr_t)fn);
	as->blr(a64::x16);
}

u32 jitReadQuick(u32 address) { return CPUReadMemoryQuick(address); }

// ---------------------------------------------------------------------------
// Shift-by-register helper.  This lives in JitEmit.cpp's anonymous namespace
// for the x64 build; the ARM64 build does not compile that TU, so it needs its
// own copy together with its implicit second return value.
// ---------------------------------------------------------------------------
u8 gJitShiftCarry = 0xFF;

enum ShiftKind
{
	SHK_LSL,        // ARM lsl Rd, Rm, Rs (raw < 32 guaranteed by the caller)
	SHK_LSR,
	SHK_ASR,
	SHK_LSL_T,      // Thumb format 4: 0 means "no shift", 32 means "shift out"
	SHK_LSR_T,
	SHK_ASR_T,
	SHK_ROR_T,      // Thumb ror Rd, Rs: a count of 0 or 32 still writes C
	SHK_ROR_V7      // V7's ARM "ror Rd, Rm, Rs": same, for any masked-to-0 count
};

u32 jitShiftReg(u32 v, u32 raw, u32 carryIn, u32 kind)
{
	(void)carryIn;
	gJitShiftCarry = 0xFF;

	switch (kind)
	{
	case SHK_LSL:
		if (raw == 0) return v;
		gJitShiftCarry = (u8)((v >> ((32 - raw) & 31)) & 1);
		return v << (raw & 31);
	case SHK_LSR:
		if (raw == 0) return v;
		gJitShiftCarry = (u8)((v >> ((raw - 1) & 31)) & 1);
		return v >> (raw & 31);
	case SHK_ASR:
		if (raw == 0) return v;
		gJitShiftCarry = (u8)(((u32)((s32)v >> ((raw - 1) & 31))) & 1);
		return (u32)((s32)v >> (raw & 31));

	case SHK_LSL_T:
		if (raw == 0) return v;
		if (raw < 32) { gJitShiftCarry = (u8)((v >> (32 - raw)) & 1); return v << raw; }
		gJitShiftCarry = (raw == 32) ? (u8)(v & 1) : 0;
		return 0;
	case SHK_LSR_T:
		if (raw == 0) return v;
		if (raw < 32) { gJitShiftCarry = (u8)((v >> (raw - 1)) & 1); return v >> raw; }
		gJitShiftCarry = (raw == 32) ? (u8)((v >> 31) & 1) : 0;
		return 0;
	case SHK_ASR_T:
		if (raw == 0) return v;
		if (raw < 32)
		{
			gJitShiftCarry = (u8)(((u32)((s32)v >> (raw - 1))) & 1);
			return (u32)((s32)v >> raw);
		}
		gJitShiftCarry = (v & 0x80000000u) ? 1 : 0;
		return (v & 0x80000000u) ? 0xFFFFFFFFu : 0;

	case SHK_ROR_T:
		if (raw == 0) return v;
		// fall through
	case SHK_ROR_V7:
	{
		const u32 n = raw & 31;
		if (n == 0) { gJitShiftCarry = (u8)((v >> 31) & 1); return v; }
		const u32 r = (v >> n) | (v << (32 - n));
		gJitShiftCarry = (u8)((r >> 31) & 1);
		return r;
	}
	default:
		return v;
	}
}

inline int bitsSet(u32 v)
{
	int n = 0;
	while (v) { v &= v - 1; n++; }
	return n;
}

inline bool isRegShift(u8 shift)
{
	return (shift >= SHIFT_LSL_REG && shift <= SHIFT_ROR_REG_T) || shift == SHIFT_ROR_REG_V7;
}

// ---------------------------------------------------------------------------
// Emitted-code error handling.  asmjit has no lastError(): a failed _emitI()
// only reports through an ErrorHandler.  Without one, a bad immediate silently
// produces a function with a few missing instructions, which breaks cycle
// accounting and PC values in ways the verifier can only see much later.
// ---------------------------------------------------------------------------
struct JitA64ErrorHandler : public ErrorHandler
{
	unsigned count = 0;
	Error    first = Error::kOk;

	void handle_error(Error err, const char *message, BaseEmitter *origin) override
	{
		(void)origin;
		if (count++ == 0)
		{
			first = err;
			if (getenv("VBA_JIT_ASM_ERRORS"))
				fprintf(stderr, "[jit-a64] emit error %u: %s\n",
						(unsigned)err, message ? message : "");
		}
	}
};

// ---------------------------------------------------------------------------
// Which instructions the backend refuses.  Identical to JitEmit.cpp's
// nativeSupported(): the reasons are guest semantics, not host ISA details.
// ---------------------------------------------------------------------------
bool nativeSupported(const JitInsn &in, bool thumb)
{
	static u32  opMask = 0;
	static bool opMaskInited = false;
	if (!opMaskInited)
	{
		const char *s = getenv("VBA_JIT_OPS");
		opMask = (s && s[0]) ? (u32)strtoul(s, NULL, 0) : 0xFFFFFFFFu;
		opMaskInited = true;
	}
	if (!((opMask >> in.op) & 1))
		return false;

	switch (in.op)
	{
	case JOP_ALU:
		if (in.d == 15 && in.flagMask != 0)
			return false;
		return true;
	case JOP_MUL:
		return in.d != 15 && (in.imm & 0x0F) != 15;
	case JOP_LOAD:
		if (in.d == 15 && (thumb || in.sub != MEM_WORD || !(in.flags & JF_PRE)))
			return false;
		if (in.n == 15 && (in.flags & JF_WRITEBACK))
			return false;
		return true;
	case JOP_STORE:
		if (in.n == 15 && (in.flags & JF_WRITEBACK))
			return false;
		return true;
	case JOP_LDM:
	case JOP_STM:
		if (in.n == 15 && (in.flags & JF_WRITEBACK))
			return false;
		return true;
	case JOP_PUSH:
	case JOP_POP:
	case JOP_BRANCH:
	case JOP_BX:
		return true;
	case JOP_SWP:
		return in.d != 15;
	case JOP_MRS:
		return in.d != 15;
	case JOP_MSR:
		return in.sub != 0;
	default:
		return false;
	}
}

bool needsPcBefore(const JitInsn &in)
{
	switch (in.op)
	{
	case JOP_ALU:
		if (in.n == 15) return true;
		if (in.flags & JF_IMM) return false;
		if (in.m == 15) return true;
		return isRegShift(in.shift) && (in.imm & 0x0F) == 15;
	case JOP_MUL:
		return in.n == 15 || in.m == 15;
	case JOP_BRANCH:
		return false;
	default:
		return true;
	}
}

bool touchesMemory(const JitInsn &in)
{
	switch (in.op)
	{
	case JOP_LOAD: case JOP_STORE: case JOP_LDM: case JOP_STM:
	case JOP_PUSH: case JOP_POP: case JOP_SWP:
		return true;
	default:
		return false;
	}
}

struct Carry
{
	enum Kind { kKeep, kConst, kGlobal } kind;
	u32 value;
};

// ---------------------------------------------------------------------------
// The emitter
// ---------------------------------------------------------------------------
class BlockEmitter
{
public:
	BlockEmitter(a64::Assembler *assembler, const JitBlock *block, JitCompiledBlock *out)
		: a(assembler), b(block), outBlock(out), thumb(block->thumb != 0), pc(0),
		  usesMem(false), count(0), chainEntryOffset(0), nChain(0),
		  needCount(0), budgetCount(0), traceDone(false)
	{
	}

	bool compile(u16 *outCount);

	u32  chainEntryOffsetOf() const { return chainEntryOffset; }
	int  chainSlotsUsed()     const { return nChain; }
	bool usesMemoryFrame()    const { return false; }  // A64 frame is fixed

private:
	a64::Assembler   *a;
	const JitBlock   *b;
	JitCompiledBlock *outBlock;
	bool  thumb;
	u32   pc;
	bool  usesMem;      // statistics only; every A64 frame is identical
	Label epilogue, chainBail;
	u16   count;
	u32   chainEntryOffset;
	int   nChain;

	Label needLabel[JIT_MAX_INSNS * 2];
	u32   needPc[JIT_MAX_INSNS * 2];
	int   needCount;
	Label budgetLabel[JIT_MAX_INSNS * 2];
	u32   budgetPc[JIT_MAX_INSNS * 2];
	int   budgetCount;
	bool  traceDone;

	int step() const { return thumb ? 2 : 4; }

	void emitPrologue();
	void emitEpilogue();
	void emitExitImm(u32 nextPc, u32 status);
	void emitExitReg(const a64::Gp &nextPc, u32 status);
	void emitChainExit(u32 nextPc);
	void queueNeedInterp(const Label &ni, bool used);
	void emitNeedInterpStubs();
	void emitBudgetStubs();
	void emitChainBail();
	void checkBudget(u32 nextPc);

	void setPc();
	void addFetch(u8 fetchCycles);
	void addConst(int cycles) { addImm(a, WCYC, (int32_t)cycles); }
	void addWait(const a64::Gp &addr, const int32 *table);
	void addAccess32(const a64::Gp &addr) { addWait(addr, memoryWait32); }
	void addAccess16(const a64::Gp &addr) { addWait(addr, memoryWait); }
	void addSeq32(const a64::Gp &addr)    { addWait(addr, memoryWaitSeq32); }
	void addSeq16(const a64::Gp &addr)    { addWait(addr, memoryWaitSeq); }

	void emitCondSkip(u8 cond, const Label &skip);
	void emitIoRangeCheck(u32 size, const Label &ni);
	void emitOperand2(const JitInsn &in, const a64::Gp &dst, Carry *carry);
	void emitOffset(const JitInsn &in, const a64::Gp &dst);
	void writeCarry(const Carry &carry);
	void emitNZ(const a64::Gp &res, u8 mask);
	void emitFlagsNative(u8 mask);
	void emitFlagsLogical(const a64::Gp &res, const Carry &carry, u8 mask);

	void emitAlu(const JitInsn &in);
	void emitMul(const JitInsn &in);
	void emitLoadStore(const JitInsn &in);
	void emitBlockTransfer(const JitInsn &in);
	void emitSwp(const JitInsn &in);
	void emitBranch(const JitInsn &in);
	void emitBx(const JitInsn &in);
	void emitMrs(const JitInsn &in);
	void emitMsr(const JitInsn &in);
	void emitOne(const JitInsn &in);
	void emitBody(const JitInsn &in);
	void emitTrace();
};

} // namespace

namespace {

// ---------------------------------------------------------------------------
// Prologue / epilogue: one fixed 96-byte frame for every block.
// ---------------------------------------------------------------------------
void BlockEmitter::emitPrologue()
{
	a->stp(a64::x29, a64::x30, a64::ptr_pre(a64::sp, -96));
	a->stp(a64::x19, a64::x20, a64::ptr(a64::sp, 16));
	a->stp(a64::x21, a64::x22, a64::ptr(a64::sp, 32));
	a->stp(a64::x23, a64::x24, a64::ptr(a64::sp, 48));
	a->stp(a64::x25, a64::x26, a64::ptr(a64::sp, 64));
	a->stp(a64::x27, a64::x28, a64::ptr(a64::sp, 80));

	a->mov(WBUD, a64::w0);          // budget  (arg 1)
	a->mov(XOUT, a64::x1);          // out     (arg 2)
	a->mov(WCYC, a64::wzr);         // cycles = 0

	emitMovImm64(a, XREG,   (uint64_t)(uintptr_t)&reg[0]);
	emitMovImm64(a, XFLAGS, (uint64_t)(uintptr_t)&N_FLAG);
	emitMovImm64(a, XWAIT,  (uint64_t)(uintptr_t)&memoryWait[0]);
	emitMovImm64(a, XMAP,   (uint64_t)(uintptr_t)&memoryMap[0].address);
	emitMovImm64(a, XCARRY, (uint64_t)(uintptr_t)&gJitShiftCarry);
}

void BlockEmitter::emitEpilogue()
{
	a->bind(epilogue);
	a->mov(a64::w0, WCYC);          // take the return value before restoring x20
	a->ldp(a64::x27, a64::x28, a64::ptr(a64::sp, 80));
	a->ldp(a64::x25, a64::x26, a64::ptr(a64::sp, 64));
	a->ldp(a64::x23, a64::x24, a64::ptr(a64::sp, 48));
	a->ldp(a64::x21, a64::x22, a64::ptr(a64::sp, 32));
	a->ldp(a64::x19, a64::x20, a64::ptr(a64::sp, 16));
	a->ldp(a64::x29, a64::x30, a64::ptr_post(a64::sp, 96));
	a->ret(a64::x30);
}

void BlockEmitter::emitExitImm(u32 nextPc, u32 status)
{
	emitMovImm32(a, a64::w3, nextPc);
	a->str(a64::w3, a64::ptr(XOUT));
	emitMovImm32(a, a64::w3, status);
	a->str(a64::w3, a64::ptr(XOUT, 4));
	a->b(epilogue);
}

void BlockEmitter::emitExitReg(const a64::Gp &nextPc, u32 status)
{
	a->mov(a64::w3, nextPc.w());
	a->str(a64::w3, a64::ptr(XOUT));
	emitMovImm32(a, a64::w3, status);
	a->str(a64::w3, a64::ptr(XOUT, 4));
	a->b(epilogue);
}

void BlockEmitter::emitChainExit(u32 nextPc)
{
	// The unlinked path has to hand nextPc/status back to runBlock(); a linked
	// successor overwrites both at its own exit, so the stores sit on that side
	// of the test and cost nothing while the chain holds.
	if (!outBlock || nChain >= JIT_MAX_CHAIN)
	{
		emitExitImm(nextPc, JITX_DONE);
		return;
	}

	JitChainSlot *slot = &outBlock->chain[nChain++];
	slot->target    = 0;
	slot->tag       = (nextPc & 0xFFFFFFFEu) | (thumb ? 1u : 0u);
	slot->linkIndex = 0xFFFF;
	slot->reserved  = 0;

	Label unlinked = a->new_label();
	emitMovImm64(a, a64::x9, (uint64_t)(uintptr_t)&slot->target);
	a->ldr(a64::x9, a64::ptr(a64::x9));
	a->cbz(a64::x9, unlinked);
	a->br(a64::x9);

	a->bind(unlinked);
	emitExitImm(nextPc, JITX_DONE);
}

void BlockEmitter::queueNeedInterp(const Label &ni, bool used)
{
	if (!used)
		return;
	needPc[needCount]    = pc;
	needLabel[needCount] = ni;
	needCount++;
}

void BlockEmitter::emitNeedInterpStubs()
{
	for (int i = 0; i < needCount; i++)
	{
		a->bind(needLabel[i]);
		emitExitImm(needPc[i], JITX_NEED_INTERP);
	}
}

void BlockEmitter::checkBudget(u32 nextPc)
{
	Label stub = a->new_label();
	a->cmp(WCYC, WBUD);
	a->b(a64::CondCode::kGE, stub);
	budgetLabel[budgetCount] = stub;
	budgetPc[budgetCount]    = nextPc;
	budgetCount++;
}

void BlockEmitter::emitBudgetStubs()
{
	for (int i = 0; i < budgetCount; i++)
	{
		a->bind(budgetLabel[i]);
		emitExitImm(budgetPc[i], JITX_BUDGET);
	}
}

void BlockEmitter::emitChainBail()
{
	// Only reachable from the chain-entry guard (a predecessor jumped here
	// after already spending the budget).  Stop *before* this block.
	a->bind(chainBail);
	emitExitImm(b->guestPc, JITX_BUDGET);
}

// ---------------------------------------------------------------------------
// Cycle accounting
// ---------------------------------------------------------------------------
void BlockEmitter::setPc()
{
	// arm-new.h/thumb.h leave reg[15] at pc+8 (ARM) or pc+4 (Thumb) while an
	// instruction runs, and the memory helpers read it for open-bus values.
	emitMovImm32(a, a64::w3, pc + (thumb ? 4u : 8u));
	a->str(a64::w3, gr(15));
}

void BlockEmitter::addFetch(u8 fetchCycles)
{
	// V7 ARM:  memoryWaitFetch32[pc >> 24]
	// V7 Thumb: thumbCycles[opcode >> 8] (baked into fetchCycles)
	//           + memoryWaitFetch[pc >> 24]
	// The index is a compile-time constant; the entry itself is a runtime read.
	if (thumb)
		addConst(fetchCycles);
	const int i = (int)((pc >> 24) & 15);
	const int32 *table = thumb ? memoryWaitFetch : memoryWaitFetch32;
	a->ldr(a64::w4, wmem(&table[i]));
	a->add(WCYC, WCYC, a64::w4);
}

void BlockEmitter::addWait(const a64::Gp &addr, const int32 *table)
{
	a->lsr(a64::w3, addr.w(), Imm(24));
	a->and_(a64::w3, a64::w3, Imm(15));
	const int32_t base = offWait(&table[0]);
	if (base)
		addImm(a, a64::w3, base / 4);
	a->ldr(a64::w4, a64::ptr(XWAIT, a64::w3, a64::uxtw(2)));
	a->add(WCYC, WCYC, a64::w4);
}

// ---------------------------------------------------------------------------
// Conditions.  The guest flags are four bytes in memory; jump to `skip` when
// the condition is false.  cset/strb and ldrb/cmp/b never rely on host flags
// surviving across instructions.
// ---------------------------------------------------------------------------
void BlockEmitter::emitCondSkip(u8 cond, const Label &skip)
{
	switch (cond)
	{
	case 0x0:   // EQ: Z == 1
		a->ldrb(a64::w3, fmem(&Z_FLAG));
		a->cmp(a64::w3, Imm(0));
		a->b(a64::CondCode::kEQ, skip);
		break;
	case 0x1:   // NE: Z == 0
		a->ldrb(a64::w3, fmem(&Z_FLAG));
		a->cmp(a64::w3, Imm(0));
		a->b(a64::CondCode::kNE, skip);
		break;
	case 0x2:   // CS/HS: C == 1
		a->ldrb(a64::w3, fmem(&C_FLAG));
		a->cmp(a64::w3, Imm(0));
		a->b(a64::CondCode::kEQ, skip);
		break;
	case 0x3:   // CC/LO: C == 0
		a->ldrb(a64::w3, fmem(&C_FLAG));
		a->cmp(a64::w3, Imm(0));
		a->b(a64::CondCode::kNE, skip);
		break;
	case 0x4:   // MI: N == 1
		a->ldrb(a64::w3, fmem(&N_FLAG));
		a->cmp(a64::w3, Imm(0));
		a->b(a64::CondCode::kEQ, skip);
		break;
	case 0x5:   // PL: N == 0
		a->ldrb(a64::w3, fmem(&N_FLAG));
		a->cmp(a64::w3, Imm(0));
		a->b(a64::CondCode::kNE, skip);
		break;
	case 0x6:   // VS: V == 1
		a->ldrb(a64::w3, fmem(&V_FLAG));
		a->cmp(a64::w3, Imm(0));
		a->b(a64::CondCode::kEQ, skip);
		break;
	case 0x7:   // VC: V == 0
		a->ldrb(a64::w3, fmem(&V_FLAG));
		a->cmp(a64::w3, Imm(0));
		a->b(a64::CondCode::kNE, skip);
		break;
	case 0x8:   // HI: C && !Z
		a->ldrb(a64::w3, fmem(&C_FLAG));
		a->cmp(a64::w3, Imm(0));
		a->b(a64::CondCode::kEQ, skip);
		a->ldrb(a64::w3, fmem(&Z_FLAG));
		a->cmp(a64::w3, Imm(0));
		a->b(a64::CondCode::kNE, skip);
		break;
	case 0x9:   // LS: !C || Z
	{
		Label ok = a->new_label();
		a->ldrb(a64::w3, fmem(&C_FLAG));
		a->cmp(a64::w3, Imm(0));
		a->b(a64::CondCode::kNE, ok);
		a->ldrb(a64::w3, fmem(&Z_FLAG));
		a->cmp(a64::w3, Imm(0));
		a->b(a64::CondCode::kEQ, skip);
		a->bind(ok);
		break;
	}
	case 0xA:   // GE: N == V
		a->ldrb(a64::w3, fmem(&N_FLAG));
		a->ldrb(a64::w4, fmem(&V_FLAG));
		a->cmp(a64::w3, a64::w4);
		a->b(a64::CondCode::kNE, skip);
		break;
	case 0xB:   // LT: N != V
		a->ldrb(a64::w3, fmem(&N_FLAG));
		a->ldrb(a64::w4, fmem(&V_FLAG));
		a->cmp(a64::w3, a64::w4);
		a->b(a64::CondCode::kEQ, skip);
		break;
	case 0xC:   // GT: !Z && N == V
		a->ldrb(a64::w3, fmem(&Z_FLAG));
		a->cmp(a64::w3, Imm(0));
		a->b(a64::CondCode::kNE, skip);
		a->ldrb(a64::w3, fmem(&N_FLAG));
		a->ldrb(a64::w4, fmem(&V_FLAG));
		a->cmp(a64::w3, a64::w4);
		a->b(a64::CondCode::kNE, skip);
		break;
	case 0xD:   // LE: Z || N != V
	{
		Label ok = a->new_label();
		a->ldrb(a64::w3, fmem(&Z_FLAG));
		a->cmp(a64::w3, Imm(0));
		a->b(a64::CondCode::kNE, ok);
		a->ldrb(a64::w3, fmem(&N_FLAG));
		a->ldrb(a64::w4, fmem(&V_FLAG));
		a->cmp(a64::w3, a64::w4);
		a->b(a64::CondCode::kEQ, skip);
		a->bind(ok);
		break;
	}
	case 0xF:   // NV
		a->b(skip);
		break;
	default:    // 0xE AL
		break;
	}
}

// A store into the IO block can start a DMA transfer; CPUWriteMemoryWrapped()
// calls CPUCheckDMA(), which rewrites CPULoop's event counter.  Check both the
// first and the last byte of the access.
void BlockEmitter::emitIoRangeCheck(u32 size, const Label &ni)
{
	// and w4, wADDR, #0x0F000000 is a valid A64 logical immediate, but
	// cmp w4, #0x04000000 is not (the encoder probe reports errors=1).  Load
	// the constant once and compare registers.
	a->and_(a64::w4, WADDR, Imm(0x0F000000u));
	emitMovImm32(a, a64::w5, 0x04000000u);
	a->cmp(a64::w4, a64::w5);
	a->b(a64::CondCode::kEQ, ni);
	if (size > 1)
	{
		a->add(a64::w4, WADDR, Imm((uint32_t)size - 1u));
		a->and_(a64::w4, a64::w4, Imm(0x0F000000u));
		a->cmp(a64::w4, a64::w5);
		a->b(a64::CondCode::kEQ, ni);
	}
}

// ---------------------------------------------------------------------------
// Operand2 / offsets / flags
// ---------------------------------------------------------------------------
void BlockEmitter::writeCarry(const Carry &carry)
{
	switch (carry.kind)
	{
	case Carry::kKeep:
		return;
	case Carry::kConst:
		emitMovImm32(a, a64::w3, carry.value ? 1u : 0u);
		a->strb(a64::w3, fmem(&C_FLAG));
		return;
	default:
	{
		// jitShiftReg() leaves 0xFF when the shift does not define a carry.
		Label done = a->new_label();
		a->ldrb(a64::w3, a64::ptr(XCARRY));
		a->cmp(a64::w3, Imm(0xFF));
		a->b(a64::CondCode::kEQ, done);
		a->strb(a64::w3, fmem(&C_FLAG));
		a->bind(done);
		return;
	}
	}
}

void BlockEmitter::emitOperand2(const JitInsn &in, const a64::Gp &dst, Carry *carry)
{
	carry->kind  = Carry::kKeep;
	carry->value = 0;

	if (in.flags & JF_IMM)
	{
		if (in.shift != SHIFT_IMM_ROT)
		{
			emitMovImm32(a, dst, in.imm);
			return;
		}
		const u32 rot  = (in.imm >> 8) & 0x0F;
		const u32 imm8 = in.imm & 0xFF;
		if (rot == 0)
		{
			emitMovImm32(a, dst, imm8);
			return;
		}
		const u32 amount = rot * 2;
		emitMovImm32(a, dst, (imm8 >> amount) | (imm8 << (32 - amount)));
		carry->kind  = Carry::kConst;
		carry->value = (imm8 >> (amount - 1)) & 1;
		return;
	}

	const u8 rm = (u8)(in.m & 0x0F);

	switch (in.shift)
	{
	case SHIFT_NONE:
		a->ldr(dst, gr(rm));
		return;

	case SHIFT_LSL_IMM:
		a->ldr(dst, gr(rm));
		if (in.imm == 0)
			return;
		a->lsr(a64::w3, dst, Imm(32u - in.imm));
		a->and_(a64::w3, a64::w3, Imm(1));
		a->strb(a64::w3, a64::ptr(XCARRY));
		carry->kind = Carry::kGlobal;
		a->lsl(dst, dst, Imm(in.imm));
		return;

	case SHIFT_LSR_IMM:
		a->ldr(dst, gr(rm));
		if (in.imm == 0)
		{
			a->lsr(a64::w3, dst, Imm(31));
			a->strb(a64::w3, a64::ptr(XCARRY));
			carry->kind = Carry::kGlobal;
			a->mov(dst, a64::wzr);
			return;
		}
		a->lsr(a64::w3, dst, Imm(in.imm - 1u));
		a->and_(a64::w3, a64::w3, Imm(1));
		a->strb(a64::w3, a64::ptr(XCARRY));
		carry->kind = Carry::kGlobal;
		a->lsr(dst, dst, Imm(in.imm));
		return;

	case SHIFT_ASR_IMM:
		a->ldr(dst, gr(rm));
		if (in.imm == 0)
		{
			a->lsr(a64::w3, dst, Imm(31));
			a->strb(a64::w3, a64::ptr(XCARRY));
			carry->kind = Carry::kGlobal;
			a->asr(dst, dst, Imm(31));
			return;
		}
		a->asr(a64::w3, dst, Imm(in.imm - 1u));
		a->and_(a64::w3, a64::w3, Imm(1));
		a->strb(a64::w3, a64::ptr(XCARRY));
		carry->kind = Carry::kGlobal;
		a->asr(dst, dst, Imm(in.imm));
		return;

	case SHIFT_ROR_IMM:
		a->ldr(dst, gr(rm));
		if (in.imm == 0)        // RRX: the incoming carry becomes bit 31
		{
			a->and_(a64::w3, dst, Imm(1));
			a->strb(a64::w3, a64::ptr(XCARRY));
			carry->kind = Carry::kGlobal;
			a->lsr(dst, dst, Imm(1));
			a->ldrb(a64::w5, fmem(&C_FLAG));
			a->lsl(a64::w5, a64::w5, Imm(31));
			a->orr(dst, dst, a64::w5);
			return;
		}
		a->ror(dst, dst, Imm(in.imm));
		a->lsr(a64::w3, dst, Imm(31));
		a->strb(a64::w3, a64::ptr(XCARRY));
		carry->kind = Carry::kGlobal;
		return;

	case SHIFT_LSL_REG:
	case SHIFT_LSR_REG:
	case SHIFT_ASR_REG:
	case SHIFT_ROR_REG:
	case SHIFT_ROR_REG_T:
	case SHIFT_ROR_REG_V7:
	{
		u32 kind;
		if (in.shift == SHIFT_LSL_REG)      kind = thumb ? SHK_LSL_T : SHK_LSL;
		else if (in.shift == SHIFT_LSR_REG) kind = thumb ? SHK_LSR_T : SHK_LSR;
		else if (in.shift == SHIFT_ASR_REG) kind = thumb ? SHK_ASR_T : SHK_ASR;
		else if (in.shift == SHIFT_ROR_REG_V7) kind = SHK_ROR_V7;
		else                                 kind = SHK_ROR_T;

		a->ldr(a64::w0, gr(rm));
		a->ldrb(a64::w1, a64::ptr(XREG, (int)(in.imm & 0x0F) * 4));
		a->ldrb(a64::w2, fmem(&C_FLAG));
		emitMovImm32(a, a64::w3, kind);
		callHelper(a, (const void *)&jitShiftReg);
		a->mov(dst, a64::w0);
		carry->kind = Carry::kGlobal;
		return;
	}

	default:
		a->ldr(dst, gr(rm));
		return;
	}
}

// Load/store offsets use OFFSET_* semantics, which differ from the ALU VALUE_*
// ones for LSR/ASR/ROR with a count of zero.
void BlockEmitter::emitOffset(const JitInsn &in, const a64::Gp &dst)
{
	const u8 rm = (u8)(in.m & 0x0F);
	switch (in.shift)
	{
	case SHIFT_LSL_IMM:
		a->ldr(dst, gr(rm));
		if (in.imm) a->lsl(dst, dst, Imm(in.imm));
		return;
	case SHIFT_LSR_IMM:
		a->ldr(dst, gr(rm));
		if (in.imm == 0) a->mov(dst, a64::wzr);
		else             a->lsr(dst, dst, Imm(in.imm));
		return;
	case SHIFT_ASR_IMM:
		a->ldr(dst, gr(rm));
		if (in.imm == 0) a->asr(dst, dst, Imm(31));
		else             a->asr(dst, dst, Imm(in.imm));
		return;
	case SHIFT_ROR_IMM:
		a->ldr(dst, gr(rm));
		if (in.imm)
		{
			a->ror(dst, dst, Imm(in.imm));
			return;
		}
		a->lsr(dst, dst, Imm(1));
		a->ldrb(a64::w5, fmem(&C_FLAG));
		a->lsl(a64::w5, a64::w5, Imm(31));
		a->orr(dst, dst, a64::w5);
		return;
	default:
		a->ldr(dst, gr(rm));
		return;
	}
}

void BlockEmitter::emitNZ(const a64::Gp &res, u8 mask)
{
	if ((mask & (JITF_N | JITF_Z)) == 0)
		return;
	a->cmp(res, Imm(0));           // cset does not disturb NZCV
	if (mask & JITF_N)
	{
		a->cset(a64::w3, a64::CondCode::kMI);
		a->strb(a64::w3, fmem(&N_FLAG));
	}
	if (mask & JITF_Z)
	{
		a->cset(a64::w3, a64::CondCode::kEQ);
		a->strb(a64::w3, fmem(&Z_FLAG));
	}
}

// A64's adds/subs/adcs/sbcs leave exactly the ARM guest flags in NZCV:
//   N = sign, Z = zero, V = signed overflow, C = carry / no-borrow.
// Unlike x86, SUB's C is already "no borrow", so add and sub both use `hs`.
// This also covers ADC/SBC/RSC: their result is produced by a single adcs/sbcs
// whose NZCV describes the whole three-operand guest operation, so A64 does
// not need x86's emitFlagsAdd()/emitFlagsSub() recomputation at all.
void BlockEmitter::emitFlagsNative(u8 mask)
{
	if (mask & JITF_N)
	{
		a->cset(a64::w3, a64::CondCode::kMI);
		a->strb(a64::w3, fmem(&N_FLAG));
	}
	if (mask & JITF_Z)
	{
		a->cset(a64::w3, a64::CondCode::kEQ);
		a->strb(a64::w3, fmem(&Z_FLAG));
	}
	if (mask & JITF_C)
	{
		a->cset(a64::w3, a64::CondCode::kHS);
		a->strb(a64::w3, fmem(&C_FLAG));
	}
	if (mask & JITF_V)
	{
		a->cset(a64::w3, a64::CondCode::kVS);
		a->strb(a64::w3, fmem(&V_FLAG));
	}
}

void BlockEmitter::emitFlagsLogical(const a64::Gp &res, const Carry &carry, u8 mask)
{
	emitNZ(res, mask);
	if (mask & JITF_C)
		writeCarry(carry);
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// Data processing
// ---------------------------------------------------------------------------
void BlockEmitter::emitAlu(const JitInsn &in)
{
	const u8   mask  = in.flagMask;
	const bool toPc  = (in.d == 15);
	const int  extra = isRegShift(in.shift) ? 1 : 0;

	Label ni;
	bool  needNi = false;

	// The ARM handlers shift a u32 by the raw Rs byte without clamping; counts
	// >= 32 are UB in C++ and the interpreter's result there is whatever the
	// host happened to do.  Hand those to the interpreter.
	if (!thumb && !(in.flags & JF_IMM) &&
		(in.shift == SHIFT_LSL_REG || in.shift == SHIFT_LSR_REG || in.shift == SHIFT_ASR_REG))
	{
		ni     = a->new_label();
		needNi = true;
		a->ldrb(a64::w3, a64::ptr(XREG, (int)(in.imm & 0x0F) * 4));
		a->cmp(a64::w3, Imm(32));
		a->b(a64::CondCode::kHS, ni);
	}

	Carry carry;
	emitOperand2(in, a64::w4, &carry);        // rhs -> w4

	const bool compareOnly = (in.sub >= ALU_TST && in.sub <= ALU_CMN);

	if (in.sub != ALU_MOV && in.sub != ALU_MVN)
	{
		a->ldr(a64::w5, gr(in.n));            // lhs
		if (in.flags & JF_PC_ALIGN4)
			a->bic(a64::w5, a64::w5, Imm(3));
	}

	// Every arithmetic form is a single A64 flag-setting instruction whose
	// NZCV describes the whole guest operation, including ADC/SBC/RSC.  The
	// carry-in is injected by `cmp w3,#1`, which leaves C = the guest C.
	switch (in.sub)
	{
	case ALU_AND: a->and_(a64::w6, a64::w5, a64::w4); break;
	case ALU_EOR: a->eor(a64::w6, a64::w5, a64::w4); break;
	case ALU_SUB: a->subs(a64::w6, a64::w5, a64::w4); break;
	case ALU_RSB: a->subs(a64::w6, a64::w4, a64::w5); break;
	case ALU_ADD: a->adds(a64::w6, a64::w5, a64::w4); break;
	case ALU_ADC:
		a->ldrb(a64::w3, fmem(&C_FLAG));
		a->cmp(a64::w3, Imm(1));
		a->adcs(a64::w6, a64::w5, a64::w4);
		break;
	case ALU_SBC:
		a->ldrb(a64::w3, fmem(&C_FLAG));
		a->cmp(a64::w3, Imm(1));
		a->sbcs(a64::w6, a64::w5, a64::w4);
		break;
	case ALU_RSC:
		a->ldrb(a64::w3, fmem(&C_FLAG));
		a->cmp(a64::w3, Imm(1));
		a->sbcs(a64::w6, a64::w4, a64::w5);
		break;
	case ALU_TST: a->and_(a64::w6, a64::w5, a64::w4); break;
	case ALU_TEQ: a->eor(a64::w6, a64::w5, a64::w4); break;
	case ALU_CMP: a->subs(a64::w6, a64::w5, a64::w4); break;
	case ALU_CMN: a->adds(a64::w6, a64::w5, a64::w4); break;
	case ALU_ORR: a->orr(a64::w6, a64::w5, a64::w4); break;
	case ALU_MOV: a->mov(a64::w6, a64::w4); break;
	case ALU_BIC: a->bic(a64::w6, a64::w5, a64::w4); break;
	case ALU_MVN: a->orn(a64::w6, a64::wzr, a64::w4); break;
	default:      a->mov(a64::w6, a64::w4); break;
	}

	if (toPc)
	{
		// Reached only with S=0 (nativeSupported).  Align for the current
		// instruction set and leave the block; the caller turns nextPc into
		// armNextPC and reg[15] = nextPc + 4/2.
		a->bic(a64::w6, a64::w6, Imm(thumb ? 1u : 3u));
		addFetch(in.fetchCycles);
		addConst(extra);
		addConst(thumb ? 1 : 0);
		emitExitReg(a64::w6, JITX_DONE);
		queueNeedInterp(ni, needNi);
		return;
	}

	if (!compareOnly)
		a->str(a64::w6, gr(in.d));

	const bool isAdd = (in.sub == ALU_ADD || in.sub == ALU_ADC || in.sub == ALU_CMN);
	const bool isSub = (in.sub == ALU_SUB || in.sub == ALU_RSB || in.sub == ALU_SBC ||
						in.sub == ALU_RSC || in.sub == ALU_CMP);

	// addFetch()/checkBudget() clobber NZCV, so read the flags out first.
	if (isAdd || isSub)
		emitFlagsNative(mask);
	else
		emitFlagsLogical(a64::w6, carry, mask);

	addFetch(in.fetchCycles);
	addConst(extra);
	checkBudget(pc + step());
	queueNeedInterp(ni, needNi);
}

// ---------------------------------------------------------------------------
// Multiply
// ---------------------------------------------------------------------------
void BlockEmitter::emitMul(const JitInsn &in)
{
	const u8  mask = in.flagMask;
	const u8  lo   = (u8)(in.imm & 0x0F);
	const bool isLong = (in.sub >= MUL_UMULL);

	a->ldr(a64::w10, gr(in.m));       // Rs: multiplicand and timing source

	switch (in.sub)
	{
	case MUL_MUL:
		a->ldr(a64::w5, gr(thumb ? in.d : in.n));
		a->mul(a64::w6, a64::w5, a64::w10);
		a->str(a64::w6, gr(in.d));
		break;

	case MUL_MLA:
		a->ldr(a64::w5, gr(in.n));
		a->mul(a64::w6, a64::w5, a64::w10);
		a->ldr(a64::w7, gr(lo));
		a->add(a64::w6, a64::w6, a64::w7);
		a->str(a64::w6, gr(in.d));
		break;

	case MUL_UMULL:
	case MUL_SMULL:
		a->ldr(a64::w5, gr(in.n));
		if (in.sub == MUL_SMULL)
			a->smull(a64::x6, a64::w5, a64::w10);
		else
			a->umull(a64::x6, a64::w5, a64::w10);
		a->str(a64::w6, gr(lo));
		a->lsr(a64::x6, a64::x6, Imm(32));
		a->str(a64::w6, gr(in.d));
		break;

	case MUL_UMLAL:
	case MUL_SMLAL:
		a->ldr(a64::w7, gr(in.d));
		a->lsl(a64::x8, a64::x7, Imm(32));
		a->ldr(a64::w7, gr(lo));
		a->orr(a64::x8, a64::x8, a64::x7);
		a->ldr(a64::w5, gr(in.n));
		if (in.sub == MUL_SMLAL)
			a->smaddl(a64::x6, a64::w5, a64::w10, a64::x8);
		else
			a->umaddl(a64::x6, a64::w5, a64::w10, a64::x8);
		a->str(a64::w6, gr(lo));
		a->lsr(a64::x6, a64::x6, Imm(32));
		a->str(a64::w6, gr(in.d));
		break;
	}

	if (mask)
	{
		if (mask & JITF_N)
		{
			a->ldr(a64::w7, gr(in.d));
			a->lsr(a64::w7, a64::w7, Imm(31));
			a->strb(a64::w7, fmem(&N_FLAG));
		}
		if (mask & JITF_Z)
		{
			a->ldr(a64::w7, gr(in.d));
			if (isLong)
			{
				a->ldr(a64::w8, gr(lo));
				a->orr(a64::w7, a64::w7, a64::w8);
			}
			a->cmp(a64::w7, Imm(0));
			a->cset(a64::w8, a64::CondCode::kEQ);
			a->strb(a64::w8, fmem(&Z_FLAG));
		}
	}

	// arm-new.h buckets Rs by magnitude.  bsr gives exactly that:
	// floor(highest set bit / 8).  A64 has clz, so use 31 - clz.
	addFetch(in.fetchCycles);
	a->mov(a64::w7, a64::w10);
	if (in.sub == MUL_MUL || in.sub == MUL_MLA ||
		in.sub == MUL_SMULL || in.sub == MUL_SMLAL)
	{
		Label pos = a->new_label();
		a->cmp(a64::w7, Imm(0));
		a->b(a64::CondCode::kGE, pos);
		a->orn(a64::w7, a64::wzr, a64::w7);   // bitwise NOT, not negation
		a->bind(pos);
	}
	a->orr(a64::w7, a64::w7, Imm(1));
	a->clz(a64::w8, a64::w7);
	emitMovImm32(a, a64::w9, 31);
	a->sub(a64::w8, a64::w9, a64::w8);
	a->lsr(a64::w8, a64::w8, Imm(3));
	a->add(WCYC, WCYC, a64::w8);

	if (thumb)
		addConst(1);                        // thumb43_1: +1..+4
	else
	{
		const int plus = (in.sub == MUL_MUL || in.sub == MUL_UMULL ||
						  in.sub == MUL_SMULL) ? 0 : 1;
		addConst(2 + plus);
	}
	checkBudget(pc + step());
}

// ---------------------------------------------------------------------------
// Single load / store
// ---------------------------------------------------------------------------
void BlockEmitter::emitLoadStore(const JitInsn &in)
{
	const bool pre  = (in.flags & JF_PRE) != 0;
	const bool up   = (in.flags & JF_UP) != 0;
	const bool wb   = (in.flags & JF_WRITEBACK) != 0;
	const u32  size = (in.sub == MEM_WORD) ? 4u : (in.sub == MEM_HALF ? 2u : 1u);

	a->ldr(WADDR, gr(in.n));
	if (in.flags & JF_PC_ALIGN4)
		a->bic(WADDR, WADDR, Imm(3));

	if (in.flags & JF_IMM)
	{
		a->mov(WWB, WADDR);
		if (in.imm)
		{
			if (up) a->add(WWB, WWB, Imm(in.imm));
			else    a->sub(WWB, WWB, Imm(in.imm));
		}
	}
	else
	{
		emitOffset(in, a64::w4);
		a->mov(WWB, WADDR);
		if (up) a->add(WWB, WWB, a64::w4);
		else    a->sub(WWB, WWB, a64::w4);
	}
	if (pre)
		a->mov(WADDR, WWB);

	Label ni;
	bool  needNi = false;

	if (in.op == JOP_STORE)
	{
		ni     = a->new_label();
		needNi = true;
		emitIoRangeCheck(size, ni);
		emitTrace();

		// arm-new.h writes the base back *before* the store for STR/STRB and
		// *after* it for STRH; only visible when Rn == Rt, but the differential
		// test compares registers.
		if (pre && wb && in.sub != MEM_HALF)
			a->str(WADDR, gr(in.n));

		switch (in.sub)
		{
		case MEM_BYTE:
			a->mov(a64::w0, WADDR);
			a->ldrb(a64::w1, a64::ptr(XREG, in.d * 4));
			callHelper(a, (const void *)&CPUWriteByte);
			break;
		case MEM_HALF:
			a->mov(a64::w0, WADDR);
			a->ldrh(a64::w1, a64::ptr(XREG, in.d * 4));
			callHelper(a, (const void *)&CPUWriteHalfWord);
			if (pre && wb)
				a->str(WADDR, gr(in.n));
			break;
		default:
			a->mov(a64::w0, WADDR);
			a->ldr(a64::w1, gr(in.d));
			callHelper(a, (const void *)&CPUWriteMemory);
			break;
		}

		if (!pre && wb)
			a->str(WWB, gr(in.n));

		addFetch(in.fetchCycles);
		if (!thumb)
			addConst(in.sub == MEM_HALF ? 4 : 2);
		if (in.sub == MEM_WORD)
			addAccess32(WADDR);
		else
			addAccess16(WADDR);
		checkBudget(pc + step());
		queueNeedInterp(ni, needNi);
		return;
	}

	// Load.  Regions 2/3, 5..7 and (for word/byte) 8..12 are plain indexed
	// loads in CPUReadMemoryWrapped(); an inline fast path can skip the helper
	// there.  The full region byte matters: 0x12xxxxxx / 0x13xxxxxx are open
	// bus, not EWRAM/IWRAM, so the looser (addr & 0x0E000000) write-invalidate
	// test must not be reused here.  ROM halfword reads keep the helper because
	// of the RTC at 0x080000C4/C6/C8.
	{
		const bool fast = (in.sub == MEM_BYTE) || (in.sub == MEM_HALF) ||
						  (in.sub == MEM_WORD && !(in.flags & JF_QUICK));
		Label slow = a->new_label();
		Label done = a->new_label();

		if (fast)
		{
			Label fastRegion = a->new_label();
			a->lsr(a64::w8, WADDR, Imm(24));
			a->cmp(a64::w8, Imm(2));
			a->b(a64::CondCode::kLO, slow);
			a->cmp(a64::w8, Imm(3));
			a->b(a64::CondCode::kLS, fastRegion);
			a->cmp(a64::w8, Imm(5));
			a->b(a64::CondCode::kLO, slow);
			a->cmp(a64::w8, Imm(7));
			a->b(a64::CondCode::kLS, fastRegion);
			if (in.sub == MEM_HALF)
			{
				a->b(slow);
			}
			else
			{
				a->cmp(a64::w8, Imm(8));
				a->b(a64::CondCode::kLO, slow);
				a->cmp(a64::w8, Imm(12));
				a->b(a64::CondCode::kHI, slow);
			}
			a->bind(fastRegion);

			a->lsl(a64::w8, a64::w8, Imm(4));       // region * sizeof(MemoryMap)
			a->ldr(a64::x9, a64::ptr(XMAP, a64::w8, a64::uxtw(0)));   // address
			a->add(a64::w8, a64::w8, Imm(8));
			a->ldr(a64::w10, a64::ptr(XMAP, a64::w8, a64::uxtw(0)));  // mask
			a->and_(a64::w10, a64::w10, WADDR);
			if (in.sub == MEM_WORD)
				a->bic(a64::w10, a64::w10, Imm(3));
			else if (in.sub == MEM_HALF)
				a->bic(a64::w10, a64::w10, Imm(1));

			if (in.sub == MEM_WORD)
			{
				a->ldr(a64::w11, a64::ptr(a64::x9, a64::w10, a64::uxtw(0)));
				// CPUReadMemoryWrapped() ends with _rotr(value,(addr&3)*8).
				a->and_(a64::w4, WADDR, Imm(3));
				a->cbz(a64::w4, done);
				a->lsl(a64::w4, a64::w4, Imm(3));
				a->ror(a64::w11, a64::w11, a64::w4);
			}
			else if (in.sub == MEM_HALF)
			{
				a->ldrh(a64::w11, a64::ptr(a64::x9, a64::w10, a64::uxtw(1)));
				// CPUReadHalfWordWrapped() ends with (v>>8)|(v<<24) on odd
				// addresses: on a zero-extended halfword that is ror w,8,
				// NOT a 16-bit byte swap.  The signed form then keeps (s8),
				// i.e. the high byte sign-extended.
				a->and_(a64::w4, WADDR, Imm(1));
				a->cbz(a64::w4, done);
				if (in.flags & JF_SIGNED)
				{
					a->lsr(a64::w11, a64::w11, Imm(8));
					a->sxtb(a64::w11, a64::w11);
				}
				else
					a->ror(a64::w11, a64::w11, Imm(8));
			}
			else
				a->ldrb(a64::w11, a64::ptr(a64::x9, a64::w10, a64::uxtw(0)));
			a->b(done);
		}

		a->bind(slow);
		a->mov(a64::w0, WADDR);
		if (in.sub == MEM_BYTE)
			callHelper(a, (const void *)&CPUReadByte);
		else if (in.sub == MEM_HALF)
		{
			if (in.flags & JF_SIGNED)
				callHelper(a, (const void *)&CPUReadHalfWordSigned);
			else
				callHelper(a, (const void *)&CPUReadHalfWord);
		}
		else
			callHelper(a, (const void *)((in.flags & JF_QUICK) ? &jitReadQuick
															  : &CPUReadMemory));
		a->mov(a64::w11, a64::w0);
		a->bind(done);

		if (in.sub == MEM_BYTE)
		{
			if (in.flags & JF_SIGNED) a->sxtb(a64::w11, a64::w11);
			else                      a->uxtb(a64::w11, a64::w11);
		}
		else if (in.sub == MEM_HALF && (in.flags & JF_SIGNED))
			a->sxth(a64::w11, a64::w11);
	}

	a->str(a64::w11, gr(in.d));
	if (in.d != in.n && wb)
	{
		if (pre) a->str(WADDR, gr(in.n));
		else     a->str(WWB, gr(in.n));
	}

	addFetch(in.fetchCycles);
	if (!thumb)
		addConst(3);
	if (in.sub == MEM_WORD)
		addAccess32(WADDR);
	else
		addAccess16(WADDR);

	if (in.d == 15)
	{
		// Only the ARM pre-indexed word form gets here: charge two more cycles
		// and jump to the loaded value aligned to four bytes.
		addConst(2);
		a->ldr(a64::w11, gr(15));
		a->bic(a64::w11, a64::w11, Imm(3));
		emitExitReg(a64::w11, JITX_DONE);
	}
	else
		checkBudget(pc + step());
	queueNeedInterp(ni, needNi);
}

// ---------------------------------------------------------------------------
// Block transfers (LDM/STM/PUSH/POP), unrolled: the register list is static.
// ---------------------------------------------------------------------------
void BlockEmitter::emitBlockTransfer(const JitInsn &in)
{
	const u32 list = in.imm & 0xFFFF;

	Label ni;
	bool  needNi = false;

	if (in.op == JOP_PUSH)
	{
		const int regs  = bitsSet(list & 0xFF);
		const bool lr   = (list & 0x4000) != 0;
		const u32  total = (u32)regs + (lr ? 1u : 0u);

		a->ldr(WADDR, gr(13));
		a->sub(WADDR, WADDR, Imm(regs * 4 + (lr ? 4 : 0)));
		a->mov(WWB, WADDR);                 // new sp (unaligned)
		a->bic(WADDR, WADDR, Imm(3));       // aligned store address

		ni     = a->new_label();
		needNi = true;
		emitIoRangeCheck(total * 4, ni);
		emitTrace();

		bool first = true;
		for (int r = 0; r < 8; r++)
		{
			if (!(list & (1u << r)))
				continue;
			a->mov(a64::w0, WADDR);
			a->ldr(a64::w1, gr(r));
			callHelper(a, (const void *)&CPUWriteMemory);
			addConst(1);
			if (first) addAccess32(WADDR); else addSeq32(WADDR);
			first = false;
			a->add(WADDR, WADDR, Imm(4));
		}
		if (lr)
		{
			a->mov(a64::w0, WADDR);
			a->ldr(a64::w1, gr(14));
			callHelper(a, (const void *)&CPUWriteMemory);
			addConst(1);
			if (first) addAccess32(WADDR); else addSeq32(WADDR);
		}

		a->str(WWB, gr(13));
		addFetch(in.fetchCycles);
		checkBudget(pc + 2);
		queueNeedInterp(ni, needNi);
		return;
	}

	if (in.op == JOP_POP)
	{
		const bool withPc = (list & 0x8000) != 0;
		const int  regs   = bitsSet(list & 0xFF);

		a->ldr(WADDR, gr(13));
		a->bic(WADDR, WADDR, Imm(3));
		a->ldr(WWB, gr(13));
		a->add(WWB, WWB, Imm(regs * 4 + (withPc ? 4 : 0)));

		bool first = true;
		for (int r = 0; r < 8; r++)
		{
			if (!(list & (1u << r)))
				continue;
			a->mov(a64::w0, WADDR);
			callHelper(a, (const void *)&CPUReadMemory);
			addConst(2);
			if (first) addAccess32(WADDR); else addSeq32(WADDR);
			first = false;
			a->str(a64::w0, gr(r));
			a->add(WADDR, WADDR, Imm(4));
		}

		if (!withPc)
		{
			a->str(WWB, gr(13));
			addFetch(in.fetchCycles);
			checkBudget(pc + 2);
			return;
		}

		// The PC slot charges a bare data access (no +2) and the loaded address
		// is only masked to halfword alignment; V7 does not switch to ARM here.
		a->mov(a64::w0, WADDR);
		callHelper(a, (const void *)&CPUReadMemory);
		if (first) addAccess32(WADDR); else addSeq32(WADDR);
		a->bic(a64::w3, a64::w0, Imm(1));
		a->str(WWB, gr(13));
		addFetch(in.fetchCycles);
		emitExitReg(a64::w3, JITX_DONE);
		return;
	}

	// LDM / STM
	const u8 baseReg = in.n;
	a->ldr(WWB, gr(baseReg));              // base (also the writeback source)
	switch (in.sub)
	{
	case BLOCK_IA:
		a->mov(WADDR, WWB);
		a->bic(WADDR, WADDR, Imm(3));
		a->add(a64::w4, WWB, Imm(4 * bitsSet(list)));
		break;
	case BLOCK_IB:
		a->add(WADDR, WWB, Imm(4));
		a->bic(WADDR, WADDR, Imm(3));
		a->add(a64::w4, WWB, Imm(4 * bitsSet(list)));
		break;
	case BLOCK_DA:
		a->sub(a64::w4, WWB, Imm(4 * bitsSet(list)));
		a->add(WADDR, a64::w4, Imm(4));
		a->bic(WADDR, WADDR, Imm(3));
		break;
	default:    // BLOCK_DB
		a->sub(a64::w4, WWB, Imm(4 * bitsSet(list)));
		a->mov(WADDR, a64::w4);
		a->bic(WADDR, WADDR, Imm(3));
		break;
	}
	a->mov(WWB, a64::w4);                  // new base, survives helper calls

	if (in.op == JOP_STM)
	{
		ni     = a->new_label();
		needNi = true;
		emitIoRangeCheck(4 * (bitsSet(list) + ((list & 0x8000) ? 1 : 0)), ni);
		emitTrace();

		bool first = true;
		for (int r = 0; r < 15; r++)
		{
			if (!(list & (1u << r)))
				continue;
			a->mov(a64::w0, WADDR);
			a->ldr(a64::w1, gr(r));
			callHelper(a, (const void *)&CPUWriteMemory);
			if (in.flags & JF_WRITEBACK)
				a->str(WWB, gr(baseReg));   // STM writes back after *every* store
			addConst(1);
			if (first) addAccess32(WADDR); else addSeq32(WADDR);
			first = false;
			a->add(WADDR, WADDR, Imm(4));
		}
		if (list & 0x8000)
		{
			a->mov(a64::w0, WADDR);
			a->ldr(a64::w1, gr(15));
			a->add(a64::w1, a64::w1, Imm(4));
			callHelper(a, (const void *)&CPUWriteMemory);
			if (in.flags & JF_WRITEBACK)
				a->str(WWB, gr(baseReg));
			addConst(1);
			if (first) addAccess32(WADDR); else addSeq32(WADDR);
		}

		addFetch(in.fetchCycles);
		if (!thumb)
			addConst(2);                    // ARM's LDM/STM prologue; thumbC0 has none
		checkBudget(pc + step());
		queueNeedInterp(ni, needNi);
		return;
	}

	// LDM
	{
		bool first = true;
		for (int r = 0; r < 15; r++)
		{
			if (!(list & (1u << r)))
				continue;
			a->mov(a64::w0, WADDR);
			callHelper(a, (const void *)&CPUReadMemory);
			addConst(thumb ? 2 : 1);
			if (first) addAccess32(WADDR); else addSeq32(WADDR);
			first = false;
			a->str(a64::w0, gr(r));
			a->add(WADDR, WADDR, Imm(4));
		}

		if (thumb)
		{
			// thumbC8 writes the base back unless it is part of the list.
			if (!(list & (1u << baseReg)))
				a->str(WWB, gr(baseReg));
			addFetch(in.fetchCycles);
			checkBudget(pc + 2);
			return;
		}

		addFetch(in.fetchCycles);
		addConst(2);

		if (list & 0x8000)
		{
			a->mov(a64::w0, WADDR);
			callHelper(a, (const void *)&CPUReadMemory);
			addConst(2);
			if (first) addAccess32(WADDR); else addSeq32(WADDR);
			if ((in.flags & JF_WRITEBACK) && !(list & (1u << baseReg)))
				a->str(WWB, gr(baseReg));
			// ARM LDM {pc} does NOT mask the loaded address.
			emitExitReg(a64::w0, JITX_DONE);
			return;
		}

		if ((in.flags & JF_WRITEBACK) && !(list & (1u << baseReg)))
			a->str(WWB, gr(baseReg));
		checkBudget(pc + 4);
	}
}

void BlockEmitter::emitSwp(const JitInsn &in)
{
	Label ni = a->new_label();
	a->ldr(WADDR, gr(in.n));
	emitIoRangeCheck(in.sub == MEM_BYTE ? 1u : 4u, ni);
	emitTrace();

	if (in.sub == MEM_BYTE)
	{
		a->mov(a64::w0, WADDR);
		callHelper(a, (const void *)&CPUReadByte);
		a->mov(WWB, a64::w0);               // keep the loaded byte across the store
		a->mov(a64::w0, WADDR);
		a->ldrb(a64::w1, a64::ptr(XREG, in.m * 4));
		callHelper(a, (const void *)&CPUWriteByte);
		a->str(WWB, gr(in.d));
	}
	else
	{
		a->mov(a64::w0, WADDR);
		callHelper(a, (const void *)&CPUReadMemory);
		a->mov(WWB, a64::w0);
		a->mov(a64::w0, WADDR);
		a->ldr(a64::w1, gr(in.m));
		callHelper(a, (const void *)&CPUWriteMemory);
		a->str(WWB, gr(in.d));
	}

	addFetch(in.fetchCycles);              // V7 charges no data cycles for SWP
	checkBudget(pc + 4);
	queueNeedInterp(ni, true);
}

} // namespace

namespace {

// Add an arbitrary 32-bit constant to a register (branch displacements and
// rotated immediates are not always encodable as A64 add immediates).
static void addU32(a64::Assembler *as, const a64::Gp &dst, u32 v)
{
	if (v == 0)
		return;
	if (v <= 4095)
	{
		as->add(dst, dst, Imm(v));
		return;
	}
	emitMovImm32(as, a64::w4, v);
	as->add(dst, dst, a64::w4);
}

// ---------------------------------------------------------------------------
// Branches
// ---------------------------------------------------------------------------
void BlockEmitter::emitBranch(const JitInsn &in)
{
	if (thumb)
	{
		if (in.flags & JF_LINK)         // BL low half: the target comes from LR
		{
			a->ldr(a64::w3, gr(14));
			addU32(a, a64::w3, in.imm);
			a->bic(a64::w3, a64::w3, Imm(1));
			emitMovImm32(a, a64::w4, (pc + 2u) | 1u);
			a->str(a64::w4, gr(14));
			addFetch(in.fetchCycles);
			emitExitReg(a64::w3, JITX_DONE);
			return;
		}

		Label skip = a->new_label();
		emitCondSkip(in.cond, skip);

		// Taken: V7 *assigns* 3 to clockTicks for a conditional branch; the
		// unconditional form charges the usual fetch cost.
		if (in.cond == 0xE) addFetch(in.fetchCycles);
		else                addConst(3);
		emitChainExit(pc + 4u + in.imm);

		a->bind(skip);
		addFetch(in.fetchCycles);
		emitChainExit(pc + 2);
		return;
	}

	// ARM: lr = pc+4 for BL, target = pc+8+imm, base+3 cycles.
	Label skip = a->new_label();
	emitCondSkip(in.cond, skip);

	if (in.flags & JF_LINK)
	{
		emitMovImm32(a, a64::w3, pc + 4u);
		a->str(a64::w3, gr(14));
	}

	addFetch(in.fetchCycles);
	addConst(3);
	emitChainExit(pc + 8u + in.imm);

	a->bind(skip);
	addFetch(in.fetchCycles);
	emitChainExit(pc + 4);
}

void BlockEmitter::emitBx(const JitInsn &in)
{
	// armState = !(target & 1); the target is aligned to 4 (ARM) or 2 (Thumb).
	// `bx r15` reads pc+4 through reg[15], which is why needsPcBefore() must
	// have run setPc() before this instruction.
	a->ldr(a64::w3, gr(in.m));
	a->and_(a64::w4, a64::w3, Imm(1));
	a->eor(a64::w4, a64::w4, Imm(1));
	a->strb(a64::w4, fmem(&armState));

	Label arm = a->new_label();
	Label done = a->new_label();
	a->cmp(a64::w4, Imm(0));
	a->b(a64::CondCode::kNE, arm);
	a->bic(a64::w3, a64::w3, Imm(1));       // Thumb
	a->b(done);
	a->bind(arm);
	a->bic(a64::w3, a64::w3, Imm(3));       // ARM
	a->bind(done);

	addFetch(in.fetchCycles);
	addConst(thumb ? 0 : 3);
	emitExitReg(a64::w3, JITX_DONE);
}

// ---------------------------------------------------------------------------
// MRS / MSR
// ---------------------------------------------------------------------------
void BlockEmitter::emitMrs(const JitInsn &in)
{
	if (!in.sub)
		callHelper(a, (const void *)&CPUUpdateCPSR);    // arm100 resyncs reg[16]
	a->ldr(a64::w3, gr(in.sub ? 17 : 16));
	a->str(a64::w3, gr(in.d));
	addFetch(in.fetchCycles);
	checkBudget(pc + step());
}

void BlockEmitter::emitMsr(const JitInsn &in)
{
	// Only the SPSR form is compiled (nativeSupported), so no mode change and
	// no CPUUpdateCPSR: arm160/arm360 just merge the selected bytes, and
	// exclude User as well as System mode.
	if (in.flags & JF_IMM)
	{
		const u32 rot = (u32)(in.n & 0x0F) * 2;
		u32 v = in.imm & 0xFF;
		if (rot)
			v = (v >> rot) | (v << (32 - rot));
		emitMovImm32(a, a64::w4, v);
	}
	else
		a->ldr(a64::w4, gr(in.m));

	Label skip = a->new_label();
	a->ldr(a64::w3, a64::ptr(XFLAGS, offFlags(&armMode)));
	a->cmp(a64::w3, Imm(0x10));
	a->b(a64::CondCode::kLE, skip);
	a->cmp(a64::w3, Imm(0x1F));
	a->b(a64::CondCode::kGE, skip);

	for (int byte = 0; byte < 4; byte++)
	{
		if (!(in.d & (1 << byte)))
			continue;
		const u32 keep  = ~(0xFFu << (byte * 8));
		const u32 field = 0xFFu << (byte * 8);
		// All four keep masks (including the two-segment 0xFFFF00FF and
		// 0xFF00FFFF) are valid A64 logical immediates; the encoder probe
		// confirmed errors=0 for each of them.
		a->ldr(a64::w3, gr(17));
		a->and_(a64::w3, a64::w3, Imm(keep));
		a->mov(a64::w5, a64::w4);
		a->and_(a64::w5, a64::w5, Imm(field));
		a->orr(a64::w3, a64::w3, a64::w5);
		a->str(a64::w3, gr(17));
	}

	a->bind(skip);
	addFetch(in.fetchCycles);
	checkBudget(pc + step());
}

// ---------------------------------------------------------------------------
void BlockEmitter::emitTrace()
{
#if JIT_PC_TRACE
	if (!jitTraceEnabled || traceDone)
		return;
	traceDone = true;
	emitMovImm32(a, a64::w0, pc);
	callHelper(a, (const void *)&jitTracePc);
#endif
}

void BlockEmitter::emitOne(const JitInsn &in)
{
	traceDone = false;

	// Predication.  Every ARM instruction carries a condition and a false one
	// still costs its fetch cycles (jitInterpInsn() returns `base` before it
	// looks at the op).  Branch encodings handle their own taken/not-taken
	// pair, because their two outcomes leave the block at different addresses.
	if (in.cond != 0xE && in.op != JOP_BRANCH)
	{
		Label skip  = a->new_label();
		Label after = a->new_label();

		emitCondSkip(in.cond, skip);
		emitBody(in);
		a->b(after);
		a->bind(skip);
		addFetch(in.fetchCycles);
		checkBudget(pc + (u32)step());
		a->bind(after);
		return;
	}

	emitBody(in);
}

void BlockEmitter::emitBody(const JitInsn &in)
{
	// Instructions with no run-time bail can be traced here; the ones below
	// call emitTrace() themselves once their IO-range guard has passed.
	if (in.op != JOP_STORE && in.op != JOP_STM && in.op != JOP_PUSH && in.op != JOP_SWP)
		emitTrace();

	switch (in.op)
	{
	case JOP_ALU:    emitAlu(in); return;
	case JOP_MUL:    emitMul(in); return;
	case JOP_LOAD:
	case JOP_STORE:  emitLoadStore(in); return;
	case JOP_LDM:
	case JOP_STM:
	case JOP_PUSH:
	case JOP_POP:    emitBlockTransfer(in); return;
	case JOP_SWP:    emitSwp(in); return;
	case JOP_BRANCH: emitBranch(in); return;
	case JOP_BX:     emitBx(in); return;
	case JOP_MRS:    emitMrs(in); return;
	case JOP_MSR:    emitMsr(in); return;
	default:         return;
	}
}

bool BlockEmitter::compile(u16 *outCount)
{
	// Decide how long the prefix is before emitting anything.
	u16 n = 0;
	while (n < b->count && nativeSupported(b->insn[n], thumb))
	{
		if (touchesMemory(b->insn[n]))
			usesMem = true;
		n++;
	}
	if (n == 0)
		return false;

	count           = 0;
	epilogue        = a->new_label();
	chainBail       = a->new_label();
	needCount       = 0;
	budgetCount     = 0;
	nChain          = 0;

	// holdState cannot change inside a compiled block - every path that sets it
	// (a HALTCNT/STOP write) is an IO store, and those go back to the
	// interpreter - so runBlock()'s per-instruction test is invariant here.
	emitPrologue();

	// A linked predecessor jumps here: x19..x28 and the stack frame are already
	// set up by the block that started the chain.  A branch-only block emits no
	// per-instruction budget check of its own, so the guard lives at the chain
	// entry - without it a two-block jump loop could spin forever inside
	// generated code.
	chainEntryOffset = (u32)a->offset();
	a->cmp(WCYC, WBUD);
	a->b(a64::CondCode::kGE, chainBail);

	for (count = 0; count < n; count++)
	{
		pc = b->guestPc + (u32)count * (u32)step();
		const JitInsn &in = b->insn[count];

		// A branch computes its own target from pc, so it does not need reg[15]
		// to be set up.  Everything else does - including BX, because `bx r15`
		// is the Thumb->ARM switch idiom and reads pc+4 through reg[15].
		if (needsPcBefore(in))
			setPc();

		emitOne(in);
	}

	// Falling out of the last compiled instruction means the prefix ended
	// before the block did: hand the rest to the IR interpreter.
	emitChainExit(b->guestPc + (u32)n * (u32)step());

	emitEpilogue();
	emitNeedInterpStubs();
	emitBudgetStubs();
	emitChainBail();
	*outCount = n;
	return true;
}

// Debug aid: VBA_JIT_ASM=<file> appends every compiled block's IR dump and the
// raw machine code to that file.  VBA_JIT_ASM_LIMIT=<n> caps how many blocks
// are dumped (default 200, 0 = no cap).
void dumpCompiledBlock(const JitBlock *block, CodeHolder *code, u16 insns)
{
	static const char *path = getenv("VBA_JIT_ASM");
	if (!path || !path[0])
		return;

	static int  limit = -1;
	static int  dumped = 0;
	static bool announced = false;
	if (limit < 0)
	{
		const char *s = getenv("VBA_JIT_ASM_LIMIT");
		limit = (s && s[0]) ? (int)strtol(s, NULL, 0) : 200;
		if (limit < 0)
			limit = 200;
	}
	if (limit && dumped >= limit)
	{
		if (!announced)
		{
			announced = true;
			FILE *f = fopen(path, "a");
			if (f)
			{
				fprintf(f, "=== dump limit %d reached; further blocks omitted "
						   "(set VBA_JIT_ASM_LIMIT=0 for no limit)\n\n", limit);
				fclose(f);
			}
		}
		return;
	}
	dumped++;

	FILE *f = fopen(path, "a");
	if (!f)
		return;

	char text[4096];
	text[0] = 0;
	jitDumpBlock(block, text, (int)sizeof(text) - 1);

	const size_t n = code->code_size();
	u8 *bytes = (u8 *)malloc(n ? n : 1);
	if (!bytes)
	{
		fclose(f);
		return;
	}
	code->copy_flattened_data(bytes, n);

	fprintf(f, "=== compiled %u of %u insns at %08X, %u bytes\n%s\n", insns, block->count,
			block->guestPc, (unsigned)n, text);
	for (size_t i = 0; i < n; i++)
		fprintf(f, "%02X%s", bytes[i], ((i & 15) == 15) ? "\n" : " ");
	fprintf(f, "\n\n");
	fclose(f);
	free(bytes);
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

#if !defined(VBA_JIT_A64_EMITTER_ONLY)

namespace {

JitRuntime *jitRuntime()
{
	static JitRuntime rt;
	return &rt;
}

} // namespace

bool jitNativeAvailable()
{
	return checkedOffsets();
}

bool jitCompileBlock(const JitBlock *block, JitCompiledBlock *out)
{
	if (!block || !out || block->count == 0 || !jitNativeAvailable())
		return false;

	out->fn          = nullptr;
	out->chainEntry  = nullptr;
	out->insnCount   = 0;
	out->chainCount  = 0;
	out->selfSlot    = 0xFFFF;
	out->frameKind   = 0;
	out->reserved2   = 0;
	for (int i = 0; i < JIT_MAX_CHAIN; i++)
	{
		out->chain[i].target    = 0;
		out->chain[i].tag       = 0;
		out->chain[i].linkIndex = 0xFFFF;
		out->chain[i].reserved  = 0;
	}

	JitRuntime *rt = jitRuntime();

	// A fresh CodeHolder per compilation.  Reusing one through reset() looks
	// tempting but rt->add() rejects a reset holder, so the arena setup (a few
	// microseconds) stays.
	CodeHolder code;
	if (code.init(rt->environment()) != Error::kOk)
		return false;

	a64::Assembler assembler(&code);
	JitA64ErrorHandler eh;
	assembler.set_error_handler(&eh);

	BlockEmitter emitter(&assembler, block, out);
	u16 insns = 0;
	// An emission error is a hard failure: return false and let JitCache fall
	// back to the IR interpreter.  Never hand a partially emitted function to
	// rt->add().
	if (!emitter.compile(&insns) || insns == 0 || eh.count != 0 ||
		assembler.finalize() != Error::kOk)
		return false;

	dumpCompiledBlock(block, &code, insns);

	JitNativeFn fn = nullptr;
	if (rt->add(&fn, &code) != Error::kOk || !fn)
		return false;

	out->fn         = fn;
	out->insnCount  = insns;
	out->chainEntry = (u8 *)fn + emitter.chainEntryOffsetOf();
	out->chainCount = (u16)emitter.chainSlotsUsed();
	out->frameKind  = 0;               // A64 frames are all identical
	return true;
}

void jitReleaseBlock(JitCompiledBlock *block)
{
	if (!block || !block->fn)
		return;
	jitRuntime()->release(block->fn);
	block->fn         = nullptr;
	block->chainEntry = nullptr;
	block->insnCount  = 0;
	block->chainCount = 0;
	for (int i = 0; i < JIT_MAX_CHAIN; i++)
		block->chain[i].target = 0;
}

#endif // !VBA_JIT_A64_EMITTER_ONLY

#else // !VBA_JIT_A64 || !USE_GBA_CORE_V7

// Win32 / x64 builds get their implementation (or stub) from JitEmit.cpp and
// do not compile this file at all.  V8 on ARM64 does compile it, and must see
// exactly the same three inert entry points as today's non-x64 stub.
bool jitNativeAvailable() { return false; }

bool jitCompileBlock(const JitBlock *block, JitCompiledBlock *out)
{
	(void)block;
	(void)out;
	return false;
}

void jitReleaseBlock(JitCompiledBlock *block) { (void)block; }

#endif
