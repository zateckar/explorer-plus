#pragma once
#include <string>
#include <vector>
#include <windows.h>

// Sync paths: '/'-separated, relative to a pair's root, NFC. Pure (unit-tested).
namespace syncpath {

std::wstring nfc(const std::wstring& s);                  // Unicode normalization form C
std::wstring case_key(const std::wstring& path);          // for case-insensitive comparison
// a name Windows can hold: no <>:"/\|?*, control characters, trailing dot or blank, nor a
// device name (CON, NUL, COM1, LPT1... with or without an extension)
bool windows_valid_name(const std::wstring& name);
bool windows_valid_path(const std::wstring& rel);
std::wstring to_local(const std::wstring& root, const std::wstring& rel);   // \\?\-prefixed full path
// "Report (conflicted copy, PC-NAME, 2026-10-17 14-03).docx"
std::wstring conflict_name(const std::wstring& name, const std::wstring& device, const SYSTEMTIME& when);
bool is_conflict_copy(const std::wstring& name);

// .syncignore: a gitignore subset. Patterns with '*', '?', '**'; a leading '/' anchors at
// the root, a trailing '/' matches folders only, '!' re-includes, '#' comments.
class Ignore {
public:
    Ignore();                                             // the built-in rules only
    void add_lines(const std::wstring& text);
    bool ignored(const std::wstring& rel, bool isDir) const;
    void add_excluded_folder(const std::wstring& rel);    // selective sync
    const std::vector<std::wstring>& excluded() const { return m_excluded; }
private:
    struct Rule { std::wstring pat; bool neg = false, dirOnly = false, anchored = false; };
    std::vector<Rule> m_rules;
    std::vector<std::wstring> m_excluded;
};
bool glob_match(const std::wstring& pat, const std::wstring& s);   // '*' and '?' within a component; '**' across

} // namespace syncpath
