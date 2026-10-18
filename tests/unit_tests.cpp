// Unit tests for the pure text handling: the settings INI document and the command
// line / typed path parsing. Run: ctest --test-dir build (or build\explorer-plus-tests.exe)
#include "ini.h"
#include "cmdline.h"
#include "playlist.h"
#include "mediadata.h"
#include "imageformats.h"
#include "shapes.h"
#include "userchoice.h"
#include "textfile.h"
#include "syntax.h"
#include "textops.h"
#include <algorithm>
#include <cmath>
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
    // a media file opened by its file type's verb: the player alone
    c = parse_command_line(L"explorer-plus.exe --play \"D:\\Music\\a b.mp3\"");
    CHECK(c.play && !c.newWindow && !c.notepad);
    CHECK_EQ(c.path, L"D:\\Music\\a b.mp3");
    c = parse_command_line(L"explorer-plus.exe --notepad \"D:\\Notes\\to do.txt\"");
    CHECK(c.notepad && !c.play);
    CHECK_EQ(c.path, L"D:\\Notes\\to do.txt");
    c = parse_command_line(L"explorer-plus.exe --notepad");
    CHECK(c.notepad && c.path.empty());
}

// ------------------------------------------------------------------
// the Notepad's files and remembered tabs

static void text_files() {
    // encodings: by the BOM, by zero bytes (UTF-16 without one), UTF-8 where valid, else ANSI
    TextFile f = text_decode("\xEF\xBB\xBFh\xC3\xA9llo");
    CHECK(f.enc == TextEncoding::Utf8Bom && f.text == L"h\u00E9llo" && !f.binary);
    f = text_decode(std::string("\xFF\xFEh\0i\0", 6));
    CHECK(f.enc == TextEncoding::Utf16Le && f.text == L"hi");
    f = text_decode(std::string("\xFE\xFF\0h\0i", 6));
    CHECK(f.enc == TextEncoding::Utf16Be && f.text == L"hi");
    f = text_decode(std::string("a\0b\0c\0\r\0\n\0", 10));
    CHECK(f.enc == TextEncoding::Utf16Le && f.text == L"abc\r");
    f = text_decode("plain");
    CHECK(f.enc == TextEncoding::Utf8 && f.text == L"plain");
    f = text_decode("\xC5\xA1koda");                          // (UTF-8 without a BOM)
    CHECK(f.enc == TextEncoding::Utf8 && f.text == L"\u0161koda");
    f = text_decode("caf\xE9 \x80");                          // (not UTF-8)
    CHECK(f.enc == TextEncoding::Ansi && !f.text.empty());
    f = text_decode(std::string("MZ\0\x90", 4));
    CHECK(f.binary);
    f = text_decode("");
    CHECK(f.enc == TextEncoding::Utf8 && f.text.empty() && f.eol == LineEnding::CrLf);
    // line endings: the most used one; the text always with CR (as the text box has it)
    f = text_decode("a\nb\nc\r\nd");
    CHECK(f.eol == LineEnding::Lf && f.text == L"a\rb\rc\rd");
    f = text_decode("a\rb\rc");
    CHECK(f.eol == LineEnding::Cr && f.text == L"a\rb\rc");
    f = text_decode("a\r\nb\nc\r\n");
    CHECK(f.eol == LineEnding::CrLf && f.text == L"a\rb\rc\r");
    // written back: in the file's line endings and encoding
    CHECK(text_encode(L"a\r\nb\rc\nd", TextEncoding::Utf8, LineEnding::Lf) == "a\nb\nc\nd");
    CHECK(text_encode(L"a\nb", TextEncoding::Utf8, LineEnding::CrLf) == "a\r\nb");
    CHECK(text_encode(L"a\r\nb", TextEncoding::Utf8, LineEnding::Cr) == "a\rb");
    CHECK(text_encode(L"a\rb\r", TextEncoding::Utf8, LineEnding::CrLf) == "a\r\nb\r\n");
    CHECK(text_encode(L"a\rb", TextEncoding::Utf8, LineEnding::Cr) == "a\rb");
    CHECK(text_encode(L"\u00E9", TextEncoding::Utf8Bom, LineEnding::CrLf) == "\xEF\xBB\xBF\xC3\xA9");
    CHECK(text_encode(L"hi", TextEncoding::Utf16Le, LineEnding::CrLf) == std::string("\xFF\xFEh\0i\0", 6));
    CHECK(text_encode(L"hi", TextEncoding::Utf16Be, LineEnding::CrLf) == std::string("\xFE\xFF\0h\0i", 6));
    bool lossy = true;
    CHECK(text_encode(L"abc", TextEncoding::Ansi, LineEnding::CrLf, &lossy) == "abc" && !lossy);
    text_encode(L"\u4E2D", TextEncoding::Ansi, LineEnding::CrLf, &lossy);
    CHECK(lossy);
    // a round trip keeps the bytes
    std::string utf16 = text_encode(L"x\r\ny", TextEncoding::Utf16Le, LineEnding::Lf);
    f = text_decode(utf16);
    CHECK(f.enc == TextEncoding::Utf16Le && f.eol == LineEnding::Lf && text_encode(f.text, f.enc, f.eol) == utf16);
    CHECK(!f.lossy && !f.mixedEol);
    // ANSI comes back byte for byte (never written empty, whatever the system's code page)
    std::string ansi = "caf\xE9 na \xE8" "e\r\n";
    f = text_decode(ansi);
    CHECK(f.enc == TextEncoding::Ansi && !f.text.empty() && !f.lossy);
    CHECK(text_encode(f.text, f.enc, f.eol) == ansi);
    // a UTF-8 BOM before bytes that aren't UTF-8: read as ANSI, so saving gives back the same bytes
    std::string bad = "\xEF\xBB\xBFok \xE9 end";
    f = text_decode(bad);
    CHECK(f.enc == TextEncoding::Ansi && text_encode(f.text, f.enc, f.eol) == bad);
    // what can't come back the same is said: an odd byte after UTF-16, mixed line endings
    f = text_decode(std::string("\xFF\xFEh\0i\0!", 7));
    CHECK(f.enc == TextEncoding::Utf16Le && f.lossy);
    f = text_decode("a\r\nb\nc\r\n");
    CHECK(f.mixedEol && !f.lossy);
}

static void note_sessions() {
    NoteSession s;
    s.active = 1;
    s.wrap = false;
    s.zoom = 150;
    s.x = -8; s.y = 20; s.w = 900; s.h = 600; s.maximized = true;
    NoteTabState a;
    a.path = L"C:\\Users\\me\\to do = list.txt";
    a.enc = TextEncoding::Utf16Be;
    a.eol = LineEnding::Lf;
    a.selStart = 3; a.selEnd = 7; a.top = 12;
    a.fileTime = 0x01DB1F0A5C1A2B3Full;
    a.syntax = L"json";
    NoteTabState b;
    b.untitled = 1;
    b.dirty = true;
    b.cache = L"note-1.txt";
    s.tabs = { a, b };
    NoteSession r = note_session_parse(note_session_serialize(s));
    CHECK(r.active == 1 && !r.wrap && r.statusBar && r.zoom == 150);
    CHECK(r.x == -8 && r.y == 20 && r.w == 900 && r.h == 600 && r.maximized);
    CHECK(r.tabs.size() == 2);
    if (r.tabs.size() == 2) {
        CHECK_EQ(r.tabs[0].path, a.path);
        CHECK(r.tabs[0].enc == TextEncoding::Utf16Be && r.tabs[0].eol == LineEnding::Lf && !r.tabs[0].dirty);
        CHECK(r.tabs[0].selStart == 3 && r.tabs[0].selEnd == 7 && r.tabs[0].top == 12 && r.tabs[0].fileTime == a.fileTime);
        CHECK(r.tabs[0].syntax == L"json" && r.tabs[1].syntax.empty());
        CHECK(r.tabs[1].untitled == 1 && r.tabs[1].dirty && r.tabs[1].path.empty());
        CHECK_EQ(r.tabs[1].cache, L"note-1.txt");
    }
    // nothing, or nonsense: defaults
    r = note_session_parse(L"");
    CHECK(r.tabs.empty() && r.wrap && r.statusBar && r.zoom == 100 && r.w == 0);
    r = note_session_parse(L"[notepad]\r\nactive=9\r\nzoom=9999\r\n[tab1]\r\nencoding=77\r\n[tab2]\r\npath=a.txt\r\n");
    CHECK(r.tabs.size() == 1 && r.active == 0 && r.zoom == 500);
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

static void playlists() {
    CHECK(playlist_file(L"C:\\a\\list.M3U") && playlist_file(L"x.m3u8") && !playlist_file(L"x.m3u8.txt"));
    const std::wstring text =
        L"\xFEFF#EXTM3U\r\n"
        L"#EXTINF:183,Artist - Song\r\n"
        L"Song.mp3\r\n"
        L"\r\n"
        L"# a comment\n"
        L"sub/Other.flac\n"
        L"#EXTINF:-1,Radio\r\n"
        L"http://example.com/stream\r\n"
        L"..\\Up\\Track.mp3\r"
        L"\\Root\\R.mp3\r\n"
        L"E:\\Abs\\A.wav\r\n"
        L"file:///D:/Url%20Dir/U.mp3\r\n"
        L"\\\\server\\share\\./x/../S.mp3\r\n"
        L"#EXTINF:12.6 tvg-id=\"x\",Spaced title  \r\n"
        L"  T.ogg  ";
    auto e = playlist_parse(text, L"C:\\Music\\List");
    CHECK(e.size() == 9);
    if (e.size() == 9) {
        CHECK_EQ(e[0].path, L"C:\\Music\\List\\Song.mp3");
        CHECK_EQ(e[0].title, L"Artist - Song");
        CHECK(e[0].duration == 1830000000LL);
        CHECK_EQ(e[1].path, L"C:\\Music\\List\\sub\\Other.flac");
        CHECK(e[1].title.empty() && e[1].duration == -1);          // (the comment isn't a title)
        CHECK_EQ(e[2].path, L"http://example.com/stream");          // (a stream: as it is, with its title)
        CHECK_EQ(e[2].title, L"Radio");
        CHECK(e[2].duration == -1);
        CHECK_EQ(e[3].path, L"C:\\Music\\Up\\Track.mp3");
        CHECK(e[3].title.empty());
        CHECK_EQ(e[4].path, L"C:\\Root\\R.mp3");
        CHECK_EQ(e[5].path, L"E:\\Abs\\A.wav");
        CHECK_EQ(e[6].path, L"D:\\Url Dir\\U.mp3");
        CHECK_EQ(e[7].path, L"\\\\server\\share\\S.mp3");
        CHECK_EQ(e[8].path, L"C:\\Music\\List\\T.ogg");
        CHECK_EQ(e[8].title, L"Spaced title");
        CHECK(e[8].duration == 126000000LL);
    }
    CHECK(playlist_parse(L"", L"C:\\").empty());
    CHECK(playlist_parse(L"spotify:track:123\r\nftp://x/y.mp3\r\n", L"C:\\").empty());     // (other schemes)
    CHECK(media_url(L"HTTPS://x/y") && media_url(L"mms://x") && !media_url(L"http://") && !media_url(L"C:\\a.mp3"));
    CHECK(playlist_file(L"a.PLS") && playlist_file(L"a.wpl") && playlist_file(L"a.zpl") && playlist_file(L"a.asx") &&
          playlist_file(L"a.wax") && playlist_file(L"a.wvx") && playlist_file(L"a.xspf") && !playlist_file(L"a.xml"));
    CHECK(!playlist_file(L"http://radio.example/listen.pls"));     // (a web address: not opened as a file)

    // PLS: by number, not by order
    auto pls = playlist_parse_pls(L"[playlist]\r\nNumberOfEntries=3\r\nFile2=b.mp3\r\nTitle2=Bee\r\nLength2=61\r\n"
                                  L"File1=http://radio.example:8000/live\r\nTitle1=Live radio\r\nLength1=-1\r\n"
                                  L"Title3=no file\r\nVersion=2\r\n", L"C:\\M");
    CHECK(pls.size() == 2);
    if (pls.size() == 2) {
        CHECK_EQ(pls[0].path, L"http://radio.example:8000/live");
        CHECK_EQ(pls[0].title, L"Live radio");
        CHECK(pls[0].duration == -1);
        CHECK_EQ(pls[1].path, L"C:\\M\\b.mp3");
        CHECK(pls[1].duration == 610000000LL && pls[1].title == L"Bee");
    }
    // WPL / ZPL
    auto wpl = playlist_parse_xml(L"<?wpl version=\"1.0\"?><smil><head><title>Mix</title></head><body><seq>"
                                  L"<media src=\"..\\Other\\a &amp; b.mp3\" tid=\"{x}\"/>"
                                  L"<!-- <media src=\"commented.mp3\"/> -->"
                                  L"<media src='C:\\Abs\\c.wma'/></seq></body></smil>", L"C:\\M\\Lists");
    CHECK(wpl.size() == 2);
    if (wpl.size() == 2) {
        CHECK_EQ(wpl[0].path, L"C:\\M\\Other\\a & b.mp3");
        CHECK(wpl[0].title.empty());                                  // (the playlist's title isn't a track's)
        CHECK_EQ(wpl[1].path, L"C:\\Abs\\c.wma");
    }
    // ASX: the first of an entry's alternatives, its title; another playlist referred to
    auto asx = playlist_parse_xml(L"<ASX version=\"3.0\"><Title>Show</Title>"
                                  L"<Entry><Title>One</Title><Ref HREF=\"mms://media.example/one\"/><Ref href=\"http://media.example/one\"/></Entry>"
                                  L"<ENTRY><REF HREF=\"two.wma\" /></ENTRY>"
                                  L"<EntryRef href=\"more.asx\"/></ASX>", L"C:\\M");
    CHECK(asx.size() == 3);
    if (asx.size() == 3) {
        CHECK_EQ(asx[0].path, L"mms://media.example/one");
        CHECK_EQ(asx[0].title, L"One");
        CHECK_EQ(asx[1].path, L"C:\\M\\two.wma");
        CHECK_EQ(asx[2].path, L"C:\\M\\more.asx");
    }
    // XSPF: file URLs, percent-encoded relative ones, durations in ms, CDATA
    auto xspf = playlist_parse_xml(L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                   L"<playlist version=\"1\" xmlns=\"http://xspf.org/ns/0/\"><title>Not a track</title><trackList>"
                                   L"<track><location>file:///D:/Music/A%20b.flac</location><title>Ay</title><duration>2500</duration></track>"
                                   L"<track><title><![CDATA[R&B]]></title><location>sub/Caf%C3%A9.mp3</location></track>"
                                   L"<track><title>no location</title></track>"
                                   L"</trackList></playlist>", L"C:\\M");
    CHECK(xspf.size() == 2);
    if (xspf.size() == 2) {
        CHECK_EQ(xspf[0].path, L"D:\\Music\\A b.flac");
        CHECK_EQ(xspf[0].title, L"Ay");
        CHECK(xspf[0].duration == 25000000LL);
        CHECK_EQ(xspf[1].path, L"C:\\M\\sub\\Caf\xE9.mp3");
        CHECK_EQ(xspf[1].title, L"R&B");
    }
    // the content over the extension
    CHECK(playlist_parse_any(L"\r\n[Playlist]\nFile1=x.mp3\n", L"C:\\", PlaylistKind::M3U).size() == 1);
    CHECK(playlist_parse_any(L"x.mp3\ny.mp3\n", L"C:\\", PlaylistKind::XML).size() == 2);
    CHECK(midi_file(L"a.MID") && midi_file(L"a.kar") && !midi_file(L"a.mp3") && cd_track_file(L"D:\\Track01.cda"));
    CHECK_EQ(playlist_parse(L"..\\..\\..\\x.mp3", L"C:\\a")[0].path, L"C:\\x.mp3");    // (not above the root)

    // written: relative inside the playlist's folder, #EXTINF where something is known
    std::vector<PlaylistEntry> w = {
        { L"C:\\Music\\List\\Song.mp3", L"Artist - Song", 1830000000LL },
        { L"c:\\music\\list\\Sub\\B.mp3", L"", -1 },
        { L"D:\\Else\\C.mp3", L"", 1234567890LL },
        { L"C:\\Music\\ListOther\\D.mp3", L"Two\nlines", -1 },
    };
    std::wstring out = playlist_write(w, L"C:\\Music\\List");
    CHECK_EQ(out, L"#EXTM3U\r\n"
                  L"#EXTINF:183,Artist - Song\r\nSong.mp3\r\n"
                  L"Sub\\B.mp3\r\n"
                  L"#EXTINF:123,\r\nD:\\Else\\C.mp3\r\n"
                  L"#EXTINF:-1,Two lines\r\nC:\\Music\\ListOther\\D.mp3\r\n");
    auto back = playlist_parse(out, L"C:\\Music\\List\\");
    CHECK(back.size() == 4 && back[1].path == L"C:\\Music\\List\\Sub\\B.mp3" && back[3].path == w[3].path);

    // bytes: .m3u8 UTF-8; .m3u ANSI where it can be
    std::wstring accented = L"#EXTM3U\r\nCaf\xE9.mp3\r\n", cjk = L"#EXTM3U\r\n\x65E5\x672C.mp3\r\n";
    CHECK(playlist_encode(accented, true) == "#EXTM3U\r\nCaf\xC3\xA9.mp3\r\n");
    CHECK(playlist_encode(cjk, true) == "#EXTM3U\r\n\xE6\x97\xA5\xE6\x9C\xAC.mp3\r\n");
    if (GetACP() == 1252) CHECK(playlist_encode(accented, false) == "#EXTM3U\r\nCaf\xE9.mp3\r\n");
    CHECK(playlist_encode(cjk, false) == playlist_encode(cjk, true) || GetACP() == 932);    // (not in the code page: UTF-8)
    CHECK_EQ(ini_decode(playlist_encode(accented, false)), accented);
}

// ------------------------------------------------------------------
// picture editor objects

static void shapes() {
    Shape r;
    r.kind = ShapeKind::Rect;
    shape_span(r, 50, 40, 10, 20, false);                  // (dragged up and left)
    CHECK(r.x0 == 10 && r.y0 == 20 && r.x1 == 50 && r.y1 == 40);
    Shape sq;
    shape_span(sq, 10, 10, 40, 20, true);                   // Shift: a square, towards the drag
    CHECK(sq.x1 - sq.x0 == 30 && sq.y1 - sq.y0 == 30 && sq.y0 == 10);
    shape_span(sq, 10, 10, -20, 5, true);
    CHECK(sq.x0 == -20 && sq.x1 == 10 && sq.y0 == -20 && sq.y1 == 10);

    double x, y;
    shape_handle_pos(r, 3, x, y);
    CHECK(x == 50 && y == 30);
    CHECK(shape_handle_at(r, 11, 21, 3) == 0);
    CHECK(shape_handle_at(r, 30, 41, 3) == 5);
    CHECK(shape_handle_at(r, 30, 30, 3) == -1);
    Shape t = r;
    t.kind = ShapeKind::Text;
    CHECK(shape_handle_at(t, 11, 21, 3) == -1);             // (text: sized by its text size)

    // hits: inside, the outline's width and the tolerance around it
    r.width = 4;
    CHECK(shape_hit(r, 30, 30, 0) && shape_hit(r, 51.5, 30, 0) && !shape_hit(r, 53, 30, 0) && shape_hit(r, 53, 30, 2));
    Shape e = r;
    e.kind = ShapeKind::Ellipse;
    e.width = 0;
    CHECK(shape_hit(e, 30, 30, 0) && shape_hit(e, 10.5, 30, 0));
    CHECK(!shape_hit(e, 11, 21, 0));                        // (the corner of its box isn't the ellipse)
    std::vector<Shape> v = { r, e };
    CHECK(shapes_hit(v, 30, 30, 0) == 1);                   // the topmost
    CHECK(shapes_hit(v, 11, 21, 0) == 0);
    CHECK(shapes_hit(v, 200, 200, 2) == -1);

    shape_move(r, 5, -5);
    CHECK(r.x0 == 15 && r.x1 == 55 && r.y0 == 15 && r.y1 == 35);

    // resizing: the opposite side stays; past it, the other handle is held
    Shape s;
    shape_span(s, 0, 0, 100, 50, false);
    CHECK(shape_resize(s, 4, 120, 60, false) == 4 && s.x1 == 120 && s.y1 == 60 && s.x0 == 0);
    CHECK(shape_resize(s, 3, -20, 999, false) == 7 && s.x0 == -20 && s.x1 == 0 && s.y1 == 60);
    CHECK(shape_resize(s, 1, 999, 80, false) == 5 && s.y0 == 60 && s.y1 == 80 && s.x0 == -20);
    shape_span(s, 0, 0, 100, 50, false);
    CHECK(shape_resize(s, 0, 150, 70, false) == 4 && s.x0 == 100 && s.x1 == 150 && s.y0 == 50 && s.y1 == 70);
    shape_span(s, 0, 0, 100, 50, false);
    CHECK(shape_resize(s, 4, 80, 20, true) == 4 && s.x1 == 80 && s.y1 == 80);
}

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

static std::wstring paths_of(const PlayQueue& q) {
    std::wstring s;
    for (auto& e : q.entries()) s += e.path.substr(1) + L" ";
    return s;
}

static void queue_move() {
    PlayQueue q;
    q.replace(names(6));
    q.set_current(2);
    unsigned id2 = q.at(2).id;
    CHECK(q.move({ 4, 1 }, 0) == 0);                       // to the top, in their order
    CHECK_EQ(paths_of(q), std::wstring(L"1 4 0 2 3 5 "));
    CHECK(q.current() == 3 && q.at(3).id == id2);          // the current one goes along
    CHECK(q.next(false) == 4 && q.previous() == 2);        // the order: as listed
    CHECK(q.move({ 0 }, 6) == 5);                          // to the end
    CHECK_EQ(paths_of(q), std::wstring(L"4 0 2 3 5 1 "));
    CHECK(q.move({ 1, 2 }, 4) == 2);                       // down, before what was row 4
    CHECK_EQ(paths_of(q), std::wstring(L"4 3 0 2 5 1 "));
    CHECK(q.move({ 3 }, 3) == 3 && q.move({ 3 }, 4) == 3);  // where it is: stays
    CHECK_EQ(paths_of(q), std::wstring(L"4 3 0 2 5 1 "));
    CHECK(q.move({ 9, -1 }, 0) == -1);                     // none: nothing
    // into another queue: ids kept; copies: new ones
    PlayQueue b;
    b.replace(names(2, 10));
    b.set_current(1);
    std::vector<QueueEntry> e{ q.at(0), q.at(1) };
    CHECK(b.insert(1, e, false) == 1);
    CHECK_EQ(paths_of(b), std::wstring(L"10 4 3 11 "));
    CHECK(b.at(1).id == q.at(0).id && b.current() == 3 && b.next(false) == -1);
    CHECK(b.insert(99, e, true) == 4 && b.size() == 6 && b.at(4).id != q.at(0).id && b.at(4).path == q.at(0).path);
    // shuffled: the play order kept, every entry still in it once
    PlayQueue s;
    s.seed(3);
    s.replace(names(10));
    s.set_current(4);
    s.set_shuffle(true);
    std::vector<unsigned> order;
    for (int i = s.current(), n = 0; i >= 0 && n < 20; i = s.next(false), ++n) { order.push_back(s.at(i).id); s.set_current(i); }
    s.set_current(s.index_of(order[0]));
    s.move({ 0, 9, 5 }, 3);
    std::vector<unsigned> after;
    for (int i = s.current(), n = 0; i >= 0 && n < 20; i = s.next(false), ++n) { after.push_back(s.at(i).id); s.set_current(i); }
    CHECK(after == order);
    s.set_current(s.index_of(order[0]));
    s.insert(2, e, true);
    int n = 0;
    for (int i = s.current(); i >= 0 && n < 20; i = s.next(false), ++n) s.set_current(i);
    CHECK(n == 12);
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
    // ids: none shared between queues (the tags read find their entry in any of them)
    PlayQueue a, b;
    a.replace(names(2));
    b.replace(names(2));
    CHECK(a.at(0).id != b.at(0).id && a.at(1).id != b.at(1).id && b.index_of(a.at(0).id) == -1);
}

static void player_sessions() {
    PlayerSession s;
    s.active = 1;
    QueueState a;
    a.name = L" Mix = [one]; \"two\" ";
    a.current = 1;
    a.entries = { { L"C:\\Music\\a = b; [c].mp3", L"Title\twith a tab", 1234567 },
                  { L"https://radio.example.com/live?x=1", L"", -1 },
                  { L"D:\\x.flac", L"two\r\nlines", -1 } };
    QueueState b;
    b.name = L"Queue 2";
    s.queues = { a, b };
    PlayerSession r = player_session_parse(player_session_serialize(s));
    CHECK(r.active == 1 && r.queues.size() == 2);
    if (r.queues.size() == 2) {
        CHECK_EQ(r.queues[0].name, a.name);
        CHECK(r.queues[0].current == 1 && r.queues[0].entries.size() == 3);
        if (r.queues[0].entries.size() == 3) {
            CHECK_EQ(r.queues[0].entries[0].path, a.entries[0].path);
            CHECK_EQ(r.queues[0].entries[0].title, L"Title\twith a tab");
            CHECK(r.queues[0].entries[0].duration == 1234567);
            CHECK_EQ(r.queues[0].entries[1].path, a.entries[1].path);
            CHECK(r.queues[0].entries[1].title.empty() && r.queues[0].entries[1].duration == -1);
            CHECK_EQ(r.queues[0].entries[2].title, L"two  lines");
        }
        CHECK_EQ(r.queues[1].name, L"Queue 2");
        CHECK(r.queues[1].entries.empty() && r.queues[1].current == -1);
    }
    // nothing, or nonsense: defaults
    r = player_session_parse(L"");
    CHECK(r.queues.empty() && r.active == 0);
    r = player_session_parse(L"[player]\r\nactive=7\r\n[queue1]\r\ncurrent=5\r\n1=\r\n2=a.mp3\t\r\nx=b.mp3\r\n[queue3]\r\n1=c.mp3\r\n");
    CHECK(r.queues.size() == 1 && r.active == 0);
    if (r.queues.size() == 1) CHECK(r.queues[0].entries.size() == 1 && r.queues[0].current == -1 && r.queues[0].name.empty());
}

static void media_text() {
    CHECK_EQ(fmt_duration(-1), L"");
    CHECK_EQ(fmt_duration(0), L"0:00");
    CHECK_EQ(fmt_duration(1870000000LL), L"3:07");
    CHECK_EQ(fmt_duration(37230000000LL), L"1:02:03");
    CHECK(media_extension_excluded(L".M3U") && media_extension_excluded(L".xspf") && !media_extension_excluded(L".mid"));
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

// ------------------------------------------------------------------
// now playing: the album cover, the spectrogram, formats, Opus in MP4

static void cover_pick() {
    CHECK(pick_cover({}) == -1);
    CHECK(is_image_name(L"Cover.JPG") && is_image_name(L"a.webp") && !is_image_name(L"cover.txt") && !is_image_name(L"jpg"));
    // the names first, in their order, whatever the sizes
    CHECK(pick_cover({ { L"back.jpg", 900000 }, { L"Folder.jpg", 1000 }, { L"cover.png", 500 } }) == 2);
    CHECK(pick_cover({ { L"back.jpg", 900000 }, { L"folder.jpg", 1000 } }) == 1);
    CHECK(pick_cover({ { L"AlbumArtSmall.jpg", 10 }, { L"AlbumArt_{X}_Large.jpg", 20 }, { L"x.png", 99 } }) == 1);
    CHECK(pick_cover({ { L"cover.jpg", 10 }, { L"Cover.png", 20 } }) == 1);       // two covers: the bigger
    CHECK(pick_cover({ { L"covers.jpg", 10 }, { L"discover.jpg", 30 }, { L"scan.png", 20 } }) == 1);   // not names: the biggest
    CHECK(pick_cover({ { L"notes.txt", 1 << 20 }, { L"a.gif", 5 } }) == 1);       // pictures only
    CHECK(pick_cover({ { L"notes.txt", 1 } }) == -1);
}

static void spectrum_math() {
    const int rate = 48000, n = 4096;
    SpectrumMath m(n, rate, 160, 40, 18000);
    CHECK(m.size() == n && m.bands() == 160);
    CHECK(m.band_freq(0) > 39 && m.band_freq(0) < 45 && m.band_freq(159) > 16000 && m.band_freq(159) <= 18000);
    for (int b = 1; b < 160; ++b) CHECK(m.band_freq(b) > m.band_freq(b - 1));
    std::vector<float> s(n, 0.0f);
    std::vector<uint8_t> col(160);
    m.column(s.data(), col.data());
    CHECK(*std::max_element(col.begin(), col.end()) == 0);           // silence: nothing
    for (double hz : { 100.0, 1000.0, 10000.0 }) {
        for (int i = 0; i < n; ++i) s[i] = 0.5f * (float)std::sin(2 * 3.14159265358979 * hz * i / rate);
        m.column(s.data(), col.data());
        int peak = (int)(std::max_element(col.begin(), col.end()) - col.begin());
        CHECK(m.band_freq(peak) > hz / 1.1 && m.band_freq(peak) < hz * 1.1);
        CHECK(col[peak] > 200);                                       // -6 dBFS: near the top
        CHECK(col[0] < col[peak] / 2 && col[159] < col[peak] / 2);   // far from it: well below (a little leaks)
    }
}

static void loudness() {
    // BS.1770's reference: a 1 kHz sine of amplitude A in one channel reads 20 log A - 3.01 LUFS
    const unsigned rate = 44100;
    auto sine = [&](double amp, unsigned ch, double secs) {
        std::vector<float> v((size_t)(rate * secs) * ch);
        for (size_t i = 0; i < v.size() / ch; ++i)
            for (unsigned c = 0; c < ch; ++c) v[i * ch + c] = (float)(amp * std::sin(2 * 3.14159265358979 * 1000 * i / rate));
        return v;
    };
    auto about = [](double a, double b) { return std::fabs(a - b) < 0.2; };
    {
        LoudnessMeter m(rate, 1);
        auto s = sine(0.1, 1, 3);
        m.add(s.data(), s.size());
        CHECK(about(m.lufs(), -23.01));
    }
    {
        LoudnessMeter m(48000, 2);                         // two channels: their sum (+3 dB)
        std::vector<float> s(48000 * 2 * 3);
        for (size_t i = 0; i < s.size() / 2; ++i)
            s[2 * i] = s[2 * i + 1] = (float)(0.1 * std::sin(2 * 3.14159265358979 * 1000 * i / 48000));
        m.add(s.data(), s.size() / 2);
        CHECK(about(m.lufs(), -20.0));
    }
    {
        // silence between: gated out (the blocks partly over the tone count: -23.35)
        LoudnessMeter m(rate, 1);
        auto s = sine(0.1, 1, 2);
        std::vector<float> quiet(rate * 5, 0.0f);
        m.add(s.data(), s.size());
        m.add(quiet.data(), quiet.size());
        m.add(s.data(), s.size());
        CHECK(about(m.lufs(), -23.35));
    }
    {
        LoudnessMeter m(rate, 1);                          // excerpts (cut between): the same
        auto loud = sine(0.1, 1, 1), soft = sine(0.05, 1, 1);
        m.add(loud.data(), loud.size());
        m.cut();
        m.add(soft.data(), soft.size());
        double l = m.lufs();
        CHECK(l > -29.1 && l < -23.0);                     // (between the two; the softer one isn't 10 LU under)
        LoudnessMeter none(rate, 1);
        CHECK(!std::isfinite(none.lufs()));
        std::vector<float> zero(rate * 2, 0.0f);
        none.add(zero.data(), zero.size());
        CHECK(!std::isfinite(none.lufs()));
        none.add(loud.data(), 1000);                       // (under 400 ms: no block)
        CHECK(!std::isfinite(none.lufs()));
    }
    CHECK(about(loudness_gain_db(-9.0), -5.0));
    CHECK(about(loudness_gain_db(-60.0), 12.0));            // (a boost has its limit)
    CHECK(about(loudness_gain_db(-HUGE_VAL), 0.0));
    CHECK(about(loudness_gain_db(NAN), 0.0));
}

static void visuals() {
    auto about = [](double a, double b) { return std::fabs(a - b) < 1e-4; };
    // the samples ring: the newest kept, a gap and what isn't held read as silence
    SampleRing ring(8);
    const float a[5] = { 1, 2, 3, 4, 5 };
    ring.put(100, a, 5);                               // samples 100..104
    float out[6];
    CHECK(ring.get(105, out, 3) == 3 && out[0] == 3 && out[2] == 5);
    CHECK(ring.get(106, out, 3) == 2 && out[0] == 4 && out[2] == 0);     // (past the end: 0)
    CHECK(ring.get(101, out, 3) == 1 && out[0] == 0 && out[2] == 1);     // (before the start: 0)
    ring.put(107, a, 2);                               // a gap (105, 106), then 107..108
    CHECK(ring.end() == 109);
    CHECK(ring.get(109, out, 4) == 4 && out[0] == 0 && out[1] == 0 && out[2] == 1 && out[3] == 2);
    CHECK(ring.get(109, out, 6) == 6 && out[0] == 4 && out[1] == 5);    // (8 held: 101..108)
    CHECK(ring.get(101, out, 1) == 0);                 // (overwritten)
    ring.put(102, a, 3);                               // older than kept: written over in place
    CHECK(ring.end() == 109);
    ring.put(5000, a, 2);                              // a jump (a seek): from there on
    CHECK(ring.end() == 5002 && ring.get(5002, out, 3) == 2 && out[0] == 0 && out[1] == 1);
    const float many[20] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 };
    SampleRing small(4);
    small.put(0, many, 20);                            // more than it holds: the last 4
    CHECK(small.get(20, out, 4) == 4 && out[0] == 16 && out[3] == 19);
    SampleRing early(8);
    early.put(-3, a, 5);                               // (a time before the start: only 0..1 kept)
    CHECK(early.end() == 2 && early.get(2, out, 2) == 2 && out[0] == 4 && out[1] == 5);
    early.put(-9, a, 5);                               // (all before it: nothing)
    CHECK(early.end() == 2);
    // a band's intensity to a level: -62 dB and less nothing, -12 dB and more all
    CHECK(vis_level(0) == 0.0f);
    CHECK(vis_level(255) == 1.0f);
    CHECK(about(vis_level(255 * (80 - 37) / 80.0f), 0.5));           // (-37 dB: half)
    // bars from bands: the loudest of each share; more bars than bands: between two
    const float bands[8] = { 0.1f, 0.9f, 0.2f, 0.3f, 0.4f, 0.0f, 0.5f, 0.6f };
    float bars[4];
    vis_bars(bands, 0, 8, bars, 4);
    CHECK(bars[0] == 0.9f && bars[1] == 0.3f && bars[2] == 0.4f && bars[3] == 0.6f);
    vis_bars(bands, 2, 6, bars, 2);                    // (only bands 2..5)
    CHECK(bars[0] == 0.3f && bars[1] == 0.4f);
    float fine[8];
    vis_bars(bands, 0, 2, fine, 4);                    // 2 bands, 4 bars: 0.1 .. 0.9 in steps
    CHECK(about(fine[0], 0.1) && about(fine[1], 0.3) && about(fine[2], 0.7) && about(fine[3], 0.9));
}

// the visualizations' beats: a track's bass drum found frame by frame, nothing else
static void beats() {
    const unsigned rate = 44100;
    const double pi = 3.14159265358979323846;
    // a band's loudness: a full-scale sine is -3 dB; out of the band, far less
    std::vector<float> tone(4096);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = (float)std::sin(2 * pi * 1000 * i / rate);
    CHECK(std::fabs(band_db(tone.data(), tone.size(), 1024, rate, 20000, false) + 3) < 0.5);
    CHECK(band_db(tone.data(), tone.size(), 1024, rate, 150, false) < -30);
    CHECK(band_db(tone.data(), tone.size(), 1024, rate, 4000, true) < -20);
    std::vector<float> none(2048, 0.0f);
    CHECK(band_db(none.data(), none.size(), 1024, rate, 150, false) == -120);
    // 20 s at 124 BPM: a kick on each beat, hi-hats on the eighths, a bass line and a pad throughout
    const double period = 60.0 / 124, secs = 20;
    std::vector<float> kick((size_t)(rate * secs)), hats(kick.size()), drone(kick.size());
    uint32_t seed = 1;
    auto noise = [&] { seed = seed * 1664525 + 1013904223; return (float)((seed >> 8) / 8388608.0 - 1); };
    for (size_t i = 0; i < kick.size(); ++i) {
        double t = (double)i / rate, k = std::fmod(t, period), e = std::fmod(t, period / 2);
        kick[i] = (float)(0.7 * std::exp(-k / 0.12) * std::sin(2 * pi * (50 * k + 60 * 0.03 * (1 - std::exp(-k / 0.03)))));
        hats[i] = (float)(0.3 * std::exp(-e / 0.03) * noise());
        drone[i] = (float)(0.25 * std::sin(2 * pi * 55 * t) + 0.1 * std::sin(2 * pi * 220 * t));
    }
    // the beats found in a mix, the bass's loudness given at `fps`; their times
    auto found = [&](bool withKick, bool withHats, bool withDrone, double fps) {
        std::vector<float> mix(kick.size());
        for (size_t i = 0; i < mix.size(); ++i)
            mix[i] = (withKick ? kick[i] : 0) + (withHats ? hats[i] : 0) + (withDrone ? drone[i] : 0);
        BeatDetector d;
        std::vector<double> at;
        for (double t = 1 / fps; t < secs; t += 1 / fps) {
            size_t end = (size_t)(t * rate);
            if (end < 2048) continue;
            if (d.update(band_db(mix.data() + end - 2048, 2048, rate / 40, rate, 150, false), (float)(1 / fps))) at.push_back(t);
        }
        return at;
    };
    for (double fps : { 60.0, 144.0 }) {
        std::vector<double> at = found(true, true, true, fps);
        int expected = 0, late = 0;
        for (double t = 2; t < secs - 0.1; t += period) ++expected;
        int got = 0;
        for (double t : at) {
            if (t < 2) continue;
            ++got;
            double since = std::fmod(t, period);    // (just after a kick starts)
            if (since > 0.06) ++late;
        }
        CHECK(std::abs(got - expected) <= 1);
        CHECK(late == 0);
    }
    // no kick: the hi-hats and the bass line aren't beats
    CHECK(found(false, true, true, 60).size() <= 1);
    CHECK(found(false, false, true, 60).size() <= 1);
}

static void formats() {
    CHECK_EQ(audio_format_name(0x55), L"MP3");
    CHECK_EQ(audio_format_name(0x1610), L"AAC");
    CHECK_EQ(audio_format_name(0xF1AC), L"FLAC");
    CHECK_EQ(audio_format_name(0x704F), L"Opus");
    CHECK_EQ(audio_format_name(0x4F707573), L"Opus");      // ('Opus' as the MP4 file gives it)
    CHECK_EQ(audio_format_name(1), L"PCM");
    CHECK_EQ(audio_format_name(0x12345678), L"");
    CHECK_EQ(fmt_sample_rate(44100), L"44.1 kHz");
    CHECK_EQ(fmt_sample_rate(48000), L"48 kHz");
    CHECK_EQ(fmt_sample_rate(0), L"");
    CHECK_EQ(fmt_channels(1), L"Mono");
    CHECK_EQ(fmt_channels(2), L"Stereo");
    CHECK_EQ(fmt_channels(6), L"5.1");
    CHECK_EQ(fmt_channels(0), L"");
    CHECK_EQ(fmt_bitrate(320000), L"320 kbps");
    CHECK_EQ(fmt_bitrate(0), L"");
    CHECK_EQ(fmt_file_size(512), L"512 bytes");
    CHECK_EQ(fmt_file_size(4404019), L"4.2 MB");
}

static void opus_head() {
    // an MP4 'Opus' sample entry's tail with its 'dOps' box (as in a real file): version 0,
    // 2 channels, pre-skip 312, 48000 Hz, gain 0, mapping family 0
    const uint8_t desc[] = { 0, 0, 0, 19, 'd', 'O', 'p', 's', 0, 2, 0x01, 0x38, 0, 0, 0xBB, 0x80, 0, 0, 0 };
    uint8_t h[19]{};
    CHECK(opus_head_from_dops(desc, sizeof(desc), h));
    const uint8_t want[19] = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0x38, 0x01, 0x80, 0xBB, 0, 0, 0, 0, 0 };
    CHECK(std::equal(h, h + 19, want));
    CHECK(!opus_head_from_dops(desc, sizeof(desc) - 1, h));            // cut short
    uint8_t surround[sizeof(desc)];
    std::copy(desc, desc + sizeof(desc), surround);
    surround[18] = 1;                                                   // mapping family 1: not converted
    CHECK(!opus_head_from_dops(surround, sizeof(surround), h));
    const uint8_t none[] = { 0, 0, 0, 8, 'e', 's', 'd', 's', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(!opus_head_from_dops(none, sizeof(none), h));
}

static void subtitles() {
    const long long S = 10000000;
    // SubRip: a BOM, CRLF, markup, two lines, a cue out of order, one with no text
    auto srt = subtitles_parse(L"\xFEFF" L"1\r\n00:00:01,000 --> 00:00:02,500\r\n<i>Hello</i> &amp; bye\r\n\r\n"
                               L"3\r\n00:00:10,000 --> 00:00:12,000\r\n{\\an8}Top\r\nsecond line\r\n\r\n"
                               L"2\r\n00:00:05,000 --> 00:00:06,000\r\n\r\n"
                               L"4\r\n01:00:00,040 --> 01:00:01,000\r\nLate\r\n");
    CHECK((int)srt.size() == 3);
    if (srt.size() == 3) {
        CHECK(srt[0].start == 1 * S);
        CHECK(srt[0].end == 25 * S / 10);
        CHECK_EQ(srt[0].text, L"Hello & bye");
        CHECK_EQ(srt[1].text, L"Top\nsecond line");
        CHECK(srt[2].start == 3600 * S + 400000);
    }
    CHECK_EQ(subtitle_at(srt, 2 * S), L"Hello & bye");
    CHECK_EQ(subtitle_at(srt, 25 * S / 10), L"");                     // the end is not shown
    CHECK_EQ(subtitle_at(srt, 11 * S), L"Top\nsecond line");
    CHECK_EQ(subtitle_at(srt, 0), L"");

    // WebVTT: a header, a NOTE, minutes-only times, cue settings, a cue id, voice tags
    auto vtt = subtitles_parse(L"WEBVTT\n\nNOTE some note\n\nintro\n00:01.000 --> 00:03.000 align:start line:0\n"
                               L"<v Roger>Hi</v> <c.yellow>there</c>\n\n00:02.000 --> 00:04.000\nOverlap\n");
    CHECK((int)vtt.size() == 2);
    if (vtt.size() == 2) {
        CHECK(vtt[0].start == 1 * S);
        CHECK(vtt[0].end == 3 * S);
        CHECK_EQ(vtt[0].text, L"Hi there");
    }
    CHECK_EQ(subtitle_at(vtt, 25 * S / 10), L"Hi there\nOverlap");

    // SubStation Alpha: its Format line, commas in the text, overrides, \N, drawings
    auto ass = subtitles_parse(L"[Script Info]\nTitle: x\n\n[V4+ Styles]\nFormat: Name, Fontname\n\n[Events]\n"
                               L"Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
                               L"Dialogue: 0,0:00:01.50,0:00:03.00,Default,,0,0,0,,{\\b1}Yes,{\\b0} no\\Nmaybe\n"
                               L"Comment: 0,0:00:01.00,0:00:09.00,Default,,0,0,0,,not shown\n"
                               L"Dialogue: 0,0:00:04.00,0:00:05.00,Default,,0,0,0,,{\\p1}m 0 0 l 10 10{\\p0}A\\hB\n");
    CHECK((int)ass.size() == 2);
    if (ass.size() == 2) {
        CHECK(ass[0].start == 15 * S / 10);
        CHECK(ass[0].end == 3 * S);
        CHECK_EQ(ass[0].text, L"Yes, no\nmaybe");
        CHECK_EQ(ass[1].text, L"A B");
    }
    // older SSA: a Marked field first, no Format line
    auto ssa = subtitles_parse(L"[Events]\nDialogue: Marked=0,0:00:02.00,0:00:04.00,Default,,0000,0000,0000,,Old one\n");
    CHECK((int)ssa.size() == 1);
    if (ssa.size() == 1) CHECK_EQ(ssa[0].text, L"Old one");
    CHECK(subtitles_parse(L"nothing here").empty());

    CHECK(subtitle_file(L"a.SRT"));
    CHECK(subtitle_file(L"a.ass"));
    CHECK(!subtitle_file(L"a.sub"));
    std::wstring label = L"x";
    CHECK(subtitle_for(L"Film.mkv", L"film.srt", &label));
    CHECK_EQ(label, L"");
    CHECK(subtitle_for(L"Film.mkv", L"Film.en.srt", &label));
    CHECK_EQ(label, L"en");
    CHECK(subtitle_for(L"Film.mkv", L"Film.English.forced.vtt", &label));
    CHECK_EQ(label, L"English.forced");
    CHECK(subtitle_for(L"Film.mkv", L"Film.mkv.srt", &label));
    CHECK_EQ(label, L"");
    CHECK(subtitle_for(L"Film.mkv", L"Film.mkv.cs.srt", &label));
    CHECK_EQ(label, L"cs");
    CHECK(!subtitle_for(L"Film.mkv", L"Film2.srt"));
    CHECK(!subtitle_for(L"Film.mkv", L"Film.nfo"));
    CHECK(!subtitle_for(L"Film.mkv", L"Filmy.en.srt"));
}

static void image_formats() {
    using V = std::vector<uint8_t>;
    auto u16 = [](V& v, unsigned x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); };
    auto b16 = [](V& v, unsigned x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); };
    auto b32 = [](V& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s)); };
    RawImage im;

    // TGA: 24-bit, bottom-up
    V tga = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    u16(tga, 2); u16(tga, 2);
    tga.push_back(24); tga.push_back(0);
    for (uint8_t b : { 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255 }) tga.push_back(b);   // red green / blue white
    CHECK(tga_decode(tga.data(), tga.size(), im) && im.w == 2 && im.h == 2);
    if (im.px.size() == 4) {
        CHECK(im.px[0] == 0xFF0000FFu && im.px[1] == 0xFFFFFFFFu);          // (top row: blue, white)
        CHECK(im.px[2] == 0xFFFF0000u && im.px[3] == 0xFF00FF00u);
    }
    CHECK(!tga_decode(tga.data(), tga.size() - 1, im));                    // cut short
    // TGA: run-length 32-bit, top-down, half see-through (premultiplied out)
    V rle = { 0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    u16(rle, 3); u16(rle, 1);
    rle.push_back(32); rle.push_back(0x28);
    for (uint8_t b : { 0x82, 10, 20, 30, 128 }) rle.push_back(b);
    CHECK(tga_decode(rle.data(), rle.size(), im) && im.w == 3 && im.px.size() == 3);
    if (im.px.size() == 3) CHECK(im.px[2] == 0x800F0A05u);
    // TGA: 32-bit that says nothing of alpha and has none: opaque
    rle[17] = 0x20;
    rle[rle.size() - 1] = 0;
    CHECK(tga_decode(rle.data(), rle.size(), im) && im.px.size() == 3 && im.px[0] == 0xFF1E140Au);

    // QOI: RGB, run, diff, luma, index
    V qoi = { 'q', 'o', 'i', 'f' };
    b32(qoi, 5); b32(qoi, 1);
    qoi.push_back(4); qoi.push_back(0);
    for (uint8_t b : { 0xFE, 255, 0, 0, 0xC0, 0x5B, 0xA5, 0x88, 0x32, 0, 0, 0, 0, 0, 0, 0, 1 }) qoi.push_back(b);
    CHECK(qoi_decode(qoi.data(), qoi.size(), im) && im.w == 5 && im.h == 1);
    if (im.px.size() == 5) {
        CHECK(im.px[0] == 0xFFFF0000u && im.px[1] == 0xFFFF0000u);
        CHECK(im.px[2] == 0xFFFE0001u);
        CHECK(im.px[3] == 0xFF030506u);
        CHECK(im.px[4] == 0xFFFF0000u);
    }
    qoi[4] = 0x7F;                                                          // (an absurd width)
    CHECK(!qoi_decode(qoi.data(), qoi.size(), im));
    qoi[4] = 0; qoi[6] = 0x10; qoi[10] = 0x10;                              // (about 4096 x 4096: more than 31 bytes can give)
    CHECK(!qoi_decode(qoi.data(), qoi.size(), im));

    // Radiance HDR: flat, and run-length coded
    std::string hs = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
    V hdr(hs.begin(), hs.end());
    for (uint8_t b : { 128, 128, 128, 129, 0, 0, 0, 0 }) hdr.push_back(b);
    CHECK(hdr_decode(hdr.data(), hdr.size(), im) && im.w == 2 && im.h == 1);
    if (im.px.size() == 2) {
        unsigned r = (im.px[0] >> 16) & 255, g = (im.px[0] >> 8) & 255, b = im.px[0] & 255;
        CHECK(r == g && g == b && r == 255);        // (within the screen's range: as it is)
        CHECK(im.px[1] == 0xFF000000u);
    }
    hs = "#?RADIANCE\n\n-Y 1 +X 8\n";
    V hdr2(hs.begin(), hs.end());
    for (uint8_t b : { 2, 2, 0, 8 }) hdr2.push_back(b);
    for (int c = 0; c < 4; ++c) { hdr2.push_back(128 + 8); hdr2.push_back(128); }
    CHECK(hdr_decode(hdr2.data(), hdr2.size(), im) && im.w == 8 && im.px.size() == 8);
    if (im.px.size() == 8) CHECK(std::all_of(im.px.begin(), im.px.end(), [&](uint32_t v) { return v == im.px[0]; }));
    hs = "#?RADIANCE\nFORMAT=32-bit_rle_xyze\n\n-Y 1 +X 1\n";
    V xyz(hs.begin(), hs.end());
    for (int k = 0; k < 4; ++k) xyz.push_back(1);
    CHECK(!hdr_decode(xyz.data(), xyz.size(), im));

    // PSD: raw RGB, and PackBits RGB
    auto psd_head = [&](unsigned ch, uint32_t h, uint32_t w, unsigned comp) {
        V v = { '8', 'B', 'P', 'S', 0, 1, 0, 0, 0, 0, 0, 0 };
        b16(v, ch); b32(v, h); b32(v, w); b16(v, 8); b16(v, 3);
        b32(v, 0); b32(v, 0); b32(v, 0);                                   // mode data, resources, layers
        b16(v, comp);
        return v;
    };
    V psd = psd_head(3, 1, 2, 0);
    for (uint8_t b : { 255, 0, 0, 255, 0, 0 }) psd.push_back(b);
    CHECK(psd_decode(psd.data(), psd.size(), im) && im.w == 2 && im.px.size() == 2);
    if (im.px.size() == 2) CHECK(im.px[0] == 0xFFFF0000u && im.px[1] == 0xFF00FF00u);
    V psdr = psd_head(3, 1, 4, 1);
    b16(psdr, 2); b16(psdr, 5); b16(psdr, 2);
    for (uint8_t b : { 0xFD, 10, 0x03, 1, 2, 3, 4, 0xFD, 7 }) psdr.push_back(b);
    CHECK(psd_decode(psdr.data(), psdr.size(), im) && im.w == 4 && im.px.size() == 4);
    if (im.px.size() == 4) CHECK(im.px[0] == 0xFF0A0107u && im.px[3] == 0xFF0A0407u);
    psdr.resize(psdr.size() - 3);
    CHECK(!psd_decode(psdr.data(), psdr.size(), im));

    // PCX: 256 colours run-length coded with the palette after; 24-bit planes
    V pcx(128, 0);
    pcx[0] = 0x0A; pcx[1] = 5; pcx[2] = 1; pcx[3] = 8;
    pcx[8] = 1; pcx[10] = 1; pcx[65] = 1; pcx[66] = 2;
    for (uint8_t b : { 0xC2, 1, 2, 0xC1, 3 }) pcx.push_back(b);
    pcx.push_back(0x0C);
    V pal(768, 0);
    pal[3] = 255; pal[7] = 255; pal[11] = 255;                              // 1 red, 2 green, 3 blue
    pcx.insert(pcx.end(), pal.begin(), pal.end());
    CHECK(pcx_decode(pcx.data(), pcx.size(), im) && im.w == 2 && im.h == 2);
    if (im.px.size() == 4)
        CHECK(im.px[0] == 0xFFFF0000u && im.px[1] == 0xFFFF0000u && im.px[2] == 0xFF00FF00u && im.px[3] == 0xFF0000FFu);
    V pcx24(128, 0);
    pcx24[0] = 0x0A; pcx24[3] = 8; pcx24[65] = 3; pcx24[66] = 1;
    for (uint8_t b : { 1, 2, 3 }) pcx24.push_back(b);
    CHECK(pcx_decode(pcx24.data(), pcx24.size(), im) && im.px.size() == 1 && im.px[0] == 0xFF010203u);

    CHECK(own_image_ext(L".TGA") && own_image_ext(L".psb") && own_image_ext(L".pic") && !own_image_ext(L".png"));
    CHECK(own_image_decode(L".Qoi", qoi.data(), 0, im) == false);

    // animation frames: kept, restored, cleared, drawn over
    AnimCanvas cv(2, 1);
    const uint32_t red[2] = { 0xFFFF0000u, 0xFFFF0000u }, blue = 0xFF0000FFu, half = 0x80808080u;
    AnimFrame a; a.w = 2; a.h = 1; a.px = red;
    CHECK(cv.add(a)[1] == 0xFFFF0000u);
    AnimFrame b; b.x = 1; b.w = 1; b.h = 1; b.px = &blue; b.dispose = AnimFrame::Restore;
    CHECK(cv.add(b)[1] == blue);
    AnimFrame c; c.w = 1; c.h = 1; c.px = &half; c.dispose = AnimFrame::Clear;
    const auto& afterC = cv.add(c);
    CHECK(afterC[0] == 0xFFFF8080u && afterC[1] == 0xFFFF0000u);           // (blue undone, half white over red)
    AnimFrame none;
    const auto& afterD = cv.add(none);
    CHECK(afterD[0] == 0 && afterD[1] == 0xFFFF0000u);

    // APNG: its frames as PNGs of their own
    auto chunk = [&](V& v, const char* t, const V& data) {
        b32(v, (uint32_t)data.size());
        v.insert(v.end(), t, t + 4);
        v.insert(v.end(), data.begin(), data.end());
        b32(v, 0);                                                          // (the CRC isn't checked)
    };
    auto fctl = [&](uint32_t seq, uint32_t w, uint32_t h, uint32_t x, uint32_t y, unsigned num, unsigned den, uint8_t disp, uint8_t blend) {
        V v;
        b32(v, seq); b32(v, w); b32(v, h); b32(v, x); b32(v, y); b16(v, num); b16(v, den);
        v.push_back(disp); v.push_back(blend);
        return v;
    };
    V png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    V ihdr;
    b32(ihdr, 2); b32(ihdr, 2);
    for (uint8_t b2 : { 8, 6, 0, 0, 0 }) ihdr.push_back(b2);
    chunk(png, "IHDR", ihdr);
    V still = png;
    chunk(png, "acTL", { 0, 0, 0, 2, 0, 0, 0, 0 });
    chunk(png, "PLTE", { 1, 2, 3 });
    chunk(png, "fcTL", fctl(0, 2, 2, 0, 0, 1, 10, 0, 0));
    chunk(png, "IDAT", { 'a', 'b', 'c' });
    chunk(png, "fcTL", fctl(1, 1, 1, 1, 1, 0, 0, 1, 1));
    chunk(png, "fdAT", { 0, 0, 0, 2, 'x', 'y' });
    chunk(png, "IEND", {});
    int cw = 0, ch = 0;
    std::vector<ApngFrame> frames;
    CHECK(apng_split(png.data(), png.size(), cw, ch, frames) && cw == 2 && ch == 2 && frames.size() == 2);
    if (frames.size() == 2) {
        CHECK(frames[0].delayMs == 100 && frames[0].dispose == AnimFrame::Keep && !frames[0].over);
        CHECK(frames[1].x == 1 && frames[1].y == 1 && frames[1].w == 1 && frames[1].delayMs == 0);
        CHECK(frames[1].dispose == AnimFrame::Clear && frames[1].over);
        const V& f = frames[1].png;
        // signature, IHDR (1 x 1), PLTE, IDAT "xy", IEND
        CHECK(f.size() == 8 + 25 + 15 + 14 + 12);
        if (f.size() == 8 + 25 + 15 + 14 + 12) {
            CHECK(f[16 + 3] == 1 && f[20 + 3] == 1);
            CHECK(std::string(f.begin() + 8 + 25 + 4, f.begin() + 8 + 25 + 8) == "PLTE");
            CHECK(std::string(f.begin() + 8 + 25 + 15 + 4, f.begin() + 8 + 25 + 15 + 10) == "IDATxy");
            const uint8_t crc[4] = { f[8 + 25 + 15 + 10], f[8 + 25 + 15 + 11], f[8 + 25 + 15 + 12], f[8 + 25 + 15 + 13] };
            CHECK(crc[0] == 0xB2 && crc[1] == 0xC4 && crc[2] == 0x87 && crc[3] == 0xDC);   // crc32("IDATxy")
        }
    }
    {
        V away = png;                                  // a frame placed past the canvas (x + w overflowing an int)
        size_t at = 8 + 25 + 20 + 15 + 8;              // (the first fcTL's data: after IHDR, acTL, PLTE)
        CHECK(std::string(away.begin() + at - 4, away.begin() + at) == "fcTL");
        V bad = fctl(0, 2, 2, 0x7FFFFFFF, 0, 1, 10, 0, 0);
        std::copy(bad.begin(), bad.end(), away.begin() + at);
        CHECK(!apng_split(away.data(), away.size(), cw, ch, frames));
    }
    chunk(still, "IDAT", { 'a' });
    chunk(still, "IEND", {});
    CHECK(!apng_split(still.data(), still.size(), cw, ch, frames));
    CHECK(anim_delay(0) == 100 && anim_delay(10) == 100 && anim_delay(40) == 40);

    // SVG sizes
    double w = 0, h = 0;
    CHECK(svg_size("<?xml version='1.0'?><svg xmlns='http://www.w3.org/2000/svg' width='100' height='50'>", w, h) && w == 100 && h == 50);
    CHECK(svg_size("<svg viewBox=\"0 0 24 12\"/>", w, h) && w == 24 && h == 12);
    CHECK(svg_size("<svg width=\"48\" viewBox=\"0 0 24 12\">", w, h) && w == 48 && h == 24);
    CHECK(svg_size("<svg width=\"100%\" height=\"100%\" viewBox=\"0,0,10,20\">", w, h) && w == 10 && h == 20);
    CHECK(svg_size("<svg width=\"1in\" height=\"72pt\">", w, h) && w == 96 && h == 96);
    CHECK(svg_size("<!-- <svg width=\"1\" height=\"1\"> --><svg>", w, h) && w == 300 && h == 150);
    CHECK(svg_size("<svg stroke-width=\"5\" height=\"10\">", w, h) && w == 300 && h == 10);
    CHECK(!svg_size("<html><svgx/></html>", w, h));
}

// ------------------------------------------------------------------
// the default app's check value (checked against hashes Windows itself wrote)

static void user_choice() {
    const std::wstring sid = L"S-1-5-21-1111111111-2222222222-3333333333-1001";
    FILETIME t{ 0x5C1A2B3Fu, 0x01DB1F0Au };
    CHECK_EQ(user_choice_hash(L".mp3", sid, L"ExplorerPlus.mp3", t), L"atetQeeExhM=");
    // case doesn't count, nor the seconds
    FILETIME later{ t.dwLowDateTime + 10000000u, t.dwHighDateTime };
    CHECK_EQ(user_choice_hash(L".MP3", sid, L"explorerplus.mp3", later), L"atetQeeExhM=");
    FILETIME nextMinute{ t.dwLowDateTime + 600000000u, t.dwHighDateTime };
    CHECK(user_choice_hash(L".mp3", sid, L"ExplorerPlus.mp3", nextMinute) != L"atetQeeExhM=");
    CHECK(user_choice_hash(L".mp4", sid, L"ExplorerPlus.mp3", t) != L"atetQeeExhM=");
}

// the colour of the first span that is exactly `piece` (Tok::Count: none is)
static Tok tok_of(Lang l, const std::wstring& text, const std::wstring& piece, int state = 0) {
    std::vector<SyntaxSpan> spans;
    syntax_lex(l, text.c_str(), text.size(), state, &spans);
    for (auto& s : spans)
        if (text.compare(s.start, s.len, piece) == 0) return s.tok;
    return Tok::Count;
}

static void syntax_colours() {
    CHECK(lang_for_file(L"C:\\x\\main.CPP") == Lang::C);
    CHECK(lang_for_file(L"a.json") == Lang::Json && lang_for_file(L"b.yml") == Lang::Yaml);
    CHECK(lang_for_file(L"Dockerfile") == Lang::Shell && lang_for_file(L"notes.txt") == Lang::None);
    CHECK(lang_from_key(lang_key(Lang::PowerShell)) == Lang::PowerShell && lang_from_key(L"nonsense") == Lang::None);

    std::wstring c = L"#include <x.h>\r// note\rint main() { return f(0x1F, 2.5e-3) + 'c'; } /* a\rb */ \"s\\\"q\"";
    CHECK(tok_of(Lang::C, c, L"#include <x.h>") == Tok::Preproc);
    CHECK(tok_of(Lang::C, c, L"// note") == Tok::Comment);
    CHECK(tok_of(Lang::C, c, L"int") == Tok::Type);
    CHECK(tok_of(Lang::C, c, L"return") == Tok::Keyword);
    CHECK(tok_of(Lang::C, c, L"main") == Tok::Function && tok_of(Lang::C, c, L"f") == Tok::Function);
    CHECK(tok_of(Lang::C, c, L"0x1F") == Tok::Number && tok_of(Lang::C, c, L"2.5e-3") == Tok::Number);
    CHECK(tok_of(Lang::C, c, L"'c'") == Tok::String && tok_of(Lang::C, c, L"\"s\\\"q\"") == Tok::String);
    CHECK(tok_of(Lang::C, c, L"/* a\rb */") == Tok::Comment);

    // resumable: a block comment, a multi-line string go on into the next piece
    std::vector<SyntaxSpan> spans;
    std::wstring a = L"x = 1 /* open\r", b = L"still */ y\r";
    int st = syntax_lex(Lang::C, a.c_str(), a.size(), 0, &spans);
    CHECK(st != 0);
    CHECK(tok_of(Lang::C, b, L"still */", st) == Tok::Comment);
    std::wstring p1 = L"s = \"\"\"doc\r", p2 = L"end\"\"\" + x\r";
    st = syntax_lex(Lang::Python, p1.c_str(), p1.size(), 0, nullptr);
    CHECK(st != 0 && tok_of(Lang::Python, p2, L"end\"\"\"", st) == Tok::String);
    CHECK(syntax_lex(Lang::Python, p2.c_str(), p2.size(), st, nullptr) == 0);
    std::wstring js = L"const t = `a\r";
    CHECK(syntax_lex(Lang::JavaScript, js.c_str(), js.size(), 0, nullptr) != 0);
    std::wstring cs = L"var p = @\"C:\\dir\r";
    st = syntax_lex(Lang::CSharp, cs.c_str(), cs.size(), 0, nullptr);
    CHECK(st != 0 && tok_of(Lang::CSharp, L"x\"\"y\" z", L"x\"\"y\"", st) == Tok::String);

    CHECK(tok_of(Lang::Python, L"@dec\rdef f(self): return None", L"@dec") == Tok::Preproc);
    CHECK(tok_of(Lang::Python, L"x = r'\\d' # c", L"r'\\d'") == Tok::String);
    CHECK(tok_of(Lang::Python, L"x = r'\\d' # c", L"# c") == Tok::Comment);
    CHECK(tok_of(Lang::PowerShell, L"$x = Get-Item -Path $env:TEMP -eq $true", L"Get-Item") == Tok::Function);
    CHECK(tok_of(Lang::PowerShell, L"$x = Get-Item -Path $env:TEMP -eq $true", L"-Path") == Tok::Attr);
    CHECK(tok_of(Lang::PowerShell, L"$x = Get-Item -Path $env:TEMP -eq $true", L"$env:TEMP") == Tok::Variable);
    CHECK(tok_of(Lang::PowerShell, L"$x = Get-Item -Path $env:TEMP -EQ $True", L"-EQ") == Tok::Keyword);
    CHECK(tok_of(Lang::PowerShell, L"$x = Get-Item -Path $env:TEMP -EQ $True", L"$True") == Tok::Literal);
    CHECK(tok_of(Lang::Shell, L"echo $HOME a#b # c", L"$HOME") == Tok::Variable);
    CHECK(tok_of(Lang::Shell, L"echo $HOME a#b # c", L"# c") == Tok::Comment);
    CHECK(tok_of(Lang::Shell, L"echo $HOME a#b # c", L"#b # c") == Tok::Count);
    CHECK(tok_of(Lang::Sql, L"SELECT name FROM t WHERE x = 'it''s' -- c", L"SELECT") == Tok::Keyword);
    CHECK(tok_of(Lang::Sql, L"SELECT name FROM t WHERE x = 'it''s' -- c", L"'it''s'") == Tok::String);
    CHECK(tok_of(Lang::Rust, L"fn f<'a>(c: char) { 'x' }", L"'a") == Tok::Variable);
    CHECK(tok_of(Lang::Rust, L"fn f<'a>(c: char) { 'x' }", L"'x'") == Tok::String);
    CHECK(tok_of(Lang::Batch, L"@echo off\rrem note\r:label\rset X=%~dp0 & echo %X% !Y! %%i", L"rem note") == Tok::Comment);
    CHECK(tok_of(Lang::Batch, L"@echo off\rrem note\r:label\rset X=%~dp0 & echo %X% !Y! %%i", L":label") == Tok::Tag);
    CHECK(tok_of(Lang::Batch, L"@echo off\rrem note\r:label\rset X=%~dp0 & echo %X% !Y! %%i", L"%~dp0") == Tok::Variable);
    CHECK(tok_of(Lang::Batch, L"@echo off\rrem note\r:label\rset X=%~dp0 & echo %X% !Y! %%i", L"%X%") == Tok::Variable);
    CHECK(tok_of(Lang::Batch, L"@echo off\rrem note\r:label\rset X=%~dp0 & echo %X% !Y! %%i", L"!Y!") == Tok::Variable);

    std::wstring json = L"{ \"key\" : \"value\", \"n\": -1.5e3, \"ok\": true, \"z\": null }";
    CHECK(tok_of(Lang::Json, json, L"\"key\"") == Tok::Key);
    CHECK(tok_of(Lang::Json, json, L"\"value\"") == Tok::String);
    CHECK(tok_of(Lang::Json, json, L"1.5e3") == Tok::Number);
    CHECK(tok_of(Lang::Json, json, L"true") == Tok::Literal && tok_of(Lang::Json, json, L"null") == Tok::Literal);

    std::wstring xml = L"<?xml version=\"1.0\"?>\r<a x=\"1\" y='2'><!-- c --><b>t &amp; u</b><![CDATA[<raw>]]></a>";
    CHECK(tok_of(Lang::Xml, xml, L"<?xml version=\"1.0\"?>") == Tok::Preproc);
    CHECK(tok_of(Lang::Xml, xml, L"<a") == Tok::Tag && tok_of(Lang::Xml, xml, L"</b") == Tok::Tag);
    CHECK(tok_of(Lang::Xml, xml, L"x") == Tok::Attr && tok_of(Lang::Xml, xml, L"\"1\"") == Tok::String);
    CHECK(tok_of(Lang::Xml, xml, L"<!-- c -->") == Tok::Comment);
    CHECK(tok_of(Lang::Xml, xml, L"&amp;") == Tok::Literal);
    CHECK(tok_of(Lang::Xml, xml, L"<![CDATA[<raw>]]>") == Tok::String);
    std::wstring xa = L"<a>\r<!-- open\r";
    st = syntax_lex(Lang::Xml, xa.c_str(), xa.size(), 0, nullptr);
    CHECK(st != 0 && tok_of(Lang::Xml, L"close --> <b>", L"close -->", st) == Tok::Comment);
    std::wstring html = L"<script>if (a<b) x = \"s\";</script><p class=\"c\">";
    CHECK(tok_of(Lang::Html, html, L"\"s\"") == Tok::String);
    CHECK(tok_of(Lang::Html, html, L"if") == Tok::Keyword);
    CHECK(tok_of(Lang::Html, html, L"</script") == Tok::Tag && tok_of(Lang::Html, html, L"class") == Tok::Attr);

    std::wstring css = L"a.btn:hover, #id { color: #fff; margin: 0 2px !important; --v: 1; }\r@media (x) {}";
    CHECK(tok_of(Lang::Css, css, L"a") == Tok::Tag && tok_of(Lang::Css, css, L".btn") == Tok::Attr);
    CHECK(tok_of(Lang::Css, css, L":hover") == Tok::Keyword && tok_of(Lang::Css, css, L"#id") == Tok::Attr);
    CHECK(tok_of(Lang::Css, css, L"color") == Tok::Key && tok_of(Lang::Css, css, L"#fff") == Tok::Number);
    CHECK(tok_of(Lang::Css, css, L"2px") == Tok::Number && tok_of(Lang::Css, css, L"!important") == Tok::Keyword);
    CHECK(tok_of(Lang::Css, css, L"--v") == Tok::Variable && tok_of(Lang::Css, css, L"@media") == Tok::Keyword);

    std::wstring md = L"# Title\r- item with `code` and **bold** and [link](http://x)\r```\rint x;\r```\r> quote";
    CHECK(tok_of(Lang::Markdown, md, L"# Title") == Tok::Heading);
    CHECK(tok_of(Lang::Markdown, md, L"-") == Tok::Keyword && tok_of(Lang::Markdown, md, L"`code`") == Tok::String);
    CHECK(tok_of(Lang::Markdown, md, L"**bold**") == Tok::Emphasis && tok_of(Lang::Markdown, md, L"[link]") == Tok::Link);
    CHECK(tok_of(Lang::Markdown, md, L"int x;") == Tok::String && tok_of(Lang::Markdown, md, L"> quote") == Tok::Comment);
    CHECK(tok_of(Lang::Markdown, L"snake_case_name", L"_case_") == Tok::Count);

    std::wstring ini = L"; c\r[section]\rkey = value 1\rn=42\rflag=true\r\"Reg\"=dword:1";
    CHECK(tok_of(Lang::Ini, ini, L"; c") == Tok::Comment && tok_of(Lang::Ini, ini, L"[section]") == Tok::Heading);
    CHECK(tok_of(Lang::Ini, ini, L"key") == Tok::Key && tok_of(Lang::Ini, ini, L"42") == Tok::Number);
    CHECK(tok_of(Lang::Ini, ini, L"true") == Tok::Literal && tok_of(Lang::Ini, ini, L"\"Reg\"") == Tok::Key);
    std::wstring toml = L"[[bin]]\rname = \"x\" # c\rlist = [1, 2]\rd = 1979-05-27T07:32:00Z\rt = { a = 1 }";
    CHECK(tok_of(Lang::Toml, toml, L"[[bin]]") == Tok::Heading && tok_of(Lang::Toml, toml, L"\"x\"") == Tok::String);
    CHECK(tok_of(Lang::Toml, toml, L"# c") == Tok::Comment && tok_of(Lang::Toml, toml, L"2") == Tok::Number);
    CHECK(tok_of(Lang::Toml, toml, L"1979-05-27T07:32:00Z") == Tok::Number && tok_of(Lang::Toml, toml, L"a") == Tok::Key);

    std::wstring yaml = L"---\rkey: value # c\rlist:\r  - 1\r  - \"two\"\rok: yes\rref: *anchor\rtext: |\r  block\r  more\rnext: 2";
    CHECK(tok_of(Lang::Yaml, yaml, L"---") == Tok::Preproc && tok_of(Lang::Yaml, yaml, L"key") == Tok::Key);
    CHECK(tok_of(Lang::Yaml, yaml, L"value") == Tok::String && tok_of(Lang::Yaml, yaml, L"# c") == Tok::Comment);
    CHECK(tok_of(Lang::Yaml, yaml, L"1") == Tok::Number && tok_of(Lang::Yaml, yaml, L"\"two\"") == Tok::String);
    CHECK(tok_of(Lang::Yaml, yaml, L"yes") == Tok::Literal && tok_of(Lang::Yaml, yaml, L"*anchor") == Tok::Variable);
    CHECK(tok_of(Lang::Yaml, yaml, L"block") == Tok::String && tok_of(Lang::Yaml, yaml, L"next") == Tok::Key);

    std::wstring diff = L"--- a\r+++ b\r@@ -1 +1 @@\r-old\r+new\r same";
    CHECK(tok_of(Lang::Diff, diff, L"--- a") == Tok::Heading && tok_of(Lang::Diff, diff, L"@@ -1 +1 @@") == Tok::Preproc);
    CHECK(tok_of(Lang::Diff, diff, L"-old") == Tok::Removed && tok_of(Lang::Diff, diff, L"+new") == Tok::Added);

    // spans come in order and stay inside the text, whatever it holds
    std::wstring junk = L"\"unterminated\r/* x\r<a b='\r`\r#\r[\r-\r";
    for (int l = 0; l < (int)Lang::Count; ++l) {
        std::vector<SyntaxSpan> v;
        syntax_lex((Lang)l, junk.c_str(), junk.size(), 0, &v);
        uint32_t last = 0;
        for (auto& s : v) {
            CHECK(s.start >= last && s.start + s.len <= junk.size());
            last = s.start;
        }
    }
}

static void formatting() {
    std::wstring out;
    size_t err = 0;
    CHECK(format_json(L"{\"a\":1,\"b\":[1,2,{}],\"c\":{\"d\":\"x,y\"}}", false, out, &err));
    CHECK_EQ(out, L"{\r\n    \"a\": 1,\r\n    \"b\": [\r\n        1,\r\n        2,\r\n        {}\r\n    ],\r\n    \"c\": {\r\n        \"d\": \"x,y\"\r\n    }\r\n}");
    CHECK(format_json(L"{ \"a\" : [ 1 , 2 ] }", true, out, &err));
    CHECK_EQ(out, L"{\"a\":[1,2]}");
    CHECK(format_json(L"{\"a\": 1, // one\n\"b\": 2}", false, out, &err));
    CHECK_EQ(out, L"{\r\n    \"a\": 1, // one\r\n    \"b\": 2\r\n}");
    CHECK(!format_json(L"{\"a\": [1, 2}", false, out, &err) && err == 11);
    CHECK(!format_json(L"{\"a\": 1", false, out, &err) && err == 7);
    CHECK(!format_json(L"{\"a\": \"open}", false, out, &err) && err == 6);
    // values need their commas and colons: plain text isn't JSON (no "Buy milk" -> "Buymilk")
    CHECK(!format_json(L"Buy milk", false, out, &err) && err == 4);
    CHECK(!format_json(L"[1 2]", false, out, &err) && err == 3);
    CHECK(!format_json(L"{\"a\" \"b\"}", false, out, &err) && err == 5);
    CHECK(!format_json(L"[,1]", false, out, &err) && err == 1);
    CHECK(!format_json(L"{} {}", false, out, &err) && err == 3);
    CHECK(format_json(L"[1, 2,]", true, out, &err) && out == L"[1,2,]");        // (JSON5's trailing comma)
    CHECK(!format_json(std::wstring(2000, L'['), false, out, &err));          // too deep: refused, not gigabytes of indent

    CHECK(format_xml(L"<?xml version=\"1.0\"?><a x=\"1>\"><b>text</b><c/><d></d><e><f/></e><!-- c --></a>", out, &err));
    CHECK_EQ(out, L"<?xml version=\"1.0\"?>\r\n<a x=\"1>\">\r\n    <b>text</b>\r\n    <c/>\r\n    <d></d>\r\n    <e>\r\n        <f/>\r\n    </e>\r\n    <!-- c -->\r\n</a>");
    CHECK(format_xml(L"<a>\n  <b>  x  </b>\n</a>", out, &err));
    CHECK_EQ(out, L"<a>\r\n    <b>x</b>\r\n</a>");
    CHECK(!format_xml(L"<a><b></a>", out, &err) && err == 6);
    CHECK(!format_xml(L"<a><b>", out, &err) && err == 3);
}

static void text_operations() {
    CHECK_EQ(line_op(LineOp::SortAsc, L"b\rA\rfile10\rfile2\ra"), L"A\ra\rb\rfile2\rfile10");
    CHECK_EQ(line_op(LineOp::SortDesc, L"b\ra\rc"), L"c\rb\ra");
    CHECK_EQ(line_op(LineOp::Unique, L"a\rb\ra\r\rb\r"), L"a\rb\r");
    CHECK_EQ(line_op(LineOp::RemoveEmpty, L"a\r\r  \rb\r\n\nc"), L"a\rb\rc");
    CHECK_EQ(line_op(LineOp::TrimTrailing, L"a  \rb\t\r c "), L"a\rb\r c");
    CHECK_EQ(line_op(LineOp::Reverse, L"1\r2\r3"), L"3\r2\r1");
    CHECK_EQ(line_op(LineOp::Join, L"a \r  b\r\r c"), L"a b c");
    CHECK_EQ(change_case(CaseOp::Upper, L"\u0161koda x"), L"\u0160KODA X");
    CHECK_EQ(change_case(CaseOp::Lower, L"ABC"), L"abc");
    CHECK_EQ(change_case(CaseOp::Title, L"hello wORLD"), L"Hello World");

    CHECK_EQ(base64_encode(L""), L"");
    CHECK_EQ(base64_encode(L"f"), L"Zg==");
    CHECK_EQ(base64_encode(L"fo"), L"Zm8=");
    CHECK_EQ(base64_encode(L"foo"), L"Zm9v");
    CHECK_EQ(base64_encode(L"\u0161"), L"xaE=");
    std::wstring t;
    CHECK(base64_decode(L"Zm9v\r\nYmFy", t) && t == L"foobar");
    CHECK(base64_decode(L"Zg", t) && t == L"f");                    // (no padding)
    CHECK(base64_decode(L"xaE=", t) && t == L"\u0161");
    CHECK(base64_decode(L"-_8", t) == false);                      // (bytes, not UTF-8)
    CHECK(!base64_decode(L"Zm9v!", t) && !base64_decode(L"Z", t) && !base64_decode(L"Zg==Zg", t));
    CHECK_EQ(url_encode(L"a b&c=\u0161/~"), L"a%20b%26c%3D%C5%A1%2F~");
    CHECK(url_decode(L"a%20b%26c%3D%C5%A1%2F~+%zz%", t) && t == L"a b&c=\u0161/~+%zz%");
    CHECK(url_decode(L"\U0001F600%20", t) && t == L"\U0001F600 ");
    CHECK(!url_decode(L"%C5", t));
}

int wmain() {
    user_choice();
    text_files();
    note_sessions();
    syntax_colours();
    formatting();
    text_operations();
    queue_order();
    queue_remove();
    queue_move();
    queue_shuffle();
    player_sessions();
    playlists();
    shapes();
    media_text();
    cover_pick();
    spectrum_math();
    loudness();
    visuals();
    beats();
    formats();
    opus_head();
    subtitles();
    image_formats();
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
