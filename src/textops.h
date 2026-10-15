#pragma once
#include <string>

// The Notepad's text operations: pure (Windows' string functions aside), tested by unit_tests.
// Lines are as the text box has them, ended by '\r' (a "\r\n" or a '\n' counts as one too);
// what comes back has '\r' between lines.

enum class LineOp { SortAsc, SortDesc, Unique, RemoveEmpty, TrimTrailing, Reverse, Join };
std::wstring line_op(LineOp op, const std::wstring& lines);

enum class CaseOp { Upper, Lower, Title };
std::wstring change_case(CaseOp op, const std::wstring& s);

// Base64 and URL (percent) coding of the text as UTF-8; decoding: false when it isn't coded
// text (not Base64, or not UTF-8 once decoded)
std::wstring base64_encode(const std::wstring& text);
bool base64_decode(const std::wstring& coded, std::wstring& text);
std::wstring url_encode(const std::wstring& text);              // all but A-Z a-z 0-9 - _ . ~
bool url_decode(const std::wstring& coded, std::wstring& text);
