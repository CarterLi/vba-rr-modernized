#ifndef EZFLASHDB_HEADER
#define EZFLASHDB_HEADER

// Reader for the EZ-Flash Omega cheat package ("EZ-FLASH OMEGA.zip").
//
// The package is a plain ZIP holding
//   GameID2cht.bin          fixed-size index, 8 characters per game:
//                           4-character GBA game code + 4-character id
//   Eng/<bucket>/<id>.cht   the cheat file itself, one per game
//
// The cheat files are hand-written text, so everything here is defensive: a bad
// line is dropped and recorded as a problem instead of aborting the import, and
// only a file that yields nothing usable counts as an error.
//
// The result is the shared CheatDbFile from cheatdb.h; neither MFC nor emulator
// state is touched, so this compiles and runs on its own (see
// documentation/cheat-database.md for the offline harness).
//
// Address translation follows the Omega kernel (source/GBApatch.c, the loop that
// fills the cheat table):
//
//   encoded >= 0x40000  ->  0x03000000 | (encoded & 0x7FFF)   IWRAM
//   encoded <  0x40000  ->  0x02000000 | (encoded & 0x3FFFF)  EWRAM
//
// Only the second form is unambiguous.  Nearly every line in the package uses
// the first one, and that form carried just the low 16 bits of the address its
// author meant: whatever converted the original action replay codes dropped the
// bank.  The kernel therefore reads all of them as IWRAM, which is wrong for
// most of them - measured against the Code Breaker database this package was
// built from, 74% of the addresses belong to EWRAM, spread over all four 64 KB
// pages, and the pages differ from one game to the next.
//
// Nothing inside the file says which bank was meant, so this reader can be told:
// `base` replaces the bank of those addresses, and 0 keeps the kernel's reading.
// See documentation/cheat-database.md.
//
// A few files instead spell the address out with its 0x02 prefix; those are
// taken as absolute and ignore `base`.  Addresses the format cannot express
// (ROM, I/O) are dropped rather than guessed at.

#include "cheatdb.h"

#include <string>

class EzCheatDb
{
public:
	EzCheatDb();
	~EzCheatDb();

	// `path` is either the .zip itself or a directory holding an extracted
	// package.  `detail` receives the offending file name, never a sentence.
	bool Open(const char* path, CheatDbError& error, std::string& detail);
	void Close();
	bool IsOpen() const { return m_open; }

	int GameCodeCount() const { return (int)m_records.size(); }

	// Index records whose 8 characters did not look like a game code and an id.
	// Non-zero means the package has been tampered with, but it still works.
	int IndexSkippedRecords() const { return m_indexSkipped; }

	// Looks the game up by its 4 character ROM code (rom[0xac]) and parses the
	// matching cheat file.  `file.title` is filled whenever a file was found,
	// even if every entry in it had to be dropped.
	CheatDbResult LoadForGameCode(const char gameCode[4], CheatDbFile& file,
	                              CheatDbError& error, std::string& detail);

	// Reads the file the last successful call used again, this time with `base`
	// replacing the bank of the truncated addresses (0 = as the file says).
	// The UI turns the memory-area choice into this; nothing else needs it.
	CheatDbResult ReloadWithBase(unsigned int base, CheatDbFile& file);

	// Offline harness support: walk every record in the index.
	int         RecordCount() const { return (int)m_records.size(); }
	const char* RecordCode(int index) const;
	CheatDbResult LoadByRecord(int index, CheatDbFile& file, CheatDbError& error,
	                           std::string& detail);

private:
	struct Record
	{
		std::string code;
		std::string id;
	};

	bool ReadEntry(const std::string& name, std::string& out, CheatDbError& error,
	               std::string& detail);
	int  FindEntry(const std::string& name) const;
	void BuildChtPath(const std::string& id, size_t language, std::string& path) const;
	bool LoadIndex(CheatDbError& error, std::string& detail);

	bool                     m_open;
	bool                     m_isDir;
	std::string              m_root;      // prefix inside the zip, "" when at the top
	std::string              m_dirPath;   // directory mode only
	void*                    m_zip;       // ZipArchive*, kept opaque here
	std::vector<std::string> m_names;     // lowercase entry names
	std::vector<std::string> m_languages; // "Eng", "Chs", ... with Eng first
	std::vector<Record>      m_records;
	std::string              m_lastText;  // text the last successful load parsed
	int                      m_indexSkipped;
};

// Parses cheat file text.  Exposed so the tolerance rules can be tested without
// a package on disk.  Returns false only when nothing usable was found.
//
// `base` is 0 for "as the file says", otherwise one of the EWRAM page bases
// (0x02000000, 0x02010000, 0x02020000, 0x02030000) or 0x03000000 for IWRAM.  It
// only affects the ambiguous address form described at the top of this file.
bool EzParseChtText(const char* text, size_t length, CheatDbFile& file,
                    unsigned int base = 0);

// A zip holding cheat files but no GameID2cht.bin is not an indexed package and
// cannot be searched by game code.  When it holds exactly one .cht there is
// nothing to choose, so it can be used as a lone file; several would be a guess,
// which the caller reports instead.  The format of that file is not examined
// here - the caller sniffs it like any other single file.
enum EzLoneChtResult
{
	EZ_LONE_READ = 0,   // `text` holds the only file, `count` is 1
	EZ_LONE_NONE,       // the zip holds no .cht at all
	EZ_LONE_SEVERAL,    // `count` says how many there are
	EZ_LONE_FAILED      // the zip, or its entry, cannot be read
};

EzLoneChtResult EzReadLoneCht(const char* path, std::string& text, int& count,
                              CheatDbError& error, std::string& detail);

#endif // EZFLASHDB_HEADER
