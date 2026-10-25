#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cassert>

#ifdef HAVE_STRINGS_H
#   include <strings.h>
#endif

#if defined(__unix) || defined(__linux) || defined(__sun) || defined(__DJGPP)
#   include <unistd.h>
#   include <sys/types.h>
#   include <sys/stat.h>
#   include <climits>
#   define stricmp strcasecmp
// FIXME: this is wrong, but we don't want buffer overflow
#   if defined _MAX_PATH
#       undef _MAX_PATH
//#       define _MAX_PATH 128
#       define _MAX_PATH 260
#   endif
#endif

#ifdef WIN32
#   include <io.h>
#   ifndef W_OK
#       define W_OK 2
#   endif
#   define ftruncate chsize
#endif

#include "movie.h"
#include "System.h"
#include "SystemGlobals.h"
#include "../gba/GBA.h"
#include "../gba/GBAGlobals.h"
#include "../gba/RTC.h"
#include "../gb/GB.h"
#include "../gb/gbGlobals.h"
#include "inputGlobal.h"
#include "Util.h"
#include <algorithm>

#include "vbalua.h"
#include "CoreOptions.h"
#include "MovieHost.h"

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

using namespace std;

SMovie Movie;
bool   loadingMovie = false;

// probably bad idea to have so many global variables, but I hate to recompile almost everything after editing VBA.h
bool autoConvertMovieWhenPlaying = false;

static u16 initialInputs[4] = { 0 };

static bool resetSignaled	  = false;
static bool resetSignaledLast = false;

static int prevEmulatorType, prevBorder, prevWinBorder, prevBorderAuto;

// little-endian integer pop/push functions:
static inline uint32 Pop32(const uint8 * &ptr)
{
	uint32 v = (ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24));
	ptr += 4;
	return v;
}

static inline uint16 Pop16(const uint8 * &ptr) /* const version */
{
	uint16 v = (ptr[0] | (ptr[1] << 8));
	ptr += 2;
	return v;
}

static inline uint16 Pop16(uint8 * &ptr) /* non-const version */
{
	uint16 v = (ptr[0] | (ptr[1] << 8));
	ptr += 2;
	return v;
}

static inline uint8 Pop8(const uint8 * &ptr)
{
	return *(ptr)++;
}

static inline void Push32(uint32 v, uint8 * &ptr)
{
	ptr[0] = (uint8)(v & 0xff);
	ptr[1] = (uint8)((v >> 8) & 0xff);
	ptr[2] = (uint8)((v >> 16) & 0xff);
	ptr[3] = (uint8)((v >> 24) & 0xff);
	ptr	  += 4;
}

static inline void Push16(uint16 v, uint8 * &ptr)
{
	ptr[0] = (uint8)(v & 0xff);
	ptr[1] = (uint8)((v >> 8) & 0xff);
	ptr	  += 2;
}

static inline void Push8(uint8 v, uint8 * &ptr)
{
	*ptr++ = v;
}

// little-endian integer read/write functions:
static inline uint16 Read16(const uint8 *ptr)
{
	return ptr[0] | (ptr[1] << 8);
}

static inline void Write16(uint16 v, uint8 *ptr)
{
	ptr[0] = uint8(v & 0xff);
	ptr[1] = uint8((v >> 8) & 0xff);
}

static long get_movie_file_size(FILE *fp)
{
	long cur_pos = ftell(fp);
	fseek(fp, 0, SEEK_END);
	long length = ftell(fp);
	fseek(fp, cur_pos, SEEK_SET);
	return length;
}

static int get_movie_frame_size(SMovie &mov)
{
	int num_controllers = 0;

	for (int i = 0; i < MOVIE_NUM_OF_POSSIBLE_CONTROLLERS; ++i)
		if (mov.header.controllerFlags & MOVIE_CONTROLLER(i))
			++num_controllers;

	return CONTROLLER_DATA_SIZE * num_controllers;
}

static void reserve_movie_buffer_space(uint32 space_needed)
{
	if (space_needed > Movie.inputBufferSize)
	{
		size_t ptr_offset	= (size_t)(Movie.inputBufferPtr - Movie.inputBuffer);
		uint32 alloc_chunks = (space_needed - 1) / BUFFER_GROWTH_SIZE + 1;
		uint32 old_size		= Movie.inputBufferSize;
		Movie.inputBufferSize = BUFFER_GROWTH_SIZE * alloc_chunks;
		void *tmp = realloc(Movie.inputBuffer, Movie.inputBufferSize);
		if (!tmp) free(Movie.inputBuffer);
		Movie.inputBuffer = reinterpret_cast<uint8 *>(tmp);
		// FIXME: this only fixes the random input problem during dma-frame-skip, but not the skip
		memset(Movie.inputBuffer + old_size, 0, Movie.inputBufferSize - old_size);
		Movie.inputBufferPtr = Movie.inputBuffer + ptr_offset;
	}
}

static int read_movie_header(FILE *file, SMovie &movie)
{
	assert(file != NULL);
	assert(VBM_HEADER_SIZE == sizeof(SMovieFileHeader)); // sanity check on the header type definition

	uint8 headerData [VBM_HEADER_SIZE];

	if (fread(headerData, 1, VBM_HEADER_SIZE, file) != VBM_HEADER_SIZE)
		return MOVIE_WRONG_FORMAT;  // if we failed to read in all VBM_HEADER_SIZE bytes of the header

	const uint8 *	  ptr	 = headerData;
	SMovieFileHeader &header = movie.header;

	header.magic = Pop32(ptr);
	if (header.magic != VBM_MAGIC)
		return MOVIE_WRONG_FORMAT;

	header.version = Pop32(ptr);
	if (header.version != VBM_VERSION)
		return MOVIE_WRONG_VERSION;

	header.uid = Pop32(ptr);
	header.length_frames  = Pop32(ptr) + 1;    // HACK: add 1 to the length for compatibility
	header.rerecord_count = Pop32(ptr);

	header.startFlags	   = Pop8(ptr);
	header.controllerFlags = Pop8(ptr);
	header.typeFlags	   = Pop8(ptr);
	header.optionFlags	   = Pop8(ptr);

	header.saveType		  = Pop32(ptr);
	header.flashSize	  = Pop32(ptr);
	header.gbEmulatorType = Pop32(ptr);

	for (int i = 0; i < 12; i++)
		header.romTitle[i] = Pop8(ptr);

	header.minorVersion = Pop8(ptr);

	header.romCRC = Pop8(ptr);
	header.romOrBiosChecksum = Pop16(ptr);
	header.romGameCode		 = Pop32(ptr);

	header.offset_to_savestate		 = Pop32(ptr);
	header.offset_to_controller_data = Pop32(ptr);

	return MOVIE_SUCCESS;
}

static void write_movie_header(FILE *file, const SMovie &movie)
{
	assert(ftell(file) == 0); // we assume file points to beginning of movie file

	uint8  headerData [VBM_HEADER_SIZE];
	uint8 *ptr = headerData;
	const SMovieFileHeader &header = movie.header;

	Push32(header.magic, ptr);
	Push32(header.version, ptr);

	Push32(header.uid, ptr);
	Push32(header.length_frames - 1, ptr);     // HACK: reduce the length by 1 for compatibility with certain faulty old tools
	                                           // like TME
	Push32(header.rerecord_count, ptr);

	Push8(header.startFlags, ptr);
	Push8(header.controllerFlags, ptr);
	Push8(header.typeFlags, ptr);
	Push8(header.optionFlags, ptr);

	Push32(header.saveType, ptr);
	Push32(header.flashSize, ptr);
	Push32(header.gbEmulatorType, ptr);

	for (int i = 0; i < 12; ++i)
		Push8(header.romTitle[i], ptr);

	Push8(header.minorVersion, ptr);

	Push8(header.romCRC, ptr);
	Push16(header.romOrBiosChecksum, ptr);
	Push32(header.romGameCode, ptr);

	Push32(header.offset_to_savestate, ptr);
	Push32(header.offset_to_controller_data, ptr);

	fwrite(headerData, 1, VBM_HEADER_SIZE, file);
}

static void flush_movie_header()
{
	assert(Movie.file != 0 && "logical error!");
	if (!Movie.file)
		return;

	long originalPos = ftell(Movie.file);

	// (over-)write the header
	fseek(Movie.file, 0, SEEK_SET);
	write_movie_header(Movie.file, Movie);

	fflush(Movie.file);

	fseek(Movie.file, originalPos, SEEK_SET);
}

static void flush_movie_frames()
{
	assert(Movie.file && "logical error!");
	if (!Movie.file)
		return;

	long originalPos = ftell(Movie.file);

	// overwrite the controller data
	fseek(Movie.file, Movie.header.offset_to_controller_data, SEEK_SET);
	fwrite(Movie.inputBuffer, 1, Movie.bytesPerFrame * Movie.header.length_frames, Movie.file);

	fflush(Movie.file);

	fseek(Movie.file, originalPos, SEEK_SET);
}

static void truncate_movie(long length)
{
	// truncate movie to length
	// NOTE: it's certain that the savestate block is never after the
	//       controller data block, because the VBM format decrees it.

	assert(Movie.file && length >= 0);
	if (!Movie.file || length < 0)
		return;

	assert(Movie.header.offset_to_savestate <= Movie.header.offset_to_controller_data);
	if (Movie.header.offset_to_savestate > Movie.header.offset_to_controller_data)
		return;

	Movie.header.length_frames = length;
	flush_movie_header();
	const long truncLen = long(Movie.header.offset_to_controller_data + Movie.bytesPerFrame * length);
	if (get_movie_file_size(Movie.file) != truncLen)
	{
		ftruncate(fileno(Movie.file), truncLen);
	}
}

static void preserve_movie_init_input()
{
	for (int i = 0; i < MOVIE_NUM_OF_POSSIBLE_CONTROLLERS; ++i)
	{
		if (systemCartridgeType == IMAGE_GBA)
		{
			initialInputs[i] = u16(~P1 & 0x03FF);
		}
		else
		{
			extern int32 gbJoymask[4];
			for (int i = 0; i < 4; ++i)
				initialInputs[i] = u16(gbJoymask[i] & 0xFFFF);
		}
	}
}

static void preserve_movie_next_input()
{
	if (Movie.currentFrame < Movie.header.length_frames)
	{
		for (int i = 0; i < 4; ++i)
		{
			u16 movieInput = 0;
			if (Movie.header.controllerFlags & MOVIE_CONTROLLER(i))
			{
				movieInput = Read16(Movie.inputBufferPtr);
			}
			nextButtons[i] = movieInput;
		}
	}
}

static void fix_movie_old_reset(bool8 reset_last_frame)
{
	// a compatibility burden
	if (Movie.currentFrame < Movie.header.length_frames)
	{
		const u8 OLD_RESET = u8(BUTTON_MASK_OLD_RESET >> 8);
		for (int i = 0; i < MOVIE_NUM_OF_POSSIBLE_CONTROLLERS; ++i)
		{
			if (Movie.header.controllerFlags & MOVIE_CONTROLLER(i))
			{
				uint8 *ptr = Movie.inputBufferPtr + CONTROLLER_DATA_SIZE * i + 1;
				*ptr = (*ptr & ~OLD_RESET) | (-int8(reset_last_frame) & OLD_RESET);
			}
		}
	}
}

static void change_movie_state(MovieState new_state)
{
#if !defined(SDL)
	g_core.frameSearching	   = false;
	g_core.frameSearchSkipping = false;
#endif

	if (new_state == MOVIE_STATE_NONE)
	{
		if (Movie.state == MOVIE_STATE_NONE)
			return;

		truncate_movie(Movie.header.length_frames);
		fclose(Movie.file);
		Movie.file = NULL;
		Movie.currentFrame		  = 0;
		Movie.unused			  = false;
		Movie.RecordedNewRerecord = false;
		Movie.RecordedThisSession = false;
#if !defined(SDL)
		// undo changes to border settings
		{
			gbBorderOn = prevBorder;
			g_core.winGbBorderOn = prevWinBorder;
			gbBorderAutomatic	 = prevBorderAuto;
			systemGbBorderOn();
		}
#endif
		gbEmulatorType = prevEmulatorType;

#ifdef USE_GBA_CORE_V7
		sramInitFix = true;
#endif

#ifdef USE_GB_CORE_V7
		gbDMASpeedVersion = true;
		gbEchoRAMFixOn	  = true;

		gbNullInputHackTempEnabled = gbNullInputHackEnabled;
#else
		gbV20GBFrameTimingHackTemp = gbV20GBFrameTimingHack;
#endif

		if (Movie.inputBuffer)
		{
			free(Movie.inputBuffer);
			Movie.inputBuffer = NULL;
		}

		// clear out the current movie
		VBAMovieInit();

	}
	else if (new_state == MOVIE_STATE_PLAY)
	{
		assert(Movie.file);

		// this would cause problems if not dealt with
		if (Movie.currentFrame >= Movie.header.length_frames)
		{
			new_state = MOVIE_STATE_END;
			Movie.inputBufferPtr = Movie.inputBuffer + Movie.bytesPerFrame * Movie.header.length_frames;
		}
		else if (Movie.state == MOVIE_STATE_RECORD)
		{
			Movie.RecordedNewRerecord = false;
			systemScreenMessage("Movie replay (continue)");
		}
	}
	else if (new_state == MOVIE_STATE_RECORD)
	{
		assert(Movie.file);

		Movie.unused = false;

		VBAMovieConvertCurrent(false); // conversion for safety

		// this would cause problems if not dealt with
		if (Movie.currentFrame > Movie.header.length_frames)
		{
			new_state = MOVIE_STATE_END;
			Movie.inputBufferPtr = Movie.inputBuffer + Movie.bytesPerFrame * Movie.header.length_frames;
		}
		else
		{
			Movie.RecordedNewRerecord = true;
		}
		fseek(Movie.file, Movie.header.offset_to_controller_data + Movie.bytesPerFrame * Movie.currentFrame, SEEK_SET);

		systemScreenMessage("Movie re-record");
	}

	if (new_state == MOVIE_STATE_END && Movie.state != MOVIE_STATE_END)
	{
#if defined(SDL)
		systemClearJoypads();
#endif
		systemScreenMessage("Movie end");
	}

	Movie.state = new_state;

	// checking for movie end
	bool willPause = false;

	// if the movie's been set to pause at a certain frame
	if (Movie.state != MOVIE_STATE_NONE && Movie.pauseFrame >= 0 && Movie.currentFrame == (uint32)Movie.pauseFrame)
	{
		Movie.pauseFrame = -1;
		willPause		 = true;
	}

	if (Movie.state == MOVIE_STATE_END)
	{
		if (Movie.currentFrame == Movie.header.length_frames)
		{
#if !defined(SDL)
			if (g_core.movieOnEndPause)
			{
				willPause = true;
			}
#else
			// SDL FIXME
#endif

#if !defined(SDL)
			switch (g_core.movieOnEndBehavior)
			{
			case 1:
				// the old behavior
				//VBAMovieRestart();
				break;
			case 2:
#else
			// SDL FIXME
#endif
				if (Movie.RecordedThisSession)
				{
					// if user has been recording this movie since the last time it started playing,
					// they probably don't want the movie to end now during playback,
					// so switch back to recording when it reaches the end
					VBAMovieSwitchToRecording();
					systemScreenMessage("Recording resumed");
					willPause = true;
				}
#if !defined(SDL)
				break;
			case 3:
				// keep open
				break;
			case 0:
			// fall through
			default:
				// close movie
				//VBAMovieStop(false);
				break;
			}
#else
				// SDL FIXME
#endif
		}
#if 1
		else if (Movie.currentFrame > Movie.header.length_frames)
		{
#if !defined(SDL)
			switch (g_core.movieOnEndBehavior)
			{
			case 1:
				// FIXME: this should be delayed till the current frame ends
				VBAMovieRestart();
				break;
			case 2:
				// nothing
				break;
			case 3:
				// keep open
				break;
			case 0:
			// fall through
			default:
				// close movie
				VBAMovieStop(false);
				break;
			}
#else
			// SDLFIXME
#endif
		}
#endif
	} // end if (Movie.state == MOVIE_STATE_END)

	if (willPause)
	{
		systemSetPause(true);
	}
}

void VBAMovieInit()
{
	MovieEditMode editMode = Movie.editMode;

	memset(&Movie, 0, sizeof(Movie));
	Movie.state		 = MOVIE_STATE_NONE;
	Movie.pauseFrame = -1;

	resetSignaled	  = false;
	resetSignaledLast = false;

	Movie.unused	  = false;
	Movie.RecordedNewRerecord = false;
	Movie.RecordedThisSession = false;

	Movie.editMode = editMode;
}

void VBAMovieGetRomInfo(const SMovie &movieInfo, char romTitle [12], uint32 &romGameCode, uint16 &checksum, uint8 &crc)
{
	if (systemCartridgeType == IMAGE_GBA) // GBA
	{
		extern u8 *rom;
		memcpy(romTitle, &rom[0xa0], 12); // GBA TITLE
		memcpy(&romGameCode, &rom[0xac], 4); // GBA ROM GAME CODE
		if ((movieInfo.header.optionFlags & MOVIE_SETTING_USEBIOSFILE) != 0)
			checksum = utilCalcBIOSChecksum(bios, 4);  // GBA BIOS CHECKSUM
		else
			checksum = 0;
		crc = rom[0xbd]; // GBA ROM CRC
	}
	else // non-GBA
	{
		extern u8 *gbRom;
		memcpy(romTitle, &gbRom[0x134], 12); // GB TITLE (note this can be 15 but is truncated to 12)
		romGameCode = (uint32)gbRom[0x146]; // GB ROM UNIT CODE

		checksum = (gbRom[0x14e] << 8) | gbRom[0x14f]; // GB ROM CHECKSUM, read from big-endian
		crc		 = gbRom[0x14d]; // GB ROM CRC
	}
}

#ifdef SDL
static void GetBatterySaveName(char *buffer)
{
	extern char batteryDir[2048], filename[2048];     // from SDL.cpp
	extern char *sdlGetFilename(char *name);     // from SDL.cpp
	if (batteryDir[0])
		sprintf(buffer, "%s/%s.sav", batteryDir, sdlGetFilename(filename));
	else
		sprintf(buffer, "%s.sav", filename);
}

#endif

static void SetPlayEmuSettings()
{
	prevEmulatorType = gbEmulatorType;
	gbEmulatorType	 = Movie.header.gbEmulatorType;

	// The movie format has a REMOVEINTROS flag, but it was never wired up:
	// VBA.h had the field commented out and nothing ever honoured it. Keep it
	// that way so old movies replay exactly as they always did.
#if !defined(SDL)
	g_core.skipBiosIntro = (Movie.header.optionFlags & MOVIE_SETTING_SKIPBIOSINTRO) != 0;
	g_core.useBiosFile	= (Movie.header.optionFlags & MOVIE_SETTING_USEBIOSFILE) != 0;
#else
	extern int	 saveType, sdlRtcEnable, sdlFlashSize;   // from SDL.cpp
	extern bool8 useBios, skipBios, removeIntros;     // from SDL.cpp
	useBios		 = (Movie.header.optionFlags & MOVIE_SETTING_USEBIOSFILE) != 0;
	skipBios	 = (Movie.header.optionFlags & MOVIE_SETTING_SKIPBIOSINTRO) != 0;
	removeIntros = false /*(Movie.header.optionFlags & MOVIE_SETTING_REMOVEINTROS) != 0*/;
#endif

#ifdef USE_GBA_CORE_V7
	extern void SetPrefetchHack(bool);
	if (systemCartridgeType == IMAGE_GBA)   // lag disablement applies only to GBA
		SetPrefetchHack((Movie.header.optionFlags & MOVIE_SETTING_LAGHACK) != 0);

	sramInitFix = (Movie.header.optionFlags & MOVIE_SETTING_SRAMINITFIX) != 0;
#endif
#ifdef USE_GB_CORE_V7
	gbNullInputHackTempEnabled = ((Movie.header.optionFlags & MOVIE_SETTING_GBINPUTHACK) != 0);
#else
	gbV20GBFrameTimingHackTemp = ((Movie.header.optionFlags & MOVIE_SETTING_GBINPUTHACK) != 0);
#endif

	// some GB/GBC games depend on the sound rate, so just use the highest one
	systemSoundSetQuality(1);
	useOldFrameTiming = false;

#ifdef USE_GB_CORE_V7
	if ((Movie.header.optionFlags & MOVIE_SETTING_GBCFF55FIX) != 0)
		gbDMASpeedVersion = true;
	else
		gbDMASpeedVersion = false;     // old CGB HDMA5 timing was used

	if ((Movie.header.optionFlags & MOVIE_SETTING_GBECHORAMFIX) != 0)
		gbEchoRAMFixOn = true;
	else
		gbEchoRAMFixOn = false;
#endif

#if !defined(SDL)
	rtcEnable((Movie.header.optionFlags & MOVIE_SETTING_RTCENABLE) != 0);
	g_core.winSaveType	= Movie.header.saveType;
	g_core.winFlashSize = Movie.header.flashSize;

	prevBorder	   = gbBorderOn;
	prevWinBorder  = g_core.winGbBorderOn;
	prevBorderAuto = gbBorderAutomatic;
	if ((gbEmulatorType == 2 || gbEmulatorType == 5)
	    && !g_core.hideMovieBorder) // games played in SGB mode can have a border
	{
		gbBorderOn = true;
		g_core.winGbBorderOn = true;
		gbBorderAutomatic	 = false;
	}
	else
	{
		gbBorderOn = false;
		g_core.winGbBorderOn = false;
		gbBorderAutomatic	 = false;
		if (g_core.hideMovieBorder)
		{
			g_core.hideMovieBorder = false;
			prevBorder = false;     // it might be expected behaviour that it stays hidden after the movie
		}
	}
	systemGbBorderOn();
#else
	sdlRtcEnable = (Movie.header.optionFlags & MOVIE_SETTING_RTCENABLE) != 0;
	saveType	 = Movie.header.saveType;
	sdlFlashSize = Movie.header.flashSize;
#endif
}

static void HardResetAndSRAMClear()
{
	// A frontend that owns the running game restarts it itself (erase the
	// battery file, then reload the ROM). Ask it first; only fall back to the
	// portable path when there is no such frontend.
	if (movie_host::hardResetAndClearSram())
		return;

#ifdef SDL
	char fname [1024];
	GetBatterySaveName(fname);
	remove(fname);     // delete the damn SRAM file
#endif

	// Henceforth, emuCleanUp means "clear out SRAM"
	//theEmulator.emuCleanUp();     // keep it from being resurrected from RAM <--This is wrong, it'll deallocate all variables
	// --Felipe

	/// FIXME the correct SDL code to call for a full restart isn't in a function yet
	theEmulator.emuReset();
}

int VBAMovieOpen(const char *filename, bool8 read_only)
{
	loadingMovie = true;
	uint8 movieReadOnly = read_only ? 1 : 0;

	FILE * file;
	STREAM stream;
	int	   result;
	int	   fn;

	char movie_filename[_MAX_PATH];
#ifdef WIN32
	_fullpath(movie_filename, filename, _MAX_PATH);
#else
	// SDL FIXME: convert to fullpath
	strncpy(movie_filename, filename, _MAX_PATH);
	movie_filename[_MAX_PATH - 1] = '\0';
#endif

	if (movie_filename[0] == '\0')
	{ loadingMovie = false; return MOVIE_FILE_NOT_FOUND; }

	if (!emulating)
	{ loadingMovie = false; return MOVIE_UNKNOWN_ERROR; }

//	bool alreadyOpen = (Movie.file != NULL && _stricmp(movie_filename, Movie.filename) == 0);

//	if (alreadyOpen)
	change_movie_state(MOVIE_STATE_NONE);     // have to stop current movie before trying to re-open it

	if (!(file = fopen(movie_filename, "rb+")))
		if (!(file = fopen(movie_filename, "rb")))
		{ loadingMovie = false; return MOVIE_FILE_NOT_FOUND; }
	//else
	//	movieReadOnly = 2; // we have to open the movie twice, no need to do this both times

//	if (!alreadyOpen)
//		change_movie_state(MOVIE_STATE_NONE); // stop current movie when we're able to open the other one
//
//	if (!(file = fopen(movie_filename, "rb+")))
//		if(!(file = fopen(movie_filename, "rb")))
//			{loadingMovie = false; return MOVIE_FILE_NOT_FOUND;}
//		else
//			movieReadOnly = 2;

	// read header
	if ((result = read_movie_header(file, Movie)) != MOVIE_SUCCESS)
	{
		fclose(file);
		{ loadingMovie = false; return result; }
	}

	// set emulator settings that make the movie more likely to stay synchronized
	SetPlayEmuSettings();

	// read the metadata / author info from file
	fread(Movie.authorInfo, 1, MOVIE_METADATA_SIZE, file);
	fn = dup(fileno(file)); // XXX: why does this fail?? it returns -1 but errno == 0
	fclose(file);

	// apparently this lseek is necessary
	lseek(fn, Movie.header.offset_to_savestate, SEEK_SET);
	if (!(stream = utilGzReopen(fn, "rb")))
		if (!(stream = utilGzOpen(movie_filename, "rb")))
		{ loadingMovie = false; return MOVIE_FILE_NOT_FOUND; }
		else
			fn = dup(fileno(file));
	// in case the above dup failed but opening the file normally doesn't fail

	if (Movie.header.startFlags & MOVIE_START_FROM_SNAPSHOT)
	{
		CallRegisteredLuaFunctions(LUACALL_BEFORESTATELOAD);

		// load the snapshot
		result = theEmulator.emuReadStateFromStream(stream) ? MOVIE_SUCCESS : MOVIE_WRONG_FORMAT;

		// FIXME: Kludge for conversion
		preserve_movie_init_input();

		CallRegisteredLuaFunctions(LUACALL_AFTERSTATELOAD);
	}
	else if (Movie.header.startFlags & MOVIE_START_FROM_SRAM)
	{
#if 1
		// 'soft' reset:
		theEmulator.emuReset();
#else
		// 'harder' reset as if just booted with SRAM
		HardReset();
#endif
		// load the SRAM
		result = theEmulator.emuReadBatteryFromStream(stream) ? MOVIE_SUCCESS : MOVIE_WRONG_FORMAT;
	}
	else
	{
		HardResetAndSRAMClear();
	}

	utilGzClose(stream);

	if (result != MOVIE_SUCCESS)
	{ loadingMovie = false; return result; }

//	if (!(file = fopen(movie_filename, /*read_only ? "rb" :*/ "rb+"))) // want to be able to switch out of read-only later
//	{
//		if(!Movie.readOnly || !(file = fopen(movie_filename, "rb"))) // try read-only if failed
//			return MOVIE_FILE_NOT_FOUND;
//	}
	if (!(file = fopen(movie_filename, "rb+")))
		if (!(file = fopen(movie_filename, "rb")))
		{ loadingMovie = false; return MOVIE_FILE_NOT_FOUND; }
		else
			movieReadOnly = 2;

	// recalculate length of movie from the file size
	Movie.bytesPerFrame = get_movie_frame_size(Movie);
	fseek(file, 0, SEEK_END);
	long fileSize = ftell(file);
	Movie.header.length_frames = (fileSize - Movie.header.offset_to_controller_data) / Movie.bytesPerFrame;

	if (fseek(file, Movie.header.offset_to_controller_data, SEEK_SET))
	{ fclose(file); loadingMovie = false; return MOVIE_WRONG_FORMAT; }

	strcpy(Movie.filename, movie_filename);
	Movie.file = file;
	Movie.inputBufferPtr = Movie.inputBuffer;
	Movie.currentFrame	 = 0;
	Movie.readOnly		 = movieReadOnly;

	// read controller data
	uint32 to_read = Movie.bytesPerFrame * Movie.header.length_frames;
	reserve_movie_buffer_space(to_read);
	fread(Movie.inputBuffer, 1, to_read, file);
	++Movie.inputGeneration;

	change_movie_state(MOVIE_STATE_PLAY);

	char messageString[64] = "Movie ";
	bool converted		   = false;
	if (autoConvertMovieWhenPlaying)
	{
		int result = VBAMovieConvertCurrent(false);
		if (result == MOVIE_SUCCESS)
			strcat(messageString, "converted and ");
		else if (result == MOVIE_WRONG_VERSION)
			strcat(messageString, "higher revision ");
	}

	if (Movie.state == MOVIE_STATE_PLAY)
		strcat(messageString, "replaying ");
	else
		strcat(messageString, "finished ");
	if (Movie.readOnly)
		strcat(messageString, "(read only)");
	else
		strcat(messageString, "(editable)");
	systemScreenMessage(messageString);

	preserve_movie_next_input();

	VBAUpdateButtonPressDisplay();
	VBAUpdateFrameCountDisplay();
	systemRefreshScreen();

	{ loadingMovie = false; return MOVIE_SUCCESS; }
}

static void SetRecordEmuSettings()
{
	Movie.header.optionFlags = 0;
#if !defined(SDL)
	if (g_core.useBiosFile)
		Movie.header.optionFlags |= MOVIE_SETTING_USEBIOSFILE;
	if (g_core.skipBiosIntro)
		Movie.header.optionFlags |= MOVIE_SETTING_SKIPBIOSINTRO;
	if (rtcIsEnabled())
		Movie.header.optionFlags |= MOVIE_SETTING_RTCENABLE;
	Movie.header.saveType  = g_core.winSaveType;
	Movie.header.flashSize = g_core.winFlashSize;
#else
	extern int	 saveType, sdlRtcEnable, sdlFlashSize;   // from SDL.cpp
	extern bool8 useBios, skipBios;     // from SDL.cpp
	if (useBios)
		Movie.header.optionFlags |= MOVIE_SETTING_USEBIOSFILE;
	if (skipBios)
		Movie.header.optionFlags |= MOVIE_SETTING_SKIPBIOSINTRO;
	if (sdlRtcEnable)
		Movie.header.optionFlags |= MOVIE_SETTING_RTCENABLE;
	Movie.header.saveType  = saveType;
	Movie.header.flashSize = sdlFlashSize;
#endif
	prevEmulatorType = Movie.header.gbEmulatorType = gbEmulatorType;

#ifdef USE_GBA_CORE_V7
	if (!memLagTempEnabled)
		Movie.header.optionFlags |= MOVIE_SETTING_LAGHACK;
	Movie.header.optionFlags |= MOVIE_SETTING_SRAMINITFIX;
	sramInitFix = true;
#endif

#ifdef USE_GB_CORE_V7
	if (gbNullInputHackTempEnabled)
#else
	if (gbV20GBFrameTimingHackTemp)
#endif
		Movie.header.optionFlags |= MOVIE_SETTING_GBINPUTHACK;

	Movie.header.optionFlags |= MOVIE_SETTING_GBCFF55FIX;
#ifdef USE_GB_CORE_V7
	gbDMASpeedVersion = true;
#endif

	Movie.header.optionFlags |= MOVIE_SETTING_GBECHORAMFIX;
#ifdef USE_GB_CORE_V7
	gbEchoRAMFixOn = true;
#endif

	// some GB/GBC games depend on the sound rate, so just use the highest one
	systemSoundSetQuality(1);

	useOldFrameTiming = false;

#if !defined(SDL)
	// see SetPlayEmuSettings() for why removeIntros is not restored
	prevBorder	   = gbBorderOn;
	prevWinBorder  = g_core.winGbBorderOn;
	prevBorderAuto = gbBorderAutomatic;
	if (gbEmulatorType == 2 || gbEmulatorType == 5)     // only games played in SGB mode will have a border
	{
		gbBorderOn = true;
		g_core.winGbBorderOn = true;
		gbBorderAutomatic	 = false;
	}
	else
	{
		gbBorderOn = false;
		g_core.winGbBorderOn = false;
		gbBorderAutomatic	 = false;
	}
	systemGbBorderOn();
#else
	/// SDLFIXME
#endif
}

int VBAGetCurrentCoreVersion()
{
#if defined(USE_GBA_CORE_V7) || defined(USE_GB_CORE_V7)
	return MOVIE_CORE_VERSION_V7;
#else
	return MOVIE_CORE_VERSION_V8;
#endif
}

int VBAMovieGetCoreVersion(const SMovie &movie)
{
	// The VBM format has no dedicated core-version field.  V7's
	// SetRecordEmuSettings() always sets SRAMINITFIX, and sets LAGHACK when
	// the lag hack is enabled.  V8 sets neither bit.  This is the only
	// reliable distinction available in existing V7/V8 recordings.
	if (movie.header.optionFlags & (MOVIE_SETTING_SRAMINITFIX | MOVIE_SETTING_LAGHACK))
		return MOVIE_CORE_VERSION_V7;

	// Recordings made before those flags were introduced, or by another
	// tool, cannot be distinguished from V8.  Treat them as V8: V8 can play
	// V7 movies, so this is the safe default when the information is absent.
	return MOVIE_CORE_VERSION_V8;
}

uint16 VBAMovieGetCurrentInputOf(int which, bool normalOnly)
{
	if (which < 0 || which >= MOVIE_NUM_OF_POSSIBLE_CONTROLLERS)
		return 0;

	return normalOnly ? (movieButtons[which] & BUTTON_REGULAR_MASK) : movieButtons[which];
}

uint16 VBAMovieGetNextInputOf(int which, bool normalOnly)
{
	if (!VBAMovieIsActive() || which < 0 || which >= MOVIE_NUM_OF_POSSIBLE_CONTROLLERS)
		return 0;

	u16 movieInput = 0;
	if (Movie.currentFrame + 1u < Movie.header.length_frames && (Movie.header.controllerFlags & MOVIE_CONTROLLER(which)))
	{
		movieInput = Read16(Movie.inputBufferPtr + Movie.bytesPerFrame + CONTROLLER_DATA_SIZE * which);
	}
	return normalOnly ? (movieInput & BUTTON_REGULAR_MASK) : movieInput;
}

int VBAMovieCreate(const char *filename, const char *authorInfo, uint8 startFlags, uint8 controllerFlags, uint8 typeFlags)
{
	// make sure at least one controller is enabled
	if ((controllerFlags & MOVIE_CONTROLLERS_ANY_MASK) == 0)
		return MOVIE_WRONG_FORMAT;

	if (!emulating)
		return MOVIE_UNKNOWN_ERROR;

	loadingMovie = true;

	FILE * file;
	STREAM stream;
	int	   fn;

	char movie_filename [_MAX_PATH];
#ifdef WIN32
	_fullpath(movie_filename, filename, _MAX_PATH);
#else
	// FIXME: convert to fullpath
	strncpy(movie_filename, filename, _MAX_PATH);
	movie_filename[_MAX_PATH - 1] = '\0';
#endif

	bool alreadyOpen = (Movie.file != NULL && stricmp(movie_filename, Movie.filename) == 0);

	if (alreadyOpen)
		change_movie_state(MOVIE_STATE_NONE);  // have to stop current movie before trying to re-open it

	if (movie_filename[0] == '\0')
	{ loadingMovie = false; return MOVIE_FILE_NOT_FOUND; }

	if (!(file = fopen(movie_filename, "wb")))
	{ loadingMovie = false; return MOVIE_FILE_NOT_FOUND; }

	if (!alreadyOpen)
		change_movie_state(MOVIE_STATE_NONE);  // stop current movie when we're able to open the other one

	// fill in the movie's header
	Movie.header.uid   = (uint32)time(NULL);
	Movie.header.magic = VBM_MAGIC;
	Movie.header.version		 = VBM_VERSION;
	Movie.header.rerecord_count	 = 0;
	Movie.header.length_frames	 = 0;
	Movie.header.startFlags		 = startFlags;
	Movie.header.controllerFlags = controllerFlags;
	Movie.header.typeFlags		 = typeFlags;
	Movie.header.minorVersion	 = VBM_REVISION;

	// set emulator settings that make the movie more likely to stay synchronized when it's later played back
	SetRecordEmuSettings();

	// set ROM and BIOS checksums and stuff
	VBAMovieGetRomInfo(Movie, Movie.header.romTitle, Movie.header.romGameCode, Movie.header.romOrBiosChecksum, Movie.header.romCRC);

	// write the header to file
	write_movie_header(file, Movie);

	// copy over the metadata / author info
	VBAMovieSetMetadata(authorInfo);

	// write the metadata / author info to file
	fwrite(Movie.authorInfo, 1, MOVIE_METADATA_SIZE, file);

	// write snapshot or SRAM if applicable
	if ((Movie.header.startFlags & MOVIE_START_FROM_SNAPSHOT)
	    || (Movie.header.startFlags & MOVIE_START_FROM_SRAM))
	{
		Movie.header.offset_to_savestate = (uint32)ftell(file);

		// close the file and reopen it as a stream:

		fn = dup(fileno(file));
		fclose(file);

		if (!(stream = utilGzReopen(fn, "ab"))) // append mode to start at end, no seek necessary
		{ loadingMovie = false; return MOVIE_FILE_NOT_FOUND; }

		// write the save data:
		if (Movie.header.startFlags & MOVIE_START_FROM_SNAPSHOT)
		{
			// save snapshot
			if (!theEmulator.emuWriteStateToStream(stream))
			{
				utilGzClose(stream);
				{ loadingMovie = false; return MOVIE_UNKNOWN_ERROR; }
			}
		}
		else if (Movie.header.startFlags & MOVIE_START_FROM_SRAM)
		{
			// save SRAM
			if (!theEmulator.emuWriteBatteryToStream(stream))
			{
				utilGzClose(stream);
				{ loadingMovie = false; return MOVIE_UNKNOWN_ERROR; }
			}

#if 1
			// 'soft' reset:
			theEmulator.emuReset();
#else
			// 'harder' reset as if just booted with SRAM
			HardReset();
#endif
		}

		utilGzClose(stream);

		// reopen the file and seek back to the end

		if (!(file = fopen(movie_filename, "rb+")))
		{ loadingMovie = false; return MOVIE_FILE_NOT_FOUND; }

		fseek(file, 0, SEEK_END);
	}
	else // no snapshot or SRAM
	{
		HardResetAndSRAMClear();
	}

	Movie.header.offset_to_controller_data = (uint32)ftell(file);

	strcpy(Movie.filename, movie_filename);
	Movie.file = file;
	Movie.bytesPerFrame	 = get_movie_frame_size(Movie);
	Movie.inputBufferPtr = Movie.inputBuffer;
	Movie.currentFrame	 = 0;
	Movie.readOnly		 = false;

	change_movie_state(MOVIE_STATE_RECORD);
	++Movie.inputGeneration;

	systemScreenMessage("Recording movie...");
	{ loadingMovie = false; return MOVIE_SUCCESS; }
}

void VBAUpdateButtonPressDisplay()
{
	const static int  KeyMaxCount = 16;
								  //  0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15
	const static char KeyMap[]	 = { 'A', 'B', 's', 'S', '>', '<', '^', 'v', 'R', 'L', '!', '?', '{', '}', 'v', '^' };
								  //  <   ^   >   v   A   B   L   R   S   s  M{  M^  M}  Mv  N?  O!
	const static int  KeyOrder[] = {  5,  6,  4,  7,  0,  1,  9,  8,  3,  2, 12, 15, 13, 14, 11, 10 };

	int which = 0;
	u32 currKeys = currentButtons[which];
	u32 nextKeys = nextButtons[which];

	if (VBALuaRemappingInputDisplay(which))
	{
		currKeys = VBALuaRemapInputDisplay(which, currKeys, LuaJoypadType::LUAJOYPAD_RECORD);
		nextKeys = VBALuaRemapInputDisplay(which, nextKeys, LuaJoypadType::LUAJOYPAD_PLAYBACK);
	}

	const int  BufferSize = 64;
	      char buffer[BufferSize] = "                    ";
	const char whiteSpaces[] = "    ";
	const int  whiteOffset = (int)strlen(whiteSpaces);
#ifdef WIN32
	const bool eraseAll = !g_core.inputDisplay;
	char grayList[BufferSize]; // FIXME: no grey available at the moment
	char colorList[BufferSize];
	memset(grayList, 4, strnlen(buffer, BufferSize));
	memset(colorList, 1, strnlen(buffer, BufferSize));

	u32	autoHeldKeys = eraseAll ? 0 : g_core.autoHold & BUTTON_REGULAR_RECORDING_MASK;
	u32	autoFireKeys = eraseAll ? 0 : (g_core.autoFire | g_core.autoFire2) & BUTTON_REGULAR_RECORDING_MASK;
	u32	pressedKeys	 = eraseAll ? 0 : currKeys;
	u32 &lastKeys     = lastButtons[which];

	if (resetSignaled)
		autoHeldKeys |= BUTTON_MASK_NEW_RESET;

	if (g_core.nextInputDisplay && !eraseAll && Movie.state != MOVIE_STATE_NONE)
	{
		for (int i = 0; i < KeyMaxCount; ++i)
		{
			int j	 = KeyOrder[i];
			int mask = (1 << (j));
			buffer[whiteOffset + i] = ((nextKeys & mask) != 0) ? KeyMap[j] : ' ';
		}
	}
	systemScreenMessage(buffer, 3, -1, grayList);

	if (!eraseAll)
	{
		for (int i = 0; i < KeyMaxCount; ++i)
		{
			const int  j		 = KeyOrder[i];
			const int  mask		 = (1 << (j));
			bool	   pressed	 = (pressedKeys  & mask) != 0;
			const bool autoHeld	 = (autoHeldKeys & mask) != 0;
			const bool autoFired = (autoFireKeys & mask) != 0;
			const bool erased	 = (lastKeys & mask) != 0 && (!pressed && !autoHeld && !autoFired);
			extern int textMethod;
			if (textMethod != 2 && (autoHeld || (autoFired && !pressed) || erased))
			{
				int colorNum = 1;     // default is white
				if (autoHeld)
					colorNum += (pressed ? 2 : 1);     // yellow if pressed, red if not
				else if (autoFired)
					colorNum += 5;     // blue if autofired and not currently pressed
				else if (erased)
					colorNum += 8;     // black on black

				colorList[whiteOffset + i] = colorNum;
				pressed = true;
			}
			buffer[whiteOffset + i] = pressed ? KeyMap[j] : ' ';
		}
	}
	systemScreenMessage(buffer, 2, -1, colorList);

	lastKeys  = currKeys;
	lastKeys |= g_core.autoHold & BUTTON_REGULAR_RECORDING_MASK;
	lastKeys |= (g_core.autoFire | g_core.autoFire2) & BUTTON_REGULAR_RECORDING_MASK;
#else
	// don't bother color-coding autofire and such
	if (Movie.state != MOVIE_STATE_NONE)
	{
		for (int i = 0; i < KeyMaxCount; ++i)
		{
			int j	 = KeyOrder[i];
			int mask = (1 << (j));
			buffer[whiteOffset + i] = ((nextKeys & mask) != 0) ? KeyMap[j] : ' ';
		}
	}
	systemScreenMessage(buffer, 3, -1);
	for (int i = 0; i < KeyMaxCount; ++i)
	{
		int j	 = KeyOrder[i];
		int mask = (1 << (j));
		buffer[whiteOffset + i] = ((currKeys & mask) != 0) ? KeyMap[j] : ' ';
	}
	systemScreenMessage(buffer, 2, -1);
#endif
}

void VBAUpdateFrameCountDisplay()
{
	const int MAGICAL_NUMBER = 64;  // FIXME: this won't do any better, but only to remind you of sz issues
	char	  frameDisplayString[MAGICAL_NUMBER * 2];
	char	  lagFrameDisplayString[MAGICAL_NUMBER];
	char	  extraCountDisplayString[MAGICAL_NUMBER];

#if !defined(SDL)
	char colorList[MAGICAL_NUMBER * 2];
	memset(colorList, 1, MAGICAL_NUMBER * 2);

	if (g_core.frameCounter)
#else
	/// SDL FIXME
#endif
	{
		switch (Movie.state)
		{
		case MOVIE_STATE_PLAY:
		case MOVIE_STATE_END:
		{
			int offset = sprintf(frameDisplayString, "%u / ", Movie.currentFrame);
			int length = sprintf(frameDisplayString + offset, "%u", Movie.header.length_frames);
			if (Movie.state == MOVIE_STATE_END)
				memset(colorList + offset, 2, length);
			if (Movie.editMode == MOVIE_EDIT_MODE_OVERWRITE)
			{
				strcat(frameDisplayString, " [W]");
			}
			else if (Movie.editMode == MOVIE_EDIT_MODE_XOR)
			{
				strcat(frameDisplayString, " [X]");
			}
			if (!Movie.readOnly)
			{
				strcat(frameDisplayString, " (editable)");
			}
			break;
		}
		case MOVIE_STATE_RECORD:
		{
			if (Movie.editMode == MOVIE_EDIT_MODE_OVERWRITE)
			{
				sprintf(frameDisplayString, "%u / %u [W](record)", Movie.currentFrame, Movie.header.length_frames);
			}
			else if (Movie.editMode == MOVIE_EDIT_MODE_XOR)
			{
				sprintf(frameDisplayString, "%u / %u [X](record)", Movie.currentFrame, Movie.header.length_frames);
			}
			else
			{
				sprintf(frameDisplayString, "%u (record)", Movie.currentFrame);
			}
			break;
		}
		default:
		{
			sprintf(frameDisplayString, "%u (no movie)", systemCounters.frameCount);
			break;
		}
		}

#if !defined(SDL)
		if (g_core.lagCounter)
#else
		/// SDL FIXME
#endif
		{
//			sprintf(lagFrameDisplayString, " %c %u", systemCounters.laggedLast ? '*' : '|', systemCounters.lagCount);
			sprintf(lagFrameDisplayString, " | %u%s", systemCounters.lagCount, systemCounters.laggedLast ? " *" : "");
			strcat(frameDisplayString, lagFrameDisplayString);
		}

#if !defined(SDL)
		if (g_core.extraCounter)
#else
		/// SDL FIXME
#endif
		{
			sprintf(extraCountDisplayString, " | %d", systemCounters.frameCount - systemCounters.extraCount);
			strcat(frameDisplayString, extraCountDisplayString);
		}
	}
#if !defined(SDL)
	else
	{
		frameDisplayString[0] = '\0';
	}
	systemScreenMessage(frameDisplayString, 1, -1, colorList);
#else
	/// SDL FIXME
	systemScreenMessage(frameDisplayString, 1, -1);
#endif
}

// this function should only be called once every frame
void VBAMovieAfterEmulation()
{
	++Movie.currentFrame;
	if (Movie.state == MOVIE_STATE_PLAY)
	{
		Movie.inputBufferPtr += Movie.bytesPerFrame;
	}
	else if (Movie.state == MOVIE_STATE_RECORD)
	{
		// use first fseek?
		fwrite(Movie.inputBufferPtr, 1, Movie.bytesPerFrame, Movie.file);
		Movie.inputBufferPtr += Movie.bytesPerFrame;
		if (Movie.editMode != MOVIE_EDIT_MODE_DISCARD && Movie.currentFrame < Movie.header.length_frames)
		{
			long original = ftell(Movie.file);
			fwrite(Movie.inputBufferPtr, 1, Movie.bytesPerFrame, Movie.file);
			fseek(Movie.file, original, SEEK_SET);
		}
		else
		{
			Movie.header.length_frames = Movie.currentFrame;
		}
	}
}

// this function should only be called once every frame
void VBAMovieUpdateState()
{
	if (Movie.state == MOVIE_STATE_PLAY)
	{
		if (Movie.currentFrame >= Movie.header.length_frames)
		{
			// the movie ends anyway; what to do next depends on the settings
			change_movie_state(MOVIE_STATE_END);
		}
	}
	else if (Movie.state == MOVIE_STATE_RECORD)
	{
		if (Movie.RecordedNewRerecord)
		{
			if (!VBALuaRerecordCountSkip())
				++Movie.header.rerecord_count;
			Movie.RecordedNewRerecord = false;
		}
		Movie.RecordedThisSession = true;
		flush_movie_header();
	}
	else if (Movie.state == MOVIE_STATE_END)
	{
		change_movie_state(MOVIE_STATE_END);
	}
}

void VBAMovieRead(int i, bool /*sensor*/)
{
	if (Movie.state != MOVIE_STATE_PLAY && Movie.state != MOVIE_STATE_RECORD)
		return;

	if (i < 0 || i >= MOVIE_NUM_OF_POSSIBLE_CONTROLLERS)
		return;      // not a controller we're recognizing

	u16 movieInput = 0;
	if (Movie.currentFrame < Movie.header.length_frames && (Movie.header.controllerFlags & MOVIE_CONTROLLER(i)))
	{
		movieInput = Read16(Movie.inputBufferPtr + CONTROLLER_DATA_SIZE * i);
	}

	// backward compatibility kludge
	//movieInput = (movieInput & ~BUTTON_MASK_OLD_RESET) | (-int(resetSignaledLast) & BUTTON_MASK_OLD_RESET);

	movieButtons[i] = movieInput;
}

void VBAMovieWrite(int i, bool /*sensor*/)
{
	if (Movie.state != MOVIE_STATE_RECORD)
		return;

	if (i < 0 || i >= MOVIE_NUM_OF_POSSIBLE_CONTROLLERS)
		return;      // not a controller we're recognizing

	reserve_movie_buffer_space((uint32)((Movie.inputBufferPtr - Movie.inputBuffer) + Movie.bytesPerFrame * 2));

	if (Movie.header.controllerFlags & MOVIE_CONTROLLER(i))
	{
		// get the current controller data
		uint16 buttonData = currentButtons[i];

		// mask away the irrelevent bits
		buttonData &= BUTTON_REGULAR_RECORDING_MASK;

		uint8 *ptr = Movie.inputBufferPtr + CONTROLLER_DATA_SIZE * i;
		if (Movie.editMode == MOVIE_EDIT_MODE_OVERWRITE || Movie.editMode == MOVIE_EDIT_MODE_XOR)
		{
			nextButtons[i] &= ~BUTTON_MASK_OLD_RESET;
			if ((buttonData & BUTTON_MASK_NEW_RESET) != 0)
				nextButtons[i] |= BUTTON_MASK_OLD_RESET;
			Write16(nextButtons[i], ptr + Movie.bytesPerFrame);
		}

		Write16(buttonData, ptr);
	}
	else
	{
		// pretend the controller is disconnected (otherwise input it gives could cause desync
		// since we're not writing it to the movie)
	}
}

void VBAMovieStop(bool8 suppress_message)
{
	if (Movie.state != MOVIE_STATE_NONE)
	{
		change_movie_state(MOVIE_STATE_NONE);
		if (!suppress_message)
			systemScreenMessage("Movie stop");
	}
}

int VBAMovieGetInfo(const char *filename, SMovie *info)
{
	assert(info != NULL);
	if (info == NULL)
		return -1;

	FILE *	file;
	int		result;
	SMovie &local_movie = *info;

	memset(info, 0, sizeof(*info));
	if (filename[0] == '\0')
		return MOVIE_FILE_NOT_FOUND;
	if (!(file = fopen(filename, "rb")))
		return MOVIE_FILE_NOT_FOUND;

	// read header
	if ((result = (read_movie_header(file, local_movie))) != MOVIE_SUCCESS)
	{
		fclose(file);
		return result;
	}

	// read the metadata / author info from file
	fread(local_movie.authorInfo, 1, sizeof(char) * MOVIE_METADATA_SIZE, file);

	strncpy(local_movie.filename, filename, SMovie::MAX_FILENAME_LENGTH);
	local_movie.filename[SMovie::MAX_FILENAME_LENGTH - 1] = '\0';

	if (Movie.file != NULL && stricmp(local_movie.filename, Movie.filename) == 0) // alreadyOpen
	{
		local_movie.bytesPerFrame		 = Movie.bytesPerFrame;
		local_movie.header.length_frames = Movie.header.length_frames;
	}
	else
	{
		// recalculate length of movie from the file size
		local_movie.bytesPerFrame = get_movie_frame_size(local_movie);
		fseek(file, 0, SEEK_END);
		int fileSize = ftell(file);
		local_movie.header.length_frames =
		    (fileSize - local_movie.header.offset_to_controller_data) / local_movie.bytesPerFrame;
	}

	fclose(file);

	if (access(filename, W_OK))
		info->readOnly = true;

	return MOVIE_SUCCESS;
}

double VBAMovieGetFrameRate()
{
	return systemGetFrameRate();
}

bool VBAMovieHasEnded()
{
	return (Movie.state == MOVIE_STATE_END);
//	return (Movie.state != MOVIE_STATE_NONE && Movie.currentFrame >= Movie.header.length_frames);
}

bool VBAMovieAllowsRerecording()
{
	bool allows = (Movie.state != MOVIE_STATE_NONE) && (Movie.currentFrame <= Movie.header.length_frames);
	return /*!VBAMovieIsReadOnly() &&*/ allows;
}

bool VBAMovieIsActive()
{
	return (Movie.state != MOVIE_STATE_NONE);
}

bool VBAMovieIsLoading()
{
	return loadingMovie;
}

bool VBAMovieIsPlaying()
{
	return (Movie.state == MOVIE_STATE_PLAY);
}

bool VBAMovieIsRecording()
{
	return (Movie.state == MOVIE_STATE_RECORD);
}

bool VBAMovieIsReadOnly()
{
	return VBAMovieIsActive() && Movie.readOnly;
}

bool VBAMovieIsXorInput()
{
	return VBAMovieIsActive() && Movie.editMode == MOVIE_EDIT_MODE_XOR;
}

void VBAMovieToggleReadOnly()
{
	if (!VBAMovieIsActive())
		return;

	if (Movie.readOnly != 2)
	{
		Movie.readOnly = !Movie.readOnly;

		systemScreenMessage(Movie.readOnly ? "Movie now read-only" : "Movie now editable");
	}
	else
	{
		systemScreenMessage("Can't toggle read-only movie");
	}
}

void VBAMoviePrevEditMode()
{
	// FIXME: UNIMPLEMENTED
}

void VBAMovieNextEditMode()
{
	switch (Movie.editMode)
	{
	case MOVIE_EDIT_MODE_DISCARD:
		Movie.editMode = MOVIE_EDIT_MODE_OVERWRITE;
		break;
	case MOVIE_EDIT_MODE_OVERWRITE:
		Movie.editMode = MOVIE_EDIT_MODE_XOR;
		break;
	case MOVIE_EDIT_MODE_XOR:
		Movie.editMode = MOVIE_EDIT_MODE_DISCARD;
		break;
	default:
		Movie.editMode = MOVIE_EDIT_MODE_DISCARD;
		break;
	}
}

void VBAMovieSetEditMode(MovieEditMode mode)
{
	Movie.editMode = mode;
}

MovieEditMode VBAMovieGetEditMode()
{
	return Movie.editMode;
}

uint32 VBAMovieGetVersion()
{
	if (!VBAMovieIsActive())
		return 0;

	return Movie.header.version;
}

uint32 VBAMovieGetMinorVersion()
{
	if (!VBAMovieIsActive())
		return 0;

	return Movie.header.minorVersion;
}

uint32 VBAMovieGetId()
{
	if (!VBAMovieIsActive())
		return 0;

	return Movie.header.uid;
}

uint32 VBAMovieGetLength()
{
	if (!VBAMovieIsActive())
		return 0;

	return Movie.header.length_frames;
}

uint32 VBAMovieGetFrameCounter()
{
	if (!VBAMovieIsActive())
		return 0;

	return Movie.currentFrame;
}

uint32 VBAMovieGetRerecordCount()
{
	if (!VBAMovieIsActive())
		return 0;

	return Movie.header.rerecord_count;
}

uint32 VBAMovieSetRerecordCount(uint32 newRerecordCount)
{
	uint32 oldRerecordCount = 0;
	if (!VBAMovieIsActive())
		return 0;

	oldRerecordCount = Movie.header.rerecord_count;
	Movie.header.rerecord_count = newRerecordCount;
	return oldRerecordCount;
}

std::string VBAMovieGetAuthorInfo()
{
	if (!VBAMovieIsActive())
		return "";

	return Movie.authorInfo;
}

std::string VBAMovieGetFilename()
{
	if (!VBAMovieIsActive())
		return "";

	return Movie.filename;
}

uint32 VBAMovieGetLastErrorInfo()
{
	return Movie.errorInfo;
}

int VBAMovieFreeze(uint8 * *buf, uint32 *size)
{
	// sanity check
	if (!VBAMovieIsActive())
	{
		return MOVIE_NOTHING;
	}

	*buf  = NULL;
	*size = 0;

	// compute size needed for the buffer
	// room for header.uid, currentFrame, and header.length_frames
	uint32 size_needed = sizeof(Movie.header.uid) + sizeof(Movie.currentFrame) + sizeof(Movie.header.length_frames);
	size_needed += (uint32)(Movie.bytesPerFrame * Movie.header.length_frames);
	*buf		 = new uint8[size_needed];
	*size		 = size_needed;

	uint8 *ptr = *buf;
	if (!ptr)
	{
		return MOVIE_FATAL_ERROR;
	}

	Push32(Movie.header.uid, ptr);
	Push32(Movie.currentFrame, ptr);
	Push32(Movie.header.length_frames - 1, ptr);   // HACK: shorten the length by 1 for backward compatibility

	memcpy(ptr, Movie.inputBuffer, Movie.bytesPerFrame * Movie.header.length_frames);

	return MOVIE_SUCCESS;
}

int VBAMovieUnfreeze(const uint8 *buf, uint32 size)
{
	// sanity check
	if (!VBAMovieIsActive())
	{
		return MOVIE_NOT_FROM_A_MOVIE;
	}

	const uint8 *ptr = buf;
	const uint32 headerSize = sizeof(Movie.header.uid) + sizeof(Movie.currentFrame) + sizeof(Movie.header.length_frames);
	if (size < headerSize)
	{
		return MOVIE_WRONG_FORMAT;
	}

	uint32 movie_id		 = Pop32(ptr);
	uint32 current_frame = Pop32(ptr);
	uint32 input_frames	 = Pop32(ptr) + 1;     // HACK: restore the length for backward compatibility
	uint32 space_needed	 = Movie.bytesPerFrame * input_frames;

	if (movie_id != Movie.header.uid)
		return MOVIE_NOT_FROM_THIS_MOVIE;

	if (space_needed > size - headerSize)
		return MOVIE_WRONG_FORMAT;

	if (Movie.readOnly)
	{
		// here, we are going to keep the input data from the movie file
		// and simply rewind to the currentFrame pointer
		// this will cause a desync if the savestate is not in sync // <-- NOT ANYMORE
		// with the on-disk recording data, but it's easily solved
		// by loading another savestate or playing the movie from the beginning

		// don't allow loading a state inconsistent with the current movie
		uint32 length_history = min(current_frame, Movie.header.length_frames);
		if (input_frames < length_history)
		{
			Movie.errorInfo = input_frames;
			return MOVIE_UNVERIFIABLE_POST_END;
		}

		for (uint32 i = 0; i < length_history; ++i)
		{
			if (memcmp(Movie.inputBuffer + i * Movie.bytesPerFrame, ptr + i * Movie.bytesPerFrame, Movie.bytesPerFrame))
			{
				Movie.errorInfo = i;
				return MOVIE_TIMELINE_INCONSISTENT_AT;
			}
		}

		Movie.currentFrame	 = current_frame;
		Movie.inputBufferPtr = Movie.inputBuffer + Movie.bytesPerFrame * min(current_frame, Movie.header.length_frames);

		change_movie_state(MOVIE_STATE_PLAY);   // check for movie end
	}
	else
	{
		// here, we are going to take the input data from the savestate
		// and make it the input data for the current movie, then continue
		// writing new input data at the currentFrame pointer
		Movie.currentFrame		   = current_frame;
		Movie.header.length_frames = input_frames;

		// do this before calling reserve_movie_buffer_space()
		Movie.inputBufferPtr = Movie.inputBuffer + Movie.bytesPerFrame * min(current_frame, Movie.header.length_frames);
		reserve_movie_buffer_space(space_needed);
		memcpy(Movie.inputBuffer, ptr, space_needed);

		// for consistency, no auto movie conversion here since we don't auto convert the corresponding savestate
		flush_movie_header();
		flush_movie_frames();

		change_movie_state(MOVIE_STATE_RECORD);
	}

	// necessary!
	resetSignaled	  = false;
	resetSignaledLast = false;

	// necessary to check if there's a reset signal at the previous frame
	if (current_frame > 0)
	{
		const u8 NEW_RESET = u8(BUTTON_MASK_NEW_RESET >> 8);
		for (int i = 0; i < MOVIE_NUM_OF_POSSIBLE_CONTROLLERS; ++i)
		{
			if ((Movie.header.controllerFlags & MOVIE_CONTROLLER(i)) && (*(Movie.inputBufferPtr + 1 - Movie.bytesPerFrame) & NEW_RESET))
			{
				resetSignaledLast = true;
				break;
			}
		}
	}

	preserve_movie_next_input();

	++Movie.inputGeneration;
	return MOVIE_SUCCESS;
}

bool VBAMovieSwitchToPlaying()
{
	if (!VBAMovieIsActive())
		return false;

	if (!Movie.readOnly)
	{
		VBAMovieToggleReadOnly();
	}

	change_movie_state(MOVIE_STATE_PLAY);

	return true;
}

bool VBAMovieSwitchToRecording()
{
	if (!VBAMovieIsActive())
		return false;

	if (Movie.readOnly)
	{
		VBAMovieToggleReadOnly();
	}

	change_movie_state(MOVIE_STATE_RECORD);

	return true;
}

uint32 VBAMovieGetState()
{
	// ?
	if (!VBAMovieIsActive())
		return MOVIE_STATE_NONE;

	return Movie.state;
}

void VBAMovieSignalReset()
{
	if (VBAMovieIsActive())
	{
		resetSignaled = !resetSignaled;
	}
}

bool VBAMovieIsResetSignaled()
{
	return VBAMovieIsActive() && resetSignaled;
}

void VBAMovieResetIfRequested()
{
	if (BUTTON_MASK_NEW_RESET & 
		(currentButtons[0] | currentButtons[1] | currentButtons[2] | currentButtons[3]))
	{
		// backward compatibility kludge
		if (resetSignaledLast)
		{
			currentButtons[0] |= BUTTON_MASK_OLD_RESET;
			currentButtons[1] |= BUTTON_MASK_OLD_RESET;
			currentButtons[2] |= BUTTON_MASK_OLD_RESET;
			currentButtons[3] |= BUTTON_MASK_OLD_RESET;
		}

		theEmulator.emuReset();
		resetSignaledLast = true;
		resetSignaled = false;
	}
	else
	{
		resetSignaledLast = false;
	}
}

void VBAMovieSetMetadata(const char *info)
{
	if (!memcmp(Movie.authorInfo, info, MOVIE_METADATA_SIZE))
		return;

	memcpy(Movie.authorInfo, info, MOVIE_METADATA_SIZE); // strncpy would omit post-0 bytes
	Movie.authorInfo[MOVIE_METADATA_SIZE - 1] = '\0';

	if (Movie.file)
	{
		// (over-)write the header
		fseek(Movie.file, 0, SEEK_SET);
		write_movie_header(Movie.file, Movie);

		// write the metadata / author info to file
		fwrite(Movie.authorInfo, 1, sizeof(char) * MOVIE_METADATA_SIZE, Movie.file);

		fflush(Movie.file);
	}
}

void VBAMovieRestart()
{
	if (VBAMovieIsActive())
	{
		systemSoundClearBuffer();

		// can't just pass in Movie.filename, since VBAMovieStop clears out Movie's variables
		char movieName [_MAX_PATH];
		strncpy(movieName, Movie.filename, _MAX_PATH);
		movieName[_MAX_PATH - 1] = '\0';

		uint8 modified = Movie.RecordedThisSession;
		uint8 readOnly = Movie.readOnly;

		VBAMovieStop(true);

		Movie.RecordedThisSession = modified;

		VBAMovieOpen(movieName, readOnly);

		systemScreenMessage("Movie replay (restart)");
	}
}

int VBAMovieGetPauseAt()
{
	return Movie.pauseFrame;
}

void VBAMovieSetPauseAt(int at)
{
	Movie.pauseFrame = at;
}

///////////////////////
// movie tools

// FIXME: is it safe to convert/flush a movie while recording it (considering fseek() problem)?
int VBAMovieConvertCurrent(bool force)
{
	if (!VBAMovieIsActive())
	{
		return MOVIE_NOTHING;
	}

	if (Movie.header.minorVersion > VBM_REVISION)
	{
		return MOVIE_WRONG_VERSION;
	}

	if (!force && Movie.header.minorVersion == VBM_REVISION)
	{
		return MOVIE_SAME_VERSION;
	}

	Movie.header.minorVersion = VBM_REVISION;
	++Movie.inputGeneration;

	if (Movie.header.length_frames == 0) // this could happen
	{
		truncate_movie(0);
		return MOVIE_SUCCESS;
	}

	// fix movies recorded from snapshots
	if (Movie.header.startFlags & MOVIE_START_FROM_SNAPSHOT)
	{
		uint8 *firstFramePtr = Movie.inputBuffer;
		for (int i = 0; i < MOVIE_NUM_OF_POSSIBLE_CONTROLLERS; ++i)
		{
			if (Movie.header.controllerFlags & MOVIE_CONTROLLER(i))
			{
				Push16(initialInputs[i], firstFramePtr);
				// note: this is correct since Push16 advances the dest pointer by sizeof u16
			}
		}
	}

	// convert old resets to new ones
	const u8 OLD_RESET = u8(BUTTON_MASK_OLD_RESET >> 8);
	const u8 NEW_RESET = u8(BUTTON_MASK_NEW_RESET >> 8);
	for (int i = 0; i < MOVIE_NUM_OF_POSSIBLE_CONTROLLERS; ++i)
	{
		if (Movie.header.controllerFlags & MOVIE_CONTROLLER(i))
		{
			uint8 *startPtr = Movie.inputBuffer + CONTROLLER_DATA_SIZE * i + 1;
			uint8 *endPtr	= Movie.inputBuffer + Movie.bytesPerFrame * (Movie.header.length_frames - 1);
			for (; startPtr < endPtr; startPtr += Movie.bytesPerFrame)
			{
				if (startPtr[Movie.bytesPerFrame] & OLD_RESET)
				{
					startPtr[0] |= NEW_RESET;
				}
#if 0
				if (startPtr[0] & NEW_RESET)
				{
					startPtr[Movie.bytesPerFrame] |= OLD_RESET;
				}
#endif
			}
		}
	}

	flush_movie_header();
	flush_movie_frames();
	return MOVIE_SUCCESS;
}

int VBAMovieInsertFrames(uint32 num)
{
	if (!VBAMovieIsActive() || VBAMovieHasEnded())
		return MOVIE_NOTHING;

	if (Movie.header.minorVersion > VBM_REVISION)
		return MOVIE_WRONG_VERSION;

	// conversion for safty
	VBAMovieConvertCurrent(false);

	uint32 newLength = (uint32)(Movie.header.length_frames + num);
	reserve_movie_buffer_space(newLength * Movie.bytesPerFrame);

	uint32 numRemaining = Movie.header.length_frames - Movie.currentFrame;
	Movie.header.length_frames = newLength;

	fix_movie_old_reset(false);
	memmove(Movie.inputBufferPtr + num * Movie.bytesPerFrame, Movie.inputBufferPtr, numRemaining * Movie.bytesPerFrame);
	memset(Movie.inputBufferPtr, 0, num * Movie.bytesPerFrame);
	fix_movie_old_reset(resetSignaledLast);

	flush_movie_header();
	flush_movie_frames();

	preserve_movie_next_input();

	++Movie.inputGeneration;
	return MOVIE_SUCCESS;
}

int VBAMovieDeleteFrames(uint32 num)
{
	if (!VBAMovieIsActive() || VBAMovieHasEnded())
		return MOVIE_NOTHING;

	if (Movie.header.minorVersion > VBM_REVISION)
		return MOVIE_WRONG_VERSION;

	// conversion for safty
	VBAMovieConvertCurrent(false);

	uint32 numRemaining = Movie.header.length_frames - Movie.currentFrame;
	if (num > numRemaining)
	{
		num = numRemaining;
	}
	Movie.header.length_frames -= num;
	memmove(Movie.inputBufferPtr, Movie.inputBufferPtr + num * Movie.bytesPerFrame, (numRemaining - num) * Movie.bytesPerFrame);
	fix_movie_old_reset(resetSignaledLast);

	flush_movie_header();
	flush_movie_frames();

	preserve_movie_next_input();

	++Movie.inputGeneration;
	return MOVIE_SUCCESS;
}

bool VBAMovieTuncateAtCurrentFrame()
{
	if (!VBAMovieIsActive())
		return false;

	truncate_movie(Movie.currentFrame);
	change_movie_state(MOVIE_STATE_END);
	++Movie.inputGeneration;
	systemScreenMessage("Movie truncated");

	return true;
}

bool VBAMovieFixHeader()
{
	if (!VBAMovieIsActive())
		return false;

	flush_movie_header();
	systemScreenMessage("Movie header fixed");
	return true;
}

// ---------------------------------------------------------------------------
// Frame timeline editing (VBM Editor).
//
// The functions above always act at the playhead; these take an absolute row.
// They obey the rule that the editor may rewrite the recorded key sequence and
// nothing else, because "what the user pressed on frame N" is the only thing a
// TAS editor is allowed to own:
//
//   * only the controller bytes of Movie.inputBuffer change, and only the
//     BUTTON_REGULAR_MASK bits of them - the reset and motion bits are not
//     keys and are preserved byte for byte;
//   * Movie.header.length_frames moves by exactly one on insert/delete;
//   * Movie.currentFrame is never touched.  It is a position on the timeline,
//     not a pointer to a particular frame's data: the savestate checks, the
//     frame counters and truncate_movie() all depend on it meaning "how many
//     frames have run";
//   * state, editMode, readOnly, pauseFrame and rerecord_count are untouched,
//     as is the emulator itself.
//
// The one consequence the callers must warn about: inserting or deleting at or
// above the playhead shifts the bytes under it, so the next frame the emulator
// reads is a different one.  Editing a cell only changes a frame that either
// already ran (desync, no immediate effect) or has not run yet (no desync).
// ---------------------------------------------------------------------------

static uint8 *movie_input_ptr(uint32 row, int controller)
{
	return Movie.inputBuffer + (size_t)Movie.bytesPerFrame * row + CONTROLLER_DATA_SIZE * controller;
}

static bool movie_timeline_editable()
{
	return VBAMovieIsActive() && Movie.file != NULL && !Movie.readOnly;
}

uint32 VBAMovieGetInputGeneration()
{
	return Movie.inputGeneration;
}

int VBAMovieGetReadOnlyMode()
{
	return VBAMovieIsActive() ? Movie.readOnly : 0;
}

uint16 VBAMovieGetInputAt(uint32 row, int controller)
{
	if (!VBAMovieIsActive() || !Movie.inputBuffer)
		return 0;
	if (controller < 0 || controller >= MOVIE_NUM_OF_POSSIBLE_CONTROLLERS)
		return 0;
	if (!(Movie.header.controllerFlags & MOVIE_CONTROLLER(controller)))
		return 0;
	if (row >= Movie.header.length_frames)
		return 0;

	return Read16(movie_input_ptr(row, controller));
}

uint16 VBAMovieGetUsedInputMask(int controller)
{
	if (!VBAMovieIsActive() || !Movie.inputBuffer || !Movie.bytesPerFrame)
		return 0;
	if (controller < 0 || controller >= MOVIE_NUM_OF_POSSIBLE_CONTROLLERS)
		return 0;
	if (!(Movie.header.controllerFlags & MOVIE_CONTROLLER(controller)))
		return 0;

	// One linear pass.  The editor calls this once per edit, and an edit can
	// only happen while the emulator is paused, so even a multi-hour movie
	// costs less than one frame's worth of time.
	uint16		 mask = 0;
	const uint8 *ptr  = Movie.inputBuffer + CONTROLLER_DATA_SIZE * controller;
	for (uint32 i = 0; i < Movie.header.length_frames; ++i, ptr += Movie.bytesPerFrame)
		mask |= Read16(ptr);

	return mask;
}

int VBAMovieSetInputAt(uint32 row, int controller, uint16 value)
{
	if (!movie_timeline_editable())
		return MOVIE_NOTHING;
	if (controller < 0 || controller >= MOVIE_NUM_OF_POSSIBLE_CONTROLLERS
	    || !(Movie.header.controllerFlags & MOVIE_CONTROLLER(controller)))
		return MOVIE_WRONG_FORMAT;
	if (row >= Movie.header.length_frames)
		return MOVIE_WRONG_FORMAT;

	uint8 *ptr = movie_input_ptr(row, controller);
	uint16 old = Read16(ptr);
	uint16 next = uint16((old & ~BUTTON_REGULAR_MASK) | (value & BUTTON_REGULAR_MASK));
	if (next == old)
		return MOVIE_SUCCESS;

	Write16(next, ptr);

	// Rewrite just these two bytes.  The rest of the file already matches the
	// buffer, and a whole-block flush per click would rewrite megabytes for no
	// reason.  The stream position is restored because recording appends at it.
	long originalPos = ftell(Movie.file);
	fseek(Movie.file,
	      Movie.header.offset_to_controller_data
	          + long((size_t)Movie.bytesPerFrame * row + CONTROLLER_DATA_SIZE * controller),
	      SEEK_SET);
	fwrite(ptr, 1, CONTROLLER_DATA_SIZE, Movie.file);
	fflush(Movie.file);
	fseek(Movie.file, originalPos, SEEK_SET);

	// keep the OSD's "next input" line in step when the playhead frame moved
	preserve_movie_next_input();

	++Movie.inputGeneration;
	return MOVIE_SUCCESS;
}

int VBAMovieInsertFrameAt(uint32 row)
{
	if (!movie_timeline_editable())
		return MOVIE_NOTHING;
	// A blank frame recorded during RECORD is undone by the next
	// VBAMovieAfterEmulation() (length_frames = currentFrame), so refuse it.
	// END refuses like VBAMovieInsertFrames() does.
	if (Movie.state == MOVIE_STATE_RECORD || Movie.state == MOVIE_STATE_END)
		return MOVIE_NOTHING;
	if (Movie.header.minorVersion > VBM_REVISION)
		return MOVIE_WRONG_VERSION;
	if (row > Movie.header.length_frames)
		return MOVIE_WRONG_FORMAT;
	if (Movie.header.length_frames >= MOVIE_MAX_FRAMES)
		return MOVIE_FATAL_ERROR;

	// same normalisation VBAMovieInsertFrames() performs, so a 1.0 movie is
	// converted to 1.1 before we write to it
	VBAMovieConvertCurrent(false);

	uint32 newLength = Movie.header.length_frames + 1;
	reserve_movie_buffer_space(newLength * Movie.bytesPerFrame);
	Movie.header.length_frames = newLength;

	uint8 *at = Movie.inputBuffer + (size_t)Movie.bytesPerFrame * row;
	memmove(at + Movie.bytesPerFrame, at, (size_t)Movie.bytesPerFrame * (newLength - 1 - row));
	memset(at, 0, Movie.bytesPerFrame);

	// The buffer may have moved, and only then does the pointer need fixing:
	// currentFrame itself stays put on purpose.
	Movie.inputBufferPtr = Movie.inputBuffer + (size_t)Movie.bytesPerFrame * min(Movie.currentFrame, Movie.header.length_frames);

	flush_movie_header();
	flush_movie_frames();

	preserve_movie_next_input();

	++Movie.inputGeneration;
	return MOVIE_SUCCESS;
}

int VBAMovieDeleteFrameAt(uint32 row)
{
	if (!movie_timeline_editable())
		return MOVIE_NOTHING;
	if (Movie.state == MOVIE_STATE_RECORD || Movie.state == MOVIE_STATE_END)
		return MOVIE_NOTHING;
	if (Movie.header.minorVersion > VBM_REVISION)
		return MOVIE_WRONG_VERSION;
	if (row >= Movie.header.length_frames)
		return MOVIE_WRONG_FORMAT;

	VBAMovieConvertCurrent(false);

	uint32 newLength = Movie.header.length_frames - 1;
	uint8 *at		 = Movie.inputBuffer + (size_t)Movie.bytesPerFrame * row;
	memmove(at, at + Movie.bytesPerFrame, (size_t)Movie.bytesPerFrame * (newLength - row));
	memset(Movie.inputBuffer + (size_t)Movie.bytesPerFrame * newLength, 0, Movie.bytesPerFrame);
	Movie.header.length_frames = newLength;

	Movie.inputBufferPtr = Movie.inputBuffer + (size_t)Movie.bytesPerFrame * min(Movie.currentFrame, Movie.header.length_frames);

	// flush the shifted data, then cut the file back to the new end - without
	// the truncate, VBAMovieOpen() would recover the deleted frame from the
	// file size
	flush_movie_frames();
	truncate_movie(newLength);

	preserve_movie_next_input();

	++Movie.inputGeneration;
	return MOVIE_SUCCESS;
}

