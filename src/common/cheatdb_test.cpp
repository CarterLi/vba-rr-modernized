// Offline harness for the cheat database readers: ezflashdb.cpp (EZ-Flash
// package), libretrocht.cpp (libretro .cht) and cheatcodetext.cpp (the code
// splitter shared with the Add dialog).
//
// Not part of the emulator build: compile it by hand when a reader changes (see
// documentation/cheat-database.md).  Pass the cheat package as argv[1]:
//
//   cheatdb_test.exe "C:\path\EZ-FLASH OMEGA.zip"
//
// It first runs the tolerance rules against synthetic input, then walks every
// record of the real package and checks the totals against numbers taken from
// an independent implementation of the same format.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <io.h>

#include "cheatdb.h"
#include "cheatcodetext.h"
#include "ezflashdb.h"
#include "libretrocht.h"
#include "ziparchive.h"

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const char* what, const char* detail = NULL)
{
	g_checks++;
	if (ok)
		return;
	g_failures++;
	printf("FAIL: %s", what);
	if (detail && *detail)
		printf(" [%s]", detail);
	printf("\n");
}

static void checkEqual(long actual, long expected, const char* what)
{
	g_checks++;
	if (actual == expected)
		return;
	g_failures++;
	printf("FAIL: %s (expected %ld, got %ld)\n", what, expected, actual);
}

static void checkText(const std::string& actual, const char* expected, const char* what)
{
	g_checks++;
	if (actual == expected)
		return;
	g_failures++;
	printf("FAIL: %s (expected '%s', got '%s')\n", what, expected, actual.c_str());
}

static CheatDbFile parse(const char* text, unsigned int base = 0)
{
	CheatDbFile file;
	EzParseChtText(text, strlen(text), file, base);
	return file;
}

static void torture()
{
	// A well formed file with the usual padding and a title line.
	{
		CheatDbFile file = parse("Some Database\r\n\r\n[Max_Hearts]\r\nSwitch=4563C,E7,03\r\n\r\n"
		                         "[GameInfo]\r\nName=Game_Name\r\nSystem=GBA\r\nText=Game_Name\r\n");
		checkEqual((long)file.entries.size(), 1, "sanity: one entry");
		checkText(file.title, "Some Database", "sanity: title");
		checkText(file.gameName, "Game_Name", "sanity: game name");
		checkText(file.system, "GBA", "sanity: system");
		if (file.entries.size() == 1)
		{
			const CheatDbEntry& e = file.entries[0];
			checkText(e.section, "Max_Hearts", "sanity: section");
			checkText(e.key, "Switch", "sanity: key");
			check(!e.useKeyName, "sanity: single key does not use the key name");
			checkEqual((long)e.writes.size(), 1, "sanity: coalesced to one write");
			if (e.writes.size() == 1)
			{
				checkEqual((long)e.writes[0].address, 0x0300563C, "sanity: IWRAM address");
				checkEqual((long)e.writes[0].value, 0x03E7, "sanity: 16 bit value");
				checkEqual((long)e.writes[0].size, 2, "sanity: 16 bit write");
			}
		}
		checkEqual((long)file.problems.size(), 0, "sanity: no problems");
	}

	// Encoding of the address: both documented forms, and the kernel's IWRAM
	// switch at 0x40000.
	{
		CheatDbFile a = parse("[x]\nSwitch=43FF0,FF\n");
		checkEqual((long)a.entries[0].writes[0].address, 0x03003FF0, "address: 0x4xxxx is IWRAM");

		CheatDbFile b = parse("[x]\nSwitch=13288,00,7F\n");
		checkEqual((long)b.entries[0].writes[0].address, 0x02013288, "address: short form is EWRAM");
		checkEqual((long)b.entries[0].writes[0].value, 0x7F00, "address: value is little endian");
		checkEqual((long)b.entries[0].writes[0].size, 2, "address: two bytes fold into one write");

		CheatDbFile c = parse("[x]\nSwitch=02030580,D0,07,00,00\n");
		checkEqual((long)c.entries[0].writes[0].address, 0x02030580, "address: 0x02 form stays absolute");
		checkEqual((long)c.entries[0].writes[0].value, 0x000007D0, "address: 32 bit value");
		checkEqual((long)c.entries[0].writes[0].size, 4, "address: four bytes fold into one write");
	}

	// Values that are not a byte sequence are dropped, and the rest of the line
	// survives.
	{
		CheatDbFile f = parse("[x]\nSwitch=100,FF,GG;200,11\n");
		checkEqual((long)f.entries.size(), 1, "value: entry kept despite one bad token");
		checkEqual((long)f.problems.size(), 1, "value: one problem recorded");
		check(f.problems.size() == 1 && f.problems[0].kind == CHEAT_DB_PROBLEM_VALUE, "value: problem kind");
	}

	// An odd number of hex digits is zero padded instead of being dropped: the
	// shipped database has exactly one such line ("40E4E,1EF,08").
	{
		CheatDbFile f = parse("[x]\nSwitch=40E4E,1EF,08\n");
		checkEqual((long)f.entries.size(), 1, "padding: entry kept");
		if (f.entries.size() != 1)
			return;
		checkEqual((long)f.entries[0].writes.size(), 2, "padding: two writes");
		if (f.entries[0].writes.size() == 2)
		{
			checkEqual((long)f.entries[0].writes[0].value, 0x01EF, "padding: 1EF becomes 0x01EF");
			checkEqual((long)f.entries[0].writes[0].size, 2, "padding: two bytes");
			checkEqual((long)f.entries[0].writes[1].address, 0x03000E50, "padding: the next token follows on");
		}
		checkEqual((long)f.problems.size(), 0, "padding: no problem reported");
	}

	// Addresses outside the format are refused instead of being folded into a
	// different location.
	{
		CheatDbFile f = parse("[x]\nSwitch=50000,FF\nSwitch=0860000,FF\n");
		checkEqual((long)f.entries.size(), 0, "address: out of range entries dropped");
		checkEqual((long)f.problems.size(), 2, "address: two problems");
		check(f.problems.size() == 2 && f.problems[0].kind == CHEAT_DB_PROBLEM_ADDRESS, "address: problem kind");
	}

	// Unterminated section header: reported, but the following entry still lands
	// in the last valid section.
	{
		CheatDbFile f = parse("[Good]\nSwitch=100,1\n[Broken\nSwitch=200,2\n");
		checkEqual((long)f.entries.size(), 2, "section: entry after a broken header survives");
		checkEqual((long)f.problems.size(), 1, "section: one problem");
		check(f.problems.size() == 1 && f.problems[0].kind == CHEAT_DB_PROBLEM_SECTION, "section: problem kind");
		check(f.entries.size() == 2 && f.entries[1].section == "Good", "section: falls back to the last one");
	}

	// An entry before any section has no name, so it cannot be offered.
	{
		CheatDbFile f = parse("Switch=100,1\n[A]\nSwitch=200,2\n");
		checkEqual((long)f.entries.size(), 1, "order: orphan entry dropped");
		checkEqual((long)f.problems.size(), 1, "order: one problem");
	}

	// Comments the kernel understands, and line endings the editor may produce.
	{
		CheatDbFile f = parse("# comment\n// comment\n/* block\n still block */\n[A]\r"
		                      "Switch=100,1\r\nSwitch=101,2\rSwitch=102,3\n");
		checkEqual((long)f.entries.size(), 3, "comment: three entries");
		checkEqual((long)f.problems.size(), 0, "comment: no problems");
	}

	// A UTF-8 BOM must not glue itself to the first section name.
	{
		CheatDbFile f = parse("\xEF\xBB\xBF[A]\nSwitch=100,1\n");
		checkEqual((long)f.entries.size(), 1, "bom: entry parsed");
		check(f.entries.size() == 1 && f.entries[0].section == "A", "bom: section name intact");
	}

	// A section offering several alternatives keeps the key names.
	{
		CheatDbFile f = parse("[Money]\n2,000=02030580,D0,07,00,00\n9,999,999=02030580,7F,96,98,00\n");
		checkEqual((long)f.entries.size(), 2, "choice: two entries");
		check(f.entries.size() == 2 && f.entries[0].useKeyName && f.entries[1].useKeyName,
		      "choice: key name is shown");
		checkText(f.entries[0].key, "2,000", "choice: key kept verbatim");
	}

	// The same address twice must not be merged into one write.
	{
		CheatDbFile f = parse("[x]\nSwitch=100,FF;100,01\n");
		checkEqual((long)f.entries.size(), 1, "duplicate: one entry");
		checkEqual((long)f.entries[0].writes.size(), 2, "duplicate: two writes");
	}

	// Long lines and oversized entries are refused rather than truncated.
	{
		std::string text = "[x]\nSwitch=100,";
		for (int i = 0; i < 600; i++)
			text += "1,";
		text += "1\n";
		CheatDbFile f = parse(text.c_str());
		bool limited = false;
		for (size_t i = 0; i < f.problems.size(); i++)
		{
			if (f.problems[i].kind == CHEAT_DB_PROBLEM_LIMIT)
				limited = true;
		}
		checkEqual((long)f.entries.size(), 0, "limit: oversized entry dropped");
		check(limited, "limit: reported");
	}

	// Empty and garbage input.
	{
		CheatDbFile f = parse("");
		checkEqual((long)f.entries.size(), 0, "empty: no entries");
		check(!EzParseChtText("", 0, f), "empty: parser reports failure");
		check(!EzParseChtText("\x01\x02\x03", 3, f), "garbage: parser reports failure");
	}
}

// The package drops the memory area of almost every address it stores, so the
// reader can be told which one to use.  These cases pin down what that changes
// and, just as importantly, what it must leave alone.
static void memoryArea()
{
	// "4328C" is 0x40000 + 0x328C: castle Aria of Sorrow's max experience, which
	// really lives at 0x0201328C.  Its two halves are contiguous, so the reader
	// folds them into one 32 bit write.
	const char* cheat = "[Max_Experience]\nSwitch=4328C,FF,E0;4328E,F5,05\n";

	{
		CheatDbFile f = parse(cheat);
		checkEqual((long)f.entries.size(), 1, "area: entry parsed");
		checkEqual(f.truncatedAddresses, 2, "area: both addresses counted as cut short");
		if (f.entries.size() == 1 && f.entries[0].writes.size() == 1)
		{
			const CheatDbWrite& w = f.entries[0].writes[0];
			checkEqual((long)w.address, 0x0300328C, "area: default is the kernel's IWRAM");
			checkEqual((long)w.size, 4, "area: the two halves coalesce");
			checkEqual((long)w.value, 0x05F5E0FF, "area: value unaffected");
		}
		else
		{
			check(false, "area: one coalesced write");
		}
	}

	{
		CheatDbFile f = parse(cheat, 0x02010000);
		check(f.entries.size() == 1 && f.entries[0].writes.size() == 1 &&
		          f.entries[0].writes[0].address == 0x0201328C,
		      "area: page 1 moves the address the package lost");
	}

	{
		CheatDbFile f = parse(cheat, 0x02000000);
		check(f.entries.size() == 1 && f.entries[0].writes.size() == 1 &&
		          f.entries[0].writes[0].address == 0x0200328C,
		      "area: page 0 keeps the low 16 bits only");
	}

	{
		CheatDbFile f = parse(cheat, 0x03000000);
		check(f.entries.size() == 1 && f.entries[0].writes.size() == 1 &&
		          f.entries[0].writes[0].address == 0x0300328C,
		      "area: asking for IWRAM matches the default");
	}

	// Writes that are not next to each other stay separate, and both move.
	{
		CheatDbFile f = parse("[x]\nSwitch=4328C,FF;43290,3F\n", 0x02010000);
		check(f.entries.size() == 1 && f.entries[0].writes.size() == 2 &&
		          f.entries[0].writes[0].address == 0x0201328C &&
		          f.entries[0].writes[1].address == 0x02013290,
		      "area: two separate writes both move");
	}

	// An offset past 0x7FFF cannot be IWRAM at all; the kernel folds it into a
	// mirror, while EWRAM can keep all sixteen bits.
	{
		CheatDbFile f = parse("[x]\nSwitch=4B000,12\n");
		check(f.entries.size() == 1 && f.entries[0].writes[0].address == 0x03003000,
		      "area: IWRAM folds the high bit");
		CheatDbFile g = parse("[x]\nSwitch=4B000,12\n", 0x02000000);
		check(g.entries.size() == 1 && g.entries[0].writes[0].address == 0x0200B000,
		      "area: EWRAM keeps all sixteen bits");
	}

	// Addresses that already spell themselves out are not the caller's to move.
	{
		CheatDbFile f = parse("[x]\nSwitch=02030580,01\n", 0x02020000);
		checkText(f.gameName, "", "area: absolute file has no game name");
		checkEqual(f.truncatedAddresses, 0, "area: an absolute address is not counted");
		check(f.entries.size() == 1 && f.entries[0].writes[0].address == 0x02030580,
		      "area: absolute address ignored the base");
	}

	// Same for the short form that fits under 0x40000: it carries its own page.
	{
		CheatDbFile f = parse("[x]\nSwitch=1328C,01\n", 0x02020000);
		checkEqual(f.truncatedAddresses, 0, "area: a full EWRAM offset is not counted");
		check(f.entries.size() == 1 && f.entries[0].writes[0].address == 0x0201328C,
		      "area: full EWRAM offset ignored the base");
	}
}

// The splitter is what the Add dialog and the libretro import have in common:
// both hand it whatever code text a user or a file provides, and both expect the
// engine's two code forms back.
static void codeText()
{
	{
		std::vector<CheatCodeText> codes;
		CheatSplitCodes("8201328C+E0FF+8201328E+05F5", codes);
		checkEqual((long)codes.size(), 2, "code text: two pairs");
		if (codes.size() == 2)
		{
			checkText(codes[0].text, "8201328C E0FF", "code text: first pair");
			check(!codes[0].gsa, "code text: eight plus four is CodeBreaker");
			check(!codes[0].v3, "code text: CodeBreaker is not a v3 code");
			checkText(codes[1].text, "8201328E 05F5", "code text: second pair");
		}
	}

	// Some database files separate address and value with a space, not with '+'.
	{
		std::vector<CheatCodeText> codes;
		CheatSplitCodes("82006E5C FFFF+82006E5E FFFF", codes);
		checkEqual((long)codes.size(), 2, "code text: space separated pairs");
		if (codes.size() == 2)
			checkText(codes[0].text, "82006E5C FFFF", "code text: space separated pair");
	}

	// Eight plus eight is a whole GameShark code, and those need a master code.
	{
		std::vector<CheatCodeText> codes;
		CheatSplitCodes("C40F4772 00008401", codes);
		check(codes.size() == 1 && codes[0].gsa && codes[0].v3 &&
		          codes[0].text == "C40F477200008401",
		      "code text: eight plus eight is a v3 GameShark code");
	}

	// The same thing written without the space.
	{
		std::vector<CheatCodeText> codes;
		CheatSplitCodes("000093DD000A0007", codes);
		check(codes.size() == 1 && codes[0].gsa && !codes[0].v3,
		      "code text: sixteen digits stay one GameShark code");
	}

	// A CodeBreaker code whose space went missing.
	{
		std::vector<CheatCodeText> codes;
		CheatSplitCodes("33003DDF00FF", codes);
		check(codes.size() == 1 && !codes[0].gsa && codes[0].text == "33003DDF 00FF",
		      "code text: twelve digits are split at eight");
	}

	// Lowercase input, and an address that never got a value.
	{
		std::vector<CheatCodeText> codes;
		CheatSplitCodes("8201328c+e0ff 82013290", codes);
		checkEqual((long)codes.size(), 2, "code text: odd token is still reported");
		if (codes.size() == 2)
		{
			checkText(codes[0].text, "8201328C E0FF", "code text: upper cased");
			checkText(codes[1].text, "82013290", "code text: leftover address");
		}
	}

	{
		std::vector<CheatCodeText> codes;
		CheatSplitCodes("", codes);
		checkEqual((long)codes.size(), 0, "code text: empty input");
		CheatSplitCodes(NULL, codes);
		checkEqual((long)codes.size(), 0, "code text: null input");
	}
}

// libretro-database .cht files: one game per file, CodeBreaker / GameShark codes
// with absolute addresses, and hand written formatting that is not line based.
static void libretro()
{
	{
		// Shaped after the real files: "cheats = N" glued to the first entry, and
		// the last description has no code under it.
		const char* text = "cheats = 2\r\n\r\n"
		                   "cheat0_desc = \"Max Experience\"\r\n"
		                   "cheat0_code = \"8201328C+E0FF+8201328E+05F5\"\r\n"
		                   "cheat0_enable = false\r\n\r\n"
		                   "cheat1_desc = \"(M) Must Be On\"\r\n"
		                   "cheat1_code = \"C40F4772 00008401\"\r\n"
		                   "cheat1_enable = falsecheat2_desc = \"No code here\"\r\n";

		CheatDbFile file;
		check(LrParseChtText(text, strlen(text), file), "libretro: file parses");
		checkEqual((long)file.entries.size(), 2, "libretro: two entries with codes");
		checkEqual(file.truncatedAddresses, 0, "libretro: addresses are absolute");
		checkEqual((long)file.problems.size(), 1, "libretro: the entry without a code is reported");
		if (file.problems.size() == 1)
		{
			checkEqual(file.problems[0].kind, CHEAT_DB_PROBLEM_EMPTY, "libretro: problem kind");
			checkEqual(file.problems[0].line, 3, "libretro: problem names the entry");
			checkText(file.problems[0].text, "No code here", "libretro: problem text");
		}

		if (file.entries.size() == 2)
		{
			checkText(file.entries[0].section, "Max Experience", "libretro: first name");
			checkText(file.entries[0].code, "8201328C+E0FF+8201328E+05F5",
			          "libretro: code text is kept as written");
			checkEqual((long)file.entries[0].writes.size(), 0,
			           "libretro: nothing is translated here");
			checkText(file.entries[1].section, "(M) Must Be On", "libretro: second name");
			checkText(file.entries[1].code, "C40F4772 00008401", "libretro: second code");
		}
	}

	// Unquoted values run to the end of the line, which is how older files
	// (and any hand edit) tend to look.
	{
		const char* text = "cheat0_desc = Unquoted Name\ncheat0_code = 8201328C E0FF\n";
		CheatDbFile file;
		check(LrParseChtText(text, strlen(text), file), "libretro: unquoted file parses");
		if (file.entries.size() == 1)
		{
			checkText(file.entries[0].section, "Unquoted Name", "libretro: unquoted name");
			checkText(file.entries[0].code, "8201328C E0FF", "libretro: unquoted code");
		}
		else
		{
			check(false, "libretro: unquoted entry");
		}
	}

	// A real file forgot the closing quote on ten long code lines.  The value has
	// to stop at the line break, otherwise it swallows the descriptions that
	// follow and those cheats arrive without a name.
	{
		const char* text = "cheat0_desc = \"Long Code\"\n"
		                   "cheat0_code = \"8201328C+E0FF\n"
		                   "cheat0_enable = false\n"
		                   "cheat1_desc = \"Next One\"\n"
		                   "cheat1_code = \"82013290+003F\"\n";
		CheatDbFile file;
		check(LrParseChtText(text, strlen(text), file), "libretro: missing quote still parses");
		checkEqual((long)file.entries.size(), 2, "libretro: missing quote keeps both entries");
		if (file.entries.size() == 2)
		{
			checkText(file.entries[0].code, "8201328C+E0FF",
			          "libretro: unterminated value ends at the line break");
			checkText(file.entries[1].section, "Next One",
			          "libretro: the next name survives");
		}
	}

	// A file whose bytes make no sense must say so instead of inventing entries.
	{
		CheatDbFile file;
		check(!LrParseChtText("this is not a cheat file", strlen("this is not a cheat file"), file),
		      "libretro: junk rejected");
		checkEqual((long)file.entries.size(), 0, "libretro: junk yields no entries");
	}

	// Reading from disk, including the failure that matters to the UI.
	{
		CheatDbFile file;
		CheatDbError error = CHEAT_DB_ERR_NONE;
		std::string detail;
		checkEqual(LrLoadFile("this-file-does-not-exist.cht", file, error, detail),
		           CHEAT_DB_ERROR, "libretro: missing file reported");
		checkEqual(error, CHEAT_DB_ERR_FILE, "libretro: missing file error kind");

		char path[512];
		const char* dir = getenv("TEMP");
		sprintf(path, "%s\\cheatdb_test_tmp.cht", (dir && *dir) ? dir : ".");

		FILE* handle = fopen(path, "wb");
		check(handle != NULL, "libretro: temporary file written");
		if (handle)
		{
			const char* body = "cheats = 1\n\ncheat0_desc = \"Infinite Health\"\n"
			                   "cheat0_code = \"8201327A+270F\"\ncheat0_enable = false\n";
			fwrite(body, 1, strlen(body), handle);
			fclose(handle);

			CheatDbFile loaded;
			checkEqual(LrLoadFile(path, loaded, error, detail), CHEAT_DB_OK,
			           "libretro: file loads");
			checkEqual((long)loaded.entries.size(), 1, "libretro: loaded one entry");
			if (loaded.entries.size() == 1)
				checkText(loaded.entries[0].code, "8201327A+270F", "libretro: loaded code");
			remove(path);
		}
	}
}

// Both formats are plain .cht, so the reader is chosen from the bytes and never
// from the name.  These cases pin down what each mark is worth, including the
// hand-written shapes found in the real files.
static void sniffing()
{
	{
		// Shaped like a package file: a section per cheat, then [GameInfo].
		const char* text = "DeadSkullzJr's GBA Cheat Database\r\n\r\n"
		                   "[Max_Health]\r\nSwitch=4563C,E7,03\r\n\r\n"
		                   "[GameInfo]\r\nName=ChuChu_Rocket\r\nSystem=GBA\r\n";
		checkEqual(CheatDbSniffFormat(text, strlen(text)), CHEAT_DB_FORMAT_EZFLASH,
		           "sniff: package text");
	}

	{
		const char* text = "cheats = 1\r\n\r\ncheat0_desc = \"Max Experience\"\r\n"
		                   "cheat0_code = \"8201328C+E0FF\"\r\ncheat0_enable = false\r\n";
		checkEqual(CheatDbSniffFormat(text, strlen(text)), CHEAT_DB_FORMAT_LIBRETRO,
		           "sniff: libretro text");
	}

	{
		checkEqual(CheatDbSniffFormat("", 0), CHEAT_DB_FORMAT_UNKNOWN, "sniff: empty text");
		checkEqual(CheatDbSniffFormat(NULL, 0), CHEAT_DB_FORMAT_UNKNOWN, "sniff: null text");
		const char* junk = "this is not a cheat file at all\n";
		checkEqual(CheatDbSniffFormat(junk, strlen(junk)), CHEAT_DB_FORMAT_UNKNOWN,
		           "sniff: junk text");
	}

	{
		// A BOM would sit in front of the first key of the first line.
		const char* text =
		    "\xEF\xBB\xBF"
		    "cheats = 1\ncheat0_desc = \"x\"\ncheat0_code = \"8201328C+E0FF\"\n";
		checkEqual(CheatDbSniffFormat(text, strlen(text)), CHEAT_DB_FORMAT_LIBRETRO,
		           "sniff: BOM is skipped");
	}

	{
		// Real files glue the header onto the first entry, and one entry onto the
		// next, without a line break anywhere.
		const char* text = "cheats = 2cheat0_desc = \"a\"cheat0_code = \"8201328C+E0FF\""
		                   "cheat0_enable = falsecheat1_desc = \"b\"cheat1_code = \"82013290+003F\"";
		checkEqual(CheatDbSniffFormat(text, strlen(text)), CHEAT_DB_FORMAT_LIBRETRO,
		           "sniff: glued keys are still found");
	}

	{
		// The [GameInfo] block was cut away, but sections and data lines remain.
		const char* text =
		    "[Infinite_Health]\nSwitch=4563C,E7,03\n[Infinite_Lives]\nSwitch=45700,63,00\n";
		checkEqual(CheatDbSniffFormat(text, strlen(text)), CHEAT_DB_FORMAT_EZFLASH,
		           "sniff: package text without [GameInfo]");
	}

	{
		const char* text = "cheats = 0\n";
		checkEqual(CheatDbSniffFormat(text, strlen(text)), CHEAT_DB_FORMAT_LIBRETRO,
		           "sniff: empty libretro file");
	}

	{
		// Both decisive marks: a hand-made mix.  Reported as unknown so the caller
		// tries both readers instead of trusting a vote over weak hints.
		const char* text = "[GameInfo]\nName=X\nSystem=GBA\n"
		                   "cheat0_desc = \"a\"\ncheat0_code = \"8201328C+E0FF\"\n";
		checkEqual(CheatDbSniffFormat(text, strlen(text)), CHEAT_DB_FORMAT_UNKNOWN,
		           "sniff: mixed file is left to the readers");
	}

	{
		// Brackets inside a value must not count as a section header, or a
		// libretro description could tip the balance.
		const char* text = "cheat0_desc = \"[12345678] Game Over\"\n"
		                   "cheat0_code = \"8201328C+E0FF\"\n";
		checkEqual(CheatDbSniffFormat(text, strlen(text)), CHEAT_DB_FORMAT_LIBRETRO,
		           "sniff: brackets inside a value are not a section");
	}

	{
		// Line endings and letter case must not matter.
		const char* cr = "[gameinfo]\rName=X\rSystem=GBA\r[One]\rSwitch=4563C,E7,03\r";
		checkEqual(CheatDbSniffFormat(cr, strlen(cr)), CHEAT_DB_FORMAT_EZFLASH,
		           "sniff: CR only, lower case section");
	}
}

// Every .cht in a directory must be recognised as libretro text.  Point this at
// a checkout of libretro-database/cht/Nintendo - Game Boy Advance.
static void walkLibretroDir(const char* dir)
{
	std::string base = dir;
	if (!base.empty() && base[base.size() - 1] != '\\' && base[base.size() - 1] != '/')
		base += '\\';

	struct _finddata_t info;
	intptr_t handle = _findfirst((base + "*.cht").c_str(), &info);
	if (handle == -1)
	{
		printf("note: no .cht files under '%s'\n", dir);
		return;
	}

	long files = 0, libretroFiles = 0, other = 0, unreadable = 0;
	do
	{
		if (info.attrib & _A_SUBDIR)
			continue;

		std::string  text;
		CheatDbError error = CHEAT_DB_ERR_NONE;
		std::string  detail;
		if (!CheatDbReadTextFile((base + info.name).c_str(), text, error, detail))
		{
			unreadable++;
			continue;
		}

		files++;
		const CheatDbFormat format = CheatDbSniffFormat(text.data(), text.size());
		if (format == CHEAT_DB_FORMAT_LIBRETRO)
			libretroFiles++;
		else
		{
			if (other < 5)
				printf("  sniff miss: %s -> %d\n", info.name, (int)format);
			other++;
		}
	} while (_findnext(handle, &info) == 0);
	_findclose(handle);

	check(files > 0, "libretro corpus: at least one file was read");
	checkEqual(unreadable, 0, "libretro corpus: every file could be read");
	checkEqual(other, 0, "libretro corpus: no file looks like a package");
	printf("libretro corpus: %ld files, %ld recognised as libretro\n", files, libretroFiles);
}

static void walkPackage(const char* path, bool dump)
{
	EzCheatDb   db;
	CheatDbError   error = CHEAT_DB_ERR_NONE;
	std::string detail;
	if (!db.Open(path, error, detail))
	{
		printf("FAIL: cannot open package '%s' (error %d, %s)\n", path, (int)error, detail.c_str());
		g_failures++;
		return;
	}

	checkEqual(db.GameCodeCount(), 3187, "package: index records");
	checkEqual(db.IndexSkippedRecords(), 0, "package: no unreadable index records");

	long files = 0, entries = 0, writes = 0;
	long sizes[5] = { 0, 0, 0, 0, 0 };
	long problems = 0, emptyFiles = 0, noFile = 0, truncated = 0;
	int  maxWritesPerEntry = 0;
	bool addressOk = true, alignmentOk = true, namesOk = true;

	for (int i = 0; i < db.RecordCount(); i++)
	{
		CheatDbFile file;
		CheatDbError   loadError = CHEAT_DB_ERR_NONE;
		std::string loadDetail;
		CheatDbResult  result = db.LoadByRecord(i, file, loadError, loadDetail);

		if (result == CHEAT_DB_ERROR)
		{
			printf("FAIL: record %d (%s) failed to load: error %d %s\n", i, db.RecordCode(i),
			       (int)loadError, loadDetail.c_str());
			g_failures++;
			continue;
		}
		if (result == CHEAT_DB_NO_CHEATS)
		{
			noFile++;
			if (dump)
				printf("dump %d 0 0\n", i);
			continue;
		}
		if (result == CHEAT_DB_EMPTY)
		{
			emptyFiles++;
			if (dump)
				printf("dump %d 0 0\n", i);
			continue;
		}

		files++;
		entries += (long)file.entries.size();
		problems += (long)file.problems.size();
		truncated += file.truncatedAddresses;

		if (dump)
		{
			long recordWrites = 0;
			for (size_t e = 0; e < file.entries.size(); e++)
				recordWrites += (long)file.entries[e].writes.size();
			printf("dump %d %d %ld\n", i, (int)file.entries.size(), recordWrites);
		}

		if (file.gameName.empty())
			namesOk = false;

		for (size_t e = 0; e < file.entries.size(); e++)
		{
			const CheatDbEntry& entry = file.entries[e];
			if ((int)entry.writes.size() > maxWritesPerEntry)
				maxWritesPerEntry = (int)entry.writes.size();

			for (size_t w = 0; w < entry.writes.size(); w++)
			{
				const CheatDbWrite& write = entry.writes[w];
				writes++;
				if (write.size < 1 || write.size > 4)
					alignmentOk = false;
				else
					sizes[write.size]++;
				if ((write.address & 0x0F000000) != 0x02000000 &&
				    (write.address & 0x0F000000) != 0x03000000)
					addressOk = false;
				if ((write.address & (unsigned)(write.size - 1)) != 0)
					alignmentOk = false;
			}
		}
	}

	// Totals from an independent implementation of the same format.
	checkEqual(files, 875, "package: games with usable cheats");
	checkEqual(entries, 7065, "package: entries");
	checkEqual(writes, 8426, "package: coalesced writes");
	checkEqual(sizes[1], 4636, "package: 8 bit writes");
	checkEqual(sizes[2], 2880, "package: 16 bit writes");
	checkEqual(sizes[4], 910, "package: 32 bit writes");
	checkEqual(problems, 0, "package: every line parses in the shipped database");
	// Every short address in this package is the 0x40000 form, so all of them had
	// their memory area dropped.  Counted independently by the analysis script.
	checkEqual(truncated, 9602, "package: short addresses that lost their memory area");
	check(addressOk, "package: all addresses are EWRAM or IWRAM");
	check(alignmentOk, "package: all writes are naturally aligned");
	check(namesOk, "package: every file names its game");
	check(maxWritesPerEntry <= 32, "package: entries stay small enough for MAX_CHEATS");

	printf("package: %ld games with cheats, %ld entries, %ld writes, %ld without a file, "
	       "%ld empty, max %d writes per entry\n",
	       files, entries, writes, noFile, emptyFiles, maxWritesPerEntry);

	// The look-up used by the emulator must agree with the record walk, which
	// also covers the game codes that appear more than once.
	long mismatches = 0;
	std::string lastCode;
	for (int i = 0; i < db.RecordCount(); i++)
	{
		const char* code = db.RecordCode(i);
		if (lastCode == code)
			continue;
		lastCode = code;

		CheatDbFile direct;
		CheatDbError   directError = CHEAT_DB_ERR_NONE;
		std::string directDetail;
		CheatDbResult  directResult = db.LoadByRecord(i, direct, directError, directDetail);

		CheatDbFile looked;
		CheatDbError   lookedError = CHEAT_DB_ERR_NONE;
		std::string lookedDetail;
		CheatDbResult  lookedResult = db.LoadForGameCode(code, looked, lookedError, lookedDetail);

		if (lookedResult != directResult || looked.entries.size() != direct.entries.size())
			mismatches++;
	}
	checkEqual(mismatches, 0, "lookup: game code look-up agrees with the record walk");

	// A code that is certainly not in the package must say so, not crash.
	{
		CheatDbFile file;
		CheatDbError   loadError = CHEAT_DB_ERR_NONE;
		std::string loadDetail;
		CheatDbResult  result = db.LoadForGameCode("ZZZZ", file, loadError, loadDetail);
		checkEqual(result, CHEAT_DB_NO_CHEATS, "lookup: unknown code reports no cheats");
	}

	// Re-reading a file with another memory area must move exactly the addresses
	// that were cut short and leave values, widths and everything else alone.
	{
		CheatDbFile before;
		bool        chosen = false;
		for (int i = 0; i < db.RecordCount() && !chosen; i++)
		{
			CheatDbFile candidate;
			CheatDbError   candidateError = CHEAT_DB_ERR_NONE;
			std::string candidateDetail;
			if (db.LoadByRecord(i, candidate, candidateError, candidateDetail) != CHEAT_DB_OK)
				continue;

			long totalWrites = 0;
			for (size_t e = 0; e < candidate.entries.size(); e++)
				totalWrites += (long)candidate.entries[e].writes.size();

			// Only games whose whole file uses the cut short form keep the
			// invariant below simple.
			if (candidate.truncatedAddresses > 0 && candidate.truncatedAddresses == totalWrites)
			{
				before = candidate;
				chosen = true;
			}
		}
		check(chosen, "reload: found a file that uses only cut short addresses");

		CheatDbFile after = before;
		checkEqual(db.ReloadWithBase(0x02010000, after), CHEAT_DB_OK, "reload: EWRAM page 1");
		checkEqual((long)after.entries.size(), (long)before.entries.size(), "reload: same entries");
		checkEqual(after.truncatedAddresses, before.truncatedAddresses, "reload: same counter");

		long moved = 0;
		bool same = true, lowHalf = true, inPage = true;
		for (size_t e = 0; e < after.entries.size() && e < before.entries.size(); e++)
		{
			if (after.entries[e].writes.size() != before.entries[e].writes.size())
				same = false;
			for (size_t w = 0; w < after.entries[e].writes.size() &&
			                    w < before.entries[e].writes.size(); w++)
			{
				const CheatDbWrite& a = before.entries[e].writes[w];
				const CheatDbWrite& b = after.entries[e].writes[w];
				if (a.value != b.value || a.size != b.size)
					same = false;
				if ((b.address & 0x7FFF) != (a.address & 0x7FFF))
					lowHalf = false;
				if ((b.address >> 16) != 0x0201)
					inPage = false;
				if (a.address != b.address)
					moved++;
			}
		}
		check(same, "reload: values and widths untouched");
		check(lowHalf, "reload: low fifteen bits preserved");
		check(inPage, "reload: every address lands in the chosen page");
		check(moved, "reload: addresses actually moved");
	}

	// The format check has to work on the package's own files, which are .cht
	// just like the libretro ones: one file that fell through would send it to
	// the wrong reader when a user picks it directly.
	{
		ZipArchive zip(path);
		check(zip.IsValid(), "sniff: package reopens for the format check");

		long chtFiles = 0, packageText = 0, parsedAsFile = 0, other = 0;
		for (int i = 0; i < zip.GetEntryCount(); i++)
		{
			ZipEntryInfo info;
			if (!zip.GetEntryInfo(i, &info))
				continue;
			const size_t length = strlen(info.name);
			if (length < 5 || _stricmp(info.name + length - 4, ".cht") != 0)
				continue;
			if (info.uncompressedSize == 0 || info.uncompressedSize > (4 << 20))
				continue;

			std::string text;
			text.resize(info.uncompressedSize);
			if (zip.ExtractEntry(i, &text[0], info.uncompressedSize) != (int)info.uncompressedSize)
				continue;

			chtFiles++;
			if (CheatDbSniffFormat(text.data(), text.size()) == CHEAT_DB_FORMAT_EZFLASH)
			{
				packageText++;

				// Exactly what the single-file path does with this file: sniff,
				// then parse with the reader the sniff picked.
				CheatDbFile file;
				if (EzParseChtText(text.data(), text.size(), file) && !file.entries.empty())
					parsedAsFile++;
			}
			else
			{
				if (other < 5)
					printf("  sniff miss: %s\n", info.name);
				other++;
			}
		}

		checkEqual(chtFiles, 928, "sniff: every cheat file of the package was read");
		checkEqual(packageText, chtFiles, "sniff: every package file is package text");
		checkEqual(other, 0, "sniff: no package file looks like libretro");
		// The rest of the package files hold GameInfo only, so they have nothing
		// to import either way; 875 is the number of games with cheats.
		checkEqual(parsedAsFile, 875, "sniff: a package file read on its own yields cheats");
	}

	// A package holds hundreds of files, so the lone-file shortcut has to refuse
	// it rather than pick one at random.
	{
		std::string  text;
		int          count = 0;
		CheatDbError loneError = CHEAT_DB_ERR_NONE;
		std::string  loneDetail;
		checkEqual(EzReadLoneCht(path, text, count, loneError, loneDetail), EZ_LONE_SEVERAL,
		           "lone: the package is not a single file");
		checkEqual(count, 928, "lone: the package counts its cheat files");
	}
}

int main(int argc, char** argv)
{
	torture();
	memoryArea();
	codeText();
	libretro();
	sniffing();

	const char* package = NULL;
	const char* libretroDir = NULL;
	bool        dump = false;

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--dump") == 0)
			dump = true;
		else if (strcmp(argv[i], "--libretro") == 0 && i + 1 < argc)
			libretroDir = argv[++i];
		else if (!package)
			package = argv[i];
	}

	if (package)
		walkPackage(package, dump);
	else
		printf("note: no package given, only the tolerance rules were exercised\n");

	if (libretroDir)
		walkLibretroDir(libretroDir);

	// Opening something that is not a package must fail loudly and cleanly.
	{
		EzCheatDb   db;
		CheatDbError   error = CHEAT_DB_ERR_NONE;
		std::string detail;
		check(!db.Open("this-path-does-not-exist.zip", error, detail), "open: missing path rejected");
		checkEqual(error, CHEAT_DB_ERR_ARCHIVE, "open: missing path error kind");
		check(!db.Open("", error, detail), "open: empty path rejected");
	}

	// The single-file shortcut inside a zip must fail cleanly too.
	{
		std::string  text;
		int          count = 0;
		CheatDbError loneError = CHEAT_DB_ERR_NONE;
		std::string  loneDetail;
		checkEqual(EzReadLoneCht("this-path-does-not-exist.zip", text, count, loneError, loneDetail),
		           EZ_LONE_FAILED, "lone: missing zip rejected");
	}

	printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
