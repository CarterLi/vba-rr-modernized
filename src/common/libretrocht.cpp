#include "libretrocht.h"

#include <cstring>
#include <map>

namespace
{

const int    LR_MAX_ENTRIES = 4096;
const size_t LR_MAX_VALUE   = 4096;      // a single desc or code string

bool lrIsSpace(char c)
{
	return c == ' ' || c == '\t';
}

bool lrIsDigit(char c)
{
	return c >= '0' && c <= '9';
}

char lrUpper(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

// Case-insensitive prefix test against a NUL terminated word.
bool lrStarts(const char* text, size_t available, const char* word)
{
	const size_t length = strlen(word);
	if (available < length)
		return false;
	for (size_t i = 0; i < length; i++)
	{
		if (lrUpper(text[i]) != lrUpper(word[i]))
			return false;
	}
	return true;
}

// Position of the next "cheatN_" key at or after `from`, or `length`.
size_t lrFindKey(const char* text, size_t length, size_t from)
{
	for (size_t i = from; i + 6 < length; i++)
	{
		if (!lrStarts(text + i, length - i, "cheat"))
			continue;
		size_t cursor = i + 5;
		if (cursor >= length || !lrIsDigit(text[cursor]))
			continue;
		while (cursor < length && lrIsDigit(text[cursor]))
			cursor++;
		if (cursor < length && text[cursor] == '_')
			return i;
	}
	return length;
}

struct LrEntry
{
	std::string desc;
	std::string code;
	bool        hasDesc;
	bool        hasCode;

	LrEntry() : hasDesc(false), hasCode(false) {}
};

} // namespace

bool LrParseChtText(const char* text, size_t length, CheatDbFile& file)
{
	file.title.clear();
	file.gameName.clear();
	file.system = "GBA";
	file.entries.clear();
	file.problems.clear();
	file.truncatedAddresses = 0;

	if (!text)
		return false;

	// A UTF-8 BOM would otherwise be skipped anyway, but this keeps the reported
	// text clean.
	if (length >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
	    (unsigned char)text[2] == 0xBF)
	{
		text += 3;
		length -= 3;
	}

	std::map<int, LrEntry> found;

	size_t pos = 0;
	while (pos + 6 < length)
	{
		if (!lrStarts(text + pos, length - pos, "cheat"))
		{
			pos++;
			continue;
		}

		size_t cursor = pos + 5;
		if (cursor >= length || !lrIsDigit(text[cursor]))
		{
			pos = cursor;   // "cheats =" and similar
			continue;
		}

		int index = 0;
		while (cursor < length && lrIsDigit(text[cursor]))
		{
			if (index < 100000)
				index = index * 10 + (text[cursor] - '0');
			cursor++;
		}
		if (cursor >= length || text[cursor] != '_')
		{
			pos = cursor;
			continue;
		}
		cursor++;

		int field = -1;   // 0 desc, 1 code, 2 enable
		if (lrStarts(text + cursor, length - cursor, "desc"))
		{
			field = 0;
			cursor += 4;
		}
		else if (lrStarts(text + cursor, length - cursor, "code"))
		{
			field = 1;
			cursor += 4;
		}
		else if (lrStarts(text + cursor, length - cursor, "enable"))
		{
			field = 2;
			cursor += 6;
		}
		else
		{
			pos = cursor;
			continue;
		}

		while (cursor < length && lrIsSpace(text[cursor]))
			cursor++;
		if (cursor >= length || text[cursor] != '=')
		{
			pos = cursor;
			continue;
		}
		cursor++;
		while (cursor < length && lrIsSpace(text[cursor]))
			cursor++;

		std::string value;
		if (cursor < length && text[cursor] == '"')
		{
			// A quoted value never crosses a line break.  One real file forgot the
			// closing quote on ten long code lines; reading on would swallow the
			// descriptions that follow, while stopping here recovers the value in
			// full.
			cursor++;
			const size_t start = cursor;
			while (cursor < length && text[cursor] != '"' && text[cursor] != '\r' &&
			       text[cursor] != '\n')
				cursor++;
			value.assign(text + start, cursor - start);
			if (cursor < length && text[cursor] == '"')
				cursor++;   // closing quote
		}
		else
		{
			// Unquoted values run to the end of the line - but no further: some
			// files glue the next key straight onto the value
			// ("cheat0_enable = falsecheat1_desc = ..."), so a key starts a new
			// field even when there is no line break between them.
			const size_t next = lrFindKey(text, length, cursor);
			size_t       end = cursor;
			while (end < length && end < next && text[end] != '\r' && text[end] != '\n')
				end++;
			value.assign(text + cursor, end - cursor);
			while (!value.empty() && (lrIsSpace(value[value.size() - 1]) || value[value.size() - 1] == '\r'))
				value.resize(value.size() - 1);
			cursor = end;
		}

		if (value.size() > LR_MAX_VALUE)
		{
			CheatDbProblem problem;
			problem.line = index + 1;
			problem.kind = CHEAT_DB_PROBLEM_LIMIT;
			problem.text = value.substr(0, 40);
			file.problems.push_back(problem);
		}
		else if (field == 0)
		{
			LrEntry& entry = found[index];
			entry.desc = value;
			entry.hasDesc = true;
		}
		else if (field == 1)
		{
			LrEntry& entry = found[index];
			entry.code = value;
			entry.hasCode = true;
		}
		// field 2 ("enable") records the author's default, which the import
		// dialog does not use: the user picks what to add.

		pos = cursor;
	}

	for (std::map<int, LrEntry>::const_iterator it = found.begin(); it != found.end(); ++it)
	{
		const LrEntry& source = it->second;

		if (!source.hasCode || source.code.empty())
		{
			if (source.hasDesc && !source.desc.empty())
			{
				CheatDbProblem problem;
				problem.line = it->first + 1;
				problem.kind = CHEAT_DB_PROBLEM_EMPTY;
				problem.text = source.desc;
				file.problems.push_back(problem);
			}
			continue;
		}
		if ((int)file.entries.size() >= LR_MAX_ENTRIES)
		{
			CheatDbProblem problem;
			problem.line = it->first + 1;
			problem.kind = CHEAT_DB_PROBLEM_LIMIT;
			problem.text = source.code.substr(0, 40);
			file.problems.push_back(problem);
			continue;
		}

		CheatDbEntry entry;
		entry.section = source.desc;
		entry.key.clear();
		entry.useKeyName = false;
		entry.code = source.code;
		file.entries.push_back(entry);
	}

	return !file.entries.empty();
}

CheatDbResult LrLoadFile(const char* path, CheatDbFile& file, CheatDbError& error,
                         std::string& detail)
{
	file = CheatDbFile();

	std::string text;
	if (!CheatDbReadTextFile(path, text, error, detail))
		return CHEAT_DB_ERROR;

	if (!LrParseChtText(text.data(), text.size(), file))
		return CHEAT_DB_EMPTY;
	return CHEAT_DB_OK;
}
