#include <cstdio>
#include <cstring>

#include "GBAGlobals.h"
#include "GBAinline.h"

extern void (*dbgSignal)(int, int);
extern void (*dbgOutput)(const char *, u32);
extern int systemVerbose;

static bool agbPrintProtect = false;
// Cleared when the ROM is too big for the little SWI 0xFA stub that CPUInit
// plants at 0x09FE209C: without that stub a ROM calling the AGBPrint entry
// point would run into its own data, so the channel has to stay shut no matter
// what the logging bit says.
static bool agbPrintAvailable = true;

// AGBPrint has a single switch: the verbose bit that the logging dialog sets.
// A print nobody displays has no visible effect at all, so "let the writes
// through" and "show the text" are the same decision.  Keeping them apart only
// made it possible to turn one on and wonder why the other one stayed silent.
static bool agbPrintActive()
{
	return agbPrintAvailable && (systemVerbose & VERBOSE_AGBPRINT) != 0;
}

bool agbPrintWrite(u32 address, u16 value)
{
	if (agbPrintActive())
	{
		if (address == 0x9fe2ffe) // protect
		{
			agbPrintProtect = (value != 0);
			CPUWriteHalfWordQuick(address, value);
			return true;
		}
		else
		{
			if (agbPrintProtect &&
			    ((address >= 0x9fe20f8 && address <= 0x9fe20ff) // control structure
			     || (address >= 0x8fd0000 && address <= 0x8fdffff)
			     || (address >= 0x9fd0000 && address <= 0x9fdffff)))
			{
				CPUWriteHalfWordQuick(address, value);
				return true;
			}
		}
	}
	return false;
}

void agbPrintReset()
{
	agbPrintProtect = false;
}

bool agbPrintIsEnabled()
{
	return agbPrintActive();
}

void agbPrintSetAvailable(bool available)
{
	agbPrintAvailable = available;
}

void agbPrintFlush()
{
	// A ROM that never had its control structure written (the usual case when
	// the bit is off) leaves get == put behind, but the pointers could also be
	// leftovers from a previously loaded ROM, since the 32 MB rom buffer is not
	// cleared on load.  Refusing to look at them at all is the only reading
	// that cannot print garbage.
	if (!agbPrintActive())
		return;

	u16 get = CPUReadHalfWordQuick(0x9fe20fc);
	u16 put = CPUReadHalfWordQuick(0x9fe20fe);

	u32 address = (CPUReadHalfWordQuick(0x9fe20fa) << 16);
	if (address != 0xfd0000 && address != 0x1fd0000)
	{
		dbgOutput("Did you forget to call AGBPrintInit?\n", 0);
		// get rid of the text otherwise we will continue to be called
		CPUWriteHalfWordQuick(0x9fe20fc, put);
		return;
	}

	u8 *data = &rom[address];

	while (get != put)
	{
		char c = data[get++];
		char s[2];
		s[0] = c;
		s[1] = 0;

		dbgOutput(s, 0);
		if (c == '\n')
			break;
	}
	CPUWriteHalfWordQuick(0x9fe20fc, get);
}

