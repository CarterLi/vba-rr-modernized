#ifndef CHEATDB_HEADER
#define CHEATDB_HEADER

// The shape every cheat source is read into, so the import dialog does not have
// to know where the cheats came from.
//
// Two readers produce it:
//   ezflashdb.{h,cpp}   the EZ-Flash Omega package ("EZ-FLASH OMEGA.zip")
//   libretrocht.{h,cpp} a libretro-database .cht file
//
// They differ in one important way, which is why an entry can carry either
// `writes` or `code`:
//
//   the package stores raw addresses, so the reader translates them itself and
//   hands over ready to use 8/16/32 bit writes;
//
//   a libretro file stores CodeBreaker / GameShark codes, whose meaning (write
//   width, condition, key press, master code) lives in the code's own type
//   nibble.  Only the emulator's cheat engine understands that, so the reader
//   keeps the text and the dialog hands it to cheatsAddGSACode/cheatsAddCBACode.
//
// Neither MFC nor emulator state is touched here.

#include <cstddef>
#include <string>
#include <vector>

// Which of the two text formats a .cht file holds.
//
// Both are plain .cht and the name says nothing: the EZ-Flash package ships its
// 928 files as .cht, and so does the libretro database.  The contents do say,
// and on the real corpus the marks never overlap (928 package files against 94
// libretro files):
//
//   mark                            package   libretro
//   [GameInfo]  fixed last section    928 / 928      0 / 94
//   [Some_Name] one per cheat         928 / 928      0 / 94
//   Switch=     the data lines        875 / 928      0 / 94
//   cheatN_desc / cheatN_code           0 / 928     94 / 94
//   cheats = N                          0 / 928     94 / 94
//
// So the reader is chosen by what is inside the bytes, never by the extension.
enum CheatDbFormat
{
	CHEAT_DB_FORMAT_UNKNOWN = 0,   // empty, junk, or a hand-made mix of both
	CHEAT_DB_FORMAT_EZFLASH,       // ezflashdb.h
	CHEAT_DB_FORMAT_LIBRETRO       // libretrocht.h
};

// One contiguous memory write, already translated to a GBA address and
// coalesced from the byte-per-comma layout of the package format.
struct CheatDbWrite
{
	unsigned int address;   // absolute GBA address
	unsigned int value;     // little endian; only the low `size` bytes matter
	int          size;      // 1, 2 or 4
};

// Why a line or an entry was dropped.  The UI turns this into localised text, so
// the readers never embed prose of their own.
enum CheatDbProblemKind
{
	CHEAT_DB_PROBLEM_SECTION = 0,   // "[" without a closing "]"
	CHEAT_DB_PROBLEM_LINE,          // line that is neither a section, an entry nor a comment
	CHEAT_DB_PROBLEM_ADDRESS,       // address is not hex, or outside what the format can express
	CHEAT_DB_PROBLEM_VALUE,         // data is not a byte sequence
	CHEAT_DB_PROBLEM_EMPTY,         // entry with no usable data
	CHEAT_DB_PROBLEM_LIMIT,         // entry or file exceeds the accepted size
	CHEAT_DB_PROBLEM_CODE           // libretro: code text the emulator cannot use
};

struct CheatDbProblem
{
	int                line;   // line in the file, or 1-based entry number for libretro
	CheatDbProblemKind kind;
	std::string        text;   // offending token or line, for the details list
};

// One selectable cheat.
struct CheatDbEntry
{
	std::string               section;     // cheat name
	std::string               key;         // the key that carried the codes
	bool                      useKeyName;  // show "section: key" (a section that offers a choice)
	std::vector<CheatDbWrite> writes;      // package: translated writes, ready to add
	std::string               code;        // libretro: raw "ADDR+VALUE+..." text
};

struct CheatDbFile
{
	std::string                title;      // first content line of a package, file name for libretro
	std::string                gameName;   // name the source records for the game
	std::string                system;     // "GBA"
	std::vector<CheatDbEntry>  entries;
	std::vector<CheatDbProblem> problems;  // tolerated defects, in file order

	// Number of address tokens that only carried the low 16 bits of the address
	// they mean, so their bank had to be assumed.  The UI mentions this and
	// offers a different base.  Always 0 for libretro files, whose addresses are
	// absolute.
	int                        truncatedAddresses;
};

// Why a source could not be used at all.  Kept apart from the tolerated per-line
// problems so the UI can say "format error" with a localised sentence.
enum CheatDbError
{
	CHEAT_DB_ERR_NONE = 0,
	CHEAT_DB_ERR_FILE,        // the path cannot be opened
	CHEAT_DB_ERR_ARCHIVE,     // not a readable zip, or a directory without a package
	CHEAT_DB_ERR_INDEX,       // no GameID2cht.bin inside
	CHEAT_DB_ERR_INDEX_DATA,  // the index is empty or not the expected fixed record size
	CHEAT_DB_ERR_ENTRY,       // a package entry cannot be extracted (encrypted, corrupt)
	CHEAT_DB_ERR_GAME_FILE    // the cheat file was found but cannot be extracted
};

enum CheatDbResult
{
	CHEAT_DB_OK = 0,
	CHEAT_DB_NO_CHEATS,     // the source is fine but has no file for this game
	CHEAT_DB_EMPTY,         // the file exists but nothing in it could be used
	CHEAT_DB_ERROR          // the source cannot be used at all
};

// Decides which reader a .cht file belongs to, from its bytes alone.  Returns
// UNKNOWN for an empty or unrecognisable file, and for one that carries the
// decisive marks of both formats (a hand-edited mix) - the caller then simply
// tries both readers, which is more reliable than voting.
CheatDbFormat CheatDbSniffFormat(const char* text, size_t length);

// Reads a whole small text file.  Shared by both readers so they fail the same
// way.  `detail` receives the offending path, never a sentence.
bool CheatDbReadTextFile(const char* path, std::string& text, CheatDbError& error,
                         std::string& detail);

#endif // CHEATDB_HEADER
