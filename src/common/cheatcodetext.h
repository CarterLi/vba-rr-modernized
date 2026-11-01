#ifndef CHEATCODETEXT_HEADER
#define CHEATCODETEXT_HEADER

// Splits the code text users paste (or that a cheat file stores) into the two
// forms the GBA cheat engine accepts:
//
//   "AAAAAAAA VVVV"        CodeBreaker, 13 characters, cheatsAddCBACode
//   "AAAAAAAABBBBBBBB"     GameShark,    16 characters, cheatsAddGSACode
//
// Tokens are separated by spaces, tabs, newlines or '+', because the databases
// use all of them: "8201328C+E0FF", "82006E5C FFFF", "C40F4772 00008401".
// An 8 digit token followed by a 4 digit one is CodeBreaker, followed by another
// 8 digit one it is GameShark.
//
// Validity is *not* judged here - only the engine knows which code types it can
// execute - so callers hand each result to cheatsValidateGSACode /
// cheatsValidateCBACode before using it.  Text that cannot be paired at all is
// still returned (as a CodeBreaker candidate) so pasted junk is reported instead
// of silently vanishing.

#include <string>
#include <vector>

struct CheatCodeText
{
	std::string text;   // already in one of the two forms above
	bool        gsa;    // true: GameShark form, false: CodeBreaker form
	bool        v3;     // GameShark v3, assembled from an address and a value
};

void CheatSplitCodes(const char* text, std::vector<CheatCodeText>& out);

#endif // CHEATCODETEXT_HEADER
