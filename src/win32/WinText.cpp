#include "stdafx.h"
#include "WinText.h"
#include "WinResUtil.h"

#include <atlconv.h>

CString winWideFromAnsi(const char *text)
{
	if (text == NULL)
		return CString();

	return CString(CA2W(text, CP_ACP));
}

CString winWideFromAnsi(const std::string &text)
{
	return winWideFromAnsi(text.c_str());
}

CString winWideFromUtf8OrAnsi(const char *text)
{
	if (text == NULL)
		return CString();

	const int length = (int)strlen(text);

	// MB_ERR_INVALID_CHARS makes the call fail instead of substituting U+FFFD,
	// which is what tells bytes that are not UTF-8 apart from ones that are.
	int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, length, NULL, 0);
	if (wideLength <= 0)
		return winWideFromAnsi(text);

	CString result;
	LPTSTR buffer = result.GetBuffer(wideLength);
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, length, buffer, wideLength);
	result.ReleaseBuffer(wideLength);

	return result;
}

CStringA winAnsiFromWide(LPCTSTR text)
{
	if (text == NULL)
		return CStringA();

	return CStringA(CW2A(text, CP_ACP));
}

CStringA winAnsiFromWide(const CString &text)
{
	return winAnsiFromWide(text.GetString());
}
