#ifndef VBA_GBACPU_H
#define VBA_GBACPU_H

#pragma once

extern int armExecute();
extern int thumbExecute();

#ifdef __GNUC__
# define INSN_REGPARM __attribute__((regparm(1)))
# define LIKELY(x) __builtin_expect(!!(x), 1)
# define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
# define INSN_REGPARM /*nothing*/
# define LIKELY(x) (x)
# define UNLIKELY(x) (x)
#endif

#define UPDATE_REG(address, value) \
	{ \
		WRITE16LE(((u16 *)&ioMem[address]), value); \
	} \

#define ARM_PREFETCH \
	{ \
		cpuPrefetch[0] = CPUReadMemoryQuick(armNextPC); \
		cpuPrefetch[1] = CPUReadMemoryQuick(armNextPC + 4); \
	}

#define THUMB_PREFETCH \
	{ \
		cpuPrefetch[0] = CPUReadHalfWordQuick(armNextPC); \
		cpuPrefetch[1] = CPUReadHalfWordQuick(armNextPC + 2); \
	}

#define ARM_PREFETCH_NEXT \
	cpuPrefetch[1] = CPUReadMemoryQuick(armNextPC + 4);

#define THUMB_PREFETCH_NEXT \
	cpuPrefetch[1] = CPUReadHalfWordQuick(armNextPC + 2);

extern int32 SWITicks;
extern u32	 mastercode;
extern bool8 busPrefetch;
extern bool8 busPrefetchEnable;
extern u32	 busPrefetchCount;
extern int32 cpuNextEvent;
extern bool8 holdState;
extern u32	 cpuPrefetch[2];
extern int32 cpuTotalTicks;
extern u8	 memoryWait[16];
extern u8	 memoryWait32[16];
extern u8	 memoryWaitSeq[16];
extern u8	 memoryWaitSeq32[16];
extern u8	 cpuBitsSet[256];
extern u8	 cpuLowestBitSet[256];

extern void CPUSwitchMode(int mode, bool saveState, bool breakLoop);
extern void CPUSwitchMode(int mode, bool saveState);
extern void CPUUpdateCPSR();
extern void CPUUpdateFlags(bool breakLoop);
extern void CPUUpdateFlags();
extern void CPUUndefinedException();
extern void CPUSoftwareInterrupt();
extern void CPUSoftwareInterrupt(int comment);

// Waitstates when accessing data
inline int dataTicksAccess16(u32 address) // DATA 8/16bits NON SEQ
{
	int addr  = (address >> 24) & 15;
	int value =  memoryWait[addr];

	if ((addr >= 0x08) || (addr < 0x02))
	{
		busPrefetchCount = 0;
		busPrefetch		 = false;
	}
	else if (busPrefetch)
	{
		int waitState = value;
		if (!waitState)
			waitState = 1;
		busPrefetchCount = ((busPrefetchCount + 1) << waitState) - 1;
	}

	return value;
}

inline int dataTicksAccess32(u32 address) // DATA 32bits NON SEQ
{
	int addr  = (address >> 24) & 15;
	int value = memoryWait32[addr];

	if ((addr >= 0x08) || (addr < 0x02))
	{
		busPrefetchCount = 0;
		busPrefetch		 = false;
	}
	else if (busPrefetch)
	{
		int waitState = value;
		if (!waitState)
			waitState = 1;
		busPrefetchCount = ((busPrefetchCount + 1) << waitState) - 1;
	}

	return value;
}

inline int dataTicksAccessSeq16(u32 address) // DATA 8/16bits SEQ
{
	int addr  = (address >> 24) & 15;
	int value = memoryWaitSeq[addr];

	if ((addr >= 0x08) || (addr < 0x02))
	{
		busPrefetchCount = 0;
		busPrefetch		 = false;
	}
	else if (busPrefetch)
	{
		int waitState = value;
		if (!waitState)
			waitState = 1;
		busPrefetchCount = ((busPrefetchCount + 1) << waitState) - 1;
	}

	return value;
}

inline int dataTicksAccessSeq32(u32 address) // DATA 32bits SEQ
{
	int addr  = (address >> 24) & 15;
	int value =  memoryWaitSeq32[addr];

	if ((addr >= 0x08) || (addr < 0x02))
	{
		busPrefetchCount = 0;
		busPrefetch		 = false;
	}
	else if (busPrefetch)
	{
		int waitState = value;
		if (!waitState)
			waitState = 1;
		busPrefetchCount = ((busPrefetchCount + 1) << waitState) - 1;
	}

	return value;
}

// Waitstates when executing opcode
inline int codeTicksAccess16(u32 address) // THUMB NON SEQ
{
	int addr = (address >> 24) & 15;

	if ((addr >= 0x08) && (addr <= 0x0D))
	{
		if (busPrefetchCount & 0x1)
		{
			if (busPrefetchCount & 0x2)
			{
				busPrefetchCount = ((busPrefetchCount & 0xFF) >> 2) | (busPrefetchCount & 0xFFFFFF00);
				return 0;
			}
			busPrefetchCount = ((busPrefetchCount & 0xFF) >> 1) | (busPrefetchCount & 0xFFFFFF00);
			return memoryWaitSeq[addr] - 1;
		}
		else
		{
			busPrefetchCount = 0;
			return memoryWait[addr];
		}
	}
	else
	{
		busPrefetchCount = 0;
		return memoryWait[addr];
	}
}

inline int codeTicksAccess32(u32 address) // ARM NON SEQ
{
	int addr = (address >> 24) & 15;

	if ((addr >= 0x08) && (addr <= 0x0D))
	{
		if (busPrefetchCount & 0x1)
		{
			if (busPrefetchCount & 0x2)
			{
				busPrefetchCount = ((busPrefetchCount & 0xFF) >> 2) | (busPrefetchCount & 0xFFFFFF00);
				return 0;
			}
			busPrefetchCount = ((busPrefetchCount & 0xFF) >> 1) | (busPrefetchCount & 0xFFFFFF00);
			return memoryWaitSeq[addr] - 1;
		}
		else
		{
			busPrefetchCount = 0;
			return memoryWait32[addr];
		}
	}
	else
	{
		busPrefetchCount = 0;
		return memoryWait32[addr];
	}
}

inline int codeTicksAccessSeq16(u32 address) // THUMB SEQ
{
	int addr = (address >> 24) & 15;

	if ((addr >= 0x08) && (addr <= 0x0D))
	{
		if (busPrefetchCount & 0x1)
		{
			busPrefetchCount = ((busPrefetchCount & 0xFF) >> 1) | (busPrefetchCount & 0xFFFFFF00);
			return 0;
		}
		else
			if (busPrefetchCount > 0xFF)
			{
				busPrefetchCount = 0;
				return memoryWait[addr];
			}
			else
				return memoryWaitSeq[addr];
	}
	else
	{
		busPrefetchCount = 0;
		return memoryWaitSeq[addr];
	}
}

inline int codeTicksAccessSeq32(u32 address) // ARM SEQ
{
	int addr = (address >> 24) & 15;

	if ((addr >= 0x08) && (addr <= 0x0D))
	{
		if (busPrefetchCount & 0x1)
		{
			if (busPrefetchCount & 0x2)
			{
				busPrefetchCount = ((busPrefetchCount & 0xFF) >> 2) | (busPrefetchCount & 0xFFFFFF00);
				return 0;
			}
			busPrefetchCount = ((busPrefetchCount & 0xFF) >> 1) | (busPrefetchCount & 0xFFFFFF00);
			return memoryWaitSeq[addr];
		}
		else
			if (busPrefetchCount > 0xFF)
			{
				busPrefetchCount = 0;
				return memoryWait32[addr];
			}
			else
				return memoryWaitSeq32[addr];
	}
	else
	{
		return memoryWaitSeq32[addr];
	}
}

// ---------------------------------------------------------------------------
// Fetch-pipeline refill cost after the PC is written.
//
// Every helper above is impure: it consumes busPrefetchCount.  Writing a sum
// such as
//
//     clockTicks = codeTicksAccessSeq16(pc) + codeTicksAccessSeq16(pc)
//                  + codeTicksAccess16(pc) + 3;
//
// therefore leaves the order of those side effects unspecified.  MSVC really
// does choose differently: at /Od it evaluates left to right (the two
// sequential fetches see the pending prefetch cycles first, the non-sequential
// fetch then pays full waitstates), while /O2 + /GL evaluates the
// non-sequential fetch first.  For a taken branch that is 6 cycles against 5 -
// one cycle, but there are ~350 taken branches per frame in a tight loop, so
// the CPU budget lands on a different instruction, and after a few thousand
// frames the Debug and Release builds are running different games.
//
// These wrappers perform the calls as separate statements in the order the
// original expressions were written, which is the order /Od uses and therefore
// the order every existing build/movie was made with.  Only the sequencing is
// pinned; the arithmetic is unchanged.
// ---------------------------------------------------------------------------
inline int codeTicksBranch16(u32 address) // 2 sequential + 1 non-sequential
{
	int seq1 = codeTicksAccessSeq16(address);
	int seq2 = codeTicksAccessSeq16(address);
	return seq1 + seq2 + codeTicksAccess16(address);
}

inline int codeTicksBranch32(u32 address)
{
	int seq1 = codeTicksAccessSeq32(address);
	int seq2 = codeTicksAccessSeq32(address);
	return seq1 + seq2 + codeTicksAccess32(address);
}

// Same intent, but written as "one sequential access counted twice", which is
// what the high-register MOV/ADD-to-PC handlers have always done.  One call, not
// two: the prefetch counter must only be consumed once.
inline int codeTicksBranchSeq2x16(u32 address)
{
	int seq = codeTicksAccessSeq16(address) * 2;
	return seq + codeTicksAccess16(address);
}

// Non-sequential first, then the two sequential refills - the order the ARM
// ALU-to-PC handlers were written in.
inline int codeTicksBranch32NonSeqFirst(u32 address)
{
	int nseq = codeTicksAccess32(address);
	int seq1 = codeTicksAccessSeq32(address);
	int seq2 = codeTicksAccessSeq32(address);
	return nseq + seq1 + seq2;
}

inline int codeTicksBranch16NonSeqFirst(u32 address)
{
	int nseq = codeTicksAccess16(address);
	int seq1 = codeTicksAccessSeq16(address);
	int seq2 = codeTicksAccessSeq16(address);
	return nseq + seq1 + seq2;
}

// Data access followed by the instruction fetch that comes after it.  Same
// reason as above: both helpers touch busPrefetchCount, so the order of the two
// calls decides how many cycles the fetch is charged.  The expressions were
// written data-first, so these keep that order.  The trailing constant the
// callers add is left outside.
inline int data16ThenCode16(u32 dataAddress, u32 codeAddress)
{
	int data = dataTicksAccess16(dataAddress);
	return data + codeTicksAccess16(codeAddress);
}

inline int data32ThenCode16(u32 dataAddress, u32 codeAddress)
{
	int data = dataTicksAccess32(dataAddress);
	return data + codeTicksAccess16(codeAddress);
}

inline int data16ThenCode32(u32 dataAddress, u32 codeAddress)
{
	int data = dataTicksAccess16(dataAddress);
	return data + codeTicksAccess32(codeAddress);
}

inline int data32ThenCode32(u32 dataAddress, u32 codeAddress)
{
	int data = dataTicksAccess32(dataAddress);
	return data + codeTicksAccess32(codeAddress);
}

#endif // VBA_GBACPU_H
