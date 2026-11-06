// x64 machine-code backend for the V7 core's IR.
//
// One JitBlock becomes one native function.  The interesting part is not the
// instruction selection - it is that every observable detail of the interpreter
// has to survive the translation:
//
//   * cycles are charged per instruction, not per block.  An IRQ has to land on
//     the same instruction as it does in arm-new.h/thumb.h or a recorded movie
//     desynchronises (documentation/jit-recompiler.md 1.2);
//   * reg[15] is pc+4/8 while an instruction runs, because the memory helpers
//     read it for open-bus values;
//   * flags live in the emulator's N_FLAG/C_FLAG/Z_FLAG/V_FLAG bytes, not in a
//     host register, because CPUUpdateCPSR() and the next block read them;
//   * a store into the IO block has to go back to the interpreter, because
//     CPUWriteMemoryWrapped() can call CPUCheckDMA() and rewrite CPULoop's own
//     event counter through extCpuNextEvent.
//
// Anything this file cannot express is not compiled: jitCompileBlock() stops the
// prefix in front of it and the block runner continues with JitInterp.cpp.  That
// makes the backend's failure mode "slower", never "different".
//
// Register use inside generated code (Windows x64 ABI):
//
//   rbx   &reg[0]; every emulator global is addressed as [rbx + disp32], where
//         disp32 is computed at compile time from the real link-time addresses
//   r12d  cycles executed so far
//   r13d  the cycle budget
//   r14   JitExecResult* (out)
//   rdi   memory address of the current load/store (survives a helper call)
//   rsi   writeback value of the current load/store (survives a helper call)
//   rax rcx rdx r8 r9 r10 r11   scratch; clobbered by helper calls

#include "JitEmit.h"

#include "JitCache.h"		// JIT_PC_TRACE
#include "JitIr.h"

#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
#define VBA_JIT_X64 1
#else
#define VBA_JIT_X64 0
#endif

#if VBA_JIT_X64 && defined(USE_GBA_CORE_V7)

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <asmjit/core.h>
#include <asmjit/core/jit_runtime.h>
#include <asmjit/x86.h>

#include "../GBAGlobals.h"		// reg[], N_FLAG..V_FLAG, armState, armNextPC
#include "../GBAinline.h"		// memoryMap, for CPUReadMemoryQuick and friends

// ---------------------------------------------------------------------------
// Emulator state.  Declared here rather than by including GBA.h / V7/GBACpu.h:
// those pull in UPDATE_REG and the handler macros, and this file has to stay
// buildable as part of the standalone JIT sources.
// ---------------------------------------------------------------------------

extern bool8 holdState;

extern int32 memoryWait[16];
extern int32 memoryWait32[16];
extern int32 memoryWaitSeq[16];
extern int32 memoryWaitSeq32[16];
extern int32 memoryWaitFetch[16];
extern int32 memoryWaitFetch32[16];

extern u32	CPUReadMemory(u32 address);
extern u32	CPUReadHalfWord(u32 address);		// V7 returns it zero-extended
extern u16	CPUReadHalfWordSigned(u32 address);
extern u8	CPUReadByte(u32 address);
extern void CPUWriteMemory(u32 address, u32 value);
extern void CPUWriteHalfWord(u32 address, u16 value);
extern void CPUWriteByte(u32 address, u8 value);
extern void CPUUpdateCPSR();

// JitCache.cpp, JIT_PC_TRACE only.
extern void jitTracePc(u32 pc);

namespace {

// ---------------------------------------------------------------------------
// Helper called by generated code for the shift-by-register forms.
//
// These are the one place where the interpreter's own behaviour is not
// portable: arm-new.h shifts a u32 by a raw byte with no clamp (UB for counts
// >= 32, and the ARM path is guarded before it ever gets here), while Thumb
// spells the >=32 cases out.  Keeping the corner cases in C++ instead of in
// emitted branches is worth one call - register-scaled shifts are rare in hot
// code, and the differential verifier compares them instruction by instruction.
//
// Returns the shifted value.  gJitShiftCarry receives 0/1 when the shift
// defines a carry and 0xFF when it leaves C alone (a raw count of 0 in the
// non-Thumb forms).
// ---------------------------------------------------------------------------

u8 gJitShiftCarry = 0xFF;

enum ShiftKind
{
	SHK_LSL,		// ARM lsl Rd, Rm, Rs (raw < 32 guaranteed by the caller)
	SHK_LSR,
	SHK_ASR,
	SHK_LSL_T,		// Thumb format 4: 0 means "no shift", 32 means "shift out"
	SHK_LSR_T,
	SHK_ASR_T,
	SHK_ROR_T,		// Thumb ror Rd, Rs: a count of 0 or 32 still writes C
	SHK_ROR_V7		// V7's ARM "ror Rd, Rm, Rs": same, for any masked-to-0 count
};

u32 jitShiftReg(u32 v, u32 raw, u32 carryIn, u32 kind)
{
	(void)carryIn;
	gJitShiftCarry = 0xFF;

	switch (kind)
	{
	case SHK_LSL:
		if (raw == 0)
			return v;
		gJitShiftCarry = (u8)((v >> ((32 - raw) & 31)) & 1);
		return v << (raw & 31);

	case SHK_LSR:
		if (raw == 0)
			return v;
		gJitShiftCarry = (u8)((v >> ((raw - 1) & 31)) & 1);
		return v >> (raw & 31);

	case SHK_ASR:
		if (raw == 0)
			return v;
		gJitShiftCarry = (u8)(((u32)((s32)v >> ((raw - 1) & 31))) & 1);
		return (u32)((s32)v >> (raw & 31));

	case SHK_LSL_T:
		if (raw == 0)
			return v;
		if (raw < 32)
		{
			gJitShiftCarry = (u8)((v >> (32 - raw)) & 1);
			return v << raw;
		}
		gJitShiftCarry = (raw == 32) ? (u8)(v & 1) : 0;
		return 0;

	case SHK_LSR_T:
		if (raw == 0)
			return v;
		if (raw < 32)
		{
			gJitShiftCarry = (u8)((v >> (raw - 1)) & 1);
			return v >> raw;
		}
		gJitShiftCarry = (raw == 32) ? (u8)((v >> 31) & 1) : 0;
		return 0;

	case SHK_ASR_T:
		if (raw == 0)
			return v;
		if (raw < 32)
		{
			gJitShiftCarry = (u8)(((u32)((s32)v >> (raw - 1))) & 1);
			return (u32)((s32)v >> raw);
		}
		gJitShiftCarry = (v & 0x80000000) ? 1 : 0;
		return (v & 0x80000000) ? 0xFFFFFFFFu : 0;

	case SHK_ROR_T:
		if (raw == 0)
			return v;
		// fall through
	case SHK_ROR_V7:
	{
		const u32 n = raw & 31;
		if (n == 0)
		{
			gJitShiftCarry = (u8)((v >> 31) & 1);
			return v;
		}
		const u32 r = (v >> n) | (v << (32 - n));
		gJitShiftCarry = (u8)((r >> 31) & 1);
		return r;
	}

	default:
		return v;
	}
}

u32 jitReadQuick(u32 address) { return CPUReadMemoryQuick(address); }

// ---------------------------------------------------------------------------
// Address helpers
// ---------------------------------------------------------------------------

// The block compiler addresses every emulator global as [rbx + disp32], with
// rbx holding &reg[0].  That works because all of them live in the same image;
// checkedDisplacements() checks once that nothing is more than 2 GB away.
bool gDispOk = false;

#if JIT_PC_TRACE
// Off unless VBA_JIT_PCTRACE names a file: the block compiler only emits the
// trace call when a trace is actually being collected.
bool jitTraceEnabled = getenv("VBA_JIT_PCTRACE") != nullptr;
#endif

inline int32_t dispOf(const void *p)
{
	return (int32_t)((intptr_t)p - (intptr_t)&reg[0]);
}

inline asmjit::x86::Mem gmem(const void *p, uint32_t size = 4)
{
	return asmjit::x86::ptr(asmjit::x86::rbx, dispOf(p), size);
}

inline int bitsSet(u32 v)
{
	int n = 0;
	while (v)
	{
		v &= v - 1;
		n++;
	}
	return n;
}

inline bool isRegShift(u8 shift)
{
	return (shift >= SHIFT_LSL_REG && shift <= SHIFT_ROR_REG_T) || shift == SHIFT_ROR_REG_V7;
}

} // namespace

// ---------------------------------------------------------------------------
// The emitter
// ---------------------------------------------------------------------------

namespace {

using namespace asmjit;

// Carry produced by the second operand: unchanged, a constant, or whatever
// jitShiftReg() left in gJitShiftCarry (0xFF = unchanged).
struct Carry
{
	enum Kind { kKeep, kConst, kGlobal } kind;
	u32 value;
};

class BlockEmitter
{
public:
	BlockEmitter(x86::Assembler *assembler, const JitBlock *block, JitCompiledBlock *out)
		: a(assembler), b(block), outBlock(out), thumb(block->thumb != 0), pc(0),
		  usesMem(false), epilogue(), count(0), chainEntryOffset(0), nChain(0)
	{
	}

	bool compile(u16 *outCount);

	u32	 chainEntryOffsetOf() const { return chainEntryOffset; }
	int	 chainSlotsUsed() const { return nChain; }
	bool usesMemoryFrame() const { return usesMem; }

private:
	x86::Assembler	 *a;
	const JitBlock	 *b;
	JitCompiledBlock *outBlock;	// where the chain slots and the entry live
	bool			  thumb;
	u32				  pc;		// address of the instruction being emitted
	bool			  usesMem;	// rdi/rsi have to be preserved
	Label			  epilogue;
	Label			  chainBail;	// budget exhausted at a chained entry
	u16				  count;	// instructions emitted so far
	u32				  chainEntryOffset;	// offset of the label after the prologue
	int				  nChain;	// chain slots handed out so far

	// Pending NEED_INTERP stubs: bound after the epilogue so the fall-through
	// path can never reach them.  A block has at most JIT_MAX_INSNS of them.
	Label			 needLabel[JIT_MAX_INSNS * 2];
	u32				 needPc[JIT_MAX_INSNS * 2];
	int				 needCount;
	// Budget exits are collected the same way: a stub per instruction in the
	// middle of the block would triple its size and push the hot instructions
	// out of the instruction cache.
	Label			 budgetLabel[JIT_MAX_INSNS * 2];
	u32				 budgetPc[JIT_MAX_INSNS * 2];
	int				 budgetCount;
	bool			 traceDone;

	// --- addresses ---------------------------------------------------------
	x86::Mem gr(int r, uint32_t size = 4) { return x86::ptr(x86::rbx, r * 4, size); }
	int		 step() const { return thumb ? 2 : 4; }

	// --- prologue / epilogue ----------------------------------------------
	void emitPrologue();
	void emitEpilogue();

	// --- exits -------------------------------------------------------------
	void emitExitImm(u32 nextPc, u32 status);
	void emitExitReg(const x86::Gp &nextPc, u32 status);
	// JITX_DONE exit with a known successor PC: jump straight into an already
	// linked successor instead of returning through jitExecute().
	void emitChainExit(u32 nextPc);
	// The "hand this instruction to the interpreter" exit is only reachable via
	// a jump from the guard that raises it, so its stub is emitted after the
	// epilogue instead of inline: the normal path must not fall into it.
	void queueNeedInterp(const Label &ni, bool used);
	void emitNeedInterpStubs();
	void emitBudgetStubs();
	void emitChainBail();

	// "total >= budget" -> leave the block at nextPc
	void checkBudget(u32 nextPc);

	// --- per-instruction building blocks ----------------------------------
	void setPc();								// reg[15] = pc + 4/8
	void addFetch(u8 fetchCycles);				// the instruction's base cost
	void addConst(int cycles) { if (cycles) a->add(x86::r12d, cycles); }
	void addWait(const x86::Gp &addr, const int32_t *table);
	void addAccess32(const x86::Gp &addr) { addWait(addr, memoryWait32); }
	void addAccess16(const x86::Gp &addr) { addWait(addr, memoryWait); }
	void addSeq32(const x86::Gp &addr) { addWait(addr, memoryWaitSeq32); }

	void emitCondSkip(u8 cond, const Label &skip);
	void emitIoRangeCheck(u32 size, const Label &ni);

	void emitOperand2(const JitInsn &in, const x86::Gp &dst, Carry *carry);
	void emitOffset(const JitInsn &in, const x86::Gp &dst);
	void writeCarry(const Carry &carry);

	void emitNZ(const x86::Gp &res, u8 mask);
	// ADD/SUB without a carry-in: the host's SF/ZF/CF/OF *are* the guest's
	// N/Z/C/V, so four setcc-to-memory replace the 15-instruction xor/shr/
	// 64-bit-carry chain.  See the note above emitFlagsNative().
	void emitFlagsNative(bool subtract, u8 mask);
	void emitFlagsLogical(const x86::Gp &res, const Carry &carry, u8 mask);
	void emitFlagsAdd(const x86::Gp &lhs, const x86::Gp &rhs, const x86::Gp &res,
					  bool carryIn, u8 mask);
	void emitFlagsSub(const x86::Gp &lhs, const x86::Gp &rhs, const x86::Gp &res,
					  bool borrowIn, u8 mask);

	// --- instruction emitters ---------------------------------------------
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
	// The PC trace has to fire once per *executed* instruction, so an
	// instruction that can bail to the interpreter at run time (a store into
	// the IO block) emits its trace call after that check instead of before it.
	void emitTrace();
};

// A helper call: the callee's address is an imm64, the result lands in rax.
void callHelper(asmjit::x86::Assembler *a, const void *fn)
{
	a->mov(x86::rax, Imm((int64_t)(uintptr_t)fn));
	a->call(x86::rax);
}

// Instructions this backend refuses.  Every "no" costs a little speed (the
// prefix ends in front of the instruction and the IR interpreter runs the rest
// of the block) and buys exactness, so the list is deliberately conservative.
bool nativeSupported(const JitInsn &in, bool thumb)
{
	// Debug: VBA_JIT_OPS=<mask> restricts which JitOp values get compiled, so a
	// miscompile can be bisected by instruction class without a rebuild.
	// Parsed exactly once: `VBA_JIT_OPS=0` is a meaningful value ("refuse
	// everything"), so 0 cannot double as the "not read yet" marker.
	static u32	opMask	= 0;
	static bool opMaskInited = false;
	if (!opMaskInited)
	{
		const char *s = getenv("VBA_JIT_OPS");
		opMask		  = (s && s[0]) ? (u32)strtoul(s, NULL, 0) : 0xFFFFFFFFu;
		opMaskInited  = true;
	}
	if (!((opMask >> in.op) & 1))
		return false;

	switch (in.op)
	{
	case JOP_ALU:
		// A data-processing write to PC with S=1 calls CPUSwitchMode(..., false)
		// -> CPUUpdateFlags(true) -> CPU_BREAK_LOOP_2, i.e. it rewrites
		// CPULoop's locals.  The S=0 form is an ordinary jump and is compiled.
		if (in.d == 15 && in.flagMask != 0)
			return false;
		return true;

	case JOP_MUL:
		// The long forms write RdLo (imm & 0xF) as well as RdHi (d).  With
		// either of them equal to 15 the instruction changes reg[15] without
		// branching, which is the one-instruction detour a block cannot model.
		return in.d != 15 && (in.imm & 0x0F) != 15;

	case JOP_LOAD:
		// Only the pre-indexed ARM word load into PC is a jump V7 implements as
		// one; the byte/halfword forms and the post-indexed word form send the
		// interpreter on a one-instruction detour (3.2.1.14 item 4).
		if (in.d == 15 && (thumb || in.sub != MEM_WORD || !(in.flags & JF_PRE)))
			return false;
		// `ldr rX, [r15, #k]!` writes the *base* back, which is PC: a PC write
		// that is not a branch, i.e. the same detour.
		if (in.n == 15 && (in.flags & JF_WRITEBACK))
			return false;
		return true;

	case JOP_STORE:
		// A writeback whose base is PC changes reg[15] without being a branch:
		// the interpreter then runs one instruction from the sequential address
		// and only the one after that jumps.  Not expressible in a block.
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
		// A word SWP into PC writes reg[15] without branching (doSwp() leaves
		// armNextPC at pc+4) - same detour problem as a MUL into PC.
		return in.d != 15;

	case JOP_MRS:
		return in.d != 15;

	case JOP_MSR:
		// Only the SPSR form is decoded for V7, and only that one is safe here.
		return in.sub != 0;

	case JOP_SWI:
	case JOP_UNDEF:
	case JOP_END:
	default:
		return false;
	}
}

// Does this instruction need reg[15] to hold pc+4/8 before it runs?  Only
// instructions that can look at PC do, plus everything that calls a memory
// helper (the helpers read reg[15] for open-bus values).  Skipping the store for
// the rest is one instruction and seven bytes per guest instruction, which is
// worth having: `add r0, r1` does not care about PC.
bool needsPcBefore(const JitInsn &in)
{
	switch (in.op)
	{
	case JOP_ALU:
		if (in.n == 15)
			return true;
		if (in.flags & JF_IMM)
			return false;
		if (in.m == 15)
			return true;
		// The shift count register (if any) can be PC as well - `bx r15` is the
		// Thumb->ARM switch idiom, and `mov r0, r1, lsl r15` reads the same PC.
		return isRegShift(in.shift) && (in.imm & 0x0F) == 15;

	case JOP_MUL:
		return in.n == 15 || in.m == 15;

	case JOP_BRANCH:
		// Computes its target from pc in the emitter, so it never reads reg[15].
		return false;

	default:
		return true;
	}
}

bool touchesMemory(const JitInsn &in){
	switch (in.op)
	{
	case JOP_LOAD:
	case JOP_STORE:
	case JOP_LDM:
	case JOP_STM:
	case JOP_PUSH:
	case JOP_POP:
	case JOP_SWP:
		return true;
	default:
		return false;
	}
}

// ---------------------------------------------------------------------------
// Prologue / epilogue
// ---------------------------------------------------------------------------

void BlockEmitter::emitPrologue()
{
	// rbx, r12, r13, r14 always; rdi/rsi only when a helper call needs the
	// address and the writeback value across it.  The push count has to stay
	// odd so that rsp is 16-byte aligned at the calls (the return address makes
	// it 8 mod 16 on entry); rbp is the padding.
	a->push(x86::rbx);
	a->push(x86::r12);
	a->push(x86::r13);
	a->push(x86::r14);
	a->push(x86::rbp);
	if (usesMem)
	{
		a->push(x86::rdi);
		a->push(x86::rsi);
	}

	a->sub(x86::rsp, 32);	// shadow space for the helper calls

	a->mov(x86::rbx, Imm((int64_t)(uintptr_t)&reg[0]));
	a->mov(x86::r13d, x86::ecx);
	a->mov(x86::r14, x86::rdx);
	a->xor_(x86::r12d, x86::r12d);
}

void BlockEmitter::emitEpilogue()
{
	a->bind(epilogue);
	a->mov(x86::eax, x86::r12d);
	a->add(x86::rsp, 32);
	if (usesMem)
	{
		a->pop(x86::rsi);
		a->pop(x86::rdi);
	}
	a->pop(x86::rbp);
	a->pop(x86::r14);
	a->pop(x86::r13);
	a->pop(x86::r12);
	a->pop(x86::rbx);
	a->ret();
}

void BlockEmitter::emitExitImm(u32 nextPc, u32 status)
{
	a->mov(x86::ptr(x86::r14, 0, 4), nextPc);
	a->mov(x86::ptr(x86::r14, 4, 4), status);
	a->jmp(epilogue);
}

void BlockEmitter::emitExitReg(const x86::Gp &nextPc, u32 status)
{
	a->mov(x86::ptr(x86::r14, 0, 4), nextPc);
	a->mov(x86::ptr(x86::r14, 4, 4), status);
	a->jmp(epilogue);
}

void BlockEmitter::emitChainExit(u32 nextPc)
{
	// The unlinked path has to hand nextPc/status back to runBlock(); a linked
	// successor overwrites both at its own exit, so the stores sit on that side
	// of the test and cost nothing while the chain holds.
	if (!outBlock || nChain >= JIT_MAX_CHAIN)
	{
		a->mov(x86::ptr(x86::r14, 0, 4), nextPc);
		a->mov(x86::ptr(x86::r14, 4, 4), JITX_DONE);
		a->jmp(epilogue);
		return;
	}

	JitChainSlot *slot = &outBlock->chain[nChain++];
	slot->target	= 0;
	slot->tag		= (nextPc & 0xFFFFFFFEu) | (thumb ? 1u : 0u);
	slot->linkIndex = 0xFFFF;
	slot->reserved	= 0;

	Label unlinked = a->new_label();

	// movabs rax, &slot->target; mov rax, [rax].  An absolute address rather
	// than a RIP-relative one: the code page comes from VirtualAlloc and can be
	// far away from the static data.
	a->mov(x86::rax, Imm((int64_t)(uintptr_t)&slot->target));
	a->mov(x86::rax, x86::ptr(x86::rax));
	a->test(x86::rax, x86::rax);
	a->je(unlinked);
	a->jmp(x86::rax);

	a->bind(unlinked);
	a->mov(x86::ptr(x86::r14, 0, 4), nextPc);
	a->mov(x86::ptr(x86::r14, 4, 4), JITX_DONE);
	a->jmp(epilogue);
}

void BlockEmitter::queueNeedInterp(const Label &ni, bool used)
{
	if (!used)
		return;
	needPc[needCount]	 = pc;
	needLabel[needCount] = ni;
	needCount++;
}

void BlockEmitter::emitNeedInterpStubs()
{
	for (int i = 0; i < needCount; i++)
	{
		a->bind(needLabel[i]);
		const u32 saved = pc;
		pc				= needPc[i];
		emitExitImm(pc, JITX_NEED_INTERP);
		pc = saved;
	}
}

void BlockEmitter::checkBudget(u32 nextPc)
{
	Label stub = a->new_label();
	a->cmp(x86::r12d, x86::r13d);
	a->jge(stub);

	budgetLabel[budgetCount] = stub;
	budgetPc[budgetCount]	 = nextPc;
	budgetCount++;
}

void BlockEmitter::emitChainBail()
{
	// Only reachable from the chain-entry guard (a predecessor jumped here
	// after already spending the budget).  Stop *before* this block.
	a->bind(chainBail);
	a->mov(x86::ptr(x86::r14, 0, 4), b->guestPc);
	a->mov(x86::ptr(x86::r14, 4, 4), JITX_BUDGET);
	a->jmp(epilogue);
}

void BlockEmitter::emitBudgetStubs()
{
	for (int i = 0; i < budgetCount; i++)
	{
		a->bind(budgetLabel[i]);
		const u32 saved = pc;
		pc				= budgetPc[i];
		emitExitImm(pc, JITX_BUDGET);
		pc = saved;
	}
}

// ---------------------------------------------------------------------------
// Cycle accounting
// ---------------------------------------------------------------------------

void BlockEmitter::setPc()
{
	// arm-new.h/thumb.h leave reg[15] at pc+8 (ARM) or pc+4 (Thumb) while an
	// instruction runs, and the memory helpers read it for open-bus values.
	a->mov(gmem(&reg[15], 4), pc + (thumb ? 4u : 8u));
}

void BlockEmitter::addFetch(u8 fetchCycles)
{
	// ARM: memoryWaitFetch32[pc >> 24]; Thumb: thumbCycles[opcode >> 8] (baked
	// into the IR as fetchCycles) plus memoryWaitFetch[pc >> 24].  The index is
	// a compile-time constant because pc is; only the entry is runtime.
	const int i = (int)((pc >> 24) & 15);
	if (thumb)
		addConst(fetchCycles);
	a->add(x86::r12d, gmem(thumb ? (const void *)&memoryWaitFetch[i]
								 : (const void *)&memoryWaitFetch32[i], 4));
}

void BlockEmitter::addWait(const x86::Gp &addr, const int32_t *table)
{
	a->mov(x86::r8d, addr);
	a->shr(x86::r8d, 24);
	a->and_(x86::r8d, 15);
	a->add(x86::r12d, x86::ptr(x86::rbx, x86::r8, 2, dispOf(table), 4));
}

// ---------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------

void BlockEmitter::emitCondSkip(u8 cond, const Label &skip)
{
	// Jump to `skip` when the condition is false.  The flags are the emulator's
	// own bytes, so every test is one or two loads; 0xE (the only predicate
	// unconditional ARM/Thumb code uses) costs nothing.
	switch (cond)
	{
	case 0x0:
		a->cmp(gmem(&Z_FLAG, 1), 0);
		a->je(skip);
		break;
	case 0x1:
		a->cmp(gmem(&Z_FLAG, 1), 0);
		a->jne(skip);
		break;
	case 0x2:
		a->cmp(gmem(&C_FLAG, 1), 0);
		a->je(skip);
		break;
	case 0x3:
		a->cmp(gmem(&C_FLAG, 1), 0);
		a->jne(skip);
		break;
	case 0x4:
		a->cmp(gmem(&N_FLAG, 1), 0);
		a->je(skip);
		break;
	case 0x5:
		a->cmp(gmem(&N_FLAG, 1), 0);
		a->jne(skip);
		break;
	case 0x6:
		a->cmp(gmem(&V_FLAG, 1), 0);
		a->je(skip);
		break;
	case 0x7:
		a->cmp(gmem(&V_FLAG, 1), 0);
		a->jne(skip);
		break;
	case 0x8:		// HI: C && !Z
		a->cmp(gmem(&C_FLAG, 1), 0);
		a->je(skip);
		a->cmp(gmem(&Z_FLAG, 1), 0);
		a->jne(skip);
		break;
	case 0x9:		// LS: !C || Z
	{
		Label ok = a->new_label();
		a->cmp(gmem(&C_FLAG, 1), 0);
		a->je(ok);
		a->cmp(gmem(&Z_FLAG, 1), 0);
		a->je(skip);
		a->bind(ok);
		break;
	}
	case 0xA:		// GE: N == V
		a->mov(x86::al, gmem(&N_FLAG, 1));
		a->cmp(x86::al, gmem(&V_FLAG, 1));
		a->jne(skip);
		break;
	case 0xB:		// LT: N != V
		a->mov(x86::al, gmem(&N_FLAG, 1));
		a->cmp(x86::al, gmem(&V_FLAG, 1));
		a->je(skip);
		break;
	case 0xC:		// GT: !Z && N == V
		a->cmp(gmem(&Z_FLAG, 1), 0);
		a->jne(skip);
		a->mov(x86::al, gmem(&N_FLAG, 1));
		a->cmp(x86::al, gmem(&V_FLAG, 1));
		a->jne(skip);
		break;
	case 0xD:		// LE: Z || N != V
	{
		Label ok = a->new_label();
		a->cmp(gmem(&Z_FLAG, 1), 0);
		a->jne(ok);
		a->mov(x86::al, gmem(&N_FLAG, 1));
		a->cmp(x86::al, gmem(&V_FLAG, 1));
		a->je(skip);
		a->bind(ok);
		break;
	}
	case 0xF:
		a->jmp(skip);
		break;
	default:		// 0xE: always
		break;
	}
}

// A store into the IO block can start a DMA transfer; CPUWriteMemoryWrapped()
// calls CPUCheckDMA(), which rewrites CPULoop's cpuNextEvent through ext*.
// hitsIo() checks the first byte and the last one of the access.
void BlockEmitter::emitIoRangeCheck(u32 size, const Label &ni)
{
	a->mov(x86::eax, x86::edi);
	a->and_(x86::eax, 0x0F000000);
	a->cmp(x86::eax, 0x04000000);
	a->je(ni);
	if (size > 1)
	{
		a->mov(x86::eax, x86::edi);
		a->add(x86::eax, (int)size - 1);
		a->and_(x86::eax, 0x0F000000);
		a->cmp(x86::eax, 0x04000000);
		a->je(ni);
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
		a->mov(gmem(&C_FLAG, 1), carry.value ? 1 : 0);
		return;
	default:
		// jitShiftReg() leaves 0xFF when the shift does not define a carry.
	{
		Label done = a->new_label();
		a->mov(x86::al, gmem(&gJitShiftCarry, 1));
		a->cmp(x86::al, 0xFF);
		a->je(done);
		a->mov(gmem(&C_FLAG, 1), x86::al);
		a->bind(done);
		return;
	}
	}
}

void BlockEmitter::emitOperand2(const JitInsn &in, const x86::Gp &dst, Carry *carry)
{
	carry->kind	 = Carry::kKeep;
	carry->value = 0;

	if (in.flags & JF_IMM)
	{
		if (in.shift != SHIFT_IMM_ROT)
		{
			a->mov(dst, in.imm);
			return;
		}
		// Rotated immediate: the value and the carry are both constants.
		const u32 rot  = (in.imm >> 8) & 0x0F;
		const u32 imm8 = in.imm & 0xFF;
		if (rot == 0)
		{
			a->mov(dst, imm8);
			return;
		}
		const u32 amount = rot * 2;
		a->mov(dst, (imm8 >> amount) | (imm8 << (32 - amount)));
		carry->kind	 = Carry::kConst;
		carry->value = (imm8 >> (amount - 1)) & 1;
		return;
	}

	const u8 rm = (u8)(in.m & 0x0F);

	switch (in.shift)
	{
	case SHIFT_NONE:
		a->mov(dst, gr(rm));
		return;

	case SHIFT_LSL_IMM:
		a->mov(dst, gr(rm));
		if (in.imm == 0)
			return;
		a->mov(x86::ecx, dst);
		a->shr(x86::ecx, 32 - in.imm);
		a->and_(x86::ecx, 1);
		a->mov(gmem(&gJitShiftCarry, 1), x86::cl);
		carry->kind = Carry::kGlobal;
		a->shl(dst, in.imm);
		return;

	case SHIFT_LSR_IMM:
		a->mov(dst, gr(rm));
		a->mov(x86::ecx, dst);
		if (in.imm == 0)
		{
			a->shr(x86::ecx, 31);
			a->mov(gmem(&gJitShiftCarry, 1), x86::cl);
			carry->kind = Carry::kGlobal;
			a->xor_(dst, dst);
			return;
		}
		a->shr(x86::ecx, in.imm - 1);
		a->and_(x86::ecx, 1);
		a->mov(gmem(&gJitShiftCarry, 1), x86::cl);
		carry->kind = Carry::kGlobal;
		a->shr(dst, in.imm);
		return;

	case SHIFT_ASR_IMM:
		a->mov(dst, gr(rm));
		a->mov(x86::ecx, dst);
		if (in.imm == 0)
		{
			a->shr(x86::ecx, 31);
			a->mov(gmem(&gJitShiftCarry, 1), x86::cl);
			carry->kind = Carry::kGlobal;
			a->sar(dst, 31);
			return;
		}
		a->sar(x86::ecx, in.imm - 1);
		a->and_(x86::ecx, 1);
		a->mov(gmem(&gJitShiftCarry, 1), x86::cl);
		carry->kind = Carry::kGlobal;
		a->sar(dst, in.imm);
		return;

	case SHIFT_ROR_IMM:
		a->mov(dst, gr(rm));
		if (in.imm == 0)		// RRX: the incoming carry becomes bit 31
		{
			a->mov(x86::ecx, dst);
			a->and_(x86::ecx, 1);
			a->mov(gmem(&gJitShiftCarry, 1), x86::cl);
			carry->kind = Carry::kGlobal;
			a->shr(dst, 1);
			a->mov(x86::ecx, gmem(&C_FLAG, 1));
			a->shl(x86::ecx, 31);
			a->or_(dst, x86::ecx);
			return;
		}
		a->ror(dst, in.imm);
		a->mov(x86::ecx, dst);
		a->shr(x86::ecx, 31);
		a->mov(gmem(&gJitShiftCarry, 1), x86::cl);
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
		if (in.shift == SHIFT_LSL_REG)
			kind = thumb ? SHK_LSL_T : SHK_LSL;
		else if (in.shift == SHIFT_LSR_REG)
			kind = thumb ? SHK_LSR_T : SHK_LSR;
		else if (in.shift == SHIFT_ASR_REG)
			kind = thumb ? SHK_ASR_T : SHK_ASR;
		else if (in.shift == SHIFT_ROR_REG_V7)
			kind = SHK_ROR_V7;
		else
			kind = SHK_ROR_T;

		a->mov(x86::ecx, gr(rm));
		a->movzx(x86::edx, x86::ptr(x86::rbx, (int)(in.imm & 0x0F) * 4, 1));
		a->movzx(x86::r8d, gmem(&C_FLAG, 1));
		a->mov(x86::r9d, kind);
		callHelper(a, (const void *)&jitShiftReg);
		a->mov(dst, x86::eax);
		carry->kind = Carry::kGlobal;
		return;
	}

	default:
		a->mov(dst, gr(rm));
		return;
	}
}

// Load/store offset (OFFSET_* semantics: lsr #0 means 0, asr #0 a full sign
// fill, ror #0 means RRX).
void BlockEmitter::emitOffset(const JitInsn &in, const x86::Gp &dst)
{
	const u8 rm = (u8)(in.m & 0x0F);

	switch (in.shift)
	{
	case SHIFT_LSL_IMM:
		a->mov(dst, gr(rm));
		if (in.imm)
			a->shl(dst, in.imm);
		return;
	case SHIFT_LSR_IMM:
		a->mov(dst, gr(rm));
		if (in.imm == 0)
			a->xor_(dst, dst);
		else
			a->shr(dst, in.imm);
		return;
	case SHIFT_ASR_IMM:
		a->mov(dst, gr(rm));
		if (in.imm == 0)
			a->sar(dst, 31);
		else
			a->sar(dst, in.imm);
		return;
	case SHIFT_ROR_IMM:
		a->mov(dst, gr(rm));
		if (in.imm)
		{
			a->ror(dst, in.imm);
			return;
		}
		a->shr(dst, 1);
		a->mov(x86::ecx, gmem(&C_FLAG, 1));
		a->shl(x86::ecx, 31);
		a->or_(dst, x86::ecx);
		return;
	default:
		a->mov(dst, gr(rm));
		return;
	}
}

void BlockEmitter::emitNZ(const x86::Gp &res, u8 mask)
{
	if ((mask & (JITF_N | JITF_Z)) == 0)
		return;
	// SETcc accepts a byte memory operand, so there is no need to bounce the
	// value through al.  Two instructions instead of five.
	a->test(res, res);		// SETcc does not disturb the flags
	if (mask & JITF_N)
		a->sets(gmem(&N_FLAG, 1));
	if (mask & JITF_Z)
		a->setz(gmem(&Z_FLAG, 1));
}

// ADD/SUB (and their compare-only forms) leave exactly the guest flags in the
// host's flags register:
//
//   N = SF   (bit 31 of the result)       Z = ZF   (result == 0)
//   V = OF   (signed overflow)            C = CF   (ADD) / !CF (SUB, no borrow)
//
// The old path recomputed all four from the operands: V needed two xor, an and
// and a shift, C needed a 64-bit add/sub plus a shift, and every flag then took
// a register-to-memory move.  setAddFlags()/setSubFlags() in arm-new.h are the
// same formulas, so SETcc is not an approximation - it is the same answer read
// out of the hardware that just did the arithmetic.
//
// ADC/SBC/RSC are excluded: their result is built from two x86 adds/subs, and
// the second one's OF/CF are not the flags of the three-operand guest op.  They
// keep emitFlagsAdd()/emitFlagsSub().  Logical ops are excluded too: their C
// comes from the shifter, not from the ALU.
void BlockEmitter::emitFlagsNative(bool subtract, u8 mask)
{
	if (mask & JITF_N)
		a->sets(gmem(&N_FLAG, 1));
	if (mask & JITF_Z)
		a->setz(gmem(&Z_FLAG, 1));
	if (mask & JITF_C)
	{
		// x86 CF is the *borrow* after SUB; ARM C is "no borrow".
		if (subtract)
			a->setae(gmem(&C_FLAG, 1));
		else
			a->setc(gmem(&C_FLAG, 1));
	}
	if (mask & JITF_V)
		a->seto(gmem(&V_FLAG, 1));
}

void BlockEmitter::emitFlagsLogical(const x86::Gp &res, const Carry &carry, u8 mask)
{
	emitNZ(res, mask);
	if (mask & JITF_C)
		writeCarry(carry);
}

// ADD family, exactly setAddFlags(): V from the sign bits, C from the 33rd bit.
void BlockEmitter::emitFlagsAdd(const x86::Gp &lhs, const x86::Gp &rhs, const x86::Gp &res,
								bool carryIn, u8 mask)
{
	emitNZ(res, mask);

	if (mask & JITF_V)
	{
		a->mov(x86::ecx, lhs);
		a->xor_(x86::ecx, res);
		a->mov(x86::edx, rhs);
		a->xor_(x86::edx, res);
		a->and_(x86::ecx, x86::edx);
		a->shr(x86::ecx, 31);
		a->mov(gmem(&V_FLAG, 1), x86::cl);
	}

	if (mask & JITF_C)
	{
		// Widen to 64 bits: the carry out of bit 31 is bit 32 of the sum.
		a->mov(x86::ecx, lhs);
		a->mov(x86::edx, rhs);
		a->add(x86::rcx, x86::rdx);
		if (carryIn)
		{
			a->movzx(x86::edx, gmem(&C_FLAG, 1));
			a->add(x86::rcx, x86::rdx);
		}
		a->shr(x86::rcx, 32);
		a->mov(gmem(&C_FLAG, 1), x86::cl);
	}
}

// SUB family, exactly setSubFlags().
void BlockEmitter::emitFlagsSub(const x86::Gp &lhs, const x86::Gp &rhs, const x86::Gp &res,
								bool borrowIn, u8 mask)
{
	emitNZ(res, mask);

	if (mask & JITF_V)
	{
		a->mov(x86::ecx, lhs);
		a->xor_(x86::ecx, rhs);
		a->mov(x86::edx, lhs);
		a->xor_(x86::edx, res);
		a->and_(x86::ecx, x86::edx);
		a->shr(x86::ecx, 31);
		a->mov(gmem(&V_FLAG, 1), x86::cl);
	}

	if (mask & JITF_C)
	{
		// The borrow out of bit 31 is bit 63 of the 64-bit difference; C is its
		// complement.  Read the incoming C before overwriting it.
		a->mov(x86::ecx, lhs);
		a->mov(x86::edx, rhs);
		a->sub(x86::rcx, x86::rdx);
		if (borrowIn)
		{
			a->movzx(x86::edx, gmem(&C_FLAG, 1));
			a->xor_(x86::edx, 1);
			a->sub(x86::rcx, x86::rdx);
		}
		a->shr(x86::rcx, 63);
		a->xor_(x86::ecx, 1);
		a->mov(gmem(&C_FLAG, 1), x86::cl);
	}
}

// ---------------------------------------------------------------------------
// Data processing
// ---------------------------------------------------------------------------

void BlockEmitter::emitAlu(const JitInsn &in)
{
	const u8   mask = in.flagMask;
	const bool toPc = (in.d == 15);
	const int  extra = isRegShift(in.shift) ? 1 : 0;

	Label ni;
	bool  needNi = false;

	// The ARM handlers shift a u32 by the raw Rs byte without clamping; counts
	// >= 32 are UB in C++ and the interpreter's result there is whatever the
	// host happened to do.  Hand those to the interpreter (3.2.1.14 item 3).
	if (!thumb && !(in.flags & JF_IMM) &&
		(in.shift == SHIFT_LSL_REG || in.shift == SHIFT_LSR_REG || in.shift == SHIFT_ASR_REG))
	{
		ni	   = a->new_label();
		needNi = true;
		a->cmp(x86::ptr(x86::rbx, (int)(in.imm & 0x0F) * 4, 1), 32);
		a->jae(ni);
	}

	Carry carry;
	emitOperand2(in, x86::r10d, &carry);		// rhs

	const bool compareOnly = (in.sub >= ALU_TST && in.sub <= ALU_CMN);

	if (in.sub != ALU_MOV && in.sub != ALU_MVN)
	{
		a->mov(x86::r9d, gr(in.n));				// lhs
		if (in.flags & JF_PC_ALIGN4)
			a->and_(x86::r9d, 0xFFFFFFFC);
	}

	bool carryIn  = false;
	bool borrowIn = false;

	switch (in.sub)
	{
	case ALU_AND: a->mov(x86::r8d, x86::r9d); a->and_(x86::r8d, x86::r10d); break;
	case ALU_EOR: a->mov(x86::r8d, x86::r9d); a->xor_(x86::r8d, x86::r10d); break;
	case ALU_SUB: a->mov(x86::r8d, x86::r9d); a->sub(x86::r8d, x86::r10d); break;
	case ALU_RSB: a->mov(x86::r8d, x86::r10d); a->sub(x86::r8d, x86::r9d); break;
	case ALU_ADD: a->mov(x86::r8d, x86::r9d); a->add(x86::r8d, x86::r10d); break;

	case ALU_ADC:
		a->movzx(x86::ecx, gmem(&C_FLAG, 1));
		a->mov(x86::r8d, x86::r9d);
		a->add(x86::r8d, x86::r10d);
		a->add(x86::r8d, x86::ecx);
		carryIn = true;
		break;

	case ALU_SBC:
		a->movzx(x86::ecx, gmem(&C_FLAG, 1));
		a->xor_(x86::ecx, 1);
		a->mov(x86::r8d, x86::r9d);
		a->sub(x86::r8d, x86::r10d);
		a->sub(x86::r8d, x86::ecx);
		borrowIn = true;
		break;

	case ALU_RSC:
		a->movzx(x86::ecx, gmem(&C_FLAG, 1));
		a->xor_(x86::ecx, 1);
		a->mov(x86::r8d, x86::r10d);
		a->sub(x86::r8d, x86::r9d);
		a->sub(x86::r8d, x86::ecx);
		borrowIn = true;
		break;

	case ALU_TST: a->mov(x86::r8d, x86::r9d); a->and_(x86::r8d, x86::r10d); break;
	case ALU_TEQ: a->mov(x86::r8d, x86::r9d); a->xor_(x86::r8d, x86::r10d); break;
	case ALU_CMP: a->mov(x86::r8d, x86::r9d); a->sub(x86::r8d, x86::r10d); break;
	case ALU_CMN: a->mov(x86::r8d, x86::r9d); a->add(x86::r8d, x86::r10d); break;
	case ALU_ORR: a->mov(x86::r8d, x86::r9d); a->or_(x86::r8d, x86::r10d); break;
	case ALU_MOV: a->mov(x86::r8d, x86::r10d); break;
	case ALU_BIC: a->mov(x86::r8d, x86::r10d); a->not_(x86::r8d); a->and_(x86::r8d, x86::r9d); break;
	case ALU_MVN: a->mov(x86::r8d, x86::r10d); a->not_(x86::r8d); break;
	default: a->mov(x86::r8d, x86::r10d); break;
	}

	if (toPc)
	{
		// Reached only with S=0 (nativeSupported): store the value, align it for
		// the instruction set and let the block end here.  The caller turns
		// nextPc into armNextPC and reg[15] = nextPc + 4/2.
		a->and_(x86::r8d, thumb ? 0xFFFFFFFEu : 0xFFFFFFFCu);
		addFetch(in.fetchCycles);
		addConst(extra);
		addConst(thumb ? 1 : 0);
		emitExitReg(x86::r8d, JITX_DONE);
		queueNeedInterp(ni, needNi);
		return;
	}

	if (!compareOnly)
		a->mov(gr(in.d), x86::r8d);

	const bool isAdd = (in.sub == ALU_ADD || in.sub == ALU_ADC || in.sub == ALU_CMN);
	const bool isSub = (in.sub == ALU_SUB || in.sub == ALU_RSB || in.sub == ALU_SBC ||
						in.sub == ALU_RSC || in.sub == ALU_CMP);

	if ((isAdd && !carryIn) || (isSub && !borrowIn))
	{
		// The arithmetic above left the guest flags in the host flags register.
		// Nothing between here and the switch touched them (a MOV does not), so
		// read them out before addFetch()/checkBudget() clobber them.
		emitFlagsNative(isSub, mask);
	}
	else if (isAdd)
		emitFlagsAdd(x86::r9d, x86::r10d, x86::r8d, carryIn, mask);
	else if (isSub)
	{
		if (in.sub == ALU_RSB || in.sub == ALU_RSC)
			emitFlagsSub(x86::r10d, x86::r9d, x86::r8d, borrowIn, mask);
		else
			emitFlagsSub(x86::r9d, x86::r10d, x86::r8d, borrowIn, mask);
	}
	else
		emitFlagsLogical(x86::r8d, carry, mask);

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
	const u8 mask = in.flagMask;
	const u8 lo	  = (u8)(in.imm & 0x0F);

	a->mov(x86::r10d, gr(in.m));		// Rs: the multiplicand and the timing source

	switch (in.sub)
	{
	case MUL_MUL:
		if (thumb)
			a->mov(x86::eax, gr(in.d));
		else
			a->mov(x86::eax, gr(in.n));
		a->imul(x86::eax, x86::r10d);
		a->mov(gr(in.d), x86::eax);
		break;

	case MUL_MLA:
		a->mov(x86::eax, gr(in.n));
		a->imul(x86::eax, x86::r10d);
		a->add(x86::eax, gr(lo));
		a->mov(gr(in.d), x86::eax);
		break;

	case MUL_UMULL:
	case MUL_SMULL:
		if (in.sub == MUL_SMULL)
		{
			a->movsxd(x86::rax, gr(in.n));
			a->movsxd(x86::rcx, x86::r10d);
		}
		else
		{
			a->mov(x86::eax, gr(in.n));
			a->mov(x86::ecx, x86::r10d);
		}
		a->imul(x86::rax, x86::rcx);
		a->mov(x86::r11, x86::rax);		// full result for the Z flag
		a->mov(gr(lo), x86::eax);
		a->shr(x86::rax, 32);
		a->mov(gr(in.d), x86::eax);
		break;

	case MUL_UMLAL:
	case MUL_SMLAL:
		if (in.sub == MUL_SMLAL)
		{
			a->movsxd(x86::rax, gr(in.n));
			a->movsxd(x86::rcx, x86::r10d);
		}
		else
		{
			a->mov(x86::eax, gr(in.n));
			a->mov(x86::ecx, x86::r10d);
		}
		a->imul(x86::rax, x86::rcx);
		a->mov(x86::ecx, gr(in.d));
		a->shl(x86::rcx, 32);
		a->mov(x86::edx, gr(lo));
		a->or_(x86::rcx, x86::rdx);
		a->add(x86::rax, x86::rcx);
		a->mov(x86::r11, x86::rax);		// full result for the Z flag
		a->mov(gr(lo), x86::eax);
		a->shr(x86::rax, 32);
		a->mov(gr(in.d), x86::eax);
		break;
	}

	if (mask)
	{
		if (in.sub >= MUL_UMULL)
		{
			// N is bit 31 of the high word, Z covers all 64 bits.
			if (mask & JITF_N)
			{
				a->mov(x86::eax, gr(in.d));
				a->shr(x86::eax, 31);
				a->mov(gmem(&N_FLAG, 1), x86::al);
			}
			if (mask & JITF_Z)
			{
				// Test the saved 64-bit product: re-reading hi/lo would see
				// the high word twice when RdLo == RdHi.
				a->test(x86::r11, x86::r11);
				a->setz(x86::al);
				a->mov(gmem(&Z_FLAG, 1), x86::al);
			}
		}
		else
		{
			if (mask & JITF_N)
			{
				a->mov(x86::eax, gr(in.d));
				a->shr(x86::eax, 31);
				a->mov(gmem(&N_FLAG, 1), x86::al);
			}
			if (mask & JITF_Z)
			{
				a->cmp(gr(in.d), 0);
				a->setz(x86::al);
				a->mov(gmem(&Z_FLAG, 1), x86::al);
			}
		}
	}

	// Timing: arm-new.h buckets Rs by magnitude (`usource & 0xFFFFFF00` chain).
	// bsr gives exactly that: floor(highest set bit / 8).
	addFetch(in.fetchCycles);
	a->mov(x86::eax, x86::r10d);
	if (in.sub == MUL_MUL || in.sub == MUL_MLA || in.sub == MUL_SMULL || in.sub == MUL_SMLAL)
	{
		Label pos = a->new_label();
		a->test(x86::eax, x86::eax);
		a->jns(pos);
		a->not_(x86::eax);			// the signed forms take the magnitude
		a->bind(pos);
	}
	a->or_(x86::eax, 1);
	a->bsr(x86::eax, x86::eax);
	a->shr(x86::eax, 3);
	a->add(x86::r12d, x86::eax);

	if (thumb)
		addConst(1);				// thumb43_1: +1..+4
	else
	{
		const int plus =
			(in.sub == MUL_MUL || in.sub == MUL_UMULL || in.sub == MUL_SMULL) ? 0 : 1;
		addConst(2 + plus);			// mul/umull/smull +2..+5, accumulating +3..+6
	}

	checkBudget(pc + step());
}

// ---------------------------------------------------------------------------
// Load / store
// ---------------------------------------------------------------------------

void BlockEmitter::emitLoadStore(const JitInsn &in)
{
	const bool pre  = (in.flags & JF_PRE) != 0;
	const bool up   = (in.flags & JF_UP) != 0;
	const bool wb   = (in.flags & JF_WRITEBACK) != 0;
	const u32  size = (in.sub == MEM_WORD) ? 4u : (in.sub == MEM_HALF ? 2u : 1u);

	// address -> edi, writeback value -> esi
	a->mov(x86::edi, gr(in.n));
	if (in.flags & JF_PC_ALIGN4)
		a->and_(x86::edi, 0xFFFFFFFC);

	if (in.flags & JF_IMM)
	{
		a->mov(x86::esi, x86::edi);
		if (in.imm)
		{
			if (up)
				a->add(x86::esi, in.imm);
			else
				a->sub(x86::esi, in.imm);
		}
	}
	else
	{
		emitOffset(in, x86::eax);
		a->mov(x86::esi, x86::edi);
		if (up)
			a->add(x86::esi, x86::eax);
		else
			a->sub(x86::esi, x86::eax);
	}
	if (pre)
		a->mov(x86::edi, x86::esi);

	Label ni;
	bool  needNi = false;

	if (in.op == JOP_STORE)
	{
		ni	   = a->new_label();
		needNi = true;
		emitIoRangeCheck(size, ni);
		emitTrace();

		// arm-new.h writes the base back *before* the store for STR/STRB and
		// *after* it for STRH; only visible when Rn == Rt, but the differential
		// test compares registers.
		if ((pre && wb) && in.sub != MEM_HALF)
			a->mov(gr(in.n), x86::edi);

		switch (in.sub)
		{
		case MEM_BYTE:
			a->mov(x86::ecx, x86::edi);
			a->movzx(x86::edx, x86::ptr(x86::rbx, in.d * 4, 1));
			callHelper(a, (const void *)&CPUWriteByte);
			break;
		case MEM_HALF:
			a->mov(x86::ecx, x86::edi);
			a->movzx(x86::edx, x86::ptr(x86::rbx, in.d * 4, 2));
			callHelper(a, (const void *)&CPUWriteHalfWord);
			if (pre && wb)
				a->mov(gr(in.n), x86::edi);
			break;
		default:
			a->mov(x86::ecx, x86::edi);
			a->mov(x86::edx, gr(in.d));
			callHelper(a, (const void *)&CPUWriteMemory);
			break;
		}

		if (!pre && wb)
			a->mov(gr(in.n), x86::esi);

		addFetch(in.fetchCycles);
		if (!thumb)
			addConst(in.sub == MEM_HALF ? 4 : 2);
		if (in.sub == MEM_WORD)
			addAccess32(x86::edi);
		else
			addAccess16(x86::edi);
		checkBudget(pc + step());
		queueNeedInterp(ni, needNi);
		return;
	}

	// Load.  A read from EWRAM/IWRAM is a plain indexed load out of
	// memoryMap[]; the helper's switch and Lua-hook test are dead weight there.
	// The region guard cannot use JIT_CODE_WRITE's (addr & 0x0E000000) test:
	// that would also accept the 0x12xxxxxx/0x13xxxxxx mirrors, which the
	// helper sends to `unreadable` (open bus).  So the region byte itself is
	// checked: 2..3 and 5..7 always, 8..12 for word/byte reads.  ROM halfword
	// reads keep the helper because of the RTC at 0x080000C4/C6/C8; IO,
	// EEPROM/SRAM and unreadable addresses keep it too, so the 0x08020000 patch
	// and the open-bus value are untouched.
	//
	// Word reads use `addr & mask & ~3`, halfword reads `addr & mask & ~1` and
	// byte reads `addr & mask`, exactly what the *Wrapped() helpers do.  Both
	// word and halfword reads rotate the result when the address is not aligned
	// (word by (addr & 3) * 8, halfword by 8), so each fast path has to
	// reproduce that; see the two `ror`s below.
	{
		const bool fast = (in.sub == MEM_BYTE) || (in.sub == MEM_HALF) ||
						  (in.sub == MEM_WORD && !(in.flags & JF_QUICK));
		Label slow = a->new_label();
		Label done = a->new_label();

		if (fast)
		{
			Label fastRegion = a->new_label();
			// The full region byte matters: CPUReadMemoryWrapped() switches on
			// address >> 24, so 0x12xxxxxx / 0x13xxxxxx are *unreadable*
			// mirrors (open bus), not EWRAM/IWRAM.  JIT_CODE_WRITE's looser
			// (addr & 0x0E000000) test is fine for writes but wrong here.
			//
			// 2/3 (EWRAM/IWRAM), 5..7 (palette/VRAM/OAM) and, for word and byte
			// reads, 8..12 (the five ROM mirrors) are plain indexed loads in the
			// helper.  Everything else keeps it: BIOS and open bus (0), IO (4,
			// reads have side effects), EEPROM (13) and SRAM/Flash (14).  The
			// 0x08020000 hack is an instruction-fetch redirect, not a data-read
			// patch, so ROM is safe here.
			//
			// Halfword ROM reads are the exception: CPUReadHalfWordWrapped()
			// sends 0x080000C4/C6/C8 to rtcRead(), and rtcEnabled lives in
			// RTC.cpp as a file-static the JIT cannot see.  Let the helper keep
			// those three addresses instead of duplicating the check.
			//
			// Keeping the region in r8 also produces the memoryMap index for
			// free: region * sizeof(MemoryMap), and sizeof(MemoryMap) is 16.
			a->mov(x86::r8d, x86::edi);
			a->shr(x86::r8d, 24);
			a->cmp(x86::r8d, 2);
			a->jb(slow);
			a->cmp(x86::r8d, 3);
			a->jbe(fastRegion);
			a->cmp(x86::r8d, 5);
			a->jb(slow);
			a->cmp(x86::r8d, 7);
			a->jbe(fastRegion);
			if (in.sub == MEM_HALF)
				a->jmp(slow);
			else
			{
				a->cmp(x86::r8d, 8);
				a->jb(slow);
				a->cmp(x86::r8d, 12);
				a->ja(slow);
			}
			a->bind(fastRegion);
			a->shl(x86::r8, 4);

			a->mov(x86::rax, x86::ptr(x86::rbx, x86::r8, 0,
									  dispOf(&memoryMap[0].address), 8));
			a->mov(x86::r9d, x86::ptr(x86::rbx, x86::r8, 0,
									  dispOf(&memoryMap[0].mask), 4));
			a->and_(x86::r9d, x86::edi);
			if (in.sub == MEM_WORD)
				a->and_(x86::r9d, ~3u);
			else if (in.sub == MEM_HALF)
				a->and_(x86::r9d, ~1u);
			if (in.sub == MEM_WORD)
			{
				a->mov(x86::eax, x86::ptr(x86::rax, x86::r9, 0, 0, 4));
				// CPUReadMemoryWrapped() ends with _rotr(value, (address&3)*8).
				// Unaligned word loads are rare but the verifier seeds them, and
				// a movie that hits one must see the rotated value.  Branch it
				// out so the aligned case does not pay for the shift.
				a->test(x86::edi, 3);
				a->jz(done);
				a->mov(x86::ecx, x86::edi);
				a->and_(x86::ecx, 3);
				a->shl(x86::ecx, 3);
				a->ror(x86::eax, x86::cl);
			}
			else if (in.sub == MEM_HALF)
			{
				a->movzx(x86::eax, x86::ptr(x86::rax, x86::r9, 0, 0, 2));
				// CPUReadHalfWordWrapped() ends with
				//   value = (value >> 8) | (value << 24);   // address & 1
				// On a zero-extended halfword that is exactly `ror eax, 8`: the
				// low byte lands in bits 24-31, not in bits 8-15.  (A plain
				// `ror ax, 8` would be a byte swap and is wrong here.)
				// CPUReadHalfWordSignedWrapped() then keeps only (s8) of the
				// result, i.e. the *high* byte sign-extended, so the signed form
				// shifts that byte down and extends it.  The movsx after `done`
				// is a no-op once either of these has run.
				a->test(x86::edi, 1);
				a->jz(done);
				if (in.flags & JF_SIGNED)
				{
					a->shr(x86::eax, 8);
					a->movsx(x86::eax, x86::al);
				}
				else
					a->ror(x86::eax, 8);
			}
			else
				a->movzx(x86::eax, x86::ptr(x86::rax, x86::r9, 0, 0, 1));
			a->jmp(done);
		}

		a->bind(slow);
		a->mov(x86::ecx, x86::edi);		// first argument of the slow helper
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
		a->bind(done);

		if (in.sub == MEM_BYTE)
		{
			if (in.flags & JF_SIGNED)
				a->movsx(x86::eax, x86::al);
			else
				a->movzx(x86::eax, x86::al);
		}
		else if (in.sub == MEM_HALF && (in.flags & JF_SIGNED))
			a->movsx(x86::eax, x86::ax);
	}

	a->mov(gr(in.d), x86::eax);
	if (in.d != in.n && wb)
	{
		if (pre)
			a->mov(gr(in.n), x86::edi);
		else
			a->mov(gr(in.n), x86::esi);
	}

	addFetch(in.fetchCycles);
	if (!thumb)
		addConst(3);
	if (in.sub == MEM_WORD)
		addAccess32(x86::edi);
	else
		addAccess16(x86::edi);

	if (in.d == 15)
	{
		// Only the ARM pre-indexed word form gets here: charge two more cycles
		// and jump to the loaded value aligned to four bytes (reg[15] already
		// holds it from the store above).
		addConst(2);
		a->mov(x86::eax, gr(15));
		a->and_(x86::eax, 0xFFFFFFFC);
		a->mov(x86::ptr(x86::r14, 0, 4), x86::eax);
		a->mov(x86::ptr(x86::r14, 4, 4), JITX_DONE);
		a->jmp(epilogue);
	}
	else
	{
		checkBudget(pc + step());
	}
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
		const int regs = bitsSet(list & 0xFF);
		const bool lr  = (list & 0x4000) != 0;
		const u32 total = (u32)regs + (lr ? 1u : 0u);

		a->mov(x86::edi, gr(13));
		a->sub(x86::edi, regs * 4 + (lr ? 4 : 0));
		a->mov(x86::esi, x86::edi);		// new sp (unaligned)
		a->and_(x86::edi, 0xFFFFFFFC);

		ni	   = a->new_label();
		needNi = true;
		emitIoRangeCheck(total * 4, ni);
		emitTrace();

		bool first = true;
		for (int r = 0; r < 8; r++)
		{
			if (!(list & (1u << r)))
				continue;
			a->mov(x86::ecx, x86::edi);
			a->mov(x86::edx, gr(r));
			callHelper(a, (const void *)&CPUWriteMemory);
			addConst(1);
			if (first)
				addAccess32(x86::edi);
			else
				addSeq32(x86::edi);
			first = false;
			a->add(x86::edi, 4);
		}
		if (lr)
		{
			a->mov(x86::ecx, x86::edi);
			a->mov(x86::edx, gr(14));
			callHelper(a, (const void *)&CPUWriteMemory);
			addConst(1);
			if (first)
				addAccess32(x86::edi);
			else
				addSeq32(x86::edi);
		}

		a->mov(gr(13), x86::esi);
		addFetch(in.fetchCycles);
		checkBudget(pc + 2);
		queueNeedInterp(ni, needNi);
		return;
	}

	if (in.op == JOP_POP)
	{
		const bool withPc = (list & 0x8000) != 0;
		const int  regs	  = bitsSet(list & 0xFF);

		a->mov(x86::edi, gr(13));
		a->and_(x86::edi, 0xFFFFFFFC);
		a->mov(x86::esi, gr(13));
		a->add(x86::esi, regs * 4 + (withPc ? 4 : 0));

		bool first = true;
		for (int r = 0; r < 8; r++)
		{
			if (!(list & (1u << r)))
				continue;
			a->mov(x86::ecx, x86::edi);
			callHelper(a, (const void *)&CPUReadMemory);
			addConst(2);
			if (first)
				addAccess32(x86::edi);
			else
				addSeq32(x86::edi);
			first = false;
			a->mov(gr(r), x86::eax);
			a->add(x86::edi, 4);
		}

		if (!withPc)
		{
			a->mov(gr(13), x86::esi);
			addFetch(in.fetchCycles);
			checkBudget(pc + 2);
			return;
		}

		// The PC slot charges a bare data access (no +2) and the loaded address
		// is only masked to halfword alignment; V7 does not switch to ARM here.
		a->mov(x86::ecx, x86::edi);
		callHelper(a, (const void *)&CPUReadMemory);
		if (first)
			addAccess32(x86::edi);
		else
			addSeq32(x86::edi);
		a->and_(x86::eax, 0xFFFFFFFE);
		a->mov(gr(13), x86::esi);
		addFetch(in.fetchCycles);
		a->mov(x86::ptr(x86::r14, 0, 4), x86::eax);
		a->mov(x86::ptr(x86::r14, 4, 4), JITX_DONE);
		a->jmp(epilogue);
		return;
	}

	// LDM / STM
	const u8 baseReg = in.n;
	a->mov(x86::esi, gr(baseReg));		// base (also the writeback source)

	switch (in.sub)
	{
	case BLOCK_IA:
		a->mov(x86::edi, x86::esi);
		a->and_(x86::edi, 0xFFFFFFFC);
		a->mov(x86::edx, x86::esi);
		a->add(x86::edx, 4 * bitsSet(list));	// new base
		break;
	case BLOCK_IB:
		a->mov(x86::edi, x86::esi);
		a->add(x86::edi, 4);
		a->and_(x86::edi, 0xFFFFFFFC);
		a->mov(x86::edx, x86::esi);
		a->add(x86::edx, 4 * bitsSet(list));
		break;
	case BLOCK_DA:
		a->mov(x86::edx, x86::esi);
		a->sub(x86::edx, 4 * bitsSet(list));
		a->mov(x86::edi, x86::edx);
		a->add(x86::edi, 4);
		a->and_(x86::edi, 0xFFFFFFFC);
		break;
	default:	// BLOCK_DB
		a->mov(x86::edx, x86::esi);
		a->sub(x86::edx, 4 * bitsSet(list));
		a->mov(x86::edi, x86::edx);
		a->and_(x86::edi, 0xFFFFFFFC);
		break;
	}
	// edx = new base value, kept across the helper calls (rdx is volatile, so
	// move it to rsi which the prologue preserved).
	a->mov(x86::esi, x86::edx);

	if (in.op == JOP_STM)
	{
		ni	   = a->new_label();
		needNi = true;
		emitIoRangeCheck(4 * (bitsSet(list) + ((list & 0x8000) ? 1 : 0)), ni);
		emitTrace();

		bool first = true;
		for (int r = 0; r < 15; r++)
		{
			if (!(list & (1u << r)))
				continue;
			a->mov(x86::ecx, x86::edi);
			a->mov(x86::edx, gr(r));
			callHelper(a, (const void *)&CPUWriteMemory);
			if (in.flags & JF_WRITEBACK)
				a->mov(gr(baseReg), x86::esi);
			addConst(1);
			if (first)
				addAccess32(x86::edi);
			else
				addSeq32(x86::edi);
			first = false;
			a->add(x86::edi, 4);
		}
		if (list & 0x8000)
		{
			a->mov(x86::ecx, x86::edi);
			a->mov(x86::edx, gr(15));
			a->add(x86::edx, 4);
			callHelper(a, (const void *)&CPUWriteMemory);
			if (in.flags & JF_WRITEBACK)
				a->mov(gr(baseReg), x86::esi);
			addConst(1);
			if (first)
				addAccess32(x86::edi);
			else
				addSeq32(x86::edi);
		}

		addFetch(in.fetchCycles);
		if (!thumb)
			addConst(2);		// ARM's LDM/STM prologue; thumbC0 has none
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
			a->mov(x86::ecx, x86::edi);
			callHelper(a, (const void *)&CPUReadMemory);
			addConst(thumb ? 2 : 1);
			if (first)
				addAccess32(x86::edi);
			else
				addSeq32(x86::edi);
			first = false;
			a->mov(gr(r), x86::eax);
			a->add(x86::edi, 4);
		}

		if (thumb)
		{
			// thumbC8 writes the base back unless it is part of the list.
			if (!(list & (1u << baseReg)))
				a->mov(gr(baseReg), x86::esi);
			addFetch(in.fetchCycles);
			checkBudget(pc + 2);
			return;
		}

		addFetch(in.fetchCycles);
		addConst(2);

		if (list & 0x8000)
		{
			a->mov(x86::ecx, x86::edi);
			callHelper(a, (const void *)&CPUReadMemory);
			addConst(2);
			if (first)
				addAccess32(x86::edi);
			else
				addSeq32(x86::edi);
			if ((in.flags & JF_WRITEBACK) && !(list & (1u << baseReg)))
				a->mov(gr(baseReg), x86::esi);
			a->mov(x86::ptr(x86::r14, 0, 4), x86::eax);
			a->mov(x86::ptr(x86::r14, 4, 4), JITX_DONE);
			a->jmp(epilogue);
			return;
		}

		if ((in.flags & JF_WRITEBACK) && !(list & (1u << baseReg)))
			a->mov(gr(baseReg), x86::esi);
		checkBudget(pc + 4);
	}
}

// ---------------------------------------------------------------------------
// SWP
// ---------------------------------------------------------------------------

void BlockEmitter::emitSwp(const JitInsn &in)
{
	Label ni = a->new_label();

	a->mov(x86::edi, gr(in.n));
	emitIoRangeCheck(in.sub == MEM_BYTE ? 1u : 4u, ni);
	emitTrace();

	if (in.sub == MEM_BYTE)
	{
		a->mov(x86::ecx, x86::edi);
		callHelper(a, (const void *)&CPUReadByte);
		a->mov(x86::esi, x86::eax);		// keep the loaded byte across the store
		a->mov(x86::ecx, x86::edi);
		a->movzx(x86::edx, x86::ptr(x86::rbx, in.m * 4, 1));
		callHelper(a, (const void *)&CPUWriteByte);
		a->mov(gr(in.d), x86::esi);
	}
	else
	{
		a->mov(x86::ecx, x86::edi);
		callHelper(a, (const void *)&CPUReadMemory);
		a->mov(x86::esi, x86::eax);
		a->mov(x86::ecx, x86::edi);
		a->mov(x86::edx, gr(in.m));
		callHelper(a, (const void *)&CPUWriteMemory);
		a->mov(gr(in.d), x86::esi);
	}

	addFetch(in.fetchCycles);		// V7 charges no data cycles for SWP
	checkBudget(pc + 4);
	queueNeedInterp(ni, true);
}

// ---------------------------------------------------------------------------
// Branches
// ---------------------------------------------------------------------------

void BlockEmitter::emitBranch(const JitInsn &in)
{
	if (thumb)
	{
		if (in.flags & JF_LINK)		// BL low half: the target comes from LR
		{
			a->mov(x86::eax, gr(14));
			if (in.imm)
				a->add(x86::eax, in.imm);
			a->and_(x86::eax, 0xFFFFFFFE);
			a->mov(gr(14), (pc + 2u) | 1u);
			addFetch(in.fetchCycles);
			a->mov(x86::ptr(x86::r14, 0, 4), x86::eax);
			a->mov(x86::ptr(x86::r14, 4, 4), JITX_DONE);
			a->jmp(epilogue);
			return;
		}

		Label skip = a->new_label();
		emitCondSkip(in.cond, skip);

		// Taken: V7 *assigns* 3 to clockTicks for a conditional branch; the
		// unconditional form charges the usual fetch cost.
		if (in.cond == 0xE)
			addFetch(in.fetchCycles);
		else
			addConst(3);
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
		a->mov(gr(14), pc + 4u);

	addFetch(in.fetchCycles);
	addConst(3);
	emitChainExit(pc + 8u + in.imm);

	a->bind(skip);
	addFetch(in.fetchCycles);
	emitChainExit(pc + 4);
}

void BlockEmitter::emitBx(const JitInsn &in)
{
	// armState = !(target & 1); reg[15] = target & ~3 / ~1
	a->mov(x86::eax, gr(in.m));
	a->mov(x86::ecx, x86::eax);
	a->and_(x86::ecx, 1);
	a->xor_(x86::ecx, 1);
	a->mov(gmem(&armState, 1), x86::cl);

	Label arm = a->new_label();
	Label done = a->new_label();
	a->cmp(x86::ecx, 0);
	a->jne(arm);
	a->and_(x86::eax, 0xFFFFFFFE);		// Thumb
	a->jmp(done);
	a->bind(arm);
	a->and_(x86::eax, 0xFFFFFFFC);		// ARM
	a->bind(done);

	addFetch(in.fetchCycles);
	addConst(thumb ? 0 : 3);
	a->mov(x86::ptr(x86::r14, 0, 4), x86::eax);
	a->mov(x86::ptr(x86::r14, 4, 4), JITX_DONE);
	a->jmp(epilogue);
}

// ---------------------------------------------------------------------------
// MRS / MSR
// ---------------------------------------------------------------------------

void BlockEmitter::emitMrs(const JitInsn &in)
{
	if (!in.sub)
		callHelper(a, (const void *)&CPUUpdateCPSR);	// arm100 resyncs reg[16]
	a->mov(x86::eax, gr(in.sub ? 17 : 16));
	a->mov(gr(in.d), x86::eax);
	addFetch(in.fetchCycles);
	checkBudget(pc + step());
}

void BlockEmitter::emitMsr(const JitInsn &in)
{
	// Only the SPSR form is compiled (nativeSupported), so no mode change and no
	// CPUUpdateCPSR: arm160/arm360 just merge the selected bytes, and exclude
	// System (0x1F) as well as User.
	if (in.flags & JF_IMM)
	{
		const u32 rot = (u32)(in.n & 0x0F) * 2;
		u32		  v	  = in.imm & 0xFF;
		if (rot)
			v = (v >> rot) | (v << (32 - rot));
		a->mov(x86::r11d, v);
	}
	else
		a->mov(x86::r11d, gr(in.m));

	Label skip = a->new_label();
	a->mov(x86::eax, gmem(&armMode, 4));
	a->cmp(x86::eax, 0x10);
	a->jle(skip);
	a->cmp(x86::eax, 0x1F);
	a->jge(skip);

	for (int byte = 0; byte < 4; byte++)
	{
		if (!(in.d & (1 << byte)))
			continue;
		const u32 keep = ~(0xFFu << (byte * 8));
		a->mov(x86::eax, gr(17));
		a->and_(x86::eax, keep);
		a->mov(x86::ecx, x86::r11d);
		a->and_(x86::ecx, 0xFFu << (byte * 8));
		a->or_(x86::eax, x86::ecx);
		a->mov(gr(17), x86::eax);
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
	a->mov(x86::ecx, pc);
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
		Label skip	= a->new_label();
		Label after = a->new_label();

		emitCondSkip(in.cond, skip);
		emitBody(in);
		a->jmp(after);
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
	// call emitTrace() themselves once their guard has passed.
	if (in.op != JOP_STORE && in.op != JOP_STM && in.op != JOP_PUSH && in.op != JOP_SWP)
		emitTrace();

	switch (in.op)
	{
	case JOP_ALU: emitAlu(in); return;
	case JOP_MUL: emitMul(in); return;
	case JOP_LOAD:
	case JOP_STORE: emitLoadStore(in); return;
	case JOP_LDM:
	case JOP_STM:
	case JOP_PUSH:
	case JOP_POP: emitBlockTransfer(in); return;
	case JOP_SWP: emitSwp(in); return;
	case JOP_BRANCH: emitBranch(in); return;
	case JOP_BX: emitBx(in); return;
	case JOP_MRS: emitMrs(in); return;
	case JOP_MSR: emitMsr(in); return;
	default: return;
	}
}

bool BlockEmitter::compile(u16 *outCount)
{
	// Decide how long the prefix is before emitting anything: the prologue
	// depends on whether any instruction calls a helper.
	u16 n = 0;
	while (n < b->count && nativeSupported(b->insn[n], thumb))
	{
		if (touchesMemory(b->insn[n]))
			usesMem = true;
		n++;
	}
	if (n == 0)
		return false;

	count		= 0;
	epilogue	= a->new_label();
	chainBail	= a->new_label();
	needCount	= 0;
	budgetCount = 0;
	nChain		= 0;

	// holdState cannot change inside a compiled block - every path that sets it
	// (a HALTCNT/STOP write) is an IO store, and those go back to the
	// interpreter - so runBlock()'s per-instruction test is invariant here.
	emitPrologue();
	// A linked predecessor jumps here: rbx/r12/r13/r14 and the stack frame are
	// already set up by the block that started the chain.  A branch-only block
	// emits no per-instruction budget check of its own, so the guard lives at
	// the chain entry - without it a two-block jump loop could spin forever
	// inside generated code.
	chainEntryOffset = (u32)a->offset();
	a->cmp(x86::r12d, x86::r13d);
	a->jge(chainBail);

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
// are dumped (default 200, 0 = no cap).  Used while bringing the backend up;
// costs one getenv() per compile and nothing at run time.
void dumpCompiledBlock(const JitBlock *block, CodeHolder *code, u16 insns)
{
	static const char *path = getenv("VBA_JIT_ASM");
	if (!path || !path[0])
		return;

	// The cap exists so that a long session does not produce a gigabyte of hex.
	// It used to be a fixed 200 with no way to tell it had been hit: blocks
	// compiled after the cap were silently absent, which reads exactly like
	// "this block was not compiled".  The number is now settable, and the
	// exhaustion is written into the dump once.
	static int	limit = -1;		// -1 = not read yet, 0 = no limit
	static int	dumped = 0;
	static bool announced = false;

	if (limit < 0)
	{
		const char *s = getenv("VBA_JIT_ASM_LIMIT");
		limit		  = (s && s[0]) ? (int)strtol(s, NULL, 0) : 200;
		if (limit < 0)
			limit = 200;
	}

	if (limit && dumped >= limit)
	{
		if (!announced)
		{
			announced = true;
			FILE *f	  = fopen(path, "a");
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

	// Sized to the block: an expanded LDM/STM can exceed any fixed bound, and
	// dropping those blocks is precisely when the dump is needed most.
	const size_t n	 = code->code_size();
	u8			*bytes = (u8 *)malloc(n ? n : 1);
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

namespace {

JitRuntime *jitRuntime()
{
	static JitRuntime rt;
	return &rt;
}

bool checkedDisplacements()
{
	if (gDispOk)
		return true;
	// Everything is addressed relative to &reg[0]; refuse to compile at all if
	// any of the globals we touch is more than 2 GB away.  Never happens with a
	// normal link, but a wrong guess here would be a silent wild write.
	const void *all[] = {&reg[0],	 &N_FLAG,			&C_FLAG,
						 &Z_FLAG,	 &V_FLAG,			&armState,
						 &armNextPC, &armMode,			&holdState,
						 &memoryWait[0], &memoryWait32[0], &memoryWaitSeq[0],
						 &memoryWaitSeq32[0], &memoryWaitFetch[0],
						 &memoryWaitFetch32[0], &gJitShiftCarry,
						 &memoryMap[0].address, &memoryMap[0].mask};

	const intptr_t base = (intptr_t)&reg[0];
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
	{
		const intptr_t d = (intptr_t)all[i] - base;
		if (d > 0x7FFFFF00 || d < -0x7FFFFF00)
			return false;
	}
	gDispOk = true;
	return true;
}

} // namespace

bool jitNativeAvailable()
{
	return checkedDisplacements();
}

bool jitCompileBlock(const JitBlock *block, JitCompiledBlock *out)
{
	if (!block || !out || block->count == 0 || !jitNativeAvailable())
		return false;

	out->fn			= nullptr;
	out->chainEntry = nullptr;
	out->insnCount	= 0;
	out->chainCount = 0;
	out->selfSlot	= 0xFFFF;
	out->frameKind	= 0;
	out->reserved2	= 0;
	for (int i = 0; i < JIT_MAX_CHAIN; i++)
	{
		out->chain[i].target   = 0;
		out->chain[i].tag	   = 0;
		out->chain[i].linkIndex = 0xFFFF;
		out->chain[i].reserved = 0;
	}

	JitRuntime *rt = jitRuntime();

	// A fresh CodeHolder per compilation.  Reusing one through reset() looks
	// tempting but rt->add() rejects a reset holder, so the arena setup (a few
	// microseconds) stays.
	CodeHolder code;
	if (code.init(rt->environment()) != Error::kOk)
		return false;

	x86::Assembler assembler(&code);

	BlockEmitter emitter(&assembler, block, out);
	u16			 insns = 0;
	if (!emitter.compile(&insns) || insns == 0)
		return false;

	dumpCompiledBlock(block, &code, insns);

	JitNativeFn fn = nullptr;
	if (rt->add(&fn, &code) != Error::kOk || !fn)
		return false;

	out->fn			 = fn;
	out->insnCount	 = insns;
	out->chainEntry	 = (u8 *)fn + emitter.chainEntryOffsetOf();
	out->chainCount	 = (u16)emitter.chainSlotsUsed();
	out->frameKind	 = emitter.usesMemoryFrame() ? 1 : 0;
	return true;
}

void jitReleaseBlock(JitCompiledBlock *block)
{
	if (!block || !block->fn)
		return;
	jitRuntime()->release(block->fn);
	block->fn		   = nullptr;
	block->chainEntry = nullptr;
	block->insnCount = 0;
	block->chainCount = 0;
	for (int i = 0; i < JIT_MAX_CHAIN; i++)
		block->chain[i].target = 0;
}

#else // !VBA_JIT_X64 || !USE_GBA_CORE_V7

bool jitNativeAvailable() { return false; }

bool jitCompileBlock(const JitBlock *block, JitCompiledBlock *out)
{
	(void)block;
	(void)out;
	return false;
}

void jitReleaseBlock(JitCompiledBlock *block) { (void)block; }

#endif
