#include "notepad.h"
#include "cmdline.h"
#include "ini.h"
#include "perf.h"
#include "playlist.h"
#include "resource.h"
#include "shell_utils.h"
#include "settings.h"
#include "syntax.h"
#include "textfile.h"
#include "textops.h"
#include "theme.h"
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <climits>
#include <memory>

namespace {

const ULONG_PTR kCopyOpen = 0x45504E31;     // WM_COPYDATA 'EPN1': files to open (paths, NUL separated)
const UINT WM_NOTEPAD_OPEN = WM_APP + 70;   // lParam: std::vector<std::wstring>* (from WM_COPYDATA)
const UINT WM_NOTEPAD_COLOUR = WM_APP + 71; // the part shown to colour (before it is painted)
const UINT WM_NOTEPAD_WORK = WM_APP + 72;   // a step of the work done between the user's input
const UINT_PTR TIMER_KEEP = 1;              // the tabs kept a moment after a change
const UINT_PTR TIMER_WORK = 2;              // (the next step, after the input and painting waiting)
const UINT WM_NOTEPAD_UNREAD = WM_APP + 73; // kept texts that couldn't be read: said, once the window is up
const UINT_PTR TIMER_UNREAD = 3;            // ... and tried again every so often
const UINT WM_NOTEPAD_DROP = WM_APP + 74;   // a restored tab whose text couldn't be read: closed (lParam: its id;
                                            // wParam: say so)
const UINT kUnreadRetryMs = 3000;
const size_t kHead = 1 << 18;               // a big text: this many characters go in at once,
const size_t kPiece = 1 << 16;              // then this many at a time (some 4 ms each)
const long kLexChunk = 1 << 16;             // lexed this many at a time; a lexer state kept every 32K
const long kFar = 1 << 20;                  // a part shown further than this past the known states: lexed from a guess
const UINT kKeepDelayMs = 1000;
const ULONGLONG kMaxBytes = 64ull << 20;    // bigger files aren't opened
const int kTipPlus = 10000;                 // the tooltip of "+" (tabs: their index + 1)
const UINT s_findMsg = RegisterWindowMessageW(FINDMSGSTRINGW);
const wchar_t kAppId[] = L"RadimZatecka.ExplorerPlus.Notepad";     // its own taskbar button
// (declared by tom.h, defined in no import library MinGW has)
const GUID kIID_ITextDocument = { 0x8CC497C0, 0xA1DF, 0x11CE, { 0x80, 0x98, 0x00, 0xAA, 0x00, 0x47, 0xBE, 0x5D } };

enum Cmd : UINT {
    CMD_NEW = 100, CMD_OPEN, CMD_SAVE, CMD_SAVEAS, CMD_SAVEALL, CMD_CLOSETAB, CMD_CLOSEWINDOW,
    CMD_UNDO, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_DELETE,
    CMD_FIND, CMD_FINDNEXT, CMD_FINDPREV, CMD_REPLACE, CMD_GOTO, CMD_SELALL, CMD_TIMEDATE,
    CMD_ZOOMIN, CMD_ZOOMOUT, CMD_ZOOMRESET, CMD_STATUSBAR, CMD_WRAP, CMD_LINENUMBERS, CMD_WHITESPACE,
    CMD_CLOSEOTHERS, CMD_CLOSERIGHT, CMD_COPYPATH, CMD_SHOWFILE,
    CMD_DUPLINE, CMD_DELLINE, CMD_LINEUP, CMD_LINEDOWN, CMD_SORTASC, CMD_SORTDESC, CMD_REVERSE, CMD_UNIQUE,
    CMD_NOEMPTY, CMD_TRIM, CMD_JOIN, CMD_UPPER, CMD_LOWER, CMD_TITLE, CMD_B64ENC, CMD_B64DEC, CMD_URLENC, CMD_URLDEC,
    CMD_FORMAT, CMD_COMPACT,
    CMD_ENC = 200,                          // + TextEncoding
    CMD_EOL = 220,                          // + LineEnding
    CMD_LANGAUTO = 239, CMD_LANG = 240,     // + Lang
};

const wchar_t* const kMenuNames[] = { L"File", L"Edit", L"View" };
enum StatusPart { SP_CARET, SP_CHARS, SP_ZOOM, SP_LANG, SP_EOL, SP_ENC, SP_COUNT };

struct Tab {
    unsigned id = 0;                        // (its own, never another's: a message posted for it)
    HWND edit = nullptr;
    ITextDocument* doc = nullptr;           // its text object model: lines as the file has them
    // restored from the session: its text is read the first time it is shown (the window opens
    // with the one shown, not after all of them); `deferKept`: from its unsaved text kept in
    // `cache`, else from its file
    bool deferred = false, deferKept = false;
    std::wstring path;                      // empty: Untitled, never saved
    TextEncoding enc = TextEncoding::Utf8;
    LineEnding eol = LineEnding::CrLf;
    bool dirty = false;                     // changed since read or saved
    std::wstring cache;                     // the file with its unsaved text, in the notes folder
    bool cacheStale = false;                // changed since that was written
    ULONGLONG fileTime = 0;                 // its file's write time when read or saved
    std::wstring caveat;                    // what saving changes in the file as read (asked once before it is)
    int pendingTop = -1;                    // (restored: the first line to show, once it is shown)
    CHARRANGE pendingSel{ -1, -1 };         // (restored: the selection, once its text is in)
    bool loading = false;                   // (text or look set here: no change of the user's)
    // a big text: its first part goes in at once, the rest a piece at a time between the user's
    // input (and all at once before anything that needs the whole text)
    std::wstring rest;
    size_t restAt = 0;
    long lineCp = -1, lineNo = 0;           // a known line: the one at lineCp (the caret's, last shown)
    long gutCp = -1, gutNo = 0;             // ... and the line numbers' (the top one's, last painted)
    std::vector<std::pair<long, long>> marks;   // (position, its line) every 1M characters, from the top up to a change
    long lines = 1;                         // the file's lines (-1: changed, counted again between the input)
    int zoom = 100;                         // percent
    // the colours: only of the part shown (and a screen around it), lexed from the nearest
    // line start whose lexer state is known
    Lang lang = Lang::None;
    bool langChosen = false;                // (in View > Syntax; else by the file's name)
    std::vector<std::pair<long, int>> states;   // (position, the lexer's state there), ascending
    long changedFrom = 0;                   // the text changed from here since coloured (LONG_MAX: not)
    long selLow = 0;                        // the lowest selection start since then (where a change began)
    long hlA = 0, hlB = -1;                 // the part coloured as it is now (hlB < hlA: none)
    bool guessed = false;                   // that from a guessed state: coloured again once lexed to there
    std::wstring word;                      // double-clicked: it and all others like it marked
    CHARRANGE wordSel{ -1, -1 };
    long wordCount = 0, countAt = -1;       // (counted up to countAt; -1: all)
};

struct Notepad {
    HWND wnd = nullptr, tip = nullptr, find = nullptr;
    int dpi = 96;
    HFONT font = nullptr, glyphs = nullptr, glyphsSmall = nullptr;
    std::vector<std::unique_ptr<Tab>> tabs;
    int active = -1;
    // tabs whose kept unsaved text couldn't be read when they came back (another program had it
    // open): not traded for the file as saved; kept in the session as they were, and tried again
    std::vector<NoteTabState> unread;
    bool wrap = true, statusBar = true, lineNumbers = true, whitespace = false;
    // the line numbers, left of the text: their own window, in the text's font at its zoom
    HWND gutter = nullptr;
    HFONT gutFont = nullptr;
    int gutFontZoom = 0, gutW = 0;
    // the layout
    int rowH = 0, menuH = 0, statusH = 0;
    int tabW = 0, scroll = 0;               // a tab's width; the strip scrolled (more tabs than fit)
    RECT strip{}, plus{}, menus[3]{}, editArea{};
    RECT parts[SP_COUNT]{};                 // the status bar's
    int tips = 0;                           // tab tooltips registered
    // the mouse
    int hotTab = -1, hotClose = -1, hotMenu = -1, openMenu = -1;
    bool hotPlus = false, tracking = false;
    int dragTab = -1, dragX = 0;
    bool dragging = false;
    int wheelV = 0, wheelH = 0;             // the wheel's turn not scrolled yet (a fine wheel's parts of a notch)
    // the title bar is the tab row: its minimize / maximize / close buttons (0, 1, 2)
    bool activeWnd = true;
    int capHot = -1, capPressed = -1;
    bool colourPosted = false, workPosted = false;
    // find and replace
    FINDREPLACEW fr{};
    bool replacing = false;
    DWORD findFlags = FR_DOWN;
    wchar_t findText[512] = L"", replaceText[512] = L"";
};

Notepad* s_np = nullptr;

int px(const Notepad& n, int v) { return MulDiv(v, n.dpi, 96); }
Tab* cur(Notepad& n) { return n.active >= 0 && n.active < (int)n.tabs.size() ? n.tabs[n.active].get() : nullptr; }

// The window class. Tests run with a settings file of their own: their Notepad has a class
// of its own too, so a test never reaches the user's real one (as the player's).
const std::wstring& notepad_class() {
    static const std::wstring name = [] {
        std::wstring n = L"ExplorerPlusNotepad";
        WCHAR ini[MAX_PATH * 2]{};
        if (GetEnvironmentVariableW(L"EXPLORERPLUS_SETTINGS", ini, ARRAYSIZE(ini)) && ini[0]) {
            unsigned h = 2166136261u;
            for (WCHAR* c = ini; *c; ++c) { h ^= (unsigned)towlower(*c); h *= 16777619u; }
            WCHAR b[16];
            swprintf_s(b, L".%08x", h);
            n += b;
        }
        return n;
    }();
    return name;
}

// One Notepad window across processes: they would share the kept notes and each write its
// own session over the other's. Start: held while a process looks for the window and makes
// its own. Owner: held by the process whose window is open.
static HANDLE s_ownerMx = nullptr;
static std::wstring instance_mutex(const wchar_t* what) {
    return L"Local\\" + notepad_class() + L"." + what;
}
static void release_instance() {
    if (!s_ownerMx) return;
    ReleaseMutex(s_ownerMx);
    CloseHandle(s_ownerMx);
    s_ownerMx = nullptr;
}

// ------------------------------------------------------------------
// files
// ------------------------------------------------------------------
ULONGLONG write_time(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
}

// what saving a file as read would change in it (empty: nothing)
std::wstring caveat_of(const TextFile& f) {
    std::wstring s;
    if (f.lossy) s += std::wstring(L"Some bytes of this file aren't text in its encoding (") + encoding_name(f.enc) + L"): saving changes them.\n";
    if (f.mixedEol) s += std::wstring(L"This file mixes line endings: saving makes them all ") + line_ending_name(f.eol) + L".\n";
    return s;
}

enum class Read { Ok, Failed, TooBig };

// (`limit`: the kept text of a file at the limit is UTF-16, twice its size)
Read read_bytes(const std::wstring& path, std::string& out, ULONGLONG limit = kMaxBytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Read::Failed;
    LARGE_INTEGER size{};
    Read r = Read::Failed;
    if (GetFileSizeEx(h, &size)) {
        if ((ULONGLONG)size.QuadPart > limit) {
            r = Read::TooBig;
        } else {
            out.resize((size_t)size.QuadPart);
            DWORD got = 0;
            if (out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size())) r = Read::Ok;
        }
    }
    CloseHandle(h);
    return r;
}

// ours (the session, kept text): written beside, then moved over
bool write_own(const std::wstring& path, std::string_view data, std::string_view more = {}) {
    std::wstring tmp = path + L".tmp";
    return write_new(tmp, data, more) == Wrote::Ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// next to the settings file (tests: theirs)
std::wstring notes_dir() {
    std::wstring ini = settings_file_path();
    size_t slash = ini.find_last_of(L"\\/");
    if (ini.empty() || slash == std::wstring::npos) return {};
    std::wstring dir = ini.substr(0, slash) + L"\\Notepad";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring tab_name(const Tab& t) { return t.path.empty() ? L"Untitled" : PathFindFileNameW(t.path.c_str()); }

std::wstring grouped(long long v) {
    wchar_t sep[8] = L",";
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, sep, ARRAYSIZE(sep));
    std::wstring d = std::to_wstring(v), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += sep;
        out += d[i];
    }
    return out;
}

// ------------------------------------------------------------------
// the text box (RichEdit: rich text, all of it put in, pasted and read as plain)
// ------------------------------------------------------------------
long text_length(HWND e) {
    GETTEXTLENGTHEX gl{ GTL_NUMCHARS | GTL_PRECISE, 1200 };
    return (long)SendMessageW(e, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
}

CHARRANGE selection(HWND e) {
    CHARRANGE cr{};
    SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&cr);
    return cr;
}

void select(HWND e, long a, long b) {
    CHARRANGE cr{ a, b };
    SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&cr);
}

// the text's font: Cascadia Mono where Windows has it (11 and later), else Consolas
const wchar_t* mono_face() {
    static const wchar_t* face = [] {
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Cascadia Mono");
        bool found = false;
        HDC dc = GetDC(nullptr);
        EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int { *(bool*)p = true; return 0; },
                            (LPARAM)&found, 0);
        ReleaseDC(nullptr, dc);
        return found ? L"Cascadia Mono" : L"Consolas";
    }();
    return face;
}

// its characters' advance, in ems (for the tab stops)
double mono_advance() {
    static const double em = [] {
        LOGFONTW lf{};
        lf.lfHeight = -1000;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, mono_face());
        HFONT f = CreateFontIndirectW(&lf);
        HDC dc = GetDC(nullptr);
        HGDIOBJ of = SelectObject(dc, f);
        SIZE sz{};
        GetTextExtentPoint32W(dc, L"0", 1, &sz);
        SelectObject(dc, of);
        ReleaseDC(nullptr, dc);
        DeleteObject(f);
        return sz.cx > 0 ? sz.cx / 1000.0 : 0.55;
    }();
    return em;
}

// its look: the palette's colours, the mono font 11 pt at the window's DPI (the control goes by
// the system's), tabs of 4, the tab's zoom, wrapping; not undoable
void style_edit(Notepad& n, Tab& t) {
    t.loading = true;
    if (t.doc) t.doc->Undo(tomSuspend, nullptr);
    const Palette& c = pal();
    SendMessageW(t.edit, EM_SETBKGNDCOLOR, 0, c.content);
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_CHARSET | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT;
    double scale = (double)n.dpi / GetDpiForSystem();
    cf.yHeight = (LONG)(220 * scale);
    cf.crTextColor = c.text;
    cf.bCharSet = DEFAULT_CHARSET;
    wcscpy_s(cf.szFaceName, mono_face());
    SendMessageW(t.edit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
    SendMessageW(t.edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    if (t.doc) t.doc->SetDefaultTabStop((float)(4 * mono_advance() * 11 * scale));
    SendMessageW(t.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(px(n, 8), px(n, 8)));
    SendMessageW(t.edit, EM_SETZOOM, t.zoom, 100);
    SendMessageW(t.edit, EM_SETTARGETDEVICE, 0, n.wrap ? 0 : 1);
    theme_style_control(t.edit);
    if (t.doc) t.doc->Undo(tomResume, nullptr);
    t.loading = false;
    t.hlA = 0;
    t.hlB = -1;                             // (SCF_ALL took the colours)
}

void schedule_work(Notepad& n);

// The text (taken: `text` is left empty). A big one's first part only: the box takes some 60 ms
// a megabyte, so the rest follows a piece at a time, between the user's input.
void set_text(Notepad& n, Tab& t, std::wstring& text) {
    t.states.clear();
    t.changedFrom = 0;
    t.guessed = false;
    t.word.clear();
    t.wordSel = { -1, -1 };
    t.countAt = -1;
    t.lineCp = t.gutCp = -1;
    t.marks.clear();
    t.lines = 1 + (long)std::count(text.begin(), text.end(), L'\r');
    t.pendingSel = { -1, -1 };
    t.rest.clear();
    t.restAt = 0;
    size_t head = text.size();
    if (head > kHead) {
        head = kHead;
        if (IS_HIGH_SURROGATE(text[head - 1])) ++head;
        t.rest.swap(text);
        t.restAt = head;
    }
    std::wstring& s = t.rest.empty() ? text : t.rest;
    wchar_t cut = head < s.size() ? s[head] : 0;
    if (cut) s[head] = 0;
    t.loading = true;
    SETTEXTEX st{ ST_DEFAULT | 0x20 /*ST_PLAINTEXTONLY: a file starting {\rtf is text too*/, 1200 };
    SendMessageW(t.edit, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)s.c_str());
    SendMessageW(t.edit, EM_EMPTYUNDOBUFFER, 0, 0);
    t.loading = false;
    if (cut) s[head] = cut;
    text.clear();
    style_edit(n, t);
    if (!t.rest.empty()) schedule_work(n);
}

// [a, b) replaced by s: as text (EM_REPLACESEL would read "{\rtf..." as RTF), one step to undo
void put_text(Tab& t, long a, long b, const std::wstring& s) {
    ITextRange* r = nullptr;
    if (!t.doc || FAILED(t.doc->Range(a, b, &r)) || !r) return;
    BSTR bs = SysAllocStringLen(s.data(), (UINT)s.size());
    r->SetText(bs);
    SysFreeString(bs);
    r->Release();
}

std::wstring text_range(HWND e, long a, long b) {
    if (b <= a) return {};
    std::wstring s((size_t)(b - a) + 1, L'\0');
    TEXTRANGEW tr{ { a, b }, s.data() };
    LRESULT got = SendMessageW(e, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    s.resize(got > 0 ? (size_t)got : 0);
    return s;
}

// [a, b) through the text object model (b -1: to the end): five times quicker than the box's
// own messages for much of it (20 ms for 50 MB)
std::wstring doc_text(Tab& t, long a, long b) {
    std::wstring s;
    if (b < 0) b = text_length(t.edit);
    ITextRange* r = nullptr;
    if (b > a && t.doc && SUCCEEDED(t.doc->Range(a, b, &r)) && r) {
        BSTR bs = nullptr;
        if (SUCCEEDED(r->GetText(&bs)) && bs) {
            s.assign(bs, SysStringLen(bs));
            SysFreeString(bs);
        }
        r->Release();
    }
    return s;
}

// The file's line (not as wrapped) at cp, from 1. Unwrapped, the box's own lines; wrapped,
// counted from a line known (knownCp's: kept here, the last asked for) - the text object counts
// from the top: 70 ms at the end of 50 MB.
long file_line(const Notepad& n, Tab& t, long cp, long& knownCp, long& knownNo) {
    if (!n.wrap || !t.doc) return (long)SendMessageW(t.edit, EM_EXLINEFROMCHAR, 0, cp) + 1;
    // (or from the nearest mark before it, when that is nearer)
    auto m = std::upper_bound(t.marks.begin(), t.marks.end(), std::make_pair(cp, LONG_MAX));
    if (m != t.marks.begin() && (knownCp < 0 || cp - (m - 1)->first < std::abs(cp - knownCp))) {
        knownCp = (m - 1)->first;
        knownNo = (m - 1)->second;
    }
    if (knownCp >= 0 && std::abs(cp - knownCp) <= (4 << 20)) {
        std::wstring s = doc_text(t, std::min(cp, knownCp), std::max(cp, knownCp));
        long k = (long)std::count(s.begin(), s.end(), L'\r');
        knownNo += cp >= knownCp ? k : -k;
    } else {
        ITextRange* r = nullptr;
        knownNo = 1;
        if (SUCCEEDED(t.doc->Range(cp, cp, &r)) && r) {
            if (FAILED(r->GetIndex(tomParagraph, &knownNo))) knownNo = 1;
            r->Release();
        }
    }
    knownCp = cp;
    return knownNo;
}

// The caret's line and column, by the file's lines (not as wrapped)
void caret_place(const Notepad& n, Tab& t, long& line, long& col) {
    CHARRANGE cr = selection(t.edit);
    long cp = cr.cpMax;
    line = 1;
    col = cp + 1;
    if (!t.doc) return;
    ITextSelection* sel = nullptr;
    if (SUCCEEDED(t.doc->GetSelection(&sel)) && sel) {
        long flags = 0;
        if (SUCCEEDED(sel->GetFlags(&flags)) && (flags & tomSelStartActive)) cp = cr.cpMin;
        sel->Release();
    }
    ITextRange* r = nullptr;
    if (FAILED(t.doc->Range(cp, cp, &r)) || !r) return;
    long start = 0;
    if (SUCCEEDED(r->StartOf(tomParagraph, tomMove, nullptr)) && SUCCEEDED(r->GetStart(&start))) col = cp - start + 1;
    r->Release();
    line = file_line(n, t, cp, t.lineCp, t.lineNo);
}

// ------------------------------------------------------------------
// the colours: the part shown and a screen around it, set through the text object model (not
// undoable, not a change). Lexed from the nearest line start before it
// whose lexer state is known: the states are kept, every 32K characters, up to a change.
// ------------------------------------------------------------------
COLORREF tok_colour(Tok t) {
    // (as Visual Studio Code's Dark+ and Light+)
    static const COLORREF dark[] = {
        0, RGB(106, 153, 85), RGB(206, 145, 120), RGB(181, 206, 168), RGB(86, 156, 214), RGB(78, 201, 176),
        RGB(220, 220, 170), RGB(197, 134, 192), RGB(86, 156, 214), RGB(156, 220, 254), RGB(156, 220, 254),
        RGB(86, 156, 214), RGB(156, 220, 254), RGB(86, 156, 214), RGB(215, 186, 125), RGB(78, 201, 176),
        RGB(129, 184, 139), RGB(244, 135, 113),
    };
    static const COLORREF light[] = {
        0, RGB(0, 128, 0), RGB(163, 21, 21), RGB(9, 134, 88), RGB(0, 0, 255), RGB(38, 127, 153),
        RGB(121, 94, 38), RGB(175, 0, 219), RGB(128, 0, 0), RGB(229, 0, 0), RGB(4, 81, 165),
        RGB(128, 0, 0), RGB(0, 16, 128), RGB(0, 0, 255), RGB(128, 0, 128), RGB(0, 102, 204),
        RGB(9, 134, 88), RGB(163, 21, 21),
    };
    static_assert(ARRAYSIZE(dark) == (size_t)Tok::Count && ARRAYSIZE(light) == (size_t)Tok::Count);
    return t == Tok::Plain || t >= Tok::Count ? pal().text : (g_dark ? dark : light)[(size_t)t];
}

COLORREF mark_colour() { return g_dark ? RGB(98, 82, 28) : RGB(255, 226, 140); }

void request_colour(Notepad& n) {
    if (n.colourPosted || !n.wnd) return;
    n.colourPosted = true;
    PostMessageW(n.wnd, WM_NOTEPAD_COLOUR, 0, 0);
}

// to colour again; `relex`: from the start (a new text or language)
void recolour(Notepad& n, Tab& t, bool relex) {
    if (relex) {
        t.states.clear();
        t.changedFrom = 0;
    }
    t.hlA = 0;
    t.hlB = -1;
    request_colour(n);
}

bool word_char(wchar_t c) { return iswalnum(c) || c == L'_'; }
long line_start(Tab& t, long cp);

void colour(Tab& t) {
    if (!t.doc || !IsWindowVisible(t.edit)) return;
    RECT rc{};
    GetClientRect(t.edit, &rc);
    POINTL tl{ 0, 0 }, br{ rc.right, rc.bottom };
    long a = (long)SendMessageW(t.edit, EM_CHARFROMPOS, 0, (LPARAM)&tl);
    long b = (long)SendMessageW(t.edit, EM_CHARFROMPOS, 0, (LPARAM)&br);
    long len = text_length(t.edit);
    long more = std::max(b - a, 2000L);
    a = std::max(0L, a - more);
    b = std::min(len, b + more);
    bool changed = t.changedFrom != LONG_MAX;
    if (!changed && a >= t.hlA && b <= t.hlB) return;
    if (changed) {
        while (!t.states.empty() && t.states.back().first > t.changedFrom) t.states.pop_back();
        t.changedFrom = LONG_MAX;
    }
    t.selLow = selection(t.edit).cpMin;
    if (t.states.empty()) t.states.push_back({ 0, 0 });
    std::vector<SyntaxSpan> spans, piece;
    t.guessed = false;
    if (t.lang != Lang::None) {
        auto it = std::upper_bound(t.states.begin(), t.states.end(), std::make_pair(a, INT_MAX)) - 1;
        long from = it->first;
        int state = it->second;
        if (a - from > kFar) {                      // (from a line a little before, as if nothing were open there)
            from = line_start(t, std::max(0L, a - 4096));
            // (one huge line, minified JSON or JS: its start is as far as the states were;
            // from a little before what's shown, mid-line, rather than all of it each scroll)
            if (a - from > kFar) from = std::max(0L, a - 4096);
            state = 0;
            t.guessed = true;
        }
        const long kChunk = kLexChunk;
        while (from < b) {
            std::wstring s = doc_text(t, from, std::min(len, from + kChunk));
            if (s.empty()) break;
            if (from + (long)s.size() < len) {              // (whole lines)
                size_t cut = s.find_last_of(L'\r');
                if (cut != std::wstring::npos && cut > 0) s.resize(cut + 1);
            }
            bool wanted = from + (long)s.size() > a;
            piece.clear();
            state = syntax_lex(t.lang, s.c_str(), s.size(), state, wanted ? &piece : nullptr);
            for (const SyntaxSpan& p : piece) {
                long pa = std::max(a, from + (long)p.start), pb = std::min(b, from + (long)(p.start + p.len));
                if (pb > pa) spans.push_back({ (uint32_t)pa, (uint32_t)(pb - pa), p.tok });
            }
            from += (long)s.size();
            if (!t.guessed && s.back() == L'\r' && from >= t.states.back().first + kChunk / 2) t.states.push_back({ from, state });
        }
    }
    // the double-clicked word's others
    std::vector<std::pair<long, long>> found;
    if (!t.word.empty()) {
        long base = std::max(0L, a - 1);
        std::wstring s = text_range(t.edit, base, std::min(len, b + 1));
        bool whole = std::all_of(t.word.begin(), t.word.end(), word_char);
        for (size_t p = s.find(t.word); p != std::wstring::npos; p = s.find(t.word, p + 1)) {
            size_t q = p + t.word.size();
            if (whole && ((p > 0 && word_char(s[p - 1])) || (q < s.size() && word_char(s[q])))) continue;
            found.push_back({ base + (long)p, base + (long)q });
        }
    }
    t.loading = true;
    t.doc->Undo(tomSuspend, nullptr);
    long frozen = 0;
    t.doc->Freeze(&frozen);
    ITextRange* r = nullptr;
    if (SUCCEEDED(t.doc->Range(a, b, &r)) && r) {
        auto paint = [&](long x, long y, long fore, long back) {
            r->SetRange(x, y);
            ITextFont* f = nullptr;
            if (SUCCEEDED(r->GetFont(&f)) && f) {
                if (fore != -1) f->SetForeColor(fore);
                if (back != -1) f->SetBackColor(back);
                f->Release();
            }
        };
        paint(a, b, (long)pal().text, tomAutoColor);
        for (const SyntaxSpan& sp : spans) paint((long)sp.start, (long)(sp.start + sp.len), (long)tok_colour(sp.tok), -1);
        for (auto& f : found) paint(f.first, f.second, -1, (long)mark_colour());
        r->Release();
    }
    t.doc->Unfreeze(&frozen);
    t.doc->Undo(tomResume, nullptr);
    t.loading = false;
    t.hlA = a;
    t.hlB = b;
}

void invalidate_status(Notepad& n);

void clear_word(Notepad& n, Tab& t) {
    if (t.word.empty()) return;
    t.word.clear();
    t.wordSel = { -1, -1 };
    t.wordCount = 0;
    t.countAt = -1;
    recolour(n, t, false);
    invalidate_status(n);
}

// a double-click: the word (without the space after it), and every other one like it marked
void mark_word(Notepad& n, Tab& t) {
    CHARRANGE cr = selection(t.edit);
    std::wstring w = text_range(t.edit, cr.cpMin, cr.cpMax);
    size_t e = w.size();
    while (e && (w[e - 1] == L' ' || w[e - 1] == L'\t')) --e;
    w.resize(e);
    if (w.empty() || w.size() > 256 || w.find(L'\r') != std::wstring::npos) { clear_word(n, t); return; }
    if ((long)e < cr.cpMax - cr.cpMin) select(t.edit, cr.cpMin, cr.cpMin + (long)e);
    t.word = w;
    t.wordSel = { cr.cpMin, cr.cpMin + (long)e };
    t.wordCount = 0;                        // (counted a piece at a time: a big text has a million)
    t.countAt = 0;
    schedule_work(n);
    recolour(n, t, false);
    invalidate_status(n);
}

// the file's lines (not as wrapped): where the one at cp starts, where it ends (at its '\r')
long line_start(Tab& t, long cp) {
    long s = cp;
    ITextRange* r = nullptr;
    if (t.doc && SUCCEEDED(t.doc->Range(cp, cp, &r)) && r) {
        r->StartOf(tomParagraph, tomMove, nullptr);
        r->GetStart(&s);
        r->Release();
    }
    return s;
}

long line_end(Tab& t, long cp) {
    long len = text_length(t.edit), e = len;
    ITextRange* r = nullptr;
    if (t.doc && SUCCEEDED(t.doc->Range(cp, cp, &r)) && r) {
        r->EndOf(tomParagraph, tomMove, nullptr);
        r->GetStart(&e);
        r->Release();
    }
    if (e > len) e = len;
    else if (e > cp && text_range(t.edit, e - 1, e) == L"\r") --e;
    return std::max(e, cp);
}

// ------------------------------------------------------------------
// the work between the user's input: a big text going in, the double-clicked word's others
// counted, the text lexed ahead (its states kept: a jump far down is coloured at once). A step
// of some 12 ms at a time; the next one posted, or after the input and painting waiting.
// ------------------------------------------------------------------
double now_ms() {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / freq;
}

void schedule_work(Notepad& n) {
    if (n.workPosted || !n.wnd) return;
    n.workPosted = true;
    PostMessageW(n.wnd, WM_NOTEPAD_WORK, 0, 0);
}

// the restored selection and first line, once their text is in
void apply_pending(Notepad& n, Tab& t) {
    bool all = t.rest.empty();
    if (t.pendingSel.cpMin >= 0 && (all || std::max(t.pendingSel.cpMin, t.pendingSel.cpMax) <= text_length(t.edit))) {
        CHARRANGE cr = t.pendingSel;
        t.pendingSel = { -1, -1 };
        select(t.edit, cr.cpMin, cr.cpMax);
    }
    if (t.pendingTop >= 0 && &t == cur(n) && (all || SendMessageW(t.edit, EM_GETLINECOUNT, 0, 0) > t.pendingTop + 200)) {
        int now = (int)SendMessageW(t.edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessageW(t.edit, EM_LINESCROLL, 0, t.pendingTop - now);
        t.pendingTop = -1;
    }
}

// more of a big text in, a piece at a time till `until` (at least one); true: more to come
bool load_more(Notepad& n, Tab& t, double until) {
    if (t.rest.empty() || !t.doc) return false;
    t.loading = true;
    t.doc->Undo(tomSuspend, nullptr);
    do {
        size_t end = std::min(t.rest.size(), t.restAt + kPiece);
        if (end < t.rest.size() && IS_HIGH_SURROGATE(t.rest[end - 1])) ++end;
        long len = text_length(t.edit);
        ITextRange* r = nullptr;
        if (SUCCEEDED(t.doc->Range(len, len, &r)) && r) {
            BSTR bs = SysAllocStringLen(t.rest.data() + t.restAt, (UINT)(end - t.restAt));
            r->SetText(bs);
            SysFreeString(bs);
            ITextFont* f = nullptr;             // (the text's colour, not the last character's: a keyword's)
            if (SUCCEEDED(r->GetFont(&f)) && f) {
                f->SetForeColor((long)pal().text);
                f->SetBackColor(tomAutoColor);
                f->Release();
            }
            r->Release();
        }
        t.restAt = end;
    } while (t.restAt < t.rest.size() && now_ms() < until);
    t.doc->Undo(tomResume, nullptr);
    t.loading = false;
    if (t.restAt >= t.rest.size()) {
        std::wstring().swap(t.rest);
        t.restAt = 0;
    }
    apply_pending(n, t);
    if (&t == cur(n)) {
        request_colour(n);
        invalidate_status(n);
    }
    return !t.rest.empty();
}

// all of a big text in, before what needs all of it (saving, searching, editing)
void finish_loading(Notepad& n, Tab& t) {
    if (t.rest.empty()) return;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    load_more(n, t, 1e300);
    SetCursor(old);
}

// the double-clicked word's others counted on (as the marks take them: whole words, _ in one)
bool count_more(Notepad& n, Tab& t, double until) {
    if (t.countAt < 0 || t.word.empty()) return false;
    long len = text_length(t.edit), wl = (long)t.word.size();
    bool whole = std::all_of(t.word.begin(), t.word.end(), word_char);
    while (t.countAt < len) {
        long end = std::min(len, t.countAt + (1L << 20)), base = std::max(0L, t.countAt - 1);
        std::wstring s = doc_text(t, base, std::min(len, end + wl));
        for (size_t p = s.find(t.word, (size_t)(t.countAt - base)); p != std::wstring::npos && base + (long)p < end;
             p = s.find(t.word, p + (size_t)wl)) {
            size_t q = p + (size_t)wl;
            if (!whole || ((p == 0 || !word_char(s[p - 1])) && (q >= s.size() || !word_char(s[q])))) ++t.wordCount;
        }
        t.countAt = end;
        if (now_ms() >= until) break;
    }
    if (t.countAt >= len && t.rest.empty()) t.countAt = -1;
    invalidate_status(n);
    return t.countAt >= 0 && t.countAt < len;
}

// lexed on from the last state known, its states kept; a part coloured from a guess, again
bool lex_ahead(Notepad& n, Tab& t, double until) {
    if (t.lang == Lang::None || !t.doc) return false;
    while (!t.states.empty() && t.states.back().first > t.changedFrom) t.states.pop_back();
    if (t.states.empty()) t.states.push_back({ 0, 0 });
    long len = text_length(t.edit);
    bool more = true;
    while (more) {
        long from = t.states.back().first;
        std::wstring s = len - from >= kLexChunk ? doc_text(t, from, from + kLexChunk) : L"";
        size_t cut = s.find_last_of(L'\r');
        if (cut == std::wstring::npos) { more = false; break; }    // (the end; or a line of 64K: lexed when shown)
        s.resize(cut + 1);
        t.states.push_back({ from + (long)s.size(), syntax_lex(t.lang, s.c_str(), s.size(), t.states.back().second, nullptr) });
        if (now_ms() >= until) break;
    }
    if (t.guessed && t.states.back().first + kFar >= t.hlA) recolour(n, t, false);
    return more;
}

void refresh_gutter(Notepad& n);

// its lines counted on from the last mark (numbered: a jump far down, wrapped, numbered at once),
// and all of them after a change (the numbers' width)
bool mark_ahead(Notepad& n, Tab& t, double until) {
    const long kMark = 1 << 20;
    if (!n.lineNumbers || !t.doc || !t.rest.empty()) return false;
    long len = text_length(t.edit);
    if (t.marks.empty()) t.marks.push_back({ 0, 1 });
    for (;;) {
        auto [cp, no] = t.marks.back();
        if (len - cp <= kMark) {
            if (t.lines < 0) {
                std::wstring s = doc_text(t, cp, len);
                t.lines = no + (long)std::count(s.begin(), s.end(), L'\r');
                refresh_gutter(n);
            }
            return false;
        }
        std::wstring s = doc_text(t, cp, cp + kMark);
        t.marks.push_back({ cp + kMark, no + (long)std::count(s.begin(), s.end(), L'\r') });
        if (now_ms() >= until) return true;
    }
}

// a step: the active tab's text in, its word counted, it lexed ahead and its lines counted;
// else another's text in
bool do_work(Notepad& n) {
    double until = now_ms() + 12;
    Tab* t = cur(n);
    if (t && (load_more(n, *t, until) || count_more(n, *t, until) || lex_ahead(n, *t, until) || mark_ahead(n, *t, until)))
        return true;
    for (auto& x : n.tabs)
        if (!x->rest.empty()) {
            load_more(n, *x, until);
            return true;
        }
    return false;
}

void work_step(Notepad& n) {
    KillTimer(n.wnd, TIMER_WORK);
    n.workPosted = false;
    if (!do_work(n)) return;
    n.workPosted = true;
    if (HIWORD(GetQueueStatus(QS_INPUT | QS_PAINT))) SetTimer(n.wnd, TIMER_WORK, USER_TIMER_MINIMUM, nullptr);
    else PostMessageW(n.wnd, WM_NOTEPAD_WORK, 0, 0);
}

// ------------------------------------------------------------------
// the line numbers (a window of their own, left of the text) and the whitespace shown (drawn
// over the text: after the box paints, and after it redraws a line of its own accord)
// ------------------------------------------------------------------
void layout(Notepad& n);

// the text's em at the tab's zoom, in pixels
double em_px(const Notepad& n, const Tab& t) { return 11.0 * n.dpi / 72 * t.zoom / 100; }

// a line shown (as wrapped): its first character, the next line's, its top and height
struct LineShown { long cp, end; int y, h; };

std::vector<LineShown> lines_shown(const Notepad& n, Tab& t) {
    std::vector<LineShown> v;
    RECT rc{};
    GetClientRect(t.edit, &rc);
    // (not EM_GETLINECOUNT: a big text, wrapped, would be laid out to its end first)
    long len = text_length(t.edit);
    long l = (long)SendMessageW(t.edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    long cp = (long)SendMessageW(t.edit, EM_LINEINDEX, l, 0);
    for (; cp >= 0; ++l) {
        POINTL p{};
        SendMessageW(t.edit, EM_POSFROMCHAR, (WPARAM)&p, cp);
        if (!v.empty()) v.back().h = std::max(1, (int)p.y - v.back().y);
        if (p.y >= rc.bottom) break;
        long next = (long)SendMessageW(t.edit, EM_LINEINDEX, l + 1, 0);
        if (next <= cp) next = -1;
        v.push_back({ cp, next < 0 ? len : next, (int)p.y, 0 });
        cp = next;
    }
    if (!v.empty() && !v.back().h) v.back().h = v.size() > 1 ? v[v.size() - 2].h : (int)(em_px(n, t) * 1.3);
    return v;
}

bool para_start(Tab& t, long cp) { return cp == 0 || text_range(t.edit, cp - 1, cp) == L"\r"; }

HFONT gutter_font(Notepad& n, const Tab& t) {
    if (n.gutFont && n.gutFontZoom == t.zoom * 1000 + n.dpi) return n.gutFont;
    if (n.gutFont) DeleteObject(n.gutFont);
    LOGFONTW lf{};
    lf.lfHeight = -(LONG)(em_px(n, t) + 0.5);
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, mono_face());
    n.gutFont = CreateFontIndirectW(&lf);
    n.gutFontZoom = t.zoom * 1000 + n.dpi;
    return n.gutFont;
}

// wide enough for the file's lines, 3 digits at least (being counted after a change: as it was)
int gutter_width(Notepad& n, Tab& t) {
    if (t.lines < 0 && n.gutW) return n.gutW;
    int digits = 1;
    for (long v = t.lines; v >= 10; v /= 10) ++digits;
    return (int)(std::max(3, digits) * mono_advance() * em_px(n, t) + 0.5) + px(n, 20);
}

// the line numbers' width changed (more lines, the zoom): laid out again
void refresh_gutter(Notepad& n) {
    if (!n.gutter) return;
    Tab* t = cur(n);
    int w = n.lineNumbers && t ? gutter_width(n, *t) : 0;
    if (w != n.gutW) layout(n);
    else if (w) InvalidateRect(n.gutter, nullptr, FALSE);
}

void paint_gutter(Notepad& n) {
    double t0 = perf_on() ? perf_now() : 0;
    struct Done { double t0; ~Done() { if (perf_on()) perf_paint("notepad line numbers", perf_now() - t0); } } done{ t0 };
    BufferedPaint bp(n.gutter);
    const Palette& c = pal();
    fill_rect(bp.dc, bp.rc, c.content);
    Tab* t = cur(n);
    if (!t) return;
    // the edit box's lines, where it has them (the windows' tops are the same)
    RECT er{};
    SendMessageW(t->edit, EM_GETRECT, 0, (LPARAM)&er);
    IntersectClipRect(bp.dc, 0, er.top, bp.rc.right, er.bottom);
    std::vector<LineShown> v = lines_shown(n, *t);
    if (v.empty()) return;
    long caret = 0, col = 0;
    caret_place(n, *t, caret, col);
    // (the first one shown: its file line, counted from the last painted; the others from it)
    long no = file_line(n, *t, v[0].cp, t->gutCp, t->gutNo) - (para_start(*t, v[0].cp) ? 1 : 0);
    HGDIOBJ of = SelectObject(bp.dc, gutter_font(n, *t));
    SetBkMode(bp.dc, TRANSPARENT);
    // on the text's baseline: where the box has it below a line's top (its leading is its own)
    TEXTMETRICW tm{};
    GetTextMetricsW(bp.dc, &tm);
    int base = tm.tmAscent;
    const LineShown& probe = v.size() > 1 && v[0].y < 0 ? v[1] : v[0];
    ITextRange* r = nullptr;
    if (t->doc && SUCCEEDED(t->doc->Range(probe.cp, probe.cp, &r)) && r) {
        long x = 0, y = 0;
        if (r->GetPoint(tomStart | TA_BASELINE | TA_LEFT, &x, &y) == S_OK) {
            POINT p{ x, y };
            ScreenToClient(t->edit, &p);
            if (p.y > probe.y && p.y <= probe.y + probe.h) base = p.y - probe.y;
        }
        r->Release();
    }
    SetTextAlign(bp.dc, TA_RIGHT | TA_BASELINE);
    int right = bp.rc.right - px(n, 8);
    for (const LineShown& l : v) {
        if (!para_start(*t, l.cp)) continue;
        ++no;
        SetTextColor(bp.dc, no == caret ? c.text : blend(c.content, c.text, 0.5));
        std::wstring s = std::to_wstring(no);
        ExtTextOutW(bp.dc, right, l.y + base, 0, nullptr, s.c_str(), (UINT)s.size(), nullptr);
    }
    SelectObject(bp.dc, of);
}

// a click on a number: its line selected (with its end, as a drag over it would)
void gutter_click(Notepad& n, int y) {
    Tab* t = cur(n);
    if (!t) return;
    POINTL p{ 0, y };
    long cp = (long)SendMessageW(t->edit, EM_CHARFROMPOS, 0, (LPARAM)&p);
    long a = line_start(*t, cp), b = line_end(*t, cp), len = text_length(t->edit);
    select(t->edit, a, std::min(len, b + 1));
    SetFocus(t->edit);
}

LRESULT CALLBACK gutter_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* n = (Notepad*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
            if (n) { paint_gutter(*n); return 0; }
            break;
        case WM_LBUTTONDOWN:
            if (n) gutter_click(*n, GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            if (Tab* t = n ? cur(*n) : nullptr) return SendMessageW(t->edit, msg, wp, lp);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND create_gutter(Notepad& n) {
    static const wchar_t* cls = [] {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = g_hinst;
        wc.lpfnWndProc = gutter_proc;
        wc.lpszClassName = L"ExplorerPlusNotepadGutter";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        return wc.lpszClassName;
    }();
    return CreateWindowExW(0, cls, nullptr, WS_CHILD, 0, 0, 0, 0, n.wnd, nullptr, g_hinst, &n);
}

// the spaces (dots) and tabs (arrows) shown, over the text as the box painted it; the same
// pixels each time, so drawn again whenever the box may have drawn over them
void draw_whitespace(Notepad& n, Tab& t) {
    if (!n.whitespace || &t != cur(n) || !IsWindowVisible(t.edit)) return;
    // (a paint of the box to come draws them: not now as well)
    if (GetUpdateRect(t.edit, nullptr, FALSE)) return;
    double t0 = perf_on() ? perf_now() : 0;
    struct Done { double t0; ~Done() { if (perf_on()) perf_paint("notepad whitespace", perf_now() - t0); } } done{ t0 };
    RECT fr{};
    SendMessageW(t.edit, EM_GETRECT, 0, (LPARAM)&fr);
    std::vector<LineShown> v = lines_shown(n, t);
    if (v.empty()) return;
    auto x_of = [&](long cp, int y, int& x) {
        POINTL p{};
        SendMessageW(t.edit, EM_POSFROMCHAR, (WPARAM)&p, cp);
        x = (int)p.x;
        return std::abs((int)p.y - y) < 2;             // (on this line: not wrapped to the next)
    };
    // a space's width as the box lays it out (the font is monospaced: as any letter's), measured
    // on a line shown; a run of spaces is asked where it starts, its dots spaced by it
    double sw = mono_advance() * em_px(n, t);
    for (const LineShown& l : v) {
        std::wstring s = text_range(t.edit, l.cp, std::min(l.end, l.cp + 64));
        long m = 0;
        while (m < (long)s.size() && s[(size_t)m] >= L' ' && s[(size_t)m] < 0x2E80) ++m;
        int x0 = 0, x1 = 0;
        if (m >= 8 && x_of(l.cp, l.y, x0) && x_of(l.cp + m, l.y, x1) && x1 > x0) {
            sw = (double)(x1 - x0) / (double)m;
            break;
        }
    }
    HDC dc = GetDC(t.edit);
    if (!dc) return;
    HideCaret(t.edit);
    int saved = SaveDC(dc);
    IntersectClipRect(dc, fr.left, fr.top, fr.right, fr.bottom);
    COLORREF col = blend(pal().content, pal().text, 0.4);
    int dot = std::max(1, (int)(sw * 0.22 + 0.5)), pw = std::max(1, (int)(sw * 0.12 + 0.5));
    HBRUSH br = CreateSolidBrush(col);
    HPEN pen = CreatePen(PS_SOLID, pw, col);
    HGDIOBJ op = SelectObject(dc, pen);
    for (const LineShown& l : v) {
        // (the part in view only: an unwrapped line may be long, scrolled sideways)
        int mid = l.y + l.h / 2;
        POINTL a{ fr.left, mid }, b{ fr.right, mid };
        long from = std::max(l.cp, (long)SendMessageW(t.edit, EM_CHARFROMPOS, 0, (LPARAM)&a));
        long to = std::min(l.end, (long)SendMessageW(t.edit, EM_CHARFROMPOS, 0, (LPARAM)&b) + 2);
        if (to <= from) continue;
        std::wstring s = text_range(t.edit, from, to);
        for (size_t i = 0; i < s.size();) {
            wchar_t ch = s[i];
            if (ch == L' ' || ch == L'\xA0') {
                size_t j = i;
                while (j < s.size() && (s[j] == L' ' || s[j] == L'\xA0')) ++j;
                int x0 = 0;
                if (!x_of(from + (long)i, l.y, x0)) { i = j; continue; }
                for (size_t k = i; k < j; ++k) {
                    int cx = x0 + (int)(sw * ((double)(k - i) + 0.5)) - dot / 2, cy = l.y + l.h / 2 - dot / 2;
                    RECT d{ cx, cy, cx + dot, cy + dot };
                    FillRect(dc, &d, br);
                }
                i = j;
            } else if (ch == L'\t') {
                int x0 = 0, x1 = 0;
                if (x_of(from + (long)i, l.y, x0)) {
                    if (!x_of(from + (long)i + 1, l.y, x1) || x1 <= x0) x1 = x0 + (int)(4 * sw);
                    int y = l.y + l.h / 2, pad = std::max(1, (int)(sw * 0.2)), head = std::max(2, (int)(sw * 0.3));
                    int ax = x0 + pad, bx = std::max(ax + head, x1 - pad);
                    POINT line[] = { { ax, y }, { bx, y } };
                    Polyline(dc, line, 2);
                    POINT tip[] = { { bx - head, y - head }, { bx, y }, { bx - head, y + head } };
                    Polyline(dc, tip, 3);
                }
                ++i;
            } else {
                ++i;
            }
        }
    }
    SelectObject(dc, op);
    DeleteObject(pen);
    DeleteObject(br);
    RestoreDC(dc, saved);
    ReleaseDC(t.edit, dc);
    ShowCaret(t.edit);
}

// ------------------------------------------------------------------
// tabs
// ------------------------------------------------------------------
void layout(Notepad& n);
void keep_soon(Notepad& n);

void update_title(Notepad& n) {
    Tab* t = cur(n);
    std::wstring s = t ? (t->dirty ? L"*" : L"") + tab_name(*t) + L" - Notepad" : L"Notepad";
    SetWindowTextW(n.wnd, s.c_str());
}

void invalidate_row(Notepad& n) {
    RECT r{ 0, 0, 0x7FFF, n.rowH + n.menuH };
    InvalidateRect(n.wnd, &r, FALSE);
}

void invalidate_status(Notepad& n) {
    if (!n.statusBar) return;
    RECT rc{};
    GetClientRect(n.wnd, &rc);
    rc.top = rc.bottom - n.statusH;
    InvalidateRect(n.wnd, &rc, FALSE);
}

void set_dirty(Notepad& n, Tab& t, bool on) {
    if (on) t.cacheStale = true;
    if (t.dirty == on) return;
    t.dirty = on;
    update_title(n);
    invalidate_row(n);
}

// The wheel: its lines (or a page) scrolled at once. The box's own wheel glides there a line
// at a time, half a second a notch, lagging behind the hand turning it.
void wheel_scroll(Notepad& n, HWND h, int delta) {
    if ((n.wheelV > 0) != (delta > 0)) n.wheelV = 0;   // (turned the other way: the rest of the last turn goes)
    n.wheelV += delta;
    UINT per = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &per, 0);
    if (per == WHEEL_PAGESCROLL) {
        for (; n.wheelV >= WHEEL_DELTA; n.wheelV -= WHEEL_DELTA) SendMessageW(h, WM_VSCROLL, SB_PAGEUP, 0);
        for (; n.wheelV <= -WHEEL_DELTA; n.wheelV += WHEEL_DELTA) SendMessageW(h, WM_VSCROLL, SB_PAGEDOWN, 0);
        return;
    }
    if (per == 0) { n.wheelV = 0; return; }
    int lines = n.wheelV * (int)per / WHEEL_DELTA;      // (a fine wheel's parts of a notch: kept till a line)
    if (!lines) return;
    n.wheelV -= lines * WHEEL_DELTA / (int)per;
    SendMessageW(h, EM_LINESCROLL, 0, -lines);
}

// The text box, as it scrolls, changes or is double-clicked: the part to show coloured before
// it is painted (the colouring is posted: it comes before the paint), the word to mark.
LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    Notepad& n = *(Notepad*)ref;
    // (an edit, or a jump to the end, while a big text goes in: all of it in first)
    if (Tab* t = cur(n); t && t->edit == h && !t->rest.empty()) {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        bool all = msg == WM_CHAR || msg == WM_PASTE || msg == WM_CUT || msg == WM_CLEAR || msg == WM_UNDO || msg == EM_UNDO ||
                   msg == EM_REDO || msg == EM_REPLACESEL || msg == WM_IME_STARTCOMPOSITION;
        if (msg == WM_KEYDOWN)
            all = wp == VK_BACK || wp == VK_DELETE || wp == VK_RETURN || wp == VK_TAB || (wp == VK_END && ctrl) ||
                  (wp == VK_INSERT && GetKeyState(VK_SHIFT) < 0) || (ctrl && (wp == 'A' || wp == 'V' || wp == 'X' || wp == 'Y' || wp == 'Z'));
        if (all) finish_loading(n, *t);
    }
    switch (msg) {
        case WM_LBUTTONDBLCLK: {
            LRESULT r = DefSubclassProc(h, msg, wp, lp);
            if (Tab* t = cur(n); t && t->edit == h) mark_word(n, *t);
            return r;
        }
        case WM_PASTE:
            return SendMessageW(h, EM_PASTESPECIAL, CF_UNICODETEXT, 0);    // (text only: no other's formatting)
        case WM_KEYDOWN:
            if ((wp == 'V' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) ||
                (wp == VK_INSERT && GetKeyState(VK_SHIFT) < 0 && GetKeyState(VK_CONTROL) >= 0)) {
                SendMessageW(h, EM_PASTESPECIAL, CF_UNICODETEXT, 0);
                request_colour(n);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                if (Tab* t = cur(n); t && t->edit == h && !t->word.empty()) { clear_word(n, *t); return 0; }
            }
            [[fallthrough]];
        case WM_MOUSEWHEEL:
            if (msg == WM_MOUSEWHEEL && !(GET_KEYSTATE_WPARAM(wp) & (MK_CONTROL | MK_SHIFT))) {
                wheel_scroll(n, h, GET_WHEEL_DELTA_WPARAM(wp));     // (its EM_LINESCROLL asks for the colours)
                return 0;
            }
            [[fallthrough]];
        case WM_VSCROLL: case WM_SIZE: case WM_TIMER: case WM_LBUTTONUP:
        case EM_LINESCROLL: case EM_SCROLLCARET: case EM_EXSETSEL: case EM_SETSEL: case EM_SETZOOM: case EM_SETTARGETDEVICE: {
            LRESULT r = DefSubclassProc(h, msg, wp, lp);
            request_colour(n);
            return r;
        }
        case WM_MOUSEMOVE:
            if (wp & MK_LBUTTON) request_colour(n);     // (selecting past the edge scrolls)
            break;
        case WM_PAINT: {
            LRESULT r = DefSubclassProc(h, msg, wp, lp);
            if (Tab* t = cur(n); t && t->edit == h) {
                draw_whitespace(n, *t);
                if (n.gutter && n.gutW) InvalidateRect(n.gutter, nullptr, FALSE);
            }
            return r;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, edit_proc, 1);
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

Tab* add_tab(Notepad& n, int at) {
    static unsigned ids = 0;
    auto t = std::make_unique<Tab>();
    t->id = ++ids;
    t->edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                              WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                                  ES_NOHIDESEL | ES_NOOLEDRAGDROP,
                              0, 0, 0, 0, n.wnd, nullptr, g_hinst, nullptr);
    if (!t->edit) return nullptr;
    // (rich text: only its runs of colour show reliably; all the same is plain - pasted, read and
    // put in as text)
    SendMessageW(t->edit, EM_SETTEXTMODE, TM_RICHTEXT | TM_MULTILEVELUNDO | TM_MULTICODEPAGE, 0);
    SendMessageW(t->edit, EM_EXLIMITTEXT, 0, 0x7FFFFFFE);
    SendMessageW(t->edit, EM_SETUNDOLIMIT, 1000, 0);
    SendMessageW(t->edit, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);
    SetWindowSubclass(t->edit, edit_proc, 1, (DWORD_PTR)&n);
    IRichEditOle* ole = nullptr;
    if (SendMessageW(t->edit, EM_GETOLEINTERFACE, 0, (LPARAM)&ole) && ole) {
        ole->QueryInterface(kIID_ITextDocument, (void**)&t->doc);
        ole->Release();
    }
    style_edit(n, *t);
    at = std::clamp(at, 0, (int)n.tabs.size());
    Tab* p = t.get();
    n.tabs.insert(n.tabs.begin() + at, std::move(t));
    if (n.active >= at) ++n.active;
    return p;
}

bool load_deferred(Notepad& n, Tab& t);

void activate(Notepad& n, int i) {
    if (i < 0 || i >= (int)n.tabs.size()) return;
    Tab* old = cur(n);
    n.active = i;
    Tab& t = *n.tabs[i];
    if (t.deferred) load_deferred(n, t);
    layout(n);
    ShowWindow(t.edit, SW_SHOW);
    if (old && old != &t) ShowWindow(old->edit, SW_HIDE);
    apply_pending(n, t);
    if (GetActiveWindow() == n.wnd) SetFocus(t.edit);
    update_title(n);
    InvalidateRect(n.wnd, nullptr, FALSE);
    request_colour(n);
    keep_soon(n);
}

Tab* new_untitled(Notepad& n) {
    Tab* t = add_tab(n, n.active + 1);
    if (t) activate(n, n.active + 1);
    return t;
}

bool untouched(Tab& t) { return t.path.empty() && !t.dirty && text_length(t.edit) == 0; }

void drop_tab(Notepad& n, int i) {
    Tab& t = *n.tabs[i];
    if (!t.cache.empty()) {
        std::wstring dir = notes_dir();
        if (!dir.empty()) DeleteFileW((dir + L"\\" + t.cache).c_str());
    }
    if (t.doc) t.doc->Release();
    DestroyWindow(t.edit);
    n.tabs.erase(n.tabs.begin() + i);
    if (n.hotTab >= (int)n.tabs.size()) n.hotTab = n.hotClose = -1;
}

bool save(Notepad& n, Tab& t, bool as);
bool keep_now(Notepad& n);

// asks about unsaved changes; false: cancelled (the tab stays)
bool close_tab(Notepad& n, int i) {
    if (i < 0 || i >= (int)n.tabs.size()) return false;
    Tab& t = *n.tabs[i];
    if (t.dirty) {
        activate(n, i);
        std::wstring q = L"Do you want to save the changes to " + tab_name(t) + L"?";
        int r = MessageBoxW(n.wnd, q.c_str(), L"Notepad", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL || (r == IDYES && !save(n, t, false))) return false;
    }
    bool wasActive = i == n.active;
    drop_tab(n, i);
    if (n.tabs.empty()) {
        n.active = -1;
        keep_now(n);
        DestroyWindow(n.wnd);
        return true;
    }
    if (i < n.active) --n.active;
    if (wasActive) {
        n.active = -1;
        activate(n, std::min(i, (int)n.tabs.size() - 1));
    } else {
        layout(n);
        InvalidateRect(n.wnd, nullptr, FALSE);
        keep_soon(n);
    }
    return true;
}

// ------------------------------------------------------------------
// opening and saving
// ------------------------------------------------------------------
bool same_file(const std::wstring& a, const std::wstring& b) { return !a.empty() && lstrcmpiW(a.c_str(), b.c_str()) == 0; }

bool open_path(Notepad& n, const std::wstring& path) {
    for (int i = 0; i < (int)n.tabs.size(); ++i)
        if (same_file(n.tabs[i]->path, path)) { activate(n, i); return true; }
    std::string bytes;
    Read r = read_bytes(path, bytes);
    if (r != Read::Ok) {
        std::wstring msg = r == Read::TooBig ? L"This file is too large for the Notepad (over 64 MB):\n" + path
                                             : L"This file couldn't be opened:\n" + path;
        MessageBoxW(n.wnd, msg.c_str(), L"Notepad", MB_OK | MB_ICONWARNING);
        return false;
    }
    TextFile f = text_decode(bytes);
    if (f.binary) {
        MessageBoxW(n.wnd, (L"This isn't a text file:\n" + path).c_str(), L"Notepad", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    // (an Untitled tab with nothing in it gives way)
    Tab* t = cur(n);
    int at = n.active;
    if (!t || !untouched(*t)) {
        at = n.active + 1;
        t = add_tab(n, at);
        if (!t) return false;
    }
    t->path = path;
    t->enc = f.enc;
    t->eol = f.eol;
    t->caveat = caveat_of(f);
    t->fileTime = write_time(path);
    t->langChosen = false;
    t->lang = lang_for_file(path);
    set_text(n, *t, f.text);
    set_dirty(n, *t, false);
    t->cacheStale = false;
    activate(n, at);
    return true;
}

// IFileDialog's folder: the active file's
void dialog_folder(IFileDialog* d, Tab* t) {
    if (!t || t->path.empty()) return;
    std::wstring dir = t->path.substr(0, t->path.find_last_of(L"\\/"));
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        d->SetFolder(folder);
        folder->Release();
    }
}

const COMDLG_FILTERSPEC kTypes[] = { { L"Text documents (*.txt)", L"*.txt" }, { L"All files (*.*)", L"*.*" } };

void open_dialog(Notepad& n) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    d->SetFileTypes(ARRAYSIZE(kTypes), kTypes);
    d->SetFileTypeIndex(2);
    FILEOPENDIALOGOPTIONS opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST);
    dialog_folder(d, cur(n));
    std::vector<std::wstring> files;
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(d->Show(n.wnd)) && SUCCEEDED(d->GetResults(&items))) {
        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            IShellItem* it = nullptr;
            PWSTR p = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                files.push_back(p);
                CoTaskMemFree(p);
            }
            if (it) it->Release();
        }
        items->Release();
    }
    d->Release();
    for (auto& f : files) open_path(n, f);
}

// the Save dialog, with the encoding to write: the file chosen, empty when cancelled
std::wstring ask_save(Notepad& n, Tab& t, TextEncoding& enc) {
    IFileSaveDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    d->SetFileTypes(ARRAYSIZE(kTypes), kTypes);
    d->SetFileTypeIndex(t.path.empty() || lstrcmpiW(PathFindExtensionW(t.path.c_str()), L".txt") == 0 ? 1 : 2);
    d->SetDefaultExtension(L"txt");
    d->SetFileName(t.path.empty() ? L"*.txt" : tab_name(t).c_str());
    FILEOPENDIALOGOPTIONS opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    dialog_folder(d, &t);
    const DWORD kGroup = 1, kCombo = 2;
    IFileDialogCustomize* custom = nullptr;
    if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&custom)))) {
        custom->StartVisualGroup(kGroup, L"Encoding:");
        custom->AddComboBox(kCombo);
        for (int e = 0; e < (int)TextEncoding::Count; ++e) custom->AddControlItem(kCombo, e, encoding_name((TextEncoding)e));
        custom->SetSelectedControlItem(kCombo, (DWORD)enc);
        custom->EndVisualGroup();
    }
    std::wstring out;
    IShellItem* item = nullptr;
    if (SUCCEEDED(d->Show(n.wnd)) && SUCCEEDED(d->GetResult(&item))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
        item->Release();
        DWORD sel = 0;
        if (custom && SUCCEEDED(custom->GetSelectedControlItem(kCombo, &sel)) && sel < (DWORD)TextEncoding::Count) enc = (TextEncoding)sel;
    }
    if (custom) custom->Release();
    d->Release();
    return out;
}

bool save(Notepad& n, Tab& t, bool as) {
    if (t.deferred && !load_deferred(n, t)) return false;
    finish_loading(n, t);
    std::wstring file = t.path;
    TextEncoding enc = t.enc;
    if (as || file.empty()) {
        file = ask_save(n, t, enc);
        if (file.empty()) return false;
    }
    if (same_file(file, t.path)) {
        // changed by another program since read here: its changes aren't lost unasked
        ULONGLONG now = write_time(file);
        if (now && t.fileTime && now != t.fileTime &&
            MessageBoxW(n.wnd, (L"This file was changed by another program since it was opened here:\n" + file +
                                L"\n\nSave over it? Its changes would be lost.").c_str(),
                        L"Notepad", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return false;
        if (!t.caveat.empty() &&
            MessageBoxW(n.wnd, (t.caveat + L"\nSave anyway?").c_str(), L"Notepad", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return false;
    }
    std::wstring text = doc_text(t, 0, -1);
    if (text.empty() && text_length(t.edit) > 0) {
        MessageBoxW(n.wnd, L"The text couldn't be read from the window: nothing was saved.", L"Notepad", MB_OK | MB_ICONWARNING);
        return false;
    }
    bool lossy = false;
    std::string bytes = text_encode(text, enc, t.eol, &lossy);
    if (lossy) {
        int r = MessageBoxW(n.wnd, L"Some characters can't be saved in ANSI and would be lost.\n\nSave as UTF-8 instead?",
                            L"Notepad", MB_YESNOCANCEL | MB_ICONWARNING);
        if (r == IDCANCEL) return false;
        if (r == IDYES) {
            enc = TextEncoding::Utf8;
            bytes = text_encode(text, enc, t.eol);
        }
    }
    if (bytes.empty() && !text.empty()) {                   // (never an empty file for a text)
        MessageBoxW(n.wnd, (std::wstring(L"The text couldn't be converted to ") + encoding_name(enc) + L": nothing was saved.").c_str(),
                    L"Notepad", MB_OK | MB_ICONWARNING);
        return false;
    }
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = text_file_write(file, bytes);
    SetCursor(old);
    if (!ok) {
        MessageBoxW(n.wnd, (L"The file couldn't be saved:\n" + file).c_str(), L"Notepad", MB_OK | MB_ICONWARNING);
        return false;
    }
    t.path = file;
    t.enc = enc;
    t.caveat.clear();
    t.fileTime = write_time(file);
    if (!t.langChosen && t.lang != lang_for_file(file)) {   // (saved under another name: its language)
        t.lang = lang_for_file(file);
        recolour(n, t, true);
    }
    t.dirty = true;                         // (the name may have changed either way)
    set_dirty(n, t, false);
    t.cacheStale = false;
    layout(n);
    invalidate_status(n);
    keep_soon(n);
    return true;
}

// a file changed by another program while open here, not changed here: read again
void check_files(Notepad& n) {
    for (auto& tp : n.tabs) {
        Tab& t = *tp;
        if (t.path.empty() || t.dirty || t.deferred) continue;     // (deferred: read as it is now when shown)
        ULONGLONG now = write_time(t.path);
        if (!now || now == t.fileTime) continue;
        std::string bytes;
        if (read_bytes(t.path, bytes) != Read::Ok) continue;
        TextFile f = text_decode(bytes);
        if (f.binary) continue;
        CHARRANGE cr = t.pendingSel.cpMin >= 0 ? t.pendingSel : selection(t.edit);
        int top = t.pendingTop >= 0 ? t.pendingTop : (int)SendMessageW(t.edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        t.enc = f.enc;
        t.eol = f.eol;
        t.caveat = caveat_of(f);
        t.fileTime = now;
        set_text(n, t, f.text);
        t.pendingSel = cr;
        t.pendingTop = top;
        apply_pending(n, t);
    }
    invalidate_status(n);
}

// ------------------------------------------------------------------
// keeping the tabs (the session, and each unsaved text) in the notes folder
// ------------------------------------------------------------------
void keep_soon(Notepad& n) { if (n.wnd) SetTimer(n.wnd, TIMER_KEEP, kKeepDelayMs, nullptr); }

// an unsaved text, as the box holds it (UTF-16, lines ended with CR: read back as any file is):
// not converted, not copied (50 MB in 70 ms)
bool keep_text(Tab& t, const std::wstring& path) {
    ITextRange* r = nullptr;
    if (!t.doc || FAILED(t.doc->Range(0, text_length(t.edit), &r)) || !r) return false;
    BSTR bs = nullptr;
    bool ok = SUCCEEDED(r->GetText(&bs)) &&
              write_own(path, std::string_view("\xFF\xFE", 2), std::string_view((const char*)bs, bs ? SysStringByteLen(bs) : 0));
    SysFreeString(bs);
    r->Release();
    return ok;
}

std::wstring new_cache_name(const std::wstring& dir) {
    static unsigned counter = 0;
    for (;;) {
        wchar_t b[64];
        swprintf_s(b, L"note-%llx-%u.txt", GetTickCount64(), ++counter);
        if (GetFileAttributesW((dir + L"\\" + b).c_str()) == INVALID_FILE_ATTRIBUTES) return b;
    }
}

// a tab as the session keeps it (its kept unsaved text aside)
NoteTabState tab_state(Tab& t) {
    NoteTabState st;
    st.path = t.path;
    st.untitled = t.path.empty() ? 1 : 0;
    st.dirty = t.dirty;
    st.enc = t.enc;
    st.eol = t.eol;
    st.fileTime = t.fileTime;
    if (t.langChosen) st.syntax = lang_key(t.lang);
    st.zoom = t.zoom;
    CHARRANGE cr = t.pendingSel.cpMin >= 0 ? t.pendingSel : selection(t.edit);
    st.selStart = cr.cpMin;
    st.selEnd = cr.cpMax;
    st.top = t.pendingTop >= 0 ? t.pendingTop : (int)SendMessageW(t.edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    return st;
}

bool keep_now(Notepad& n) {
    if (n.wnd) KillTimer(n.wnd, TIMER_KEEP);
    std::wstring dir = notes_dir();
    if (dir.empty()) return false;
    NoteSession s;
    s.active = std::max(0, n.active);
    s.wrap = n.wrap;
    s.statusBar = n.statusBar;
    s.lineNumbers = n.lineNumbers;
    s.whitespace = n.whitespace;
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (n.wnd && GetWindowPlacement(n.wnd, &wp)) {
        const RECT& r = wp.rcNormalPosition;
        s.x = r.left;
        s.y = r.top;
        s.w = r.right - r.left;
        s.h = r.bottom - r.top;
        s.maximized = wp.showCmd == SW_SHOWMAXIMIZED || (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    }
    bool ok = true;
    for (auto& tp : n.tabs) {
        Tab& t = *tp;
        NoteTabState st = tab_state(t);
        if (t.dirty) {
            if (t.cache.empty()) {
                t.cache = new_cache_name(dir);
                t.cacheStale = true;
            }
            if (t.cacheStale) {
                finish_loading(n, t);
                if (keep_text(t, dir + L"\\" + t.cache)) t.cacheStale = false;
                else ok = false;
            }
            st.cache = t.cache;
        } else if (!t.cache.empty()) {
            DeleteFileW((dir + L"\\" + t.cache).c_str());
            t.cache.clear();
        }
        s.tabs.push_back(std::move(st));
    }
    for (auto& st : n.unread) s.tabs.push_back(st);         // (as they were: tried again next time too)
    return write_own(dir + L"\\session.ini", text_encode(note_session_serialize(s), TextEncoding::Utf16Le, LineEnding::CrLf)) && ok;
}

// a tab as it was left, last: `f` its text (`kept`: its unsaved text, from the notes folder)
Tab* restore_tab(Notepad& n, const NoteTabState& st, TextFile& f, bool kept) {
    Tab* t = add_tab(n, (int)n.tabs.size());
    if (!t) return nullptr;
    t->path = st.path;
    t->enc = f.enc;
    t->eol = f.eol;
    t->langChosen = !st.syntax.empty();
    t->lang = t->langChosen ? lang_from_key(st.syntax) : lang_for_file(st.path);
    t->zoom = st.zoom;
    set_text(n, *t, f.text);                // (styled: the zoom too)
    if (kept) {
        t->dirty = true;
        t->cache = st.cache;
        t->fileTime = st.fileTime;
    } else {
        t->fileTime = st.path.empty() ? 0 : write_time(st.path);
        t->caveat = caveat_of(f);
    }
    t->pendingSel = { st.selStart, st.selEnd };
    t->pendingTop = st.top;
    apply_pending(n, *t);
    return t;
}

// a tab's kept unsaved text: Ok, or Failed with the file still there (unread: kept for later),
// or Failed with it gone
Read read_kept(const std::wstring& dir, const NoteTabState& st, TextFile& f, bool& unread) {
    std::string bytes;
    std::wstring file = dir + L"\\" + st.cache;
    Read r = read_bytes(file, bytes, 2 * kMaxBytes + 2);
    unread = r == Read::Failed && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (r == Read::Ok) {
        f = text_decode(bytes);
        f.enc = st.enc;
        f.eol = st.eol;
    }
    return r;
}

// a tab as it was left, last, its text not read yet (`kept`: from its unsaved text)
Tab* defer_tab(Notepad& n, const NoteTabState& st, bool kept) {
    Tab* t = add_tab(n, (int)n.tabs.size());
    if (!t) return nullptr;
    t->path = st.path;
    t->enc = st.enc;
    t->eol = st.eol;
    t->langChosen = !st.syntax.empty();
    t->lang = t->langChosen ? lang_from_key(st.syntax) : lang_for_file(st.path);
    t->zoom = st.zoom;
    t->fileTime = st.fileTime;
    t->dirty = kept;
    if (kept) t->cache = st.cache;
    t->pendingSel = { st.selStart, st.selEnd };
    t->pendingTop = st.top;
    t->deferred = true;
    t->deferKept = kept;
    return t;
}

// a restored tab's text, the first time it is shown: as restore_tab would have had it. False:
// it can't be read now (the tab is closed a moment later; unsaved text another program has
// open is kept, and tried again)
bool load_deferred(Notepad& n, Tab& t) {
    t.deferred = false;
    TextFile f;
    bool ok = false;
    if (t.deferKept) {
        std::wstring dir = notes_dir();
        NoteTabState st = tab_state(t);
        st.cache = t.cache;
        bool unread = false;
        ok = !dir.empty() && read_kept(dir, st, f, unread) == Read::Ok;
        if (unread) {
            n.unread.push_back(st);
            t.cache.clear();                // (not deleted with the tab: it is the unread one's)
            t.dirty = false;
            SetTimer(n.wnd, TIMER_UNREAD, kUnreadRetryMs, nullptr);
            PostMessageW(n.wnd, WM_NOTEPAD_UNREAD, 0, 0);
            PostMessageW(n.wnd, WM_NOTEPAD_DROP, FALSE, t.id);
            return false;
        }
        if (!ok) {                          // (gone: its file as saved)
            t.dirty = false;
            t.cache.clear();
        }
    }
    bool kept = ok;
    if (!ok && !t.path.empty()) {
        std::string bytes;
        if (read_bytes(t.path, bytes) == Read::Ok) {
            f = text_decode(bytes);
            ok = !f.binary;
        }
    }
    if (!ok) {
        PostMessageW(n.wnd, WM_NOTEPAD_DROP, TRUE, t.id);
        return false;
    }
    CHARRANGE sel = t.pendingSel;
    int top = t.pendingTop;
    set_text(n, t, f.text);                 // (styled: the zoom too)
    t.enc = f.enc;
    t.eol = f.eol;
    if (!kept) {
        t.fileTime = write_time(t.path);
        t.caveat = caveat_of(f);
    }
    t.pendingSel = sel;
    t.pendingTop = top;
    return true;
}

// the unread kept texts tried again: those that can be read now come back as tabs (last, not
// made the active one: the user may be typing in another). True: none left.
bool retry_unread(Notepad& n) {
    std::wstring dir = notes_dir();
    if (dir.empty()) return n.unread.empty();
    bool added = false;
    for (size_t i = 0; i < n.unread.size();) {
        TextFile f;
        bool unread = false;
        Read r = read_kept(dir, n.unread[i], f, unread);
        if (unread) { ++i; continue; }
        if (r == Read::Ok && restore_tab(n, n.unread[i], f, true)) added = true;
        n.unread.erase(n.unread.begin() + (ptrdiff_t)i);   // (gone meanwhile: nothing left to bring back)
    }
    if (added && n.wnd) {
        // (the empty Untitled made for want of them gives way, as to a file opened)
        if (n.tabs.size() > 1 && n.active == 0 && untouched(*n.tabs[0])) close_tab(n, 0);
        if (n.active < 0) activate(n, 0);
        layout(n);
        InvalidateRect(n.wnd, nullptr, FALSE);
        keep_soon(n);
    }
    return n.unread.empty();
}

// said once the window is up: what couldn't be read, kept and tried again
void tell_unread(Notepad& n) {
    while (!n.unread.empty()) {
        std::wstring names;
        for (auto& st : n.unread) names += L"\n" + (st.path.empty() ? std::wstring(L"Untitled") : st.path);
        std::wstring msg = L"The unsaved text of these tabs couldn't be read (another program may have it open):\n" + names +
                           L"\n\nIt is kept: the Notepad tries again in a moment, and the next time it opens.";
        if (MessageBoxW(n.wnd, msg.c_str(), L"Notepad", MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY) break;
        retry_unread(n);
    }
    if (n.unread.empty()) KillTimer(n.wnd, TIMER_UNREAD);
}

// the tabs as they were left; the window's place and size into `s`
void restore(Notepad& n, NoteSession& s) {
    std::wstring dir = notes_dir();
    if (dir.empty()) return;
    std::string raw;
    Read sr = read_bytes(dir + L"\\session.ini", raw);
    std::wstring ini = sr == Read::Ok ? ini_decode(raw) : std::wstring();
    if (sr == Read::Ok) s = note_session_parse(ini);
    // a session that couldn't be read (locked, empty, [tab] sections that gave no tabs): its kept
    // texts aren't swept as orphans
    bool sessionRead = sr == Read::Ok ? !s.tabs.empty() || (!ini.empty() && ini.find(L"[tab") == std::wstring::npos)
                                      : GetFileAttributesW((dir + L"\\session.ini").c_str()) == INVALID_FILE_ATTRIBUTES;
    n.wrap = s.wrap;
    n.statusBar = s.statusBar;
    n.lineNumbers = s.lineNumbers;
    n.whitespace = s.whitespace;
    int active = 0;
    std::vector<std::wstring> caches;
    for (int i = 0; i < (int)s.tabs.size(); ++i) {
        const NoteTabState& st = s.tabs[i];
        // its unsaved text; else its file as it is now; else (an empty Untitled) nothing. Unsaved
        // text that is there but can't be read now isn't traded for the file: kept for later.
        // Read when the tab is first shown (load_deferred): here only whether they are there.
        bool kept = st.dirty && !st.cache.empty() && GetFileAttributesW((dir + L"\\" + st.cache).c_str()) != INVALID_FILE_ATTRIBUTES;
        Tab* t = nullptr;
        if (kept || !st.path.empty()) {
            // (a file gone is left out; one on a share isn't asked: the server may be away)
            DWORD a = kept || path_is_network(st.path) ? 0 : GetFileAttributesW(st.path.c_str());
            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) continue;
            t = defer_tab(n, st, kept);
        } else if (!st.dirty) {
            TextFile empty;
            t = restore_tab(n, st, empty, false);
        } else {
            continue;
        }
        if (!t) break;
        if (kept) caches.push_back(st.cache);
        if (i <= s.active) active = (int)n.tabs.size() - 1;
    }
    // text kept for tabs no longer there: only when the session said so, and only a week after it
    // was last written (another Notepad's, or one this session failed to read, isn't lost at once)
    for (auto& st : s.tabs) if (!st.cache.empty()) caches.push_back(st.cache);
    WIN32_FIND_DATAW fd;
    HANDLE h = sessionRead ? FindFirstFileW((dir + L"\\note-*.txt").c_str(), &fd) : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        ULONGLONG weekAgo = ((ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime) - 7ull * 24 * 3600 * 10000000;
        do {
            ULONGLONG written = (ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
            if (written < weekAgo &&
                std::none_of(caches.begin(), caches.end(), [&](const std::wstring& c) { return lstrcmpiW(c.c_str(), fd.cFileName) == 0; }))
                DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    n.active = n.tabs.empty() ? -1 : std::min(active, (int)n.tabs.size() - 1);
}

// ------------------------------------------------------------------
// find, replace, go to
// ------------------------------------------------------------------
HWND message_owner(Notepad& n) { return n.find && IsWindowVisible(n.find) ? n.find : n.wnd; }

void not_found(Notepad& n) {
    std::wstring msg = L"Cannot find \"" + std::wstring(n.findText) + L"\"";
    MessageBoxW(message_owner(n), msg.c_str(), L"Notepad", MB_OK | MB_ICONINFORMATION);
}

void show_find(Notepad& n, bool replace) {
    Tab* t = cur(n);
    // the selection (on one line) is what is looked for
    if (t) {
        CHARRANGE cr = selection(t->edit);
        if (cr.cpMax > cr.cpMin && cr.cpMax - cr.cpMin < (long)ARRAYSIZE(n.findText)) {
            std::wstring s((size_t)(cr.cpMax - cr.cpMin) + 1, L'\0');
            LRESULT got = SendMessageW(t->edit, EM_GETSELTEXT, 0, (LPARAM)s.data());
            s.resize(got > 0 ? (size_t)got : 0);
            if (!s.empty() && s.find_first_of(L"\r\n") == std::wstring::npos) wcscpy_s(n.findText, s.c_str());
        }
    }
    if (n.find) {
        if (n.replacing == replace) {
            SetDlgItemTextW(n.find, 0x0480 /*edt1*/, n.findText);
            SetActiveWindow(n.find);
            return;
        }
        DestroyWindow(n.find);
        n.find = nullptr;
    }
    n.fr = { sizeof(n.fr) };
    n.fr.hwndOwner = n.wnd;
    n.fr.Flags = n.findFlags & (FR_DOWN | FR_MATCHCASE | FR_WHOLEWORD);
    n.fr.lpstrFindWhat = n.findText;
    n.fr.wFindWhatLen = ARRAYSIZE(n.findText);
    n.fr.lpstrReplaceWith = n.replaceText;
    n.fr.wReplaceWithLen = ARRAYSIZE(n.replaceText);
    n.replacing = replace;
    n.find = replace ? ReplaceTextW(&n.fr) : FindTextW(&n.fr);
    if (n.find) {
        BOOL dark = g_dark;
        DwmSetWindowAttribute(n.find, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    }
}

// the next one from the selection, round past the end; false: none
bool find_next(Notepad& n, bool down) {
    Tab* t = cur(n);
    if (!t) return false;
    if (!n.findText[0]) { show_find(n, false); return false; }
    DWORD flags = (down ? FR_DOWN : 0) | (n.findFlags & (FR_MATCHCASE | FR_WHOLEWORD));
    CHARRANGE cr = selection(t->edit);
    FINDTEXTEXW ft{};
    ft.lpstrText = n.findText;
    ft.chrg = down ? CHARRANGE{ cr.cpMax, -1 } : CHARRANGE{ cr.cpMin, 0 };
    LRESULT at = SendMessageW(t->edit, EM_FINDTEXTEXW, flags, (LPARAM)&ft);
    if (at < 0) {
        ft.chrg = down ? CHARRANGE{ 0, -1 } : CHARRANGE{ text_length(t->edit), 0 };
        at = SendMessageW(t->edit, EM_FINDTEXTEXW, flags, (LPARAM)&ft);
    }
    if (at < 0) {
        not_found(n);
        return false;
    }
    select(t->edit, ft.chrgText.cpMin, ft.chrgText.cpMax);
    SendMessageW(t->edit, EM_SCROLLCARET, 0, 0);
    return true;
}

bool selection_is_found(Notepad& n, Tab& t) {
    CHARRANGE cr = selection(t.edit);
    if (cr.cpMax <= cr.cpMin) return false;
    std::wstring s((size_t)(cr.cpMax - cr.cpMin) + 1, L'\0');
    LRESULT got = SendMessageW(t.edit, EM_GETSELTEXT, 0, (LPARAM)s.data());
    s.resize(got > 0 ? (size_t)got : 0);
    return CompareStringOrdinal(s.c_str(), (int)s.size(), n.findText, -1, !(n.findFlags & FR_MATCHCASE)) == CSTR_EQUAL;
}

void replace_one(Notepad& n) {
    Tab* t = cur(n);
    if (!t || !n.findText[0]) return;
    if (selection_is_found(n, *t)) {
        CHARRANGE cr = selection(t->edit);
        std::wstring with = n.replaceText;
        put_text(*t, cr.cpMin, cr.cpMax, with);
        select(t->edit, cr.cpMin + (long)with.size(), cr.cpMin + (long)with.size());
    }
    find_next(n, true);
}

// every one, found by the control's own search, put in at once: one step to undo
void replace_all(Notepad& n) {
    Tab* t = cur(n);
    if (!t || !n.findText[0]) return;
    std::wstring text = doc_text(*t, 0, -1);                // (positions as the control counts them)
    std::vector<CHARRANGE> hits;
    FINDTEXTEXW ft{};
    ft.lpstrText = n.findText;
    ft.chrg = { 0, -1 };
    DWORD flags = FR_DOWN | (n.findFlags & (FR_MATCHCASE | FR_WHOLEWORD));
    while (SendMessageW(t->edit, EM_FINDTEXTEXW, flags, (LPARAM)&ft) >= 0 && ft.chrgText.cpMax > ft.chrgText.cpMin) {
        hits.push_back(ft.chrgText);
        ft.chrg.cpMin = ft.chrgText.cpMax;
    }
    if (hits.empty()) { not_found(n); return; }
    std::wstring out;
    size_t pos = 0;
    for (const CHARRANGE& h : hits) {
        if ((size_t)h.cpMax > text.size()) break;
        out.append(text, pos, (size_t)h.cpMin - pos);
        out += n.replaceText;
        pos = (size_t)h.cpMax;
    }
    out.append(text, std::min(pos, text.size()), std::wstring::npos);
    SendMessageW(t->edit, WM_SETREDRAW, FALSE, 0);
    select(t->edit, 0, 0);
    put_text(*t, 0, (long)text.size(), out);
    select(t->edit, 0, 0);
    SendMessageW(t->edit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(t->edit, nullptr, TRUE);
}

// Go to line: a small dialog made in memory
struct GotoData { long line, lines; };
HBRUSH s_dlgBrush = nullptr, s_boxBrush = nullptr;

INT_PTR CALLBACK goto_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const Palette& c = pal();
    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(h, DWLP_USER, lp);
            auto* d = (GotoData*)lp;
            SetDlgItemInt(h, 100, (UINT)d->line, FALSE);
            SendDlgItemMessageW(h, 100, EM_SETSEL, 0, -1);
            BOOL dark = g_dark;
            DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
            for (int id : { IDOK, IDCANCEL, 100 }) theme_style_control(GetDlgItem(h, id));
            if (s_dlgBrush) DeleteObject(s_dlgBrush);
            if (s_boxBrush) DeleteObject(s_boxBrush);
            s_dlgBrush = CreateSolidBrush(c.content);
            s_boxBrush = CreateSolidBrush(c.boxFill);
            return TRUE;
        }
        case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
            SetTextColor((HDC)wp, c.text);
            SetBkColor((HDC)wp, c.content);
            return (INT_PTR)s_dlgBrush;
        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)wp, c.text);
            SetBkColor((HDC)wp, c.boxFill);
            return (INT_PTR)s_boxBrush;
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                auto* d = (GotoData*)GetWindowLongPtrW(h, DWLP_USER);
                BOOL ok = FALSE;
                UINT v = GetDlgItemInt(h, 100, &ok, FALSE);
                if (!ok || v < 1 || (long)v > d->lines) {
                    MessageBoxW(h, L"The line number is beyond the total number of lines.", L"Notepad - Go to line", MB_OK | MB_ICONINFORMATION);
                    SetFocus(GetDlgItem(h, 100));
                    SendDlgItemMessageW(h, 100, EM_SETSEL, 0, -1);
                    return TRUE;
                }
                d->line = (long)v;
                EndDialog(h, IDOK);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) { EndDialog(h, IDCANCEL); return TRUE; }
            break;
    }
    return FALSE;
}

void go_to(Notepad& n) {
    Tab* t = cur(n);
    if (!t || !t->doc) return;
    GotoData d{};
    long col = 0;
    caret_place(n, *t, d.line, col);
    ITextRange* all = nullptr;
    if (FAILED(t->doc->Range(0, 0, &all)) || !all) return;
    all->EndOf(tomStory, tomMove, nullptr);
    all->GetIndex(tomParagraph, &d.lines);
    all->Release();
    // the template: a caption, a label, a number box, Go to and Cancel
    std::vector<WORD> v;
    auto dword = [&](DWORD x) { v.push_back(LOWORD(x)); v.push_back(HIWORD(x)); };
    auto str = [&](const wchar_t* s) { while (*s) v.push_back(*s++); v.push_back(0); };
    dword(DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    dword(0);
    v.push_back(4);
    for (short x : { 0, 0, 180, 66 }) v.push_back((WORD)x);
    v.push_back(0);
    v.push_back(0);
    str(L"Go to line");
    v.push_back(9);
    str(L"Segoe UI");
    auto item = [&](DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text) {
        if (v.size() % 2) v.push_back(0);
        dword(style | WS_CHILD | WS_VISIBLE);
        dword(0);
        for (short k : { x, y, cx, cy }) v.push_back((WORD)k);
        v.push_back(id);
        v.push_back(0xFFFF);
        v.push_back(cls);
        str(text);
        v.push_back(0);
    };
    item(SS_LEFT, 8, 8, 164, 9, 0xFFFF, 0x0082, L"Line number:");
    item(WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 8, 20, 164, 13, 100, 0x0081, L"");
    item(WS_TABSTOP | BS_DEFPUSHBUTTON, 68, 44, 50, 14, IDOK, 0x0080, L"Go to");
    item(WS_TABSTOP | BS_PUSHBUTTON, 122, 44, 50, 14, IDCANCEL, 0x0080, L"Cancel");
    if (DialogBoxIndirectParamW(g_hinst, (LPCDLGTEMPLATEW)v.data(), n.wnd, goto_proc, (LPARAM)&d) != IDOK) return;
    ITextRange* r = nullptr;
    if (FAILED(t->doc->Range(0, 0, &r)) || !r) return;
    long cp = 0;
    if (SUCCEEDED(r->SetIndex(tomParagraph, d.line, 0)) && SUCCEEDED(r->GetStart(&cp))) {
        select(t->edit, cp, cp);
        SendMessageW(t->edit, EM_SCROLLCARET, 0, 0);
    }
    r->Release();
}

void insert_time(Tab& t) {
    wchar_t tm[64] = L"", dt[64] = L"";
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, tm, ARRAYSIZE(tm));
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, dt, ARRAYSIZE(dt), nullptr);
    std::wstring s = std::wstring(tm) + L" " + dt;
    CHARRANGE cr = selection(t.edit);
    put_text(t, cr.cpMin, cr.cpMax, s);
    select(t.edit, cr.cpMin + (long)s.size(), cr.cpMin + (long)s.size());
}

// the active tab's zoom (each tab has its own)
void set_zoom(Notepad& n, int z) {
    Tab* t = cur(n);
    if (!t) return;
    t->zoom = std::clamp(z, 10, 500);
    SendMessageW(t->edit, EM_SETZOOM, t->zoom, 100);
    layout(n);                              // (the line numbers' width, in the text's size)
    invalidate_status(n);
    keep_soon(n);
}

// ------------------------------------------------------------------
// lines, conversions, formatting: each one step to undo
// ------------------------------------------------------------------
void replace_range(Tab& t, long a, long b, const std::wstring& s, long selA, long selB) {
    select(t.edit, a, a);
    put_text(t, a, b, s);
    select(t.edit, selA, selB);
    SendMessageW(t.edit, EM_SCROLLCARET, 0, 0);
}

// the whole lines the selection is on: [a, b), b at the last one's '\r'
std::pair<long, long> selected_lines(Tab& t) {
    CHARRANGE cr = selection(t.edit);
    long last = cr.cpMax;
    if (cr.cpMax > cr.cpMin && line_start(t, cr.cpMax) == cr.cpMax) --last;   // (ending at a line's start: not that line)
    return { line_start(t, cr.cpMin), line_end(t, std::max(last, cr.cpMin)) };
}

void line_command(Tab& t, UINT id) {
    if (!t.doc) return;
    CHARRANGE cr = selection(t.edit);
    auto [a, b] = selected_lines(t);
    long len = text_length(t.edit);
    std::wstring block = text_range(t.edit, a, b);
    switch (id) {
        case CMD_DUPLINE: {
            long d = (long)block.size() + 1;
            replace_range(t, b, b, L"\r" + block, cr.cpMin + d, cr.cpMax + d);
            return;
        }
        case CMD_DELLINE:
            if (b < len) replace_range(t, a, b + 1, L"", a, a);
            else if (a > 0) replace_range(t, a - 1, b, L"", line_start(t, a - 1), line_start(t, a - 1));
            else replace_range(t, 0, b, L"", 0, 0);
            return;
        case CMD_LINEUP: {
            if (a == 0) return;
            long pa = line_start(t, a - 1), d = a - pa;
            replace_range(t, pa, b, block + L"\r" + text_range(t.edit, pa, a - 1), cr.cpMin - d, cr.cpMax - d);
            return;
        }
        case CMD_LINEDOWN: {
            if (b >= len) return;
            long nb = line_end(t, b + 1), d = nb - b;
            replace_range(t, a, nb, text_range(t.edit, b + 1, nb) + L"\r" + block, cr.cpMin + d, cr.cpMax + d);
            return;
        }
    }
    // the others: on the lines selected (more than one), else on all of them
    bool all = block.find(L'\r') == std::wstring::npos;
    if (all) {
        a = 0;
        b = len;
        block = text_range(t.edit, 0, len);
    }
    LineOp op = id == CMD_SORTASC ? LineOp::SortAsc : id == CMD_SORTDESC ? LineOp::SortDesc : id == CMD_REVERSE ? LineOp::Reverse
              : id == CMD_UNIQUE ? LineOp::Unique : id == CMD_NOEMPTY ? LineOp::RemoveEmpty : id == CMD_TRIM ? LineOp::TrimTrailing
              : LineOp::Join;
    if (op == LineOp::Join && all) {                // (one line: it and the next)
        if (b >= len) return;
        a = line_start(t, cr.cpMin);
        b = line_end(t, line_end(t, cr.cpMin) + 1);
        block = text_range(t.edit, a, b);
    }
    std::wstring out = line_op(op, block);
    if (out == block) return;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    if (all && op != LineOp::Join) replace_range(t, a, b, out, 0, 0);
    else replace_range(t, a, b, out, a, a + (long)out.size());
    SetCursor(old);
}

// case: the selection, else the word at the caret; coding: the selection, else the line
void convert_command(Notepad& n, Tab& t, UINT id) {
    if (!t.doc) return;
    CHARRANGE cr = selection(t.edit);
    bool casing = id == CMD_UPPER || id == CMD_LOWER || id == CMD_TITLE;
    if (cr.cpMax == cr.cpMin) {
        if (casing) {
            ITextRange* r = nullptr;
            if (SUCCEEDED(t.doc->Range(cr.cpMin, cr.cpMin, &r)) && r) {
                r->Expand(tomWord, nullptr);
                r->GetStart(&cr.cpMin);
                r->GetEnd(&cr.cpMax);
                r->Release();
            }
            std::wstring w = text_range(t.edit, cr.cpMin, cr.cpMax);
            while (!w.empty() && (w.back() == L' ' || w.back() == L'\t' || w.back() == L'\r')) { w.pop_back(); --cr.cpMax; }
        } else {
            auto [a, b] = selected_lines(t);
            cr = { a, b };
        }
        if (cr.cpMax <= cr.cpMin) return;
    }
    std::wstring s = text_range(t.edit, cr.cpMin, cr.cpMax), out;
    // (line breaks coded as the file has them; decoded ones as the box has them)
    const wchar_t* eol = t.eol == LineEnding::CrLf ? L"\r\n" : t.eol == LineEnding::Lf ? L"\n" : L"\r";
    auto file_lines = [&](const std::wstring& x) {
        std::wstring y;
        for (wchar_t c : x) {
            if (c == L'\r') y += eol;
            else y += c;
        }
        return y;
    };
    bool ok = true;
    switch (id) {
        case CMD_UPPER: out = change_case(CaseOp::Upper, s); break;
        case CMD_LOWER: out = change_case(CaseOp::Lower, s); break;
        case CMD_TITLE: out = change_case(CaseOp::Title, s); break;
        case CMD_B64ENC: out = base64_encode(file_lines(s)); break;
        case CMD_URLENC: out = url_encode(file_lines(s)); break;
        case CMD_B64DEC: ok = base64_decode(s, out); break;
        case CMD_URLDEC: ok = url_decode(s, out); break;
    }
    if (!ok) {
        MessageBoxW(n.wnd, id == CMD_B64DEC ? L"This isn't text coded in Base64." : L"This isn't URL-coded text (UTF-8).", L"Notepad",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring box;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == L'\r' && i + 1 < out.size() && out[i + 1] == L'\n') continue;
        box += out[i] == L'\n' ? L'\r' : out[i];
    }
    if (box == s) return;
    replace_range(t, cr.cpMin, cr.cpMax, box, cr.cpMin, cr.cpMin + (long)box.size());
}

bool formattable(const Tab& t) { return t.lang == Lang::Json || t.lang == Lang::Xml || t.lang == Lang::None; }

// JSON or XML (plain text: whichever it looks like), indented; or JSON on one line
void format_document(Notepad& n, Tab& t, bool compact) {
    std::wstring text = text_range(t.edit, 0, text_length(t.edit));
    Lang l = t.lang;
    if (l == Lang::None) {
        size_t k = text.find_first_not_of(L" \t\r\n\xFEFF");
        l = k != std::wstring::npos && text[k] == L'<' ? Lang::Xml : Lang::Json;
    }
    if (l != Lang::Json && l != Lang::Xml) return;
    if (compact && l != Lang::Json) return;
    std::wstring out;
    size_t err = 0;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = l == Lang::Json ? format_json(text, compact, out, &err) : format_xml(text, out, &err);
    SetCursor(old);
    if (!ok) {
        long line = 1 + (long)std::count(text.begin(), text.begin() + (long)std::min(err, text.size()), L'\r');
        select(t.edit, (long)err, (long)err);
        SendMessageW(t.edit, EM_SCROLLCARET, 0, 0);
        std::wstring msg = std::wstring(l == Lang::Json ? L"This isn't valid JSON" : L"This isn't valid XML") +
                           L": see line " + std::to_wstring(line) + L".";
        MessageBoxW(n.wnd, msg.c_str(), L"Notepad", MB_OK | MB_ICONINFORMATION);
        return;
    }
    out.erase(std::remove(out.begin(), out.end(), L'\n'), out.end());
    if (!text.empty() && text.back() == L'\r') out += L'\r';
    if (out == text) return;
    replace_range(t, 0, (long)text.size(), out, 0, 0);
}

HMENU syntax_menu(Tab* t) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (t && !t->langChosen ? MF_CHECKED : 0), CMD_LANGAUTO, L"By file type");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (int l = 0; l < (int)Lang::Count; ++l)
        AppendMenuW(m, MF_STRING | (t && (int)t->lang == l ? MF_CHECKED : 0), CMD_LANG + l, lang_name((Lang)l));
    return m;
}

// ------------------------------------------------------------------
// menus and commands
// ------------------------------------------------------------------
HMENU build_menu(Notepad& n, int which) {
    HMENU m = CreatePopupMenu();
    Tab* t = cur(n);
    auto add = [&](UINT id, const wchar_t* text, bool on = true, bool checked = false) {
        AppendMenuW(m, MF_STRING | (on ? 0 : MF_GRAYED) | (checked ? MF_CHECKED : 0), id, text);
    };
    auto sep = [&] { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); };
    if (which == 0) {
        add(CMD_NEW, L"New tab\tCtrl+N");
        add(CMD_OPEN, L"Open...\tCtrl+O");
        add(CMD_SAVE, L"Save\tCtrl+S", t != nullptr);
        add(CMD_SAVEAS, L"Save as...\tCtrl+Shift+S", t != nullptr);
        add(CMD_SAVEALL, L"Save all", std::any_of(n.tabs.begin(), n.tabs.end(), [](auto& x) { return x->dirty; }));
        sep();
        HMENU enc = CreatePopupMenu(), eol = CreatePopupMenu();
        for (int e = 0; e < (int)TextEncoding::Count; ++e)
            AppendMenuW(enc, MF_STRING | (t && (int)t->enc == e ? MF_CHECKED : 0), CMD_ENC + e, encoding_name((TextEncoding)e));
        for (int e = 0; e < (int)LineEnding::Count; ++e)
            AppendMenuW(eol, MF_STRING | (t && (int)t->eol == e ? MF_CHECKED : 0), CMD_EOL + e, line_ending_name((LineEnding)e));
        AppendMenuW(m, MF_POPUP | (t ? 0 : MF_GRAYED), (UINT_PTR)enc, L"Encoding");
        AppendMenuW(m, MF_POPUP | (t ? 0 : MF_GRAYED), (UINT_PTR)eol, L"Line endings");
        sep();
        add(CMD_CLOSETAB, L"Close tab\tCtrl+W");
        add(CMD_CLOSEWINDOW, L"Close window\tCtrl+Shift+W");
    } else if (which == 1) {
        HWND e = t ? t->edit : nullptr;
        CHARRANGE cr = e ? selection(e) : CHARRANGE{};
        bool sel = cr.cpMax > cr.cpMin;
        add(CMD_UNDO, L"Undo\tCtrl+Z", e && SendMessageW(e, EM_CANUNDO, 0, 0));
        add(CMD_REDO, L"Redo\tCtrl+Y", e && SendMessageW(e, EM_CANREDO, 0, 0));
        sep();
        add(CMD_CUT, L"Cut\tCtrl+X", sel);
        add(CMD_COPY, L"Copy\tCtrl+C", sel);
        add(CMD_PASTE, L"Paste\tCtrl+V", e && SendMessageW(e, EM_CANPASTE, CF_UNICODETEXT, 0));
        add(CMD_DELETE, L"Delete\tDel", sel);
        sep();
        add(CMD_FIND, L"Find...\tCtrl+F", e != nullptr);
        add(CMD_FINDNEXT, L"Find next\tF3", e != nullptr);
        add(CMD_FINDPREV, L"Find previous\tShift+F3", e != nullptr);
        add(CMD_REPLACE, L"Replace...\tCtrl+H", e != nullptr);
        add(CMD_GOTO, L"Go to...\tCtrl+G", e != nullptr);
        sep();
        add(CMD_SELALL, L"Select all\tCtrl+A", e != nullptr);
        add(CMD_TIMEDATE, L"Time/Date\tF5", e != nullptr);
        sep();
        HMENU lines = CreatePopupMenu(), conv = CreatePopupMenu();
        auto item = [](HMENU s, UINT id, const wchar_t* text) { AppendMenuW(s, id ? MF_STRING : MF_SEPARATOR, id, text); };
        item(lines, CMD_DUPLINE, L"Duplicate line\tCtrl+D");
        item(lines, CMD_DELLINE, L"Delete line\tCtrl+Shift+K");
        item(lines, CMD_LINEUP, L"Move line up\tAlt+Up");
        item(lines, CMD_LINEDOWN, L"Move line down\tAlt+Down");
        item(lines, CMD_JOIN, L"Join lines\tCtrl+J");
        item(lines, 0, nullptr);
        item(lines, CMD_SORTASC, L"Sort ascending");
        item(lines, CMD_SORTDESC, L"Sort descending");
        item(lines, CMD_REVERSE, L"Reverse order");
        item(lines, CMD_UNIQUE, L"Remove duplicate lines");
        item(lines, CMD_NOEMPTY, L"Remove empty lines");
        item(lines, CMD_TRIM, L"Trim trailing spaces");
        item(conv, CMD_UPPER, L"UPPER CASE\tCtrl+Shift+U");
        item(conv, CMD_LOWER, L"lower case\tCtrl+U");
        item(conv, CMD_TITLE, L"Title Case");
        item(conv, 0, nullptr);
        item(conv, CMD_B64ENC, L"Base64 encode");
        item(conv, CMD_B64DEC, L"Base64 decode");
        item(conv, CMD_URLENC, L"URL encode");
        item(conv, CMD_URLDEC, L"URL decode");
        AppendMenuW(m, MF_POPUP | (e ? 0 : MF_GRAYED), (UINT_PTR)lines, L"Lines");
        AppendMenuW(m, MF_POPUP | (e ? 0 : MF_GRAYED), (UINT_PTR)conv, L"Convert");
        add(CMD_FORMAT, L"Format document\tAlt+Shift+F", t && formattable(*t));
        add(CMD_COMPACT, L"Compact JSON", t && (t->lang == Lang::Json || t->lang == Lang::None));
    } else {
        HMENU zoom = CreatePopupMenu();
        AppendMenuW(zoom, MF_STRING, CMD_ZOOMIN, L"Zoom in\tCtrl+Plus");
        AppendMenuW(zoom, MF_STRING, CMD_ZOOMOUT, L"Zoom out\tCtrl+Minus");
        AppendMenuW(zoom, MF_STRING, CMD_ZOOMRESET, L"Restore default zoom\tCtrl+0");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)zoom, L"Zoom");
        add(CMD_STATUSBAR, L"Status bar", true, n.statusBar);
        add(CMD_LINENUMBERS, L"Line numbers", true, n.lineNumbers);
        add(CMD_WHITESPACE, L"Show whitespace", true, n.whitespace);
        add(CMD_WRAP, L"Word wrap", true, n.wrap);
        sep();
        AppendMenuW(m, MF_POPUP | (t ? 0 : MF_GRAYED), (UINT_PTR)syntax_menu(t), L"Syntax");
    }
    return m;
}

void command(Notepad& n, UINT id);

UINT track(Notepad& n, HMENU m, POINT pt) {
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, n.wnd, nullptr);
    DestroyMenu(m);
    return id;
}

void show_menu(Notepad& n, int which) {
    n.openMenu = which;
    invalidate_row(n);
    UpdateWindow(n.wnd);
    POINT pt{ n.menus[which].left, n.menus[which].bottom };
    ClientToScreen(n.wnd, &pt);
    UINT id = track(n, build_menu(n, which), pt);
    n.openMenu = -1;
    invalidate_row(n);
    if (id) command(n, id);
}

void tab_menu(Notepad& n, int i, POINT screen) {
    Tab& t = *n.tabs[i];
    HMENU m = CreatePopupMenu();
    bool file = !t.path.empty();
    AppendMenuW(m, MF_STRING, CMD_CLOSETAB, L"Close tab\tCtrl+W");
    AppendMenuW(m, MF_STRING | (n.tabs.size() > 1 ? 0 : MF_GRAYED), CMD_CLOSEOTHERS, L"Close other tabs");
    AppendMenuW(m, MF_STRING | (i + 1 < (int)n.tabs.size() ? 0 : MF_GRAYED), CMD_CLOSERIGHT, L"Close tabs to the right");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (file ? 0 : MF_GRAYED), CMD_COPYPATH, L"Copy path");
    AppendMenuW(m, MF_STRING | (file ? 0 : MF_GRAYED), CMD_SHOWFILE, L"Open file location");
    UINT id = track(n, m, screen);
    switch (id) {
        case CMD_CLOSETAB: close_tab(n, i); break;
        case CMD_CLOSEOTHERS:
            for (int k = (int)n.tabs.size() - 1; k >= 0; --k)
                if (n.tabs[k].get() != &t && !close_tab(n, k)) break;
            break;
        case CMD_CLOSERIGHT:
            for (int k = (int)n.tabs.size() - 1; k > i; --k)
                if (!close_tab(n, k)) break;
            break;
        case CMD_COPYPATH:
            if (OpenClipboard(n.wnd)) {
                EmptyClipboard();
                size_t bytes = (t.path.size() + 1) * sizeof(wchar_t);
                if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                    memcpy(GlobalLock(g), t.path.c_str(), bytes);
                    GlobalUnlock(g);
                    if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
                }
                CloseClipboard();
            }
            break;
        case CMD_SHOWFILE: {                // (Explorer Plus: the folder, the file selected)
            WCHAR exe[MAX_PATH * 2]{};
            GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
            ShellExecuteW(n.wnd, nullptr, exe, quote_command_arg(t.path).c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }
    }
}

void command(Notepad& n, UINT id) {
    Tab* t = cur(n);
    HWND e = t ? t->edit : nullptr;
    if (id >= CMD_ENC && id < CMD_ENC + (UINT)TextEncoding::Count) {
        if (t && t->enc != (TextEncoding)(id - CMD_ENC)) {
            t->enc = (TextEncoding)(id - CMD_ENC);
            set_dirty(n, *t, true);
            invalidate_status(n);
            keep_soon(n);
        }
        return;
    }
    if (id >= CMD_EOL && id < CMD_EOL + (UINT)LineEnding::Count) {
        if (t && t->eol != (LineEnding)(id - CMD_EOL)) {
            t->eol = (LineEnding)(id - CMD_EOL);
            set_dirty(n, *t, true);
            invalidate_status(n);
            keep_soon(n);
        }
        return;
    }
    // (a big text still going in: all of it first, but for what only changes the window)
    if (t && !t->rest.empty() && id < CMD_ENC) {
        switch (id) {
            case CMD_NEW: case CMD_OPEN: case CMD_CLOSETAB: case CMD_CLOSEWINDOW: case CMD_COPY: case CMD_ZOOMIN:
            case CMD_ZOOMOUT: case CMD_ZOOMRESET: case CMD_STATUSBAR: case CMD_WRAP: case CMD_LINENUMBERS:
            case CMD_WHITESPACE: case CMD_CLOSEOTHERS:
            case CMD_CLOSERIGHT: case CMD_COPYPATH: case CMD_SHOWFILE:
                break;
            default:
                finish_loading(n, *t);
        }
    }
    if (id == CMD_LANGAUTO || (id >= CMD_LANG && id < CMD_LANG + (UINT)Lang::Count)) {
        if (!t) return;
        Lang l = id == CMD_LANGAUTO ? lang_for_file(t->path) : (Lang)(id - CMD_LANG);
        t->langChosen = id != CMD_LANGAUTO;
        if (l != t->lang) {
            t->lang = l;
            recolour(n, *t, true);
        }
        invalidate_status(n);
        keep_soon(n);
        return;
    }
    switch (id) {
        case CMD_NEW: new_untitled(n); break;
        case CMD_DUPLINE: case CMD_DELLINE: case CMD_LINEUP: case CMD_LINEDOWN: case CMD_SORTASC: case CMD_SORTDESC:
        case CMD_REVERSE: case CMD_UNIQUE: case CMD_NOEMPTY: case CMD_TRIM: case CMD_JOIN:
            if (t) line_command(*t, id);
            break;
        case CMD_UPPER: case CMD_LOWER: case CMD_TITLE: case CMD_B64ENC: case CMD_B64DEC: case CMD_URLENC: case CMD_URLDEC:
            if (t) convert_command(n, *t, id);
            break;
        case CMD_FORMAT: case CMD_COMPACT:
            if (t) format_document(n, *t, id == CMD_COMPACT);
            break;
        case CMD_OPEN: open_dialog(n); break;
        case CMD_SAVE: if (t) save(n, *t, false); break;
        case CMD_SAVEAS: if (t) save(n, *t, true); break;
        case CMD_SAVEALL:
            for (int i = 0; i < (int)n.tabs.size(); ++i)
                if (n.tabs[i]->dirty) {
                    if (n.tabs[i]->path.empty()) activate(n, i);    // (which one the dialog is for)
                    if (!save(n, *n.tabs[i], false)) break;
                }
            break;
        case CMD_CLOSETAB: close_tab(n, n.active); return;
        case CMD_CLOSEWINDOW: PostMessageW(n.wnd, WM_CLOSE, 0, 0); return;
        case CMD_UNDO: if (e) SendMessageW(e, EM_UNDO, 0, 0); break;
        case CMD_REDO: if (e) SendMessageW(e, EM_REDO, 0, 0); break;
        case CMD_CUT: if (e) SendMessageW(e, WM_CUT, 0, 0); break;
        case CMD_COPY: if (e) SendMessageW(e, WM_COPY, 0, 0); break;
        case CMD_PASTE: if (e) SendMessageW(e, EM_PASTESPECIAL, CF_UNICODETEXT, 0); break;
        case CMD_DELETE: if (e) SendMessageW(e, WM_CLEAR, 0, 0); break;
        case CMD_FIND: show_find(n, false); return;
        case CMD_REPLACE: show_find(n, true); return;
        case CMD_FINDNEXT: find_next(n, true); break;
        case CMD_FINDPREV: find_next(n, false); break;
        case CMD_GOTO: go_to(n); break;
        case CMD_SELALL: if (e) select(e, 0, -1); break;
        case CMD_TIMEDATE: if (t) insert_time(*t); break;
        case CMD_ZOOMIN: if (t) set_zoom(n, t->zoom < 100 ? t->zoom + 10 : t->zoom + 25); break;
        case CMD_ZOOMOUT: if (t) set_zoom(n, t->zoom <= 100 ? t->zoom - 10 : t->zoom - 25); break;
        case CMD_ZOOMRESET: set_zoom(n, 100); break;
        case CMD_LINENUMBERS:
            n.lineNumbers = !n.lineNumbers;
            layout(n);
            keep_soon(n);
            break;
        case CMD_WHITESPACE:
            n.whitespace = !n.whitespace;
            if (t) InvalidateRect(t->edit, nullptr, FALSE);
            keep_soon(n);
            break;
        case CMD_STATUSBAR:
            n.statusBar = !n.statusBar;
            layout(n);
            InvalidateRect(n.wnd, nullptr, FALSE);
            keep_soon(n);
            break;
        case CMD_WRAP:
            n.wrap = !n.wrap;
            for (auto& x : n.tabs) SendMessageW(x->edit, EM_SETTARGETDEVICE, 0, n.wrap ? 0 : 1);
            invalidate_status(n);
            keep_soon(n);
            break;
    }
    if ((t = cur(n))) SetFocus(t->edit);
}

// The keys: the commands', and the text box's own editing ones. Its formatting keys (Ctrl+B,
// Ctrl+E, Ctrl+1, ...) would change the look of the whole text: they do nothing.
bool key(Notepad& n, const MSG& m) {
    UINT vk = (UINT)m.wParam;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    if (alt && !ctrl) {
        if (m.message != WM_SYSKEYDOWN) return false;
        if ((vk == VK_UP || vk == VK_DOWN) && !shift) { command(n, vk == VK_UP ? CMD_LINEUP : CMD_LINEDOWN); return true; }
        if (vk == 'F' && shift) { command(n, CMD_FORMAT); return true; }
        if (shift) return false;
        for (int i = 0; i < 3; ++i)
            if (vk == (UINT)kMenuNames[i][0]) { show_menu(n, i); return true; }
        return false;
    }
    if (ctrl && alt) return false;                  // (AltGr: a character)
    if (!ctrl) {
        switch (vk) {
            case VK_F3: command(n, shift ? CMD_FINDPREV : CMD_FINDNEXT); return true;
            case VK_F5: command(n, CMD_TIMEDATE); return true;
        }
        return false;
    }
    switch (vk) {
        case 'N': case 'T': command(n, CMD_NEW); return true;
        case 'O': command(n, CMD_OPEN); return true;
        case 'S': command(n, shift ? CMD_SAVEAS : CMD_SAVE); return true;
        case 'W': command(n, shift ? CMD_CLOSEWINDOW : CMD_CLOSETAB); return true;
        case VK_F4: command(n, CMD_CLOSETAB); return true;
        case 'F': command(n, CMD_FIND); return true;
        case 'H': command(n, CMD_REPLACE); return true;
        case 'G': command(n, CMD_GOTO); return true;
        case 'D': command(n, CMD_DUPLINE); return true;
        case 'J': command(n, CMD_JOIN); return true;
        case 'U': command(n, shift ? CMD_UPPER : CMD_LOWER); return true;
        case 'K': if (shift) command(n, CMD_DELLINE); return true;
        case VK_OEM_PLUS: case VK_ADD: command(n, CMD_ZOOMIN); return true;
        case VK_OEM_MINUS: case VK_SUBTRACT: command(n, CMD_ZOOMOUT); return true;
        case '0': case VK_NUMPAD0: command(n, CMD_ZOOMRESET); return true;
        case 'Z': if (shift) { command(n, CMD_REDO); return true; } return false;
        case VK_TAB: case VK_NEXT: case VK_PRIOR: {
            int k = (int)n.tabs.size();
            if (k > 1) activate(n, (n.active + ((vk == VK_TAB ? !shift : vk == VK_NEXT) ? 1 : k - 1)) % k);
            return true;
        }
        case 'A': case 'C': case 'V': case 'X': case 'Y':
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END:
        case VK_BACK: case VK_DELETE: case VK_INSERT: case VK_CONTROL: case VK_SHIFT:
            return false;
    }
    if (vk >= '1' && vk <= '9') {
        int k = (int)n.tabs.size();
        if (k) activate(n, vk == '9' ? k - 1 : std::min((int)(vk - '1'), k - 1));
        return true;
    }
    return true;
}

// ------------------------------------------------------------------
// the window: the tab row, the menu row, the text, the status bar
// ------------------------------------------------------------------
RECT tab_rect(const Notepad& n, int i) {
    int x = n.strip.left + i * n.tabW - n.scroll;
    return { x, px(n, 6), x + n.tabW, n.rowH };
}

RECT close_rect(const Notepad& n, int i) {
    RECT r = tab_rect(n, i);
    int s = px(n, 24), top = (r.top + r.bottom - s) / 2;
    return { r.right - px(n, 8) - s, top, r.right - px(n, 8), top + s };
}

void make_fonts(Notepad& n) {
    for (HFONT f : { n.font, n.glyphs, n.glyphsSmall })
        if (f) DeleteObject(f);
    n.font = theme_create_font(n.dpi, FW_REGULAR);
    auto glyph_font = [&](int size) {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(size, n.dpi, 96);
        lf.lfWeight = FW_REGULAR;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = ANTIALIASED_QUALITY;
        wcscpy_s(lf.lfFaceName, theme_glyph_face());
        return CreateFontIndirectW(&lf);
    };
    n.glyphs = glyph_font(12);
    n.glyphsSmall = glyph_font(10);
}

void layout(Notepad& n) {
    RECT rc{};
    GetClientRect(n.wnd, &rc);
    n.rowH = px(n, 40);
    n.menuH = px(n, 34);
    n.statusH = n.statusBar ? px(n, 26) : 0;
    HDC dc = GetDC(n.wnd);
    HGDIOBJ of = SelectObject(dc, n.font);
    int x = px(n, 6);
    for (int i = 0; i < 3; ++i) {
        int w = text_width(dc, kMenuNames[i]) + px(n, 20);
        n.menus[i] = { x, n.rowH + px(n, 3), x + w, n.rowH + n.menuH - px(n, 3) };
        x += w + px(n, 2);
    }
    SelectObject(dc, of);
    ReleaseDC(n.wnd, dc);
    n.editArea = { 0, n.rowH + n.menuH + 1, rc.right, std::max(n.rowH + n.menuH + 1, (int)rc.bottom - n.statusH) };
    Tab* t = cur(n);
    int eh = n.editArea.bottom - n.editArea.top;
    n.gutW = n.lineNumbers && t ? std::min(gutter_width(n, *t), (int)(n.editArea.right - n.editArea.left) / 2) : 0;
    if (n.gutter) {
        MoveWindow(n.gutter, n.editArea.left, n.editArea.top, n.gutW, eh, TRUE);
        ShowWindow(n.gutter, n.gutW ? SW_SHOWNA : SW_HIDE);
        if (n.gutW) InvalidateRect(n.gutter, nullptr, FALSE);
    }
    if (t) MoveWindow(t->edit, n.editArea.left + n.gutW, n.editArea.top, n.editArea.right - n.editArea.left - n.gutW, eh, TRUE);
    // the tabs share the row (each between a minimum and a maximum width) with the window's
    // buttons and some room to drag it by; more than fit scroll, the active one in view
    int count = (int)n.tabs.size(), plusW = px(n, 36);
    int left = px(n, 8), avail = std::max(px(n, 80), (int)rc.right - left - plusW - 3 * px(n, 46) - px(n, 48));
    n.tabW = count ? std::clamp(avail / count, px(n, 90), px(n, 240)) : 0;
    int total = count * n.tabW;
    if (total <= avail) {
        n.scroll = 0;
    } else {
        int a = std::max(0, n.active) * n.tabW;
        n.scroll = std::clamp(n.scroll, a + n.tabW - avail, a);
        n.scroll = std::clamp(n.scroll, 0, total - avail);
    }
    n.strip = { left, 0, left + std::min(total, avail), n.rowH };
    int py = px(n, 8);
    n.plus = { n.strip.right + px(n, 4), py, n.strip.right + px(n, 4) + px(n, 32), py + px(n, 28) };
    // the tooltips: one per tab, and "+"
    for (int i = 0; i < n.tips; ++i) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = n.wnd;
        ti.uId = (UINT_PTR)i + 1;
        SendMessageW(n.tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    n.tips = count;
    for (int i = 0; i <= count; ++i) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = n.wnd;
        ti.uId = i < count ? (UINT_PTR)i + 1 : kTipPlus;
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        if (i < count) {
            RECT r = tab_rect(n, i);
            IntersectRect(&ti.rect, &r, &n.strip);
        } else {
            ti.rect = n.plus;
        }
        SendMessageW(n.tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
        SendMessageW(n.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

void glyph(Notepad& n, HDC dc, wchar_t g, const RECT& r, COLORREF c, bool small = false) {
    HGDIOBJ of = SelectObject(dc, small ? n.glyphsSmall : n.glyphs);
    SetTextColor(dc, c);
    RECT rr = r;
    wchar_t s[2] = { g, 0 };
    DrawTextW(dc, s, 1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

void paint_status(Notepad& n, HDC dc, const RECT& rc) {
    const Palette& c = pal();
    RECT bar{ 0, rc.bottom - n.statusH, rc.right, rc.bottom };
    fill_rect(dc, bar, c.layer);
    fill_rect(dc, RECT{ 0, bar.top, rc.right, bar.top + 1 }, c.divider);
    for (RECT& r : n.parts) r = {};
    Tab* t = cur(n);
    if (!t) return;
    long line = 1, col = 1;
    caret_place(n, *t, line, col);
    CHARRANGE cr = selection(t->edit);
    long chars = text_length(t->edit);
    std::wstring text[SP_COUNT] = {
        L"Ln " + grouped(line) + L", Col " + grouped(col),
        !t->rest.empty()         ? L"Loading… " + std::to_wstring(t->restAt * 100 / t->rest.size()) + L"%"
        : !t->word.empty()       ? grouped(t->wordCount) + (t->wordCount == 1 ? L" occurrence" : L" occurrences") +
                                       (t->countAt >= 0 ? L"…" : L"")
        : cr.cpMax > cr.cpMin    ? grouped(cr.cpMax - cr.cpMin) + L" of " + grouped(chars) + L" characters"
                                 : grouped(chars) + (chars == 1 ? L" character" : L" characters"),
        std::to_wstring(t->zoom) + L"%",
        lang_name(t->lang),
        line_ending_name(t->eol),
        encoding_name(t->enc),
    };
    SetTextColor(dc, c.text);
    int x = rc.right - px(n, 8);
    for (int i = SP_ENC; i >= SP_ZOOM; --i) {
        int w = std::max(text_width(dc, text[i].c_str()) + px(n, 28), px(n, i == SP_ZOOM ? 60 : 110));
        n.parts[i] = { x - w, bar.top + 1, x, bar.bottom };
        x -= w;
        fill_rect(dc, RECT{ x, bar.top + px(n, 6), x + 1, bar.bottom - px(n, 6) }, c.divider);
    }
    int lx = px(n, 16);
    for (int i = SP_CARET; i <= SP_CHARS; ++i) {
        int w = std::max(text_width(dc, text[i].c_str()) + px(n, 28), px(n, i == SP_CARET ? 140 : 180));
        n.parts[i] = { lx, bar.top + 1, std::min(lx + w, x), bar.bottom };
        lx += w;
        if (i == SP_CARET) fill_rect(dc, RECT{ lx - px(n, 14), bar.top + px(n, 6), lx - px(n, 13), bar.bottom - px(n, 6) }, c.divider);
    }
    for (int i = 0; i < SP_COUNT; ++i) {
        RECT r = n.parts[i];
        if (r.right <= r.left) continue;
        if (i >= SP_ZOOM) InflateRect(&r, -px(n, 14), 0);
        DrawTextW(dc, text[i].c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | (i >= SP_ZOOM ? DT_LEFT : 0));
    }
}

// the window's buttons, at the tab row's right end: minimize, maximize, close
RECT cap_button(const Notepad& n, int i) {
    RECT rc{};
    GetClientRect(n.wnd, &rc);
    int w = px(n, 46), right = rc.right - (2 - i) * w;
    return { right - w, 0, right, n.rowH };
}

void paint_cap_buttons(Notepad& n, HDC dc, COLORREF bg) {
    const Palette& c = pal();
    static const wchar_t kGlyph[3] = { L'\xE921', L'\xE922', L'\xE8BB' };
    for (int i = 0; i < 3; ++i) {
        RECT r = cap_button(n, i);
        COLORREF fg = n.activeWnd ? c.text : c.textDisabled;
        bool on = n.capHot == i || n.capPressed == i;
        if (i == 2 && on) {
            fill_rect(dc, r, n.capPressed == 2 ? blend(c.closeHover, RGB(0, 0, 0), 0.1) : c.closeHover);
            fg = RGB(255, 255, 255);
        } else if (on) {
            fill_rect(dc, r, blend(bg, g_dark ? RGB(255, 255, 255) : RGB(0, 0, 0), n.capPressed == i ? 0.04 : 0.07));
        }
        glyph(n, dc, i == 1 && IsZoomed(n.wnd) ? L'\xE923' : kGlyph[i], r, fg, true);
    }
}

void paint(Notepad& n) {
    BufferedPaint bp(n.wnd);
    HDC dc = bp.dc;
    const Palette& c = pal();
    RECT rc = bp.rc;
    COLORREF capBg = n.activeWnd ? c.caption : c.captionInactive;
    fill_rect(dc, RECT{ 0, 0, rc.right, n.rowH }, capBg);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, n.font);
    paint_cap_buttons(n, dc, capBg);
    // the tabs (scrolled: cut at the strip's ends)
    int saved = SaveDC(dc);
    if (n.scroll || (int)n.tabs.size() * n.tabW > n.strip.right - n.strip.left)
        IntersectClipRect(dc, n.strip.left, 0, n.strip.right, n.rowH);
    for (int i = 0; i < (int)n.tabs.size(); ++i) {
        const Tab& t = *n.tabs[i];
        RECT r = tab_rect(n, i);
        if (r.right < n.strip.left || r.left > n.strip.right) continue;
        bool act = i == n.active, hot = i == n.hotTab;
        if (act) {
            fill_tab_shape(dc, r, px(n, 8), c.layer);
        } else if (hot) {
            RECT h{ r.left + px(n, 2), r.top + px(n, 2), r.right - px(n, 2), r.bottom - px(n, 4) };
            fill_rrect(dc, h, px(n, 6), c.tabHover);
        } else if (i + 1 != n.active && i + 1 != n.hotTab && i + 1 < (int)n.tabs.size()) {
            fill_rect(dc, RECT{ r.right - 1, r.top + px(n, 10), r.right, r.bottom - px(n, 10) }, c.divider);
        }
        RECT cr = close_rect(n, i);
        RECT tr{ r.left + px(n, 12), r.top, cr.left - px(n, 4), r.bottom - (act ? 0 : px(n, 2)) };
        std::wstring name = tab_name(t);
        SetTextColor(dc, act ? c.text : c.textSecondary);
        DrawTextW(dc, name.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        // unsaved: a dot, a close button when pointed at
        if (t.dirty && !hot) {
            int d = px(n, 8), cx = (cr.left + cr.right) / 2, cy = (cr.top + cr.bottom) / 2;
            fill_rrect(dc, RECT{ cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d }, d / 2, act ? c.text : c.textSecondary);
        } else if (act || hot) {
            if (n.hotClose == i) fill_rrect(dc, cr, px(n, 4), act ? c.hover : c.layer);
            glyph(n, dc, L'\xE711', cr, c.text, true);
        }
    }
    RestoreDC(dc, saved);
    if (n.hotPlus) fill_rrect(dc, n.plus, px(n, 4), c.tabHover);
    glyph(n, dc, L'\xE710', n.plus, c.text);
    // the menu row
    fill_rect(dc, RECT{ 0, n.rowH, rc.right, n.rowH + n.menuH }, c.layer);
    for (int i = 0; i < 3; ++i) {
        if (n.openMenu == i || n.hotMenu == i) fill_rrect(dc, n.menus[i], px(n, 4), n.openMenu == i ? c.pressed : c.hover);
        SetTextColor(dc, c.text);
        RECT r = n.menus[i];
        DrawTextW(dc, kMenuNames[i], -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }
    fill_rect(dc, RECT{ 0, n.rowH + n.menuH, rc.right, n.rowH + n.menuH + 1 }, c.divider);
    if (n.tabs.empty()) fill_rect(dc, n.editArea, c.content);
    if (n.statusBar) paint_status(n, dc, rc);
    SelectObject(dc, of);
}

void set_hot(Notepad& n, int tab, int close, int menu, bool plus) {
    if (tab == n.hotTab && close == n.hotClose && menu == n.hotMenu && plus == n.hotPlus) return;
    n.hotTab = tab;
    n.hotClose = close;
    n.hotMenu = menu;
    n.hotPlus = plus;
    invalidate_row(n);
}

int tab_at(const Notepad& n, POINT pt) {
    if (!PtInRect(&n.strip, pt)) return -1;
    for (int i = 0; i < (int)n.tabs.size(); ++i) {
        RECT r = tab_rect(n, i);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

void on_move(Notepad& n, POINT pt) {
    if (!n.tracking) {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, n.wnd, 0 };
        n.tracking = TrackMouseEvent(&tme) != FALSE;
    }
    if (n.dragTab >= 0) {
        if (!n.dragging && abs(pt.x - n.dragX) > GetSystemMetrics(SM_CXDRAG)) n.dragging = true;
        if (n.dragging && n.tabW > 0) {
            int to = std::clamp((int)(pt.x - n.strip.left + n.scroll) / n.tabW, 0, (int)n.tabs.size() - 1);
            if (to != n.dragTab) {
                auto t = std::move(n.tabs[n.dragTab]);
                n.tabs.erase(n.tabs.begin() + n.dragTab);
                n.tabs.insert(n.tabs.begin() + to, std::move(t));
                n.active = n.dragTab = to;
                layout(n);
                set_hot(n, to, -1, -1, false);
                invalidate_row(n);
                keep_soon(n);
            }
        }
        return;
    }
    int tab = tab_at(n, pt), close = -1, menu = -1;
    if (tab >= 0) {
        RECT cr = close_rect(n, tab);
        if (PtInRect(&cr, pt)) close = tab;
    }
    for (int i = 0; i < 3; ++i)
        if (PtInRect(&n.menus[i], pt)) menu = i;
    set_hot(n, tab, close, menu, PtInRect(&n.plus, pt) != FALSE);
}

void on_down(Notepad& n, POINT pt) {
    int tab = tab_at(n, pt);
    if (tab >= 0) {
        RECT cr = close_rect(n, tab);
        if (PtInRect(&cr, pt) && (tab == n.active || tab == n.hotTab)) { close_tab(n, tab); return; }
        if (tab != n.active) activate(n, tab);
        n.dragTab = tab;
        n.dragX = pt.x;
        n.dragging = false;
        SetCapture(n.wnd);
        return;
    }
    if (PtInRect(&n.plus, pt)) { command(n, CMD_NEW); return; }
    for (int i = 0; i < 3; ++i)
        if (PtInRect(&n.menus[i], pt)) { show_menu(n, i); return; }
    if (Tab* t = cur(n)) {
        HMENU m = nullptr;
        if (PtInRect(&n.parts[SP_LANG], pt)) {
            RECT r = n.parts[SP_LANG];
            POINT at{ r.left, r.top };
            ClientToScreen(n.wnd, &at);
            HMENU sm = syntax_menu(t);
            UINT id = TrackPopupMenuEx(sm, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, at.x, at.y, n.wnd, nullptr);
            DestroyMenu(sm);
            if (id) command(n, id);
        } else if (PtInRect(&n.parts[SP_EOL], pt) || PtInRect(&n.parts[SP_ENC], pt)) {
            bool enc = PtInRect(&n.parts[SP_ENC], pt) != FALSE;
            m = CreatePopupMenu();
            if (enc) {
                for (int e = 0; e < (int)TextEncoding::Count; ++e)
                    AppendMenuW(m, MF_STRING | ((int)t->enc == e ? MF_CHECKED : 0), CMD_ENC + e, encoding_name((TextEncoding)e));
            } else {
                for (int e = 0; e < (int)LineEnding::Count; ++e)
                    AppendMenuW(m, MF_STRING | ((int)t->eol == e ? MF_CHECKED : 0), CMD_EOL + e, line_ending_name((LineEnding)e));
            }
            RECT r = n.parts[enc ? SP_ENC : SP_EOL];
            POINT at{ r.left, r.top };
            ClientToScreen(n.wnd, &at);
            UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, at.x, at.y, n.wnd, nullptr);
            DestroyMenu(m);
            if (id) command(n, id);
        } else if (PtInRect(&n.parts[SP_ZOOM], pt)) {
            command(n, CMD_ZOOMRESET);
        } else if (PtInRect(&n.parts[SP_CARET], pt)) {
            command(n, CMD_GOTO);
        }
    }
}

void apply_theme(Notepad& n) {
    BOOL dark = g_dark;
    DwmSetWindowAttribute(n.wnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    for (auto& t : n.tabs) style_edit(n, *t);
    request_colour(n);
    if (n.find) DwmSetWindowAttribute(n.find, 20, &dark, sizeof(dark));
    RedrawWindow(n.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}

void on_close(Notepad& n) {
    // the tabs are kept, unsaved text too, for next time; where that fails, the usual question
    if (!keep_now(n)) {
        for (int i = (int)n.tabs.size() - 1; i >= 0; --i) {
            if (!n.tabs[i]->dirty) continue;
            if (!close_tab(n, i)) return;
            if (!s_np) return;              // (that was the last tab: the window is gone)
        }
    }
    DestroyWindow(n.wnd);
}

// The frame, as the main window's (chrome.cpp): no title bar, the tab row in its place. Its
// empty part moves the window (and snaps it, maximizes on a double-click), its buttons are the
// system's to Windows (snap layouts over maximize).
int frame_y(const Notepad& n) {
    return GetSystemMetricsForDpi(SM_CYFRAME, n.dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, n.dpi);
}

void invalidate_cap_buttons(Notepad& n) {
    RECT r = cap_button(n, 0);
    r.right = cap_button(n, 2).right;
    InvalidateRect(n.wnd, &r, FALSE);
}

int cap_button_of(WPARAM ht) { return ht == HTMINBUTTON ? 0 : ht == HTMAXBUTTON ? 1 : ht == HTCLOSE ? 2 : -1; }

LRESULT hit_test(Notepad& n, LPARAM lp) {
    LRESULT def = DefWindowProcW(n.wnd, WM_NCHITTEST, 0, lp);   // (the side and bottom borders)
    if (def != HTCLIENT) return def;
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ScreenToClient(n.wnd, &pt);
    if (pt.y >= n.rowH) return HTCLIENT;
    int b = -1;
    for (int i = 0; i < 3; ++i) {
        RECT r = cap_button(n, i);
        if (PtInRect(&r, pt)) b = i;
    }
    if (b >= 0) return b == 0 ? HTMINBUTTON : b == 1 ? HTMAXBUTTON : HTCLOSE;
    int fy = frame_y(n);
    if (!IsZoomed(n.wnd) && pt.y < fy) {        // (the top border is in the client area)
        RECT rc{};
        GetClientRect(n.wnd, &rc);
        return pt.x < fy * 2 ? HTTOPLEFT : pt.x >= rc.right - fy * 2 ? HTTOPRIGHT : HTTOP;
    }
    if (n.dragTab >= 0 || tab_at(n, pt) >= 0 || PtInRect(&n.plus, pt)) return HTCLIENT;
    return HTCAPTION;
}

bool frame_message(Notepad& n, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    switch (msg) {
        case WM_NCCALCSIZE: {
            if (!wp) return false;
            auto* p = (NCCALCSIZE_PARAMS*)lp;
            LONG top = p->rgrc[0].top;
            DefWindowProcW(n.wnd, msg, wp, lp);
            p->rgrc[0].top = top + (IsZoomed(n.wnd) ? frame_y(n) : 0);    // (maximized: the frame is off the screen)
            *result = 0;
            return true;
        }
        case WM_NCHITTEST:
            *result = hit_test(n, lp);
            return true;
        case WM_NCACTIVATE:
            n.activeWnd = wp != FALSE;
            invalidate_row(n);
            *result = DefWindowProcW(n.wnd, msg, wp, -1);   // (-1: no classic frame painted)
            return true;
        case WM_NCMOUSEMOVE: {
            int b = cap_button_of(wp);
            if (b != n.capHot) { n.capHot = b; invalidate_cap_buttons(n); }
            TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE | TME_NONCLIENT, n.wnd, 0 };
            TrackMouseEvent(&te);
            if (b < 0) return false;
            *result = 0;
            return true;
        }
        case WM_NCMOUSELEAVE:
        case WM_MOUSEMOVE:                          // (into the client area: a press there is let go)
            if (n.capHot >= 0 || n.capPressed >= 0) { n.capHot = n.capPressed = -1; invalidate_cap_buttons(n); }
            return false;
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK: {
            int b = cap_button_of(wp);
            if (b < 0) return false;
            n.capPressed = b;
            invalidate_cap_buttons(n);
            *result = 0;
            return true;
        }
        case WM_NCLBUTTONUP: {
            int b = cap_button_of(wp);
            bool fire = b >= 0 && b == n.capPressed;
            if (n.capPressed >= 0) { n.capPressed = -1; invalidate_cap_buttons(n); }
            if (b < 0) return false;
            if (fire) {
                n.capHot = -1;
                PostMessageW(n.wnd, WM_SYSCOMMAND, b == 0 ? SC_MINIMIZE : b == 2 ? SC_CLOSE : IsZoomed(n.wnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
            }
            *result = 0;
            return true;
        }
    }
    return false;
}

void set_app_id(HWND hwnd, bool on);

LRESULT CALLBACK notepad_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* np = static_cast<Notepad*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        np->wnd = hwnd;
        np->dpi = (int)GetDpiForWindow(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)np);
    }
    auto* pn = reinterpret_cast<Notepad*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!pn) return DefWindowProcW(hwnd, msg, wp, lp);
    Notepad& n = *pn;
    if (msg == s_findMsg && s_findMsg) {
        auto* fr = (FINDREPLACEW*)lp;
        if (fr->Flags & FR_DIALOGTERM) { n.find = nullptr; return 0; }
        n.findFlags = (fr->Flags & (FR_MATCHCASE | FR_WHOLEWORD)) | (n.replacing || (fr->Flags & FR_DOWN) ? FR_DOWN : 0);
        if (Tab* t = cur(n)) finish_loading(n, *t);
        if (fr->Flags & FR_FINDNEXT) find_next(n, (n.findFlags & FR_DOWN) != 0);
        else if (fr->Flags & FR_REPLACE) replace_one(n);
        else if (fr->Flags & FR_REPLACEALL) replace_all(n);
        return 0;
    }
    if (LRESULT r = 0; frame_message(n, msg, wp, lp, &r)) return r;
    switch (msg) {
        case WM_CREATE:
            n.dpi = (int)GetDpiForWindow(hwnd);
            make_fonts(n);
            n.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                                    hwnd, nullptr, g_hinst, nullptr);
            SendMessageW(n.tip, TTM_SETMAXTIPWIDTH, 0, MulDiv(600, n.dpi, 96));
            n.gutter = create_gutter(n);
            DragAcceptFiles(hwnd, TRUE);
            apply_theme(n);
            return 0;
        case WM_SIZE:
            layout(n);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = { px(n, 420), px(n, 300) };
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(n); return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
            on_down(n, POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            return 0;
        case WM_LBUTTONUP:
            if (n.dragTab >= 0) {
                n.dragTab = -1;
                n.dragging = false;
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            n.dragTab = -1;
            n.dragging = false;
            return 0;
        case WM_MBUTTONUP: {
            int tab = tab_at(n, POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (tab >= 0) close_tab(n, tab);
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int tab = tab_at(n, pt);
            if (tab >= 0) {
                ClientToScreen(hwnd, &pt);
                tab_menu(n, tab, pt);
            }
            return 0;
        }
        case WM_MOUSEMOVE: on_move(n, POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }); return 0;
        case WM_MOUSELEAVE:
            n.tracking = false;
            if (n.dragTab < 0) set_hot(n, -1, -1, -1, false);
            return 0;
        case WM_CONTEXTMENU:
            if (Tab* t = cur(n); t && (HWND)wp == t->edit) {
                POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                if (pt.x == -1 && pt.y == -1) {         // (the keyboard's: at the caret)
                    CHARRANGE cr = selection(t->edit);
                    SendMessageW(t->edit, EM_POSFROMCHAR, (WPARAM)&pt, cr.cpMax);
                    ClientToScreen(t->edit, &pt);
                }
                UINT id = track(n, build_menu(n, 1), pt);
                if (id) command(n, id);
                return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* hdr = (NMHDR*)lp;
            if (hdr->code == EN_SELCHANGE) {
                auto* sc = (SELCHANGE*)lp;
                for (auto& t : n.tabs) {
                    if (t->edit != hdr->hwndFrom) continue;
                    if (!t->loading) t->pendingSel = { -1, -1 };    // (the user's own: not the restored one)
                    t->selLow = std::min(t->selLow, sc->chrg.cpMin);
                    if (!t->word.empty() && (sc->chrg.cpMin != t->wordSel.cpMin || sc->chrg.cpMax != t->wordSel.cpMax)) clear_word(n, *t);
                }
                invalidate_status(n);
                request_colour(n);                  // (the caret's line number; the selection drawn over the whitespace)
                return 0;
            }
            if (hdr->hwndFrom == n.tip && hdr->code == TTN_GETDISPINFOW) {
                auto* di = (NMTTDISPINFOW*)lp;
                static std::wstring text;
                int i = (int)hdr->idFrom - 1;
                text = hdr->idFrom == (UINT_PTR)kTipPlus ? L"New tab (Ctrl+N)"
                       : i >= 0 && i < (int)n.tabs.size() ? (n.tabs[i]->path.empty() ? L"Untitled" : n.tabs[i]->path) : L"";
                di->lpszText = text.data();
                return 0;
            }
            break;
        }
        case WM_COMMAND:
            if (HIWORD(wp) == EN_CHANGE) {
                for (auto& t : n.tabs)
                    if (t->edit == (HWND)lp && !t->loading) {
                        set_dirty(n, *t, true);
                        keep_soon(n);
                        // (where it changed: from before the selection, now or since the last colouring)
                        long at = std::min(t->selLow, selection(t->edit).cpMin), from = std::min(t->changedFrom, at);
                        t->changedFrom = std::max(0L, from - 1);
                        if (at < t->lineCp) t->lineCp = -1;     // (the lines before the known one changed)
                        if (at < t->gutCp) t->gutCp = -1;
                        while (!t->marks.empty() && t->marks.back().first >= at) t->marks.pop_back();
                        t->lines = -1;
                        clear_word(n, *t);
                        request_colour(n);
                    }
                invalidate_status(n);
            }
            return 0;
        case WM_TIMER:
            if (wp == TIMER_KEEP) keep_now(n);
            if (wp == TIMER_WORK) work_step(n);
            if (wp == TIMER_UNREAD && retry_unread(n)) KillTimer(hwnd, TIMER_UNREAD);
            return 0;
        case WM_NOTEPAD_UNREAD:
            tell_unread(n);
            return 0;
        case WM_NOTEPAD_DROP: {
            // a restored tab whose text couldn't be read when it was shown (gone since)
            auto find = [&] {
                for (int i = 0; i < (int)n.tabs.size(); ++i)
                    if (n.tabs[i]->id == (unsigned)lp) return i;
                return -1;
            };
            int i = find();
            if (i < 0) return 0;
            if (wp) {
                std::wstring msg = L"This file couldn't be opened:\n" + n.tabs[i]->path;
                MessageBoxW(hwnd, msg.c_str(), L"Notepad", MB_OK | MB_ICONWARNING);
                if (!s_np || (i = find()) < 0) return 0;
            }
            if (n.tabs.size() == 1 && !new_untitled(n)) return 0;     // (the window stays)
            if ((i = find()) < 0) return 0;
            n.tabs[i]->dirty = false;
            close_tab(n, i);
            return 0;
        }
        case WM_NOTEPAD_WORK:
            work_step(n);
            return 0;
        case WM_NOTEPAD_COLOUR:
            n.colourPosted = false;
            if (Tab* t = cur(n)) {
                colour(*t);
                schedule_work(n);                   // (lexed ahead: a jump down is coloured at once)
                draw_whitespace(n, *t);             // (the box may have drawn a line over its own)
                refresh_gutter(n);
            }
            return 0;
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> files;
            for (UINT i = 0; i < count; ++i) {
                WCHAR p[MAX_PATH * 4];
                if (DragQueryFileW(drop, i, p, ARRAYSIZE(p)) && !(GetFileAttributesW(p) & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(p);
            }
            DragFinish(drop);
            for (auto& f : files) open_path(n, f);
            return 0;
        }
        case WM_COPYDATA: {
            // another Explorer Plus process hands over its files
            auto* cd = (COPYDATASTRUCT*)lp;
            if (!cd || cd->dwData != kCopyOpen || cd->cbData % sizeof(wchar_t) || cd->cbData > (16u << 20)) return FALSE;
            auto* files = new std::vector<std::wstring>(unpack_paths((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t)));
            PostMessageW(hwnd, WM_NOTEPAD_OPEN, 0, (LPARAM)files);
            return TRUE;
        }
        case WM_NOTEPAD_OPEN: {
            std::unique_ptr<std::vector<std::wstring>> files((std::vector<std::wstring>*)lp);
            for (auto& f : *files) open_path(n, f);
            if (n.tabs.empty()) new_untitled(n);
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            return 0;
        }
        case WM_SETFOCUS:
            if (Tab* t = cur(n)) SetFocus(t->edit);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) {
                check_files(n);
                if (Tab* t = cur(n); t && !HIWORD(wp)) SetFocus(t->edit);
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (!((lp && lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) || wp == SPI_SETHIGHCONTRAST)) break;
            [[fallthrough]];
        case WM_SYSCOLORCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            theme_reload();
            apply_theme(n);
            break;
        case WM_DPICHANGED: {
            n.dpi = HIWORD(wp);
            make_fonts(n);
            for (auto& t : n.tabs) style_edit(n, *t);
            request_colour(n);
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout(n);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_QUERYENDSESSION:
            keep_now(n);
            return TRUE;
        case WM_ENDSESSION:
            if (wp) keep_now(n);
            return 0;
        case WM_CLOSE: on_close(n); return 0;
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_KEEP);
            set_app_id(hwnd, false);
            if (n.find) DestroyWindow(n.find);
            for (auto& t : n.tabs)
                if (t->doc) t->doc->Release();
            for (HFONT f : { n.font, n.glyphs, n.glyphsSmall, n.gutFont })
                if (f) DeleteObject(f);
            if (n.gutter) SetWindowLongPtrW(n.gutter, GWLP_USERDATA, 0);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            s_np = nullptr;
            delete pn;
            release_instance();
            app_window_closed();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// its own taskbar button, with its own icon (as the player's)
void set_app_id(HWND hwnd, bool on) { window_app_id(hwnd, on ? kAppId : nullptr); }

bool create_window(const std::vector<std::wstring>& files) {
    static bool registered = false;
    if (!registered) {
        registered = true;
        LoadLibraryExW(L"msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = g_hinst;
        wc.lpfnWndProc = notepad_proc;
        wc.lpszClassName = notepad_class().c_str();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_DBLCLKS;
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_NOTEPAD), LIM_LARGE, &wc.hIcon);
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_NOTEPAD), LIM_SMALL, &wc.hIconSm);
        RegisterClassExW(&wc);
    }
    auto* n = new Notepad;
    s_np = n;
    HWND w = CreateWindowExW(0, notepad_class().c_str(), L"Notepad", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                             CW_USEDEFAULT, 900, 640, nullptr, nullptr, g_hinst, n);
    if (!w) {
        s_np = nullptr;
        delete n;
        return false;
    }
    SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    set_app_id(w, true);                    // (before it's shown: the button is made then)
    // shown cloaked until it is all painted (as the main window): else a white frame first
    BOOL cloak = TRUE;
    DwmSetWindowAttribute(w, DWMWA_CLOAK, &cloak, sizeof(cloak));
    NoteSession s;
    restore(*n, s);
    // where it was; else on the monitor with the mouse, centred
    RECT saved{ s.x, s.y, s.x + s.w, s.y + s.h };
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (s.w > 200 && s.h > 150 && MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
        wp.rcNormalPosition = saved;
        wp.showCmd = s.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        SetWindowPlacement(w, &wp);
    } else {
        POINT cur{};
        GetCursorPos(&cur);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &mi);
        RECT work = mi.rcWork;
        SetWindowPos(w, nullptr, work.left + 40, work.top + 40, 800, 600, SWP_NOZORDER | SWP_NOACTIVATE);   // (its DPI)
        int ww = std::min(px(*n, 960), (int)(work.right - work.left)), wh = std::min(px(*n, 680), (int)(work.bottom - work.top));
        SetWindowPos(w, nullptr, work.left + (work.right - work.left - ww) / 2, work.top + (work.bottom - work.top - wh) / 2, ww, wh,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(w, SW_SHOWNORMAL);
    }
    // the files asked for, before it is shown: it opens with them (not the tab left active, a
    // moment before them); each in after the tab left active, as when it is open
    for (auto& f : files) {
        if (!s_np) break;
        open_path(*s_np, f);
    }
    if (s_np && s_np->active >= 0 && !(GetWindowLongPtrW(s_np->tabs[s_np->active]->edit, GWL_STYLE) & WS_VISIBLE)) {
        int a = s_np->active;               // (none of them opened: the one left active)
        s_np->active = -1;
        activate(*s_np, a);
    }
    if (s_np && s_np->tabs.empty()) new_untitled(*s_np);
    if (s_np) {
        RedrawWindow(w, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
        cloak = FALSE;
        DwmSetWindowAttribute(w, DWMWA_CLOAK, &cloak, sizeof(cloak));
        perf_event("notepad.shown", -1, (long long)s_np->tabs.size());
    }
    if (s_np && !s_np->unread.empty()) {
        SetTimer(w, TIMER_UNREAD, kUnreadRetryMs, nullptr);
        PostMessageW(w, WM_NOTEPAD_UNREAD, 0, 0);
    }
    return s_np != nullptr;
}

// to another process's Notepad; false: there is none
bool hand_over(const std::vector<std::wstring>& files) {
    HWND other = FindWindowW(notepad_class().c_str(), nullptr);
    if (!other) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    if (pid == GetCurrentProcessId()) return false;
    std::wstring data = pack_paths(files);
    COPYDATASTRUCT cd{ kCopyOpen, (DWORD)(data.size() * sizeof(wchar_t)), (void*)data.data() };
    AllowSetForegroundWindow(pid);
    DWORD_PTR ok = 0;
    if (!SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &ok) || !ok) return false;
    if (IsIconic(other)) ShowWindowAsync(other, SW_RESTORE);
    SetForegroundWindow(other);
    return true;
}

} // namespace

// no window here: handed to another process's, or one of this process' own (false: neither)
static bool open_instance(const std::vector<std::wstring>& files) {
    if (hand_over(files)) return false;
    HANDLE start = CreateMutexW(nullptr, FALSE, instance_mutex(L"Start").c_str());
    if (start) WaitForSingleObject(start, 10000);           // (one started a moment ago: its window)
    bool made = false;
    if (!hand_over(files)) {
        HANDLE owner = CreateMutexW(nullptr, FALSE, instance_mutex(L"Owner").c_str());
        DWORD w = owner ? WaitForSingleObject(owner, 0) : WAIT_FAILED;
        if (w == WAIT_TIMEOUT) {
            // another process has the Notepad open, and it didn't answer
            CloseHandle(owner);
            MessageBoxW(nullptr, L"The Notepad is open in another window that isn't responding. Try again in a moment.",
                        L"Notepad", MB_OK | MB_ICONINFORMATION);
        } else {
            if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) s_ownerMx = owner;
            else if (owner) CloseHandle(owner);
            made = create_window(files);
            if (!made) release_instance();
        }
    }
    if (start) { ReleaseMutex(start); CloseHandle(start); }
    return made;
}

void notepad_open(const std::vector<std::wstring>& files) {
    if (!s_np) {
        if (!open_instance(files)) return;  // (the files opened as it was made)
    } else {
        for (auto& f : files) {
            if (!s_np) return;
            open_path(*s_np, f);
        }
    }
    if (!s_np) return;
    Notepad& n = *s_np;
    if (n.tabs.empty()) new_untitled(n);
    if (IsIconic(n.wnd)) ShowWindow(n.wnd, SW_RESTORE);
    SetForegroundWindow(n.wnd);
}

bool notepad_open_file(const std::wstring& path) {
    std::vector<std::wstring> files;
    if (!path.empty()) files.push_back(path);
    notepad_open(files);
    return s_np != nullptr;
}

HWND notepad_window() { return s_np ? s_np->wnd : nullptr; }

bool notepad_owns(HWND h) {
    if (!h || !s_np) return false;
    return h == s_np->wnd || GetAncestor(h, GA_ROOT) == s_np->wnd || GetAncestor(h, GA_ROOTOWNER) == s_np->wnd;
}

bool notepad_translate(MSG* m) {
    if (!s_np) return false;
    Notepad& n = *s_np;
    if (n.find && IsDialogMessageW(n.find, m)) return true;
    HWND root = GetAncestor(m->hwnd, GA_ROOT);
    if (root != n.wnd) return false;        // (a dialog of its own: its keys)
    if (m->message == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(m->wParam) & MK_CONTROL)) {
        command(n, GET_WHEEL_DELTA_WPARAM(m->wParam) > 0 ? CMD_ZOOMIN : CMD_ZOOMOUT);
        return true;
    }
    if (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) return key(n, *m);
    return false;
}
