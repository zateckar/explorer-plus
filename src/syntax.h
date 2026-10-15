#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The Notepad's syntax colours and document formatting: pure (no windows), tested by
// unit_tests. Lexers run over whole lines and are resumable: each returns a state to start
// the next lines from, so the Notepad lexes a big file in pieces and only as far as is shown.

enum class Lang : uint8_t {
    None, C, CSharp, Java, JavaScript, TypeScript, Go, Rust, Python, Ruby, Lua, PowerShell, Batch, Shell,
    Sql, Css, Json, Xml, Html, Markdown, Ini, Toml, Yaml, Diff, Count
};

enum class Tok : uint8_t {
    Plain, Comment, String, Number, Keyword, Type, Function, Preproc, Tag, Attr, Key, Heading, Variable,
    Literal, Emphasis, Link, Added, Removed, Count
};

struct SyntaxSpan {
    uint32_t start, len;
    Tok tok;
};

const wchar_t* lang_name(Lang l);                   // shown: "C / C++", "Plain text"
const wchar_t* lang_key(Lang l);                    // kept in the session: "c", "none"
Lang lang_from_key(const std::wstring& key);        // (unknown: None)
Lang lang_for_file(const std::wstring& path);       // by its extension or name

// Lexes s[0, n), whole lines (ended by '\r' or '\n'), from `state` (0: the text's start);
// adds the coloured pieces (positions in s, in order) to *out when given. Returns the state
// at the end.
int syntax_lex(Lang l, const wchar_t* s, size_t n, int state, std::vector<SyntaxSpan>* out);

// Formatting: indented four spaces, lines ended by "\r\n". False: not valid, *errorAt the
// position in `in` where that showed.
bool format_json(const std::wstring& in, bool compact, std::wstring& out, size_t* errorAt);
bool format_xml(const std::wstring& in, std::wstring& out, size_t* errorAt);
