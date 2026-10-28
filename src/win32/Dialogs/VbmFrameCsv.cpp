// VbmFrameCsv.cpp : the CSV the VBM Editor puts on the clipboard.
//
// Plain string work on purpose - no MFC, no windows, no movie.  The editor
// calls parse()/serialize(), and the probe calls exactly the same functions.
// See VbmFrameCsv.h for the format and the limits.

#include "VbmFrameCsv.h"

#include "../../common/inputGlobal.h"

namespace vbmcsv
{

namespace
{

// The columns, in the order serialize() writes them and the order the grid
// shows them.  frame carries no bit: it is there for the human reading the
// text, and parse() only checks that it is a number.
struct Column
{
	const wchar_t *name;
	uint16		   mask;
};

const Column kColumns[] = {
	{ L"frame", 0 },
	{ L"Left", BUTTON_MASK_LEFT },
	{ L"Up", BUTTON_MASK_UP },
	{ L"Right", BUTTON_MASK_RIGHT },
	{ L"Down", BUTTON_MASK_DOWN },
	{ L"A", BUTTON_MASK_A },
	{ L"B", BUTTON_MASK_B },
	{ L"L", BUTTON_MASK_L },
	{ L"R", BUTTON_MASK_R },
	{ L"Start", BUTTON_MASK_START },
	{ L"Select", BUTTON_MASK_SELECT },
};

const int kColumnCount = int(sizeof(kColumns) / sizeof(kColumns[0]));

bool isBlank(wchar_t c)
{
	return c == L' ' || c == L'\t';
}

bool isDigit(wchar_t c)
{
	return c >= L'0' && c <= L'9';
}

bool isLineBreak(wchar_t c)
{
	return c == L'\r' || c == L'\n';
}

std::wstring trim(const std::wstring &text)
{
	size_t first = 0;
	size_t last  = text.size();

	while (first < last && isBlank(text[first]))
		++first;
	while (last > first && isBlank(text[last - 1]))
		--last;

	return text.substr(first, last - first);
}

// ASCII case folding only: the column names are ASCII, and the clipboard may
// come from anything, so no locale is consulted anywhere in this file.
wchar_t lower(wchar_t c)
{
	if (c >= L'A' && c <= L'Z')
		return wchar_t(c - L'A' + L'a');
	return c;
}

bool sameName(const std::wstring &left, const wchar_t *right)
{
	size_t i = 0;
	for (; i < left.size(); ++i)
	{
		if (right[i] == 0 || lower(left[i]) != lower(right[i]))
			return false;
	}

	return right[i] == 0;
}

// Splits on commas.  No quoting: a field that contains a comma is a field count
// mismatch, which is what the format says should happen.
void splitFields(const std::wstring &line, std::vector<std::wstring> &fields)
{
	fields.clear();

	size_t start = 0;
	for (;;)
	{
		const size_t comma = line.find(L',', start);
		if (comma == std::wstring::npos)
		{
			fields.push_back(line.substr(start));
			return;
		}

		fields.push_back(line.substr(start, comma - start));
		start = comma + 1;
	}
}

// Non-negative decimal, no sign, no blanks, no overflow.  Hand-rolled so that
// "12abc" cannot sneak through the way %u would let it.
bool parseFrame(const std::wstring &field, uint32 &value)
{
	if (field.empty())
		return false;

	uint32 result = 0;
	for (size_t i = 0; i < field.size(); ++i)
	{
		if (!isDigit(field[i]))
			return false;

		const uint32 digit = uint32(field[i] - L'0');
		if (result > (0xFFFFFFFFu - digit) / 10)
			return false;

		result = result * 10 + digit;
	}

	value = result;
	return true;
}

// 0, 1 or nothing at all.
bool parseBit(const std::wstring &field, bool &set)
{
	if (field.empty())
	{
		set = false;
		return true;
	}

	if (field.size() != 1 || (field[0] != L'0' && field[0] != L'1'))
		return false;

	set = (field[0] == L'1');
	return true;
}

void appendUnsigned(std::wstring &text, uint32 value)
{
	wchar_t digits[12];
	int		count = 0;

	do
	{
		digits[count++] = wchar_t(L'0' + (value % 10));
		value /= 10;
	} while (value != 0);

	while (count > 0)
		text.push_back(digits[--count]);
}

// Walks the text one line at a time, accepting CRLF, LF and a lone CR.
class LineReader
{
public:
	explicit LineReader(const std::wstring &text) : m_text(text), m_pos(0)
	{
		// A file pasted through an editor arrives with a byte order mark; it
		// is not part of the first column name, so it is skipped here rather
		// than being trimmed off every field later.
		if (!m_text.empty() && m_text[0] == 0xFEFF)
			m_pos = 1;
	}

	bool next(std::wstring &line)
	{
		if (m_pos >= m_text.size())
			return false;

		size_t end = m_pos;
		while (end < m_text.size() && !isLineBreak(m_text[end]))
			++end;

		line = m_text.substr(m_pos, end - m_pos);

		if (end < m_text.size())
		{
			if (m_text[end] == L'\r' && end + 1 < m_text.size() && m_text[end + 1] == L'\n')
				m_pos = end + 2;
			else
				m_pos = end + 1;
		}
		else
			m_pos = end;

		return true;
	}

private:
	const std::wstring &m_text;
	size_t				m_pos;
};

} // namespace

Result parse(const std::wstring &text, std::vector<uint16> &buttons, int *badLine)
{
	buttons.clear();

	if (badLine)
		*badLine = 0;

	if (text.size() > size_t(MAX_CHARS))
		return Result::TooLarge;

	LineReader reader(text);

	std::wstring line;
	std::vector<std::wstring> fields;
	std::vector<int>		  mapping;	// header position -> index into kColumns
	int						  lineNumber = 0;

	while (reader.next(line))
	{
		++lineNumber;

		if (trim(line).empty())
			continue;	// leading, trailing and in-between blank lines

		splitFields(line, fields);

		// The first real line is the header.  Every name has to be one of the
		// eleven, exactly once each - that is what makes a paste predictable.
		if (mapping.empty())
		{
			if (int(fields.size()) != kColumnCount)
			{
				if (badLine)
					*badLine = lineNumber;
				return Result::BadHeader;
			}

			mapping.assign(fields.size(), -1);

			for (size_t i = 0; i < fields.size(); ++i)
			{
				const std::wstring name = trim(fields[i]);
				if (name.empty())
				{
					if (badLine)
						*badLine = lineNumber;
					return Result::BadHeader;
				}

				int found = -1;
				for (int c = 0; c < kColumnCount; ++c)
				{
					if (sameName(name, kColumns[c].name))
					{
						found = c;
						break;
					}
				}

				if (found < 0)
				{
					if (badLine)
						*badLine = lineNumber;
					return Result::BadHeader;
				}

				for (size_t j = 0; j < i; ++j)
				{
					if (mapping[j] == found)
					{
						if (badLine)
							*badLine = lineNumber;
						return Result::BadHeader;	// the same column twice
					}
				}

				mapping[i] = found;
			}

			continue;
		}

		if (fields.size() != mapping.size())
		{
			if (badLine)
				*badLine = lineNumber;
			return Result::BadFieldCount;
		}

		uint16 value = 0;
		for (size_t i = 0; i < fields.size(); ++i)
		{
			const std::wstring field = trim(fields[i]);

			if (kColumns[mapping[i]].mask == 0)
			{
				uint32 frame = 0;
				if (!parseFrame(field, frame))
				{
					if (badLine)
						*badLine = lineNumber;
					return Result::BadFrame;
				}
				continue;	// read for the reader's sake, never used
			}

			bool set = false;
			if (!parseBit(field, set))
			{
				if (badLine)
					*badLine = lineNumber;
				return Result::BadValue;
			}

			if (set)
				value = uint16(value | kColumns[mapping[i]].mask);
		}

		if (buttons.size() >= size_t(MAX_ROWS))
		{
			if (badLine)
				*badLine = lineNumber;
			return Result::TooManyRows;
		}

		buttons.push_back(value);
	}

	if (mapping.empty())
		return Result::Empty;

	if (buttons.empty())
		return Result::NoRows;

	return Result::Ok;
}

Result serialize(const std::vector<Row> &rows, std::wstring &text)
{
	text.clear();

	for (int i = 0; i < kColumnCount; ++i)
	{
		if (i)
			text.push_back(L',');
		text.append(kColumns[i].name);
	}
	text.append(L"\r\n");

	for (size_t i = 0; i < rows.size(); ++i)
	{
		appendUnsigned(text, rows[i].frame);

		const uint16 value = uint16(rows[i].buttons & BUTTON_REGULAR_MASK);
		for (int c = 0; c < kColumnCount; ++c)
		{
			if (kColumns[c].mask == 0)
				continue;	// frame, already written

			text.push_back(L',');
			text.push_back((value & kColumns[c].mask) ? L'1' : L'0');
		}

		text.append(L"\r\n");
	}

	return Result::Ok;
}

} // namespace vbmcsv
