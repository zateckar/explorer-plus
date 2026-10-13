// Unit tests for the pure text handling: the settings INI document and the command
// line / typed path parsing. Run: ctest --test-dir build (or build\explorer-plus-tests.exe)
#include "ini.h"
#include "cmdline.h"
#include "playlist.h"
#include <algorithm>
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

// ------------------------------------------------------------------
// media player queue

static std::vector<std::wstring> names(int n, int from = 0) {
    std::vector<std::wstring> v;
    for (int i = 0; i < n; ++i) v.push_back(L"f" + std::to_wstring(from + i));
    return v;
}

static void queue_order() {
    PlayQueue q;
    CHECK(q.current() == -1 && q.next(false) == -1 && q.previous() == -1);
    q.replace(names(3));
    CHECK(q.current() == 0 && q.next(false) == 1 && q.previous() == -1);
    q.set_current(2);
    CHECK(q.next(true) == -1);                             // the end: done
    q.repeat = Repeat::All;
    CHECK(q.next(true) == 0);                              // wraps
    q.set_current(0);
    CHECK(q.previous() == 2);
    q.repeat = Repeat::One;
    CHECK(q.next(true) == 0);                              // ended by itself: again
    CHECK(q.next(false) == 1);                             // Next: the next one
    CHECK(q.append(names(2, 3)) == 3 && q.size() == 5 && q.current() == 0);
    q.set_current(7);
    CHECK(q.current() == -1);
    CHECK(q.next(false) == 0);                             // nothing current: the first
}

static void queue_remove() {
    PlayQueue q;
    q.replace(names(5));
    q.set_current(2);
    CHECK(!q.remove({ 0 }));                               // before the current one
    CHECK(q.current() == 1 && q.entries()[1].path == L"f2");
    CHECK(q.remove({ 1, 2 }));                             // the current one and the next
    CHECK(q.size() == 2 && q.current() == 1 && q.entries()[1].path == L"f4");
    CHECK(q.remove({ 1 }));                                // the last: none current
    CHECK(q.current() == -1 && q.size() == 1);
    CHECK(!q.remove({ 9, -1 }));                           // out of range: nothing
    q.clear();
    CHECK(q.empty() && q.current() == -1);
}

static void queue_shuffle() {
    PlayQueue q;
    q.seed(7);
    q.replace(names(20));
    q.set_current(5);
    q.set_shuffle(true);
    CHECK(q.current() == 5 && q.previous() == -1);         // the current one leads
    // every entry exactly once, following Next from the current one
    std::vector<bool> seen(20, false);
    int n = 0;
    for (int i = q.current(); i >= 0 && n < 25; i = q.next(false), ++n) {
        CHECK(!seen[(size_t)i]);
        seen[(size_t)i] = true;
        q.set_current(i);
    }
    CHECK(n == 20);
    // added while shuffled: still each one once, all after the current one
    q.set_current(5);
    q.append(names(5, 20));
    std::fill(seen.begin(), seen.end(), false);
    seen.resize(25, false);
    n = 0;
    for (int i = q.current(); i >= 0 && n < 30; i = q.next(false), ++n) {
        CHECK(!seen[(size_t)i]);
        seen[(size_t)i] = true;
        q.set_current(i);
    }
    CHECK(n == 25);
    q.set_current(3);
    q.set_shuffle(false);
    CHECK(q.current() == 3 && q.next(false) == 4 && q.previous() == 2);
    q.set_shuffle(true);
    q.remove({ 0, 1, 2 });                                 // removal keeps the order consistent
    CHECK(q.current() == 0);
    n = 0;
    for (int i = q.current(); i >= 0 && n < 30; i = q.next(false), ++n) q.set_current(i);
    CHECK(n == 22);
}

static void media_text() {
    CHECK_EQ(fmt_duration(-1), L"");
    CHECK_EQ(fmt_duration(0), L"0:00");
    CHECK_EQ(fmt_duration(1870000000LL), L"3:07");
    CHECK_EQ(fmt_duration(37230000000LL), L"1:02:03");
    CHECK(media_extension_excluded(L".M3U") && media_extension_excluded(L".mid"));
    CHECK(!media_extension_excluded(L".mp3"));
    CHECK(!media_file_playable(L"C:\\x\\notes.txt"));
    CHECK(!media_file_playable(L"C:\\x\\list.m3u"));
    CHECK(!media_file_playable(L"C:\\x\\noextension"));
    std::vector<std::wstring> paths = { L"C:\\a.mp3", L"\\\\srv\\share\\b c.mkv", L"D:\\Sta\x17E" L"en\xE9\\x.wav" };
    std::wstring packed = pack_paths(paths);
    auto back = unpack_paths(packed.data(), packed.size());
    CHECK(back == paths);
    CHECK(unpack_paths(L"C:\\a\0\0C:\\b", 10).size() == 2);   // empty parts dropped, no final NUL
    CHECK(unpack_paths(nullptr, 5).empty());
}

int wmain() {
    queue_order();
    queue_remove();
    queue_shuffle();
    media_text();
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
