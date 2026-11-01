#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#include "ezflashdb.h"
#include "ziparchive.h"

// Hard limits, all far above what a real package needs (the largest file in the
// shipped database is 3.5 KB) but low enough that a corrupted or hostile entry
// cannot make the emulator allocate wildly.
#define EZ_MAX_ENTRY_SIZE   (4 * 1024 * 1024)
#define EZ_MAX_LINE_SIZE    1024
#define EZ_MAX_ENTRIES      4096
#define EZ_MAX_WRITES       256

static bool ezIsDirectory(const char* path)
{
#ifdef _MSC_VER
	struct _stat st;
	if (_stat(path, &st) != 0)
		return false;
	return (st.st_mode & _S_IFDIR) != 0;
#else
	struct stat st;
	if (stat(path, &st) != 0)
		return false;
	return S_ISDIR(st.st_mode) != 0;
#endif
}

static std::string ezTrim(const std::string& s)
{
	size_t b = 0;
	size_t e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t'))
		b++;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t'))
		e--;
	return s.substr(b, e - b);
}

static std::string ezLower(const std::string& s)
{
	std::string out(s);
	for (size_t i = 0; i < out.size(); i++)
	{
		if (out[i] >= 'A' && out[i] <= 'Z')
			out[i] = (char)(out[i] - 'A' + 'a');
	}
	return out;
}

// The shipped database is pure ASCII.  Anything else would reach the dialogs
// through the ANSI code page and turn into mojibake, so it is replaced instead
// of being passed through; a name is never worth a crash or a garbled list.
static std::string ezSanitize(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++)
	{
		unsigned char c = (unsigned char)s[i];
		if (c < 0x20)
			continue;
		out += (c < 0x80) ? (char)c : '?';
	}
	return out;
}

static bool ezIsHex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Case insensitive compare for the few fixed keywords the format knows
// ("GameInfo", "Name", ...).  Kept local so this file stays free of CRT
// extensions and of Windows headers.
static bool ezEqualsNoCase(const char* a, const char* b)
{
	while (*a && *b)
	{
		char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
		if (ca != cb)
			return false;
		a++;
		b++;
	}
	return *a == *b;
}

static int ezHexValue(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return c - 'A' + 10;
}

static bool ezParseHex(const std::string& s, size_t maxDigits, unsigned int& out)
{
	if (s.empty() || s.size() > maxDigits)
		return false;
	unsigned int v = 0;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (!ezIsHex(s[i]))
			return false;
		v = (v << 4) | (unsigned int)ezHexValue(s[i]);
	}
	out = v;
	return true;
}

// Turns the encoding used inside the package into an absolute GBA address.
// Returns false for anything the format cannot express, so the caller can report
// it instead of writing to a nonsense location.
//
// `base` only matters for the "0x40000 + offset" form, which is the one that
// lost its bank; 0 means "use the kernel's reading", which is IWRAM.  `truncated`
// reports when that form was used, so the caller can count it.
static bool ezTranslateAddress(size_t digits, unsigned int raw, unsigned int base,
                               bool& truncated, unsigned int& out)
{
	truncated = false;
	if (digits >= 7)
	{
		unsigned int region = raw >> 24;
		if (region == 0x02 || region == 0x03)
		{
			out = raw;
			return true;
		}
		if (region != 0x00)
			return false;   // ROM / I/O / unmapped: the kernel cannot patch these either
		// "0001563C" and friends fall through to the encoded form
	}

	if (raw >= 0x40000)
	{
		// 0x40000 + offset is how the kernel spells IWRAM.  Offsets past 0x7FFF
		// do not fit there, and its mask would fold them somewhere else entirely,
		// so refuse instead of guessing.
		if (raw > 0x4FFFF)
			return false;

		truncated = true;

		if (base == 0)
		{
			out = 0x03000000u | (raw & 0x7FFFu);
			return true;
		}

		// The file only kept the low 16 bits, so `base` decides the bank and the
		// page.  IWRAM keeps the kernel's fold, EWRAM can address all 64 KB.
		const unsigned int mask = (base == 0x03000000u) ? 0x7FFFu : 0xFFFFu;
		out = base | (raw & mask);
		return true;
	}

	out = 0x02000000u | (raw & 0x3FFFFu);
	return true;
}

static void ezAddProblem(CheatDbFile& file, int line, CheatDbProblemKind kind, const std::string& text)
{
	CheatDbProblem p;
	p.line = line;
	p.kind = kind;
	p.text = text.size() > 64 ? text.substr(0, 64) : text;
	file.problems.push_back(p);
}

// The file format writes one byte per comma separated token and bumps the
// address for each of them, which is how the kernel reads it too.  Neighbouring
// bytes are then folded back into 16/32 bit writes so a fifteen-byte cheat does
// not eat fifteen of the hundred slots the cheat list has.
static void ezCoalesce(const std::vector<std::pair<unsigned int, unsigned char> >& bytes,
                       std::vector<CheatDbWrite>& out)
{
	size_t i = 0;
	while (i < bytes.size())
	{
		unsigned int start = bytes[i].first;
		size_t run = 1;
		while (i + run < bytes.size() && bytes[i + run].first == start + (unsigned int)run)
			run++;

		size_t done = 0;
		while (done < run)
		{
			unsigned int address = bytes[i + done].first;
			int size = 1;
			if (run - done >= 4 && (address & 3) == 0)
				size = 4;
			else if (run - done >= 2 && (address & 1) == 0)
				size = 2;

			unsigned int value = 0;
			for (int b = 0; b < size; b++)
				value |= (unsigned int)bytes[i + done + b].second << (8 * b);

			CheatDbWrite w;
			w.address = address;
			w.value = value;
			w.size = size;
			out.push_back(w);
			done += (size_t)size;
		}
		i += run;
	}
}

static void ezParseValue(const std::string& value, int line, unsigned int base,
                         std::vector<CheatDbWrite>& writes, CheatDbFile& file)
{
	std::vector<std::pair<unsigned int, unsigned char> > bytes;

	size_t pos = 0;
	while (pos <= value.size())
	{
		size_t sep = value.find(';', pos);
		std::string group = (sep == std::string::npos)
		                        ? value.substr(pos)
		                        : value.substr(pos, sep - pos);
		pos = (sep == std::string::npos) ? value.size() + 1 : sep + 1;

		group = ezTrim(group);
		if (group.empty())
			continue;   // tolerated: repeated or trailing ';'

		std::vector<std::string> tokens;
		size_t t = 0;
		for (;;)
		{
			size_t comma = group.find(',', t);
			if (comma == std::string::npos)
			{
				tokens.push_back(ezTrim(group.substr(t)));
				break;
			}
			tokens.push_back(ezTrim(group.substr(t, comma - t)));
			t = comma + 1;
		}

		if (tokens.size() < 2)
		{
			ezAddProblem(file, line, CHEAT_DB_PROBLEM_VALUE, group);
			continue;
		}

		std::string addressToken = tokens[0];
		if (addressToken.size() > 2 && addressToken[0] == '0' &&
		    (addressToken[1] == 'x' || addressToken[1] == 'X'))
			addressToken = addressToken.substr(2);

		unsigned int raw = 0;
		unsigned int address = 0;
		bool         truncated = false;
		if (!ezParseHex(addressToken, 8, raw))
		{
			ezAddProblem(file, line, CHEAT_DB_PROBLEM_ADDRESS, tokens[0]);
			continue;
		}
		if (!ezTranslateAddress(addressToken.size(), raw, base, truncated, address))
		{
			ezAddProblem(file, line, CHEAT_DB_PROBLEM_ADDRESS, tokens[0]);
			continue;
		}

		// The file kept only the low half of this address, so which bank it means
		// had to be assumed.  Counted for the UI, which offers another answer.
		if (truncated)
			file.truncatedAddresses++;

		unsigned int cursor = address;
		for (size_t i = 1; i < tokens.size(); i++)
		{
			const std::string& token = tokens[i];
			unsigned int v = 0;

			// One or two digits is a single byte, which is how nearly every line
			// in the package is written.  Wider tokens are zero padded on the
			// left and taken as one little endian number ("1EF" is 0x01EF, two
			// bytes), which is the only reading that keeps such a line usable.
			int count = 0;
			if (token.size() >= 1 && token.size() <= 8)
				count = (int)((token.size() + 1) / 2);

			if (count == 0 || !ezParseHex(token, 8, v))
			{
				ezAddProblem(file, line, CHEAT_DB_PROBLEM_VALUE, token);
				continue;
			}

			if (bytes.size() + (size_t)count > EZ_MAX_WRITES)
			{
				ezAddProblem(file, line, CHEAT_DB_PROBLEM_LIMIT, tokens[0]);
				break;
			}

			for (int b = 0; b < count; b++)
			{
				bytes.push_back(std::make_pair(cursor, (unsigned char)((v >> (8 * b)) & 0xFF)));
				cursor++;
			}
		}
	}

	ezCoalesce(bytes, writes);
}

bool EzParseChtText(const char* text, size_t length, CheatDbFile& file, unsigned int base)
{
	file.title.clear();
	file.gameName.clear();
	file.system.clear();
	file.entries.clear();
	file.problems.clear();
	file.truncatedAddresses = 0;

	if (!text)
		return false;

	// A UTF-8 BOM would otherwise glue itself to the first section header.
	if (length >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
	    (unsigned char)text[2] == 0xBF)
	{
		text += 3;
		length -= 3;
	}

	std::string section;
	size_t      sectionFirst = 0;
	bool        inGameInfo = false;
	bool        inBlockComment = false;
	bool        haveTitle = false;
	bool        stopped = false;
	int         line = 0;

	size_t pos = 0;
	while (pos < length)
	{
		size_t start = pos;
		while (pos < length && text[pos] != '\n' && text[pos] != '\r')
			pos++;

		std::string raw(text + start, pos - start);
		line++;

		if (pos < length)
		{
			if (text[pos] == '\r')
			{
				pos++;
				if (pos < length && text[pos] == '\n')
					pos++;
			}
			else
			{
				pos++;
			}
		}

		if (stopped)
			break;
		if (raw.size() > EZ_MAX_LINE_SIZE)
		{
			ezAddProblem(file, line, CHEAT_DB_PROBLEM_LIMIT, raw.substr(0, 40));
			continue;
		}

		std::string text2 = ezTrim(raw);
		if (text2.empty())
			continue;

		if (inBlockComment)
		{
			if (text2.find("*/") != std::string::npos)
				inBlockComment = false;
			continue;
		}
		if (text2[0] == '#' || (text2.size() > 1 && text2[0] == '/' && text2[1] == '/'))
			continue;
		if (text2.size() > 1 && text2[0] == '/' && text2[1] == '*')
		{
			if (text2.find("*/", 2) == std::string::npos)
				inBlockComment = true;
			continue;
		}

		if (text2[0] == '[')
		{
			if (text2[text2.size() - 1] != ']')
			{
				ezAddProblem(file, line, CHEAT_DB_PROBLEM_SECTION, text2);
				continue;
			}
			if (file.entries.size() - sectionFirst > 1)
			{
				for (size_t i = sectionFirst; i < file.entries.size(); i++)
					file.entries[i].useKeyName = true;
			}
			section = ezSanitize(ezTrim(text2.substr(1, text2.size() - 2)));
			inGameInfo = ezEqualsNoCase(section.c_str(), "GameInfo");
			sectionFirst = file.entries.size();
			continue;
		}

		size_t equal = text2.find('=');
		if (equal == std::string::npos)
		{
			if (!haveTitle)
			{
				file.title = ezSanitize(text2);
				haveTitle = true;
			}
			else
			{
				ezAddProblem(file, line, CHEAT_DB_PROBLEM_LINE, text2);
			}
			continue;
		}

		std::string key = ezTrim(text2.substr(0, equal));
		std::string value = ezTrim(text2.substr(equal + 1));
		if (key.empty())
		{
			ezAddProblem(file, line, CHEAT_DB_PROBLEM_LINE, text2);
			continue;
		}

		if (inGameInfo)
		{
			if (ezEqualsNoCase(key.c_str(), "Name"))
				file.gameName = ezSanitize(value);
			else if (ezEqualsNoCase(key.c_str(), "System"))
				file.system = ezSanitize(value);
			else if (ezEqualsNoCase(key.c_str(), "Text") && file.gameName.empty())
				file.gameName = ezSanitize(value);
			continue;
		}

		if (section.empty())
		{
			// An entry outside any section has no name to show, so it cannot be
			// offered to the user even though its codes may be fine.
			ezAddProblem(file, line, CHEAT_DB_PROBLEM_LINE, text2);
			continue;
		}

		if (file.entries.size() >= EZ_MAX_ENTRIES)
		{
			ezAddProblem(file, line, CHEAT_DB_PROBLEM_LIMIT, text2);
			stopped = true;
			continue;
		}

		CheatDbEntry entry;
		entry.section = section;
		entry.key = ezSanitize(key);
		entry.useKeyName = false;
		ezParseValue(value, line, base, entry.writes, file);

		if (entry.writes.empty())
		{
			// ezParseValue already recorded why; keep one marker per dropped
			// entry so the details list accounts for it.
			if (file.problems.empty() || file.problems.back().line != line)
				ezAddProblem(file, line, CHEAT_DB_PROBLEM_EMPTY, key);
			continue;
		}

		file.entries.push_back(entry);
	}

	if (file.entries.size() - sectionFirst > 1 && !file.entries.empty())
	{
		for (size_t i = sectionFirst; i < file.entries.size(); i++)
			file.entries[i].useKeyName = true;
	}

	return !file.entries.empty();
}

EzCheatDb::EzCheatDb()
    : m_open(false), m_isDir(false), m_zip(NULL), m_indexSkipped(0)
{
}

EzCheatDb::~EzCheatDb()
{
	Close();
}

void EzCheatDb::Close()
{
	if (m_zip)
	{
		delete (ZipArchive*)m_zip;
		m_zip = NULL;
	}
	m_open = false;
	m_isDir = false;
	m_root.clear();
	m_dirPath.clear();
	m_names.clear();
	m_languages.clear();
	m_records.clear();
	m_lastText.clear();
	m_indexSkipped = 0;
}

int EzCheatDb::FindEntry(const std::string& name) const
{
	std::string wanted = ezLower(name);
	for (size_t i = 0; i < m_names.size(); i++)
	{
		if (m_names[i] == wanted)
			return (int)i;
	}
	return -1;
}

bool EzCheatDb::ReadEntry(const std::string& name, std::string& out, CheatDbError& error,
                          std::string& detail)
{
	out.clear();
	detail = name;

	if (m_isDir)
	{
		std::string full = m_dirPath + "/" + name;
		FILE* f = fopen(full.c_str(), "rb");
		if (!f)
		{
			error = CHEAT_DB_ERR_FILE;
			return false;
		}
		if (fseek(f, 0, SEEK_END) != 0)
		{
			fclose(f);
			error = CHEAT_DB_ERR_FILE;
			return false;
		}
		long size = ftell(f);
		rewind(f);
		if (size <= 0 || size > EZ_MAX_ENTRY_SIZE)
		{
			fclose(f);
			error = CHEAT_DB_ERR_ENTRY;
			return false;
		}
		out.resize((size_t)size);
		size_t got = fread(&out[0], 1, (size_t)size, f);
		fclose(f);
		if (got != (size_t)size)
		{
			out.clear();
			error = CHEAT_DB_ERR_ENTRY;
			return false;
		}
		error = CHEAT_DB_ERR_NONE;
		return true;
	}

	int index = FindEntry(name);
	if (index < 0)
	{
		error = CHEAT_DB_ERR_FILE;
		return false;
	}

	ZipEntryInfo info;
	if (!((ZipArchive*)m_zip)->GetEntryInfo(index, &info))
	{
		error = CHEAT_DB_ERR_ENTRY;
		return false;
	}
	if (info.uncompressedSize == 0 || info.uncompressedSize > EZ_MAX_ENTRY_SIZE)
	{
		error = CHEAT_DB_ERR_ENTRY;
		return false;
	}

	out.resize(info.uncompressedSize);
	int got = ((ZipArchive*)m_zip)->ExtractEntry(index, &out[0], info.uncompressedSize);
	if (got != (int)info.uncompressedSize)
	{
		out.clear();
		error = CHEAT_DB_ERR_ENTRY;
		return false;
	}
	error = CHEAT_DB_ERR_NONE;
	return true;
}

bool EzCheatDb::LoadIndex(CheatDbError& error, std::string& detail)
{
	std::string indexName = m_root + "GameID2cht.bin";
	std::string data;
	if (!ReadEntry(indexName, data, error, detail))
	{
		if (error == CHEAT_DB_ERR_FILE)
			error = CHEAT_DB_ERR_INDEX;
		return false;
	}

	if (data.size() < 8)
	{
		error = CHEAT_DB_ERR_INDEX_DATA;
		detail = indexName;
		return false;
	}

	for (size_t pos = 0; pos + 8 <= data.size(); pos += 8)
	{
		bool ok = true;
		for (size_t i = 0; i < 4; i++)
		{
			char c = data[pos + i];
			if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
				ok = false;
		}
		for (size_t i = 4; i < 8; i++)
		{
			char c = data[pos + i];
			if (!((c >= '0' && c <= '9') || (c == 'x' || c == 'X') || (c >= 'a' && c <= 'f') ||
			      (c >= 'A' && c <= 'F')))
				ok = false;
		}
		if (!ok)
		{
			m_indexSkipped++;
			continue;
		}

		Record record;
		std::string code = data.substr(pos, 4);
		for (size_t i = 0; i < code.size(); i++)
		{
			if (code[i] >= 'a' && code[i] <= 'z')
				code[i] = (char)(code[i] - 'a' + 'A');
		}
		record.code = code;
		record.id = ezLower(data.substr(pos + 4, 4));
		m_records.push_back(record);
	}

	if (m_records.empty())
	{
		error = CHEAT_DB_ERR_INDEX_DATA;
		detail = indexName;
		return false;
	}
	return true;
}

bool EzCheatDb::Open(const char* path, CheatDbError& error, std::string& detail)
{
	Close();
	error = CHEAT_DB_ERR_NONE;
	detail.clear();

	if (!path || !*path)
	{
		error = CHEAT_DB_ERR_FILE;
		return false;
	}

	if (ezIsDirectory(path))
	{
		m_isDir = true;
		m_dirPath = path;
		// Directory mode cannot list its own tree without platform APIs, so it
		// uses the language directory the package ships with.
		m_languages.push_back("Eng");
		{
			std::string probe = m_dirPath + "/Eng";
			if (!ezIsDirectory(probe.c_str()))
			{
				error = CHEAT_DB_ERR_ARCHIVE;
				detail = path;
				Close();
				return false;
			}
		}
	}
	else
	{
		ZipArchive* zip = new ZipArchive(path);
		if (!zip->IsValid())
		{
			delete zip;
			error = CHEAT_DB_ERR_ARCHIVE;
			detail = path;
			Close();
			return false;
		}
		m_zip = zip;

		int count = zip->GetEntryCount();
		if (count <= 0)
		{
			error = CHEAT_DB_ERR_ARCHIVE;
			detail = path;
			Close();
			return false;
		}

		std::string indexEntry;
		for (int i = 0; i < count; i++)
		{
			ZipEntryInfo info;
			if (!zip->GetEntryInfo(i, &info))
				continue;
			m_names.push_back(ezLower(info.name));
			const std::string& lowered = m_names.back();
			if (indexEntry.empty() && lowered.size() >= 14 &&
			    lowered.compare(lowered.size() - 14, 14, "gameid2cht.bin") == 0)
				indexEntry = info.name;
		}

		if (indexEntry.empty())
		{
			error = CHEAT_DB_ERR_INDEX;
			detail = path;
			Close();
			return false;
		}

		size_t slash = indexEntry.find_last_of('/');
		m_root = (slash == std::string::npos) ? std::string() : indexEntry.substr(0, slash + 1);

		// Collect the language directories that actually hold cheat files so a
		// repackaged tree (Eng/Chs/...) still resolves; Eng wins ties.
		for (size_t i = 0; i < m_names.size(); i++)
		{
			const std::string& name = m_names[i];
			if (name.compare(0, m_root.size(), m_root) != 0)
				continue;
			if (name.size() < 5 || name.compare(name.size() - 4, 4, ".cht") != 0)
				continue;
			std::string rest = name.substr(m_root.size());
			size_t cut = rest.find('/');
			if (cut == std::string::npos)
				continue;
			std::string language = rest.substr(0, cut);
			bool known = false;
			for (size_t j = 0; j < m_languages.size(); j++)
			{
				if (m_languages[j] == language)
					known = true;
			}
			if (!known)
				m_languages.push_back(language);
		}
		for (size_t i = 0; i < m_languages.size(); i++)
		{
			if (m_languages[i] == "eng" && i != 0)
			{
				std::string eng = m_languages[i];
				m_languages.erase(m_languages.begin() + i);
				m_languages.insert(m_languages.begin(), eng);
				break;
			}
		}
	}

	if (!LoadIndex(error, detail))
	{
		Close();
		return false;
	}

	m_open = true;
	return true;
}

void EzCheatDb::BuildChtPath(const std::string& id, size_t language, std::string& path) const
{
	// The package numbers its files in groups of two hundred:
	//   Eng/<bucket>/<id>.cht,  bucket = id - id % 200
	// A leading 'x' marks the second batch of ids and shortens both numbers
	// ("x454" lives in "x400/").
	const char* languageName = (language < m_languages.size()) ? m_languages[language].c_str() : "eng";
	const bool  secondary = (id.size() == 4 && id[0] == 'x');
	const char* digits = secondary ? id.c_str() + 1 : id.c_str();

	// Ids are decimal in the shipped package; a repackaged index sometimes uses
	// hex, so fall back to that when a letter shows up.
	int  base = 10;
	for (const char* p = digits; *p; p++)
	{
		if (!((*p >= '0' && *p <= '9')))
			base = 16;
	}
	long value = strtol(digits, NULL, base);
	if (value < 0)
		value = 0;
	long bucket = value - (value % 200);

	char buffer[32];
	if (secondary)
		snprintf(buffer, sizeof(buffer), "x%03ld", bucket);
	else
		snprintf(buffer, sizeof(buffer), "%04ld", bucket);

	path = m_root;
	path += languageName;
	path += "/";
	path += buffer;
	path += "/";
	path += id;
	path += ".cht";
}

const char* EzCheatDb::RecordCode(int index) const
{
	if (index < 0 || index >= (int)m_records.size())
		return "";
	return m_records[index].code.c_str();
}

CheatDbResult EzCheatDb::LoadByRecord(int index, CheatDbFile& file, CheatDbError& error,
                                   std::string& detail)
{
	file = CheatDbFile();
	error = CHEAT_DB_ERR_NONE;
	detail.clear();

	if (!m_open || index < 0 || index >= (int)m_records.size())
	{
		error = CHEAT_DB_ERR_INDEX_DATA;
		return CHEAT_DB_ERROR;
	}

	const std::string& id = m_records[index].id;

	// Ids are unique, but the language tree is not known in advance (Eng, Chs,
	// ...), so every candidate directory is tried until one holds the file.
	CheatDbError   corruptError = CHEAT_DB_ERR_NONE;
	std::string corruptPath;
	for (size_t language = 0; language < m_languages.size(); language++)
	{
		std::string path;
		BuildChtPath(id, language, path);

		std::string text;
		CheatDbError   entryError = CHEAT_DB_ERR_NONE;
		std::string entryDetail;
		if (!ReadEntry(path, text, entryError, entryDetail))
		{
			if (entryError == CHEAT_DB_ERR_ENTRY)
			{
				if (corruptError == CHEAT_DB_ERR_NONE)
					corruptPath = path;
				corruptError = CHEAT_DB_ERR_GAME_FILE;
			}
			continue;
		}

		// Kept so the UI can re-read the same file with another memory base.
		m_lastText = text;
		if (!EzParseChtText(text.data(), text.size(), file))
		{
			detail = path;
			return CHEAT_DB_EMPTY;
		}
		return CHEAT_DB_OK;
	}

	if (corruptError != CHEAT_DB_ERR_NONE)
	{
		error = corruptError;
		detail = corruptPath;
		return CHEAT_DB_ERROR;
	}

	detail = m_records[index].code + " -> " + id;
	return CHEAT_DB_NO_CHEATS;
}

CheatDbResult EzCheatDb::LoadForGameCode(const char gameCode[4], CheatDbFile& file,
                                      CheatDbError& error, std::string& detail)
{
	file = CheatDbFile();
	error = CHEAT_DB_ERR_NONE;
	detail.clear();

	if (!m_open)
	{
		error = CHEAT_DB_ERR_ARCHIVE;
		return CHEAT_DB_ERROR;
	}
	if (!gameCode)
	{
		detail = "?";
		return CHEAT_DB_NO_CHEATS;
	}

	char wanted[5] = { 0, 0, 0, 0, 0 };
	for (int i = 0; i < 4; i++)
	{
		char c = gameCode[i];
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		wanted[i] = c;
	}

	CheatDbResult last = CHEAT_DB_NO_CHEATS;
	for (size_t i = 0; i < m_records.size(); i++)
	{
		if (m_records[i].code != wanted)
			continue;

		CheatDbFile candidate;
		CheatDbError   candidateError = CHEAT_DB_ERR_NONE;
		std::string candidateDetail;
		CheatDbResult  result = LoadByRecord((int)i, candidate, candidateError, candidateDetail);
		if (result == CHEAT_DB_OK)
		{
			file = candidate;
			return CHEAT_DB_OK;
		}
		if (result == CHEAT_DB_EMPTY)
		{
			// Keep the first broken file around: an empty file is still more
			// informative than "not in the database".
			file = candidate;
			detail = candidateDetail;
			last = CHEAT_DB_EMPTY;
		}
		else if (result == CHEAT_DB_ERROR)
		{
			error = candidateError;
			detail = candidateDetail;
			last = CHEAT_DB_ERROR;
		}
	}

	if (last == CHEAT_DB_NO_CHEATS)
		detail = wanted;
	return last;
}

CheatDbResult EzCheatDb::ReloadWithBase(unsigned int base, CheatDbFile& file)
{
	if (m_lastText.empty())
		return CHEAT_DB_ERROR;

	CheatDbFile parsed;
	if (!EzParseChtText(m_lastText.data(), m_lastText.size(), parsed, base))
		return CHEAT_DB_EMPTY;

	file = parsed;
	return CHEAT_DB_OK;
}

EzLoneChtResult EzReadLoneCht(const char* path, std::string& text, int& count,
                              CheatDbError& error, std::string& detail)
{
	text.clear();
	count = 0;
	error = CHEAT_DB_ERR_NONE;
	detail = path ? path : "";

	if (!path || !*path)
	{
		error = CHEAT_DB_ERR_FILE;
		return EZ_LONE_FAILED;
	}

	ZipArchive zip(path);
	if (!zip.IsValid())
	{
		error = CHEAT_DB_ERR_ARCHIVE;
		return EZ_LONE_FAILED;
	}

	// The package keeps thousands of files in nested folders; a repackaged tree
	// can put them anywhere, so only the extension is trusted here.
	int found = -1;
	const int entries = zip.GetEntryCount();
	for (int i = 0; i < entries; i++)
	{
		ZipEntryInfo info;
		if (!zip.GetEntryInfo(i, &info))
			continue;
		const size_t length = strlen(info.name);
		if (length < 5 || _stricmp(info.name + length - 4, ".cht") != 0)
			continue;
		if (found < 0)
			found = i;
		count++;
	}

	if (found < 0)
		return EZ_LONE_NONE;
	if (count > 1)
		return EZ_LONE_SEVERAL;

	ZipEntryInfo info;
	if (!zip.GetEntryInfo(found, &info) || info.uncompressedSize == 0 ||
	    info.uncompressedSize > EZ_MAX_ENTRY_SIZE)
	{
		error = CHEAT_DB_ERR_ENTRY;
		return EZ_LONE_FAILED;
	}

	text.resize(info.uncompressedSize);
	if (zip.ExtractEntry(found, &text[0], info.uncompressedSize) != (int)info.uncompressedSize)
	{
		text.clear();
		error = CHEAT_DB_ERR_ENTRY;
		return EZ_LONE_FAILED;
	}

	return EZ_LONE_READ;
}
