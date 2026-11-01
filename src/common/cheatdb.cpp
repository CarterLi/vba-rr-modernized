// Pieces both cheat readers need: telling their formats apart, and reading a
// small text file.  See cheatdb.h for why the format cannot come from the
// extension.

#include "cheatdb.h"

#include <cstdio>
#include <cstring>

namespace
{

const long SNIFF_MAX_FILE = 4 << 20;   // the real files are a few kilobytes

bool sniffIsSpace(char c)
{
	return c == ' ' || c == '\t';
}

char sniffUpper(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

bool sniffEquals(const char* text, size_t length, const char* word)
{
	const size_t wordLength = strlen(word);
	if (length != wordLength)
		return false;
	for (size_t i = 0; i < length; i++)
	{
		if (sniffUpper(text[i]) != sniffUpper(word[i]))
			return false;
	}
	return true;
}

bool sniffStartsWith(const char* text, size_t length, const char* word)
{
	const size_t wordLength = strlen(word);
	if (length < wordLength)
		return false;
	for (size_t i = 0; i < wordLength; i++)
	{
		if (sniffUpper(text[i]) != sniffUpper(word[i]))
			return false;
	}
	return true;
}

// "cheat" followed by at least one digit and an underscore: the libretro keys.
bool sniffIsEntryKey(const char* text, size_t length)
{
	if (length < 7 || !sniffStartsWith(text, length, "cheat"))
		return false;

	size_t cursor = 5;
	if (text[cursor] < '0' || text[cursor] > '9')
		return false;
	while (cursor < length && text[cursor] >= '0' && text[cursor] <= '9')
		cursor++;
	return cursor < length && text[cursor] == '_';
}

} // namespace

CheatDbFormat CheatDbSniffFormat(const char* text, size_t length)
{
	if (!text || length == 0)
		return CHEAT_DB_FORMAT_UNKNOWN;

	// A BOM would otherwise sit in front of the first key on the first line.
	if (length >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
	    (unsigned char)text[2] == 0xBF)
	{
		text += 3;
		length -= 3;
	}

	bool gameInfo  = false;   // the package's fixed last section
	int  entryKeys = 0;       // cheatN_desc / cheatN_code / cheatN_enable
	int  headers   = 0;       // cheats = N
	int  sections  = 0;       // [Some_Name], one per cheat
	int  dataLines = 0;       // Switch=addr,bytes

	size_t pos = 0;
	while (pos < length)
	{
		size_t end = pos;
		while (end < length && text[end] != '\r' && text[end] != '\n')
			end++;

		size_t begin = pos;
		while (begin < end && sniffIsSpace(text[begin]))
			begin++;
		size_t stop = end;
		while (stop > begin && sniffIsSpace(text[stop - 1]))
			stop--;

		if (stop > begin)
		{
			if (text[begin] == '[')
			{
				// A section header is the whole line, brackets included.
				if (stop - begin >= 2 && text[stop - 1] == ']')
				{
					if (sniffEquals(text + begin + 1, stop - begin - 2, "GameInfo"))
						gameInfo = true;
					else
						sections++;
				}
			}
			else if (!(text[begin] == '#' ||
			           (begin + 1 < stop && text[begin] == '/' && text[begin + 1] == '/')))
			{
				size_t equals = begin;
				while (equals < stop && text[equals] != '=')
					equals++;
				if (equals < stop)
				{
					size_t keyEnd = equals;
					while (keyEnd > begin && sniffIsSpace(text[keyEnd - 1]))
						keyEnd--;

					if (sniffIsEntryKey(text + begin, keyEnd - begin))
						entryKeys++;
					else if (sniffEquals(text + begin, keyEnd - begin, "cheats"))
						headers++;
					else if (sniffEquals(text + begin, keyEnd - begin, "Switch"))
						dataLines++;
				}
			}
		}

		pos = end + 1;   // a "\r\n" pair just skips an empty line next round
	}

	// A file carrying both decisive marks has been pieced together by hand, and
	// no vote over weak hints can be trusted for it.  The callers try both
	// readers in that case, which settles it from what actually parses.
	if (gameInfo && entryKeys > 0)
		return CHEAT_DB_FORMAT_UNKNOWN;

	if (gameInfo)
		return CHEAT_DB_FORMAT_EZFLASH;
	if (entryKeys > 0)
		return CHEAT_DB_FORMAT_LIBRETRO;

	// Nothing decisive: weigh what is left.  The counts are capped so that one
	// long file cannot outvote a short one on sheer size.
	const int ezScore = (sections > 20 ? 20 : sections) + (dataLines > 20 ? 20 : dataLines);
	const int lrScore = (headers > 2 ? 2 : headers) * 3;

	if (ezScore > lrScore)
		return CHEAT_DB_FORMAT_EZFLASH;
	if (lrScore > ezScore)
		return CHEAT_DB_FORMAT_LIBRETRO;
	return CHEAT_DB_FORMAT_UNKNOWN;
}

bool CheatDbReadTextFile(const char* path, std::string& text, CheatDbError& error,
                         std::string& detail)
{
	text.clear();
	detail = path ? path : "";
	error = CHEAT_DB_ERR_FILE;

	FILE* handle = (path && *path) ? fopen(path, "rb") : NULL;
	if (!handle)
		return false;

	bool ok = false;
	if (fseek(handle, 0, SEEK_END) == 0)
	{
		const long size = ftell(handle);
		rewind(handle);
		if (size > 0 && size <= SNIFF_MAX_FILE)
		{
			text.resize((size_t)size);
			if (fread(&text[0], 1, (size_t)size, handle) == (size_t)size)
				ok = true;
			else
				text.clear();
		}
	}
	fclose(handle);

	if (ok)
		error = CHEAT_DB_ERR_NONE;
	return ok;
}
