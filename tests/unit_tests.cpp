// Unit tests for the pure text handling: the settings INI document and the command
// line / typed path parsing. Run: ctest --test-dir build (or build\explorer-plus-tests.exe)
#include "ini.h"
#include "cmdline.h"
#include "playlist.h"
#include "mediadata.h"
#include "shapes.h"
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
    CHECK(c.play && !c.newWindow);
    CHECK_EQ(c.path, L"D:\\Music\\a b.mp3");
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

int wmain() {
    queue_order();
    queue_remove();
    queue_shuffle();
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
