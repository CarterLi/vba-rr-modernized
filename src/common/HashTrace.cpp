#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "zlib.h"

#include "HashTrace.h"
#include "System.h"
#include "SystemGlobals.h"
#include "Util.h" // IMAGE_GBA

#include "../gba/GBAGlobals.h" // reg / internalRAM / workRAM / vram / paletteRAM / oam / ioMem

// Same size as REWIND_SIZE: the project already treats this as large enough to
// hold a complete state.
#define HASH_TRACE_STATE_SIZE 400000

// GBA region sizes, taken from CPUWriteStateToStream in gba/V7|V8/GBA.cpp so
// they match the actual allocations.
#define HT_INTERNAL_RAM 0x08000
#define HT_WORK_RAM		0x40000
#define HT_VRAM			0x20000
#define HT_PALETTE_RAM	0x00400
#define HT_OAM			0x00400
#define HT_IO_MEM		0x00400

namespace
{
	FILE *s_file		 = NULL;
	char *s_buf			 = NULL;
	unsigned int s_frame = 0;
	unsigned int s_failed = 0;
	bool s_ready		 = false;

	// ---------------------------------------------------------------------
	// One-shot state dump, for comparing two builds frame by frame.
	//
	// Driven by the environment instead of the command line so that the whole
	// debugging aid stays inside common/ (no MFC, no argument span changes)
	// and can be switched on for an already built exe:
	//
	//     set VBA_DUMP_STATE_FRAME=4200
	//     set VBA_DUMP_STATE_PATH=D:\tmp\state.bin
	//
	// The frame number counts completed frames, which is exactly what
	// systemCounters.frameCount holds at this point of the frame boundary, so
	// the same value selects the same moment in both builds.
	// ---------------------------------------------------------------------
	bool s_dumpInit			 = false;
	bool s_dumpEnabled		 = false;
	unsigned int s_dumpFrame = 0;
	unsigned int s_tick		 = 0;
	char s_dumpPath[512]	 = {0};
	char *s_dumpBuf			 = NULL;

	// ---- PC trace, see HashTrace.h ----
	FILE *s_pcFile			 = NULL;
	bool s_pcInit			 = false;
	bool s_pcActive			 = false;
	unsigned int s_pcStart	 = 0;
	unsigned int s_pcEnd	 = 1;
	unsigned long long s_pcCount = 0;

	void pcTraceInit()
	{
		s_pcInit = true;

		const char *path  = getenv("VBA_PC_LOG");
		const char *start = getenv("VBA_PC_START");
		const char *end	  = getenv("VBA_PC_END");
		if (!path || !path[0])
			return;

		s_pcStart = start ? (unsigned int)strtoul(start, NULL, 10) : 0;
		s_pcEnd	  = end ? (unsigned int)strtoul(end, NULL, 10) : s_pcStart + 1;

		s_pcFile = fopen(path, "wb");
		if (s_pcFile)
			setvbuf(s_pcFile, NULL, _IOFBF, 1 << 20);
	}

	void pcTraceTick(unsigned int tick)
	{
		if (!s_pcFile)
			return;

		if (tick == s_pcStart)
			s_pcActive = true;

		if (tick == s_pcEnd)
		{
			s_pcActive = false;
			fclose(s_pcFile);
			s_pcFile = NULL;
		}
	}

	void stateDumpInit()
	{
		s_dumpInit = true;

		const char *frame = getenv("VBA_DUMP_STATE_FRAME");
		const char *path  = getenv("VBA_DUMP_STATE_PATH");
		if (!frame || !frame[0] || !path || !path[0])
			return;

		s_dumpFrame = (unsigned int)strtoul(frame, NULL, 10);
		strncpy(s_dumpPath, path, sizeof(s_dumpPath) - 1);
		s_dumpPath[sizeof(s_dumpPath) - 1] = 0;

		s_dumpBuf = (char *)malloc(HASH_TRACE_STATE_SIZE);
		if (!s_dumpBuf)
			return;

		s_dumpEnabled = true;
	}

	void hashTraceAtExit()
	{
		hashTraceClose();
	}

	unsigned int crcOf(unsigned int crc, const void *data, size_t size)
	{
		if (!data || !size)
			return crc;
		return (unsigned int)crc32(crc, (const Bytef *)data, (uInt)size);
	}

	// "Gameplay memory": the regions that actually determine GBA behaviour.
	//
	// pix (4*241*162) is deliberately excluded. CPUWriteStateToStream serialises
	// pix as well, and pix carries OSD text whose lifetime is measured in wall
	// clock milliseconds (DrawTextMessages), so the same message survives a
	// different number of frames on a fast run and a slow one. That is a dirty
	// framebuffer, not emulation drift - see tests/baseline/README.md.
	unsigned int ramHash()
	{
		if (systemCartridgeType != IMAGE_GBA)
			return 0; // GB mode not covered yet: different region layout

		unsigned int crc = (unsigned int)crc32(0L, Z_NULL, 0);
		crc				 = crcOf(crc, internalRAM, HT_INTERNAL_RAM);
		crc				 = crcOf(crc, workRAM, HT_WORK_RAM);
		crc				 = crcOf(crc, vram, HT_VRAM);
		crc				 = crcOf(crc, paletteRAM, HT_PALETTE_RAM);
		crc				 = crcOf(crc, oam, HT_OAM);
		crc				 = crcOf(crc, ioMem, HT_IO_MEM);
		crc				 = crcOf(crc, reg, sizeof(reg));
		return crc;
	}

	struct StateDumpHeader
	{
		unsigned int magic;		// 'VDMP'
		unsigned int version;	// 1
		unsigned int frame;		// completed frames at the boundary
		unsigned int ramCrc;	// same value as the ram column of -hashtrace
		unsigned int fullCrc;	// same value as the full column of -hashtrace
		unsigned int size;		// bytes of state that follow
	};

	void stateDumpWrite(unsigned int frame)
	{
		memset(s_dumpBuf, 0, HASH_TRACE_STATE_SIZE);

		bool ok = false;
		if (theEmulator.emuWriteMemState)
			ok = theEmulator.emuWriteMemState(s_dumpBuf, HASH_TRACE_STATE_SIZE);

		FILE *file = fopen(s_dumpPath, "wb");
		if (!file)
		{
			systemMessage(0, "Cannot open state dump file: %s", s_dumpPath);
			return;
		}

		StateDumpHeader header;
		header.magic   = 0x504D4456; // 'VDMP' little endian
		header.version = 1;
		header.frame   = frame;
		header.ramCrc  = ramHash();
		header.fullCrc = (unsigned int)crc32(0L, (const Bytef *)s_dumpBuf, HASH_TRACE_STATE_SIZE);
		header.size	   = HASH_TRACE_STATE_SIZE;

		fwrite(&header, sizeof(header), 1, file);
		fwrite(s_dumpBuf, 1, HASH_TRACE_STATE_SIZE, file);
		fflush(file);
		fclose(file);

		// A small sidecar is easier to read than the binary header when the
		// two dumps are compared with a text tool.
		char sidecar[600];
		strncpy(sidecar, s_dumpPath, sizeof(sidecar) - 1);
		sidecar[sizeof(sidecar) - 1] = 0;
		char *dot = strrchr(sidecar, '.');
		if (dot)
			strcpy(dot, ".txt");
		else
			strncat(sidecar, ".txt", sizeof(sidecar) - strlen(sidecar) - 1);

		FILE *text = fopen(sidecar, "wb");
		if (text)
		{
			fprintf(text, "frame=%u\nok=%d\nram=%08x\nfull=%08x\nsize=%u\n",
			        frame, ok ? 1 : 0, header.ramCrc, header.fullCrc, header.size);
			fclose(text);
		}
	}
}

bool hashTraceEnabled()
{
	return s_file != NULL;
}

void hashTraceEnable(const char *path)
{
	if (s_file || !path || !path[0])
		return;

	s_file = fopen(path, "wb");
	if (!s_file)
	{
		systemMessage(0, "Cannot open hash trace file: %s", path);
		return;
	}

	s_buf = (char *)malloc(HASH_TRACE_STATE_SIZE);
	if (!s_buf)
	{
		fclose(s_file);
		s_file = NULL;
		systemMessage(0, "Cannot allocate %d bytes for the hash trace buffer", HASH_TRACE_STATE_SIZE);
		return;
	}

	fprintf(s_file, "# vba-rr state hash trace v2\n");
	fprintf(s_file, "# state_size=%d\n", HASH_TRACE_STATE_SIZE);
	fprintf(s_file, "# ram = internalRAM+workRAM+vram+paletteRAM+oam+ioMem+reg (no pix/sound/rtc)\n");
	fprintf(s_file, "# full = crc32 over the gzipped emuWriteMemState blob (includes pix: may vary with OSD timing)\n");
	fprintf(s_file, "frame,ram,full,ok\n");

	s_frame	 = 0;
	s_failed = 0;
	s_ready	 = true;

	atexit(hashTraceAtExit);
}

void hashTraceFrameBoundary()
{
	// The dump and the trace share the same per-frame call point but are
	// independent switches: s_tick counts frames even when -hashtrace is off.
	if (!s_dumpInit)
		stateDumpInit();
	if (!s_pcInit)
		pcTraceInit();

	const unsigned int tick = s_tick++;
	pcTraceTick(tick);

	if (s_dumpEnabled && tick == s_dumpFrame)
	{
		s_dumpEnabled = false; // one shot: never overwrite a good dump
		stateDumpWrite(tick);
	}

	if (!s_ready)
		return;

	// Zero first: emuWriteMemState does not report how many bytes it wrote, so
	// the unused tail has to be deterministic.
	memset(s_buf, 0, HASH_TRACE_STATE_SIZE);

	bool ok = false;
	if (theEmulator.emuWriteMemState)
		ok = theEmulator.emuWriteMemState(s_buf, HASH_TRACE_STATE_SIZE);

	unsigned int ram  = ramHash();
	unsigned int full = (unsigned int)crc32(0L, (const Bytef *)s_buf, HASH_TRACE_STATE_SIZE);

	fprintf(s_file, "%u,%08x,%08x,%d\n", s_frame, ram, full, ok ? 1 : 0);
	if (!ok)
		++s_failed;

	++s_frame;

	// Flush about every 10 seconds so a crash late in a long replay does not
	// lose the whole trace.
	if ((s_frame % 600) == 0)
		fflush(s_file);
}

void hashTraceClose()
{
	if (!s_ready)
		return;

	s_ready = false;

	if (s_file)
	{
		fprintf(s_file, "# end frames=%u failed=%u\n", s_frame, s_failed);
		fflush(s_file);
		fclose(s_file);
		s_file = NULL;
	}

	if (s_buf)
	{
		free(s_buf);
		s_buf = NULL;
	}
}

void pcTraceRecord(unsigned int pc, int ticks, unsigned int prefetchCount,
                   unsigned int totalTicks, int nextEvent, int prefetch)
{
	if (!s_pcActive || !s_pcFile)
		return;

	unsigned int record[6];
	record[0] = pc;
	record[1] = (unsigned int)ticks;
	record[2] = prefetchCount;
	record[3] = totalTicks;
	record[4] = (unsigned int)nextEvent;
	record[5] = (unsigned int)prefetch;
	fwrite(record, sizeof(record), 1, s_pcFile);
	++s_pcCount;
}
