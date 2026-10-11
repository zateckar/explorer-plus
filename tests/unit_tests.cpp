// Unit tests for the pure text handling: the settings INI document and the command
// line / typed path parsing. Run: ctest --test-dir build (or build\explorer-plus-tests.exe)
#include "ini.h"
#include "cmdline.h"
#include <cstdio>
#include <string>
#include <vector>

static int s_failed = 0, s_run = 0;

static std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 0x80 ? (char)c : '?';
    return s;
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) check_eq((a), (b), #a, __FILE__, __LINE__)

static void check(bool ok, const char* what, const char* file, int line) {
    ++s_run;
    if (ok) return;
    ++s_failed;
    std::printf("FAIL %s:%d: %s\n", file, line, what);
}

static void check_eq(const std::wstring& a, const std::wstring& b, const char* what, const char* file, int line) {
    ++s_run;
    if (a == b) return;
    ++s_failed;
    std::printf("FAIL %s:%d: %s\n  got:      \"%s\"\n  expected: \"%s\"\n", file, line, what,
                narrow(a).c_str(), narrow(b).c_str());
}

// ------------------------------------------------------------------
// INI document

static void ini_round_trip() {
    const std::wstring text =
        L"; written by hand\r\n"
        L"\r\n"
        L"[main]\r\n"
        L"view=1\r\n"
        L"; a comment between keys\r\n"
        L"hidden=0\r\n"
        L"[tabs]\r\n"
        L"tab0=C:\\Users\r\n";
    CHECK_EQ(ini_serialize(ini_parse(text)), text);
    // LF-only files (edited elsewhere) come back with CRLF, otherwise the same
    std::wstring lf = L"[a]\nk=v\n";
    CHECK_EQ(ini_serialize(ini_parse(lf)), L"[a]\r\nk=v\r\n");
}

static void ini_values() {
    IniDoc d = ini_parse(L"[ Main ]\r\n  Key  =  \"quoted value\"  \r\nempty=\r\n=novalue\r\n;k=comment\r\n");
    const std::wstring* v = d.get(L"main", L"KEY");       // sections and keys ignore case
    CHECK(v && *v == L"quoted value");
    v = d.get(L"main", L"empty");
    CHECK(v && v->empty());
    CHECK(d.get(L"main", L"") == nullptr);                 // "=novalue" isn't an entry
    CHECK(d.get(L"main", L";k") == nullptr);               // nor is a comment
    CHECK(d.get(L"other", L"key") == nullptr);
    CHECK_EQ(ini_trim(L" \t x y \t"), L"x y");
}

static void ini_set_erase() {
    IniDoc d = ini_parse(L"[s]\r\na=1\r\nb=2\r\n");
    d.set(L"s", L"A", L"3");                               // set again: moves to the end
    KeyValues kv = d.entries(L"s");
    CHECK(kv.size() == 2 && kv[0].first == L"b" && kv[1].first == L"A" && kv[1].second == L"3");
    d.erase(L"s", L"b");
    CHECK(d.entries(L"s").size() == 1);
    d.set(L"new", L"k", L"v");                             // a new section at the end
    CHECK_EQ(ini_serialize(d), L"[s]\r\nA=3\r\n[new]\r\nk=v\r\n");
    d.replace(L"s", { { L"x", L"1" }, { L"y", L"2" } });
    CHECK(d.entries(L"s").size() == 2 && *d.get(L"s", L"y") == L"2");
}

static void ini_keys() {
    const std::vector<std::wstring> keys = {
        L"C:\\Users\\me", L"D:\\Sta\x17E" L"en\xE9 = [x]; 50%", L" leading", L"trailing ", L"a|b",
        L"\"quoted\"", L"\\\\server\\share\\dir", L"tab\there", L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
    };
    for (auto& k : keys) {
        std::wstring e = ini_escape_key(k);
        CHECK(e.find_first_of(L"=;[]\"") == std::wstring::npos);
        CHECK(e.empty() || (e.front() != L' ' && e.back() != L' '));
        CHECK_EQ(ini_unescape_key(e), k);
        // and through a file: written, parsed, the same key again
        IniDoc d;
        d.set(L"views", e, L"1");
        IniDoc back = ini_parse(ini_serialize(d));
        KeyValues kv = back.entries(L"views");
        CHECK(kv.size() == 1 && ini_unescape_key(kv[0].first) == k);
    }
    CHECK_EQ(ini_escape_key(L"C:\\x"), L"C:|x");           // backslashes stay readable
    CHECK_EQ(ini_unescape_key(L"C:|x|y"), L"C:\\x\\y");    // legacy files
    CHECK_EQ(ini_unescape_key(L"100%"), L"100%");          // a lone % stays
}

static void ini_decoding() {
    std::wstring want = L"[s]\r\nk=Sta\x17E" L"en\xE9\r\n";
    std::string utf16 = "\xFF\xFE";
    utf16.append(reinterpret_cast<const char*>(want.data()), want.size() * sizeof(wchar_t));
    CHECK_EQ(ini_decode(utf16), want);
    std::string utf8 = "[s]\r\nk=Sta\xC5\xBE" "en\xC3\xA9\r\n";
    CHECK_EQ(ini_decode("\xEF\xBB\xBF" + utf8), want);     // UTF-8 with a BOM
    CHECK_EQ(ini_decode(utf8), want);                      // and without
    CHECK(!ini_decode("k=\xE9t\xE9").empty());             // not UTF-8: read as ANSI
    CHECK(ini_decode("").empty());
    CHECK(ini_decode("\xFF\xFE").empty());
}

// ------------------------------------------------------------------
// command line and typed paths

static void command_line() {
    CommandLine c = parse_command_line(L"explorer-plus.exe \"D:\\\"");
    CHECK_EQ(c.path, L"D:\\");                             // arrives as D:" from a "%1" verb
    CHECK(!c.newWindow);
    c = parse_command_line(L"\"C:\\Program Files\\ep\\explorer-plus.exe\" --new-window \"C:\\Program Files\"");
    CHECK(c.newWindow);
    CHECK_EQ(c.path, L"C:\\Program Files");
    c = parse_command_line(L"explorer-plus.exe C:\\My Folder\\sub");
    CHECK_EQ(c.path, L"C:\\My Folder\\sub");               // unquoted, with spaces
    c = parse_command_line(L"explorer-plus.exe --NEW-WINDOW");
    CHECK(c.newWindow && c.path.empty());
    c = parse_command_line(L"explorer-plus.exe");
    CHECK(c.path.empty() && !c.newWindow);
    c = parse_command_line(L"");
    CHECK(c.path.empty());
}

static void command_line_round_trip() {
    // what "New window" starts must arrive as the folder it was made from
    const std::vector<std::wstring> paths = {
        L"C:\\", L"D:\\My Folder\\", L"\\\\server\\share", L"C:\\a\\b", L"::desktop",
        L"C:\\Users\\me\\Sta\x17E" L"en\xE9 = [x]; 50%", L"C:\\trailing space ",
    };
    for (auto& p : paths) {
        std::wstring line = quote_command_arg(L"C:\\Program Files\\ep\\explorer-plus.exe") + L" --new-window " +
                            quote_command_arg(p);
        CommandLine c = parse_command_line(line.c_str());
        CHECK(c.newWindow);
        CHECK_EQ(c.path, p);
    }
}

static void typed_text() {
    CHECK_EQ(typed_text_clean(L"  \"C:\\x y\"  "), L"C:\\x y");
    CHECK_EQ(typed_text_clean(L"\t..\\up "), L"..\\up");
    CHECK_EQ(typed_text_clean(L""), L"");
    CHECK_EQ(typed_text_clean(L"\"\""), L"");
    CHECK_EQ(typed_text_clean(L"\""), L"\"");              // a lone quote isn't a pair
    wchar_t root[MAX_PATH]{};
    GetEnvironmentVariableW(L"SystemRoot", root, MAX_PATH);
    CHECK_EQ(typed_text_clean(L"%SystemRoot%\\System32"), std::wstring(root) + L"\\System32");
    CHECK_EQ(typed_text_clean(L"%NO_SUCH_VARIABLE_EP%\\x"), L"%NO_SUCH_VARIABLE_EP%\\x");
}

int wmain() {
    ini_round_trip();
    ini_values();
    ini_set_erase();
    ini_keys();
    ini_decoding();
    command_line();
    command_line_round_trip();
    typed_text();
    std::printf("%d/%d checks passed\n", s_run - s_failed, s_run);
    return s_failed ? 1 : 0;
}
