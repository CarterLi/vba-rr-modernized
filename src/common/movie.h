#ifndef VBA_MOVIE_H
#define VBA_MOVIE_H

#pragma once

#include <ctime>
#include <string>

#include "../Port.h"

#define ZLIB
///#ifdef ZLIB
#ifndef WIN32
#include "zlib.h"
#endif

#ifndef MOVIE_SUCCESS
#  define MOVIE_SUCCESS 1
#  define MOVIE_NOTHING 0
#  define MOVIE_WRONG_FORMAT (-1)
#  define MOVIE_WRONG_VERSION (-2)
#  define MOVIE_FILE_NOT_FOUND (-3)
#  define MOVIE_NOT_FROM_THIS_MOVIE (-4)
#  define MOVIE_NOT_FROM_A_MOVIE (-5)
#  define MOVIE_UNVERIFIABLE_POST_END (-6)
#  define MOVIE_UNRECORDED_INPUT (-7)
#  define MOVIE_SAME_VERSION (-8)
#  define MOVIE_TIMELINE_INCONSISTENT_AT (-9)
#  define MOVIE_FATAL_ERROR (-32768)
#  define MOVIE_UNKNOWN_ERROR (-2147483647 - 1)
#endif

// Core versions detectable from the existing VBM header.  The movie format
// itself is unchanged; these values are derived from fields that V7/V8
// already write (see VBAMovieGetCoreVersion).
#define MOVIE_CORE_VERSION_UNKNOWN 0
#define MOVIE_CORE_VERSION_V7      7
#define MOVIE_CORE_VERSION_V8      8

#define VBM_MAGIC (0x1a4D4256) // VBM0x1a
#define VBM_VERSION (1)
#define VBM_HEADER_SIZE (64)
#define CONTROLLER_DATA_SIZE (2)
#define BUFFER_GROWTH_SIZE (4096)
#define MOVIE_METADATA_SIZE (192)
#define MOVIE_METADATA_AUTHOR_SIZE (64)

// revision 1 uses (?) insted of (!) as reset
#define VBM_REVISION   (1)

#define MOVIE_START_FROM_SNAPSHOT   (1<<0)
#define MOVIE_START_FROM_SRAM       (1<<1)

#define MOVIE_CONTROLLER(i)         (1<<(i))
#define MOVIE_CONTROLLERS_ANY_MASK  (MOVIE_CONTROLLER(0)|MOVIE_CONTROLLER(1)|MOVIE_CONTROLLER(2)|MOVIE_CONTROLLER(3))
#define MOVIE_NUM_OF_POSSIBLE_CONTROLLERS   (4)

#define MOVIE_TYPE_GBA              (1<<0)
#define MOVIE_TYPE_GBC              (1<<1)
#define MOVIE_TYPE_SGB              (1<<2)

// using BIOS/RTC should have been made movie start flags
#define MOVIE_SETTING_USEBIOSFILE   (1<<0)
#define MOVIE_SETTING_SKIPBIOSINTRO (1<<1)
#define MOVIE_SETTING_RTCENABLE     (1<<2)
#define MOVIE_SETTING_GBINPUTHACK   (1<<3)
#define MOVIE_SETTING_LAGHACK       (1<<4)
#define MOVIE_SETTING_GBCFF55FIX    (1<<5)
#define MOVIE_SETTING_GBECHORAMFIX  (1<<6)
#define MOVIE_SETTING_SRAMINITFIX   (1<<7)

#define STREAM gzFile
/*#define READ_STREAM(p,l,s) gzread (s,p,l)
 #define WRITE_STREAM(p,l,s) gzwrite (s,p,l)
 #define OPEN_STREAM(f,m) gzopen (f,m)
 #define REOPEN_STREAM(f,m) gzdopen (f,m)
 #define FIND_STREAM(f)	gztell(f)
 #define REVERT_STREAM(f,o,s)  gzseek(f,o,s)
 #define CLOSE_STREAM(s) gzclose (s)
 #else
 #define STREAM FILE *
 #define READ_STREAM(p,l,s) fread (p,1,l,s)
 #define WRITE_STREAM(p,l,s) fwrite (p,1,l,s)
 #define OPEN_STREAM(f,m) fopen (f,m)
 #define REOPEN_STREAM(f,m) fdopen (f,m)
 #define FIND_STREAM(f)	ftell(f)
 #define REVERT_STREAM(f,o,s)	 fseek(f,o,s)
 #define CLOSE_STREAM(s) fclose (s)
 #endif*/

enum MovieState
{
	MOVIE_STATE_NONE = 0,
	MOVIE_STATE_PLAY,
	MOVIE_STATE_RECORD,
	MOVIE_STATE_END
};

enum MovieEditMode
{
	MOVIE_EDIT_MODE_DISCARD = 0,
	MOVIE_EDIT_MODE_OVERWRITE,
	MOVIE_EDIT_MODE_INSERT,
	MOVIE_EDIT_MODE_XOR,

	MOVIE_EDIT_MODE_COUNT
};

struct SMovieFileHeader
{
	uint32 magic;       // VBM0x1a
	uint32 version;     // 1
	int32  uid;         // used to match savestates to a particular movie
	uint32 length_frames;
	uint32 rerecord_count;
	uint8  startFlags;
	uint8  controllerFlags;
	uint8  typeFlags;
	uint8  optionFlags;
	uint32 saveType;        // emulator setting value
	uint32 flashSize;       // emulator setting value
	uint32 gbEmulatorType;  // emulator setting value
	char   romTitle[12];
	uint8  minorVersion;	// minor version/revision of the current movie version
	uint8  romCRC;						// the CRC of the ROM used while recording
	uint16 romOrBiosChecksum;			// the Checksum of the ROM used while recording, or a CRC of the BIOS if GBA
	uint32 romGameCode;					// the Game Code of the ROM used while recording, or "\0\0\0\0" if not GBA
	uint32 offset_to_savestate;         // offset to the savestate or SRAM inside file, set to 0 if unused
	uint32 offset_to_controller_data;   // offset to the controller data inside file
};

struct SMovie
{
	enum   {MAX_FILENAME_LENGTH = 260};
	enum   MovieState state;
	enum   MovieEditMode editMode;
	char   filename[MAX_FILENAME_LENGTH]; // FIXME: should use a string instead
	FILE*  file;

	SMovieFileHeader header;
	char  authorInfo[MOVIE_METADATA_SIZE];

	int32  pauseFrame;	// FIXME: byte size
	uint32 currentFrame;    // should == length_frame when recording, and be < length_frames when playing
	uint32 bytesPerFrame;
	uint32 inputBufferSize;
	uint8* inputBuffer;
	uint8* inputBufferPtr;

	// bool8 doesn't make much sense if it is meant to solve any portability problem,
	//   because there's no guarantee that true == 1 and false == 0 (or TRUE == 1 and FALSE == 0) on all platforms.
	//   while using user-defined boolean types might impact on performance.
	//   the more reliable (and faster!) way to maintain cross-platform I/O compatibility is
	//   to manually map from/to built-in boolean types to/from fixed-sized types value by value ONLY when doing I/O
	//   e.g. bool(true) <-> u8(1) and <-> bool(false) <-> u8(0), BOOL(TRUE) <-> s32(-1) and BOOL(FALSE) <-> s32(0) etc.
	uint8 readOnly;
	uint8 unused;
	uint8 RecordedNewRerecord;
	uint8 RecordedThisSession;

	uint32 errorInfo;

	// Bumped by anything that rewrites the timeline (open, savestate load,
	// conversion, insert/delete, cell edit).  Front ends that mirror the
	// timeline poll this instead of comparing buffers.  Not part of the file
	// format and never written anywhere.
	uint32 inputGeneration;
};

// methods used by the user-interface code
int VBAGetCurrentCoreVersion();
int VBAMovieGetCoreVersion(const SMovie &movie);
int VBAMovieOpen(const char *filename, bool8 read_only);
int VBAMovieCreate(const char *filename, const char *authorInfo, uint8 startFlags, uint8 controllerFlags, uint8 typeFlags);
int VBAMovieGetInfo(const char *filename, SMovie*info);
double VBAMovieGetFrameRate();
void VBAMovieGetRomInfo(const SMovie &movieInfo, char romTitle[12], uint32 &romGameCode, uint16 &checksum, uint8 &crc);
void VBAMovieStop(bool8 suppress_message);

// methods used by the emulation
void VBAMovieInit();
void VBAMovieAfterEmulation();
void VBAMovieUpdateState();
void VBAMovieRead(int controllerNum = 0, bool sensor = false);
void VBAMovieWrite(int controllerNum = 0, bool sensor = false);
void VBAUpdateButtonPressDisplay();
void VBAUpdateFrameCountDisplay();
//bool8 VBAMovieRewind (uint32 at_frame);
int VBAMovieFreeze(uint8 **buf, uint32 *size);
int VBAMovieUnfreeze(const uint8 *buf, uint32 size);
void VBAMovieRestart();

// accessor functions
bool VBAMovieIsActive();
bool VBAMovieIsLoading();
bool VBAMovieIsPlaying();
bool VBAMovieIsRecording();
// the following accessors return 0/false if !VBAMovieIsActive()
bool VBAMovieIsReadOnly();
bool VBAMovieIsXorInput();
bool VBAMovieHasEnded();
bool VBAMovieAllowsRerecording();
uint32 VBAMovieGetVersion();
uint32 VBAMovieGetMinorVersion();
uint32 VBAMovieGetId();
uint32 VBAMovieGetLength();
uint32 VBAMovieGetFrameCounter();
uint32 VBAMovieGetState();
uint32 VBAMovieGetRerecordCount ();
uint32 VBAMovieSetRerecordCount (uint32 newRerecordCount);
std::string VBAMovieGetAuthorInfo();
std::string VBAMovieGetFilename();
uint32 VBAMovieGetLastErrorInfo();

uint16 VBAMovieGetCurrentInputOf(int which, bool normalOnly = false);
uint16 VBAMovieGetNextInputOf(int which, bool normalOnly = false);

// --- frame timeline editing (VBM Editor) -----------------------------------
//
// Absolute-row counterparts of VBAMovieInsertFrames()/VBAMovieDeleteFrames(),
// which can only act at the playhead, plus the cell edit the editor needs.
//
// They only ever change the movie: the controller bytes of the timeline, the
// movie length, and the matching bytes in the file.  They never touch
// currentFrame, the movie state, the edit mode, the read-only flag or the
// emulator, and they never write the reset or motion bits.
//
// Inserting or deleting at or above the playhead does shift the data the
// emulator is about to read - the callers are expected to warn about that.
#define MOVIE_MAX_FRAMES (0x0FFFFFFFu)

uint32 VBAMovieGetInputGeneration();
uint16 VBAMovieGetInputAt(uint32 row, int controller = 0);
uint16 VBAMovieGetUsedInputMask(int controller = 0);
int	   VBAMovieSetInputAt(uint32 row, int controller, uint16 value);
int	   VBAMovieInsertFrameAt(uint32 row);
int	   VBAMovieDeleteFrameAt(uint32 row);

// Batch variants of the two calls above.  rows must be ascending, unique and
// must leave at least one frame in the movie (a zero-frame movie opens as
// ENDED and can never be edited again).  Insert values are masked with
// BUTTON_REGULAR_MASK before they are written; values must not point into
// Movie.inputBuffer.  count == 0 is a successful no-op, so callers do not need
// a special case for an empty selection.
int	   VBAMovieInsertFramesAt(uint32 row, const uint16 *values, uint32 count, int controller = 0);
int	   VBAMovieDeleteFramesAt(const uint32 *rows, uint32 count);

// 0: editable, 1: read-only mode, 2: the file itself cannot be written to
// (opened "rb" because "rb+" failed), which read-only mode cannot undo.
int VBAMovieGetReadOnlyMode();

void VBAMovieSignalReset();
bool VBAMovieIsResetSignaled();
void VBAMovieResetIfRequested();
void VBAMovieSetMetadata(const char *info);
void VBAMovieToggleReadOnly();
void VBAMoviePrevEditMode();
void VBAMovieNextEditMode();
void VBAMovieSetEditMode(MovieEditMode mode);
MovieEditMode VBAMovieGetEditMode();
bool VBAMovieSwitchToPlaying();
bool VBAMovieSwitchToRecording();
int  VBAMovieGetPauseAt();
void VBAMovieSetPauseAt(int at);
int  VBAMovieConvertCurrent(bool force = false);
int VBAMovieInsertFrames(uint32 num);
int VBAMovieDeleteFrames(uint32 num);
bool VBAMovieTuncateAtCurrentFrame();
bool VBAMovieFixHeader();

#endif // VBA_MOVIE_H
