#ifndef VBMFRAMECSV_H
#define VBMFRAMECSV_H

#pragma once

#include "../../Port.h"

#include <string>
#include <vector>

// The clipboard format of the VBM Editor: one CSV row per frame.
//
//   frame,Left,Up,Right,Down,A,B,L,R,Start,Select
//   120,0,0,0,0,0,0,0,0,0,0
//
// A user is expected to copy a block of frames, paste it into Notepad, edit it
// by hand and paste it back, so the format is deliberately boring: no quotes,
// no escapes, no commas inside a field.  The frame column is there so the text
// can be read on its own; paste ignores it.
//
// Nothing here touches MFC or the movie: parse() and serialize() are plain
// string work, which is what lets the probe exercise them without a ROM.
namespace vbmcsv
{

// Hard limits, checked before anything is allocated.  A whole TAS movie is
// 100 000-200 000 frames; half a million rows is already far past what anybody
// pastes by hand, and 16 M characters is the point where the clipboard stops
// being text and starts being a denial of service.
enum { MAX_ROWS = 500000, MAX_CHARS = 16 * 1024 * 1024 };

struct Row
{
	uint32 frame;	// for reading only - parse() never looks at it
	uint16 buttons; // the ten pad bits, see BUTTON_REGULAR_MASK
};

enum class Result
{
	Ok,
	Empty,			// nothing but blanks
	TooLarge,		// more than MAX_CHARS
	BadHeader,		// a column is missing, unknown or appears twice
	BadFieldCount,	// a data row does not have as many fields as the header
	BadFrame,		// the frame column is not a non-negative decimal integer
	BadValue,		// a button column is not 0, 1 or empty
	NoRows,			// a header and nothing else
	TooManyRows		// more than MAX_ROWS
};

// Reads the button columns into one uint16 per frame.  The frame column is
// validated and then thrown away, columns may be in any order, and the names
// are matched without regard to case or surrounding blanks.
//
// badLine, when given, receives the 1-based line number of the line that was
// refused, counted the way the text editor the user pasted from counts them
// (blank lines included).  It stays 0 when the text as a whole was refused.
// An error code alone would leave somebody hunting for one bad field in five
// hundred lines by hand.
Result parse(const std::wstring &text, std::vector<uint16> &buttons, int *badLine = 0);

// Writes the fixed header followed by one line per row, CRLF and a trailing
// newline so Notepad is happy.  Only the ten pad bits are written.
Result serialize(const std::vector<Row> &rows, std::wstring &text);

} // namespace vbmcsv

#endif // VBMFRAMECSV_H
