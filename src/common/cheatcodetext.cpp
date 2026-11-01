#include "cheatcodetext.h"

#include <cstring>

namespace
{

bool isSeparator(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '+';
}

char asciiUpper(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

} // namespace

void CheatSplitCodes(const char* text, std::vector<CheatCodeText>& out)
{
	out.clear();
	if (!text)
		return;

	std::string pending;
	const size_t length = strlen(text);

	size_t pos = 0;
	while (pos < length)
	{
		while (pos < length && isSeparator(text[pos]))
			pos++;
		if (pos >= length)
			break;

		size_t start = pos;
		while (pos < length && !isSeparator(text[pos]))
			pos++;

		std::string token(text + start, pos - start);
		for (size_t i = 0; i < token.size(); i++)
			token[i] = asciiUpper(token[i]);

		CheatCodeText code;
		code.v3 = false;

		if (token.size() == 16)
		{
			// A whole GameShark code in one token.
			code.text = token;
			code.gsa = true;
			out.push_back(code);
			continue;
		}
		if (token.size() == 12)
		{
			// A CodeBreaker code with the separating space missing.
			code.text = token.substr(0, 8) + " " + token.substr(8);
			code.gsa = false;
			out.push_back(code);
			continue;
		}
		if (pending.empty())
		{
			pending = token;
			continue;
		}

		if (token.size() == 4)
		{
			code.text = pending + " " + token;
			code.gsa = false;
		}
		else
		{
			code.text = pending + token;
			code.gsa = true;
			code.v3 = true;
		}
		pending.clear();
		out.push_back(code);
	}

	if (!pending.empty())
	{
		// An odd number of tokens: report the address that never got a value.
		CheatCodeText code;
		code.text = pending;
		code.gsa = false;
		code.v3 = false;
		out.push_back(code);
	}
}
