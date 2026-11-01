#ifndef LIBRETROCHT_HEADER
#define LIBRETROCHT_HEADER

// Reader for the cheat files of the libretro database
// (libretro-database/cht/Nintendo - Game Boy Advance/*.cht):
//
//   cheats = 2
//   cheat0_desc = "Max Experience"
//   cheat0_code = "8201328C+E0FF+8201328E+05F5"
//   cheat0_enable = false
//
// The codes are CodeBreaker / GameShark codes, so their addresses are absolute
// and nothing is translated here: the entry keeps the code text and the UI hands
// it to the emulator's own parser, which knows what each code type means.
//
// The files are hand written and not always neatly formatted (some glue
// "cheats = 27" to the first entry, some separate address and value with a space
// instead of '+'), so this reader scans for keys instead of parsing lines.
//
// Nothing here depends on MFC or on the emulator; see cheatdb.h for the shape.

#include "cheatdb.h"

#include <cstddef>
#include <string>

// Reads and parses one .cht file.  `detail` receives the offending path, never a
// sentence.  Returns CHEAT_DB_EMPTY when the file opens but holds no code the
// emulator could use, CHEAT_DB_ERROR when it cannot be read at all.
CheatDbResult LrLoadFile(const char* path, CheatDbFile& file, CheatDbError& error,
                         std::string& detail);

bool LrParseChtText(const char* text, size_t length, CheatDbFile& file);

#endif // LIBRETROCHT_HEADER
