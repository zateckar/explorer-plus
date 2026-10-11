#pragma once
#include <windows.h>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

// The settings file's INI document (settings.cpp has the file access): sections and
// lines in file order, comments kept. Pure text handling, unit-tested (tests/).

bool ini_ieq(const std::wstring& a, const std::wstring& b);     // equal ignoring case
std::wstring ini_trim(const std::wstring& s);                    // blanks and tabs off both ends

using KeyValues = std::vector<std::pair<std::wstring, std::wstring>>;

struct IniLine {
    bool entry = false;            // key=value (else a comment / blank line, kept as `raw`)
    std::wstring key, value, raw;
};
struct IniSection {
    std::wstring name;             // "" for lines before the first header
    std::vector<IniLine> lines;
};

struct IniDoc {
    std::vector<IniSection> sections;

    IniSection* find(const std::wstring& name, bool create = false) {
        for (auto& s : sections) if (!s.name.empty() && ini_ieq(s.name, name)) return &s;
        if (!create) return nullptr;
        sections.push_back({ name, {} });
        return &sections.back();
    }
    const IniSection* find(const std::wstring& name) const {
        return const_cast<IniDoc*>(this)->find(name, false);
    }
    const std::wstring* get(const std::wstring& sec, const std::wstring& key) const {
        if (const IniSection* s = find(sec))
            for (auto& l : s->lines) if (l.entry && ini_ieq(l.key, key)) return &l.value;
        return nullptr;
    }
    // a key set again moves to the end of its section: the order is "least recently
    // written first", which is what pruning goes by
    void set(const std::wstring& sec, const std::wstring& key, const std::wstring& value) {
        IniSection* s = find(sec, true);
        erase_in(s, key);
        IniLine l;
        l.entry = true; l.key = key; l.value = value;
        s->lines.push_back(std::move(l));
    }
    void erase(const std::wstring& sec, const std::wstring& key) {
        if (IniSection* s = find(sec)) erase_in(s, key);
    }
    KeyValues entries(const std::wstring& sec) const {
        KeyValues out;
        if (const IniSection* s = find(sec))
            for (auto& l : s->lines) if (l.entry) out.push_back({ l.key, l.value });
        return out;
    }
    void replace(const std::wstring& sec, const KeyValues& kv) {
        IniSection* s = find(sec, true);
        s->lines.clear();
        for (auto& [k, v] : kv) { IniLine l; l.entry = true; l.key = k; l.value = v; s->lines.push_back(std::move(l)); }
    }
private:
    static void erase_in(IniSection* s, const std::wstring& key) {
        s->lines.erase(std::remove_if(s->lines.begin(), s->lines.end(),
                                      [&](const IniLine& l) { return l.entry && ini_ieq(l.key, key); }),
                       s->lines.end());
    }
};

// UTF-16LE (with BOM, as written here), UTF-8 (a file edited in Notepad) or ANSI (very old)
std::wstring ini_decode(const std::string& raw);
IniDoc ini_parse(const std::wstring& text);
std::wstring ini_serialize(const IniDoc& d);                     // CRLF lines

// INI keys cannot hold '=', ';', '[', ']' or leading/trailing blanks; '\' is
// stored as '|' (illegal in paths, keeps legacy files readable), the rest as %XX.
std::wstring ini_escape_key(const std::wstring& k);
std::wstring ini_unescape_key(const std::wstring& k);
