#include "notepad.h"
#include "cmdline.h"
#include "ini.h"
#include "playlist.h"
#include "resource.h"
#include "settings.h"
#include "textfile.h"
#include "theme.h"
#include <commdlg.h>
#include <dwmapi.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <memory>

namespace {

const ULONG_PTR kCopyOpen = 0x45504E31;     // WM_COPYDATA 'EPN1': files to open (paths, NUL separated)
const UINT WM_NOTEPAD_OPEN = WM_APP + 70;   // lParam: std::vector<std::wstring>* (from WM_COPYDATA)
const UINT_PTR TIMER_KEEP = 1;              // the tabs kept a moment after a change
const UINT kKeepDelayMs = 1000;
const ULONGLONG kMaxBytes = 64ull << 20;    // bigger files aren't opened
const int kTipPlus = 10000;                 // the tooltip of "+" (tabs: their index + 1)
const UINT s_findMsg = RegisterWindowMessageW(FINDMSGSTRINGW);
// (declared by tom.h, defined in no import library MinGW has)
const GUID kIID_ITextDocument = { 0x8CC497C0, 0xA1DF, 0x11CE, { 0x80, 0x98, 0x00, 0xAA, 0x00, 0x47, 0xBE, 0x5D } };

enum Cmd : UINT {
    CMD_NEW = 100, CMD_OPEN, CMD_SAVE, CMD_SAVEAS, CMD_SAVEALL, CMD_CLOSETAB, CMD_CLOSEWINDOW,
    CMD_UNDO, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_DELETE,
    CMD_FIND, CMD_FINDNEXT, CMD_FINDPREV, CMD_REPLACE, CMD_GOTO, CMD_SELALL, CMD_TIMEDATE,
    CMD_ZOOMIN, CMD_ZOOMOUT, CMD_ZOOMRESET, CMD_STATUSBAR, CMD_WRAP,
    CMD_CLOSEOTHERS, CMD_CLOSERIGHT, CMD_COPYPATH, CMD_SHOWFILE,
    CMD_ENC = 200,                          // + TextEncoding
    CMD_EOL = 220,                          // + LineEnding
};

const wchar_t* const kMenuNames[] = { L"File", L"Edit", L"View" };
enum StatusPart { SP_CARET, SP_CHARS, SP_ZOOM, SP_EOL, SP_ENC, SP_COUNT };

struct Tab {
    HWND edit = nullptr;
    ITextDocument* doc = nullptr;           // its text object model: lines as the file has them
    std::wstring path;                      // empty: Untitled, never saved
    TextEncoding enc = TextEncoding::Utf8;
    LineEnding eol = LineEnding::CrLf;
    bool dirty = false;                     // changed since read or saved
    std::wstring cache;                     // the file with its unsaved text, in the notes folder
    bool cacheStale = false;                // changed since that was written
    ULONGLONG fileTime = 0;                 // its file's write time when read or saved
    int pendingTop = -1;                    // (restored: the first line to show, once it is shown)
    bool loading = false;                   // (text or look set here: no change of the user's)
};

struct Notepad {
    HWND wnd = nullptr, tip = nullptr, find = nullptr;
    int dpi = 96;
    HFONT font = nullptr, glyphs = nullptr, glyphsSmall = nullptr;
    std::vector<std::unique_ptr<Tab>> tabs;
    int active = -1;
    bool wrap = true, statusBar = true;
    int zoom = 100;
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

// ------------------------------------------------------------------
// files
// ------------------------------------------------------------------
ULONGLONG write_time(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
}

enum class Read { Ok, Failed, TooBig };

Read read_bytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Read::Failed;
    LARGE_INTEGER size{};
    Read r = Read::Failed;
    if (GetFileSizeEx(h, &size)) {
        if ((ULONGLONG)size.QuadPart > kMaxBytes) {
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

bool write_new(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD done = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &done, nullptr) && done == data.size());
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// (written beside it, then put in its place: a failed write leaves the old one; the file keeps
// its attributes and permissions. A folder that takes no new files: written over in place.)
bool write_file(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".saving~";
    if (write_new(tmp, data)) {
        bool ok = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES
                      ? ReplaceFileW(path.c_str(), tmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS,
                                     nullptr, nullptr)
                      : MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (ok) return true;
        DeleteFileW(tmp.c_str());
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD done = 0;
    bool ok = (data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &done, nullptr) && done == data.size())) &&
              SetEndOfFile(h);
    CloseHandle(h);
    return ok;
}

// ours (the session, kept text): written beside, then moved over
bool write_own(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    return write_new(tmp, data) && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
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
// the text box (RichEdit, plain text)
// ------------------------------------------------------------------
std::wstring edit_text(HWND e, bool crlf = true) {
    GETTEXTLENGTHEX gl{ (DWORD)(GTL_NUMCHARS | GTL_PRECISE | (crlf ? GTL_USECRLF : 0)), 1200 };
    LRESULT len = SendMessageW(e, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    if (len <= 0) return {};
    std::wstring s((size_t)len + 1, L'\0');
    GETTEXTEX gt{};
    gt.cb = (DWORD)(s.size() * sizeof(wchar_t));
    gt.flags = crlf ? GT_USECRLF : GT_DEFAULT;
    gt.codepage = 1200;
    LRESULT got = SendMessageW(e, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)s.data());
    s.resize(got > 0 ? (size_t)got : 0);
    return s;
}

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

// its look: the palette's colours, Consolas 11 pt at the window's DPI (the control goes by the
// system's), tabs of 4, the zoom, wrapping; not undoable
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
    wcscpy_s(cf.szFaceName, L"Consolas");
    SendMessageW(t.edit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
    SendMessageW(t.edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    if (t.doc) t.doc->SetDefaultTabStop((float)(4 * 0.5498 * 11 * scale));   // (Consolas' advance: 0.55 em)
    SendMessageW(t.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(px(n, 8), px(n, 8)));
    SendMessageW(t.edit, EM_SETZOOM, n.zoom, 100);
    SendMessageW(t.edit, EM_SETTARGETDEVICE, 0, n.wrap ? 0 : 1);
    theme_style_control(t.edit);
    if (t.doc) t.doc->Undo(tomResume, nullptr);
    t.loading = false;
}

void set_text(Notepad& n, Tab& t, const std::wstring& text) {
    t.loading = true;
    SETTEXTEX st{ ST_DEFAULT, 1200 };
    SendMessageW(t.edit, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)text.c_str());
    SendMessageW(t.edit, EM_EMPTYUNDOBUFFER, 0, 0);
    t.loading = false;
    style_edit(n, t);
}

// the caret's line and column, by the file's lines (not as wrapped)
void caret_place(Tab& t, long& line, long& col) {
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
    if (SUCCEEDED(r->GetIndex(tomParagraph, &line)) && SUCCEEDED(r->StartOf(tomParagraph, tomMove, nullptr)) &&
        SUCCEEDED(r->GetStart(&start)))
        col = cp - start + 1;
    r->Release();
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

Tab* add_tab(Notepad& n, int at) {
    auto t = std::make_unique<Tab>();
    t->edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                              WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                                  ES_NOHIDESEL | ES_NOOLEDRAGDROP,
                              0, 0, 0, 0, n.wnd, nullptr, g_hinst, nullptr);
    if (!t->edit) return nullptr;
    SendMessageW(t->edit, EM_SETTEXTMODE, TM_PLAINTEXT | TM_MULTILEVELUNDO | TM_MULTICODEPAGE, 0);
    SendMessageW(t->edit, EM_EXLIMITTEXT, 0, 0x7FFFFFFE);
    SendMessageW(t->edit, EM_SETUNDOLIMIT, 1000, 0);
    SendMessageW(t->edit, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);
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

void activate(Notepad& n, int i) {
    if (i < 0 || i >= (int)n.tabs.size()) return;
    Tab* old = cur(n);
    n.active = i;
    Tab& t = *n.tabs[i];
    layout(n);
    ShowWindow(t.edit, SW_SHOW);
    if (old && old != &t) ShowWindow(old->edit, SW_HIDE);
    if (t.pendingTop >= 0) {
        int now = (int)SendMessageW(t.edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessageW(t.edit, EM_LINESCROLL, 0, t.pendingTop - now);
        t.pendingTop = -1;
    }
    if (GetActiveWindow() == n.wnd) SetFocus(t.edit);
    update_title(n);
    InvalidateRect(n.wnd, nullptr, FALSE);
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
    t->fileTime = write_time(path);
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
    std::wstring file = t.path;
    TextEncoding enc = t.enc;
    if (as || file.empty()) {
        file = ask_save(n, t, enc);
        if (file.empty()) return false;
    }
    std::wstring text = edit_text(t.edit);
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
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = write_file(file, bytes);
    SetCursor(old);
    if (!ok) {
        MessageBoxW(n.wnd, (L"The file couldn't be saved:\n" + file).c_str(), L"Notepad", MB_OK | MB_ICONWARNING);
        return false;
    }
    t.path = file;
    t.enc = enc;
    t.fileTime = write_time(file);
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
        if (t.path.empty() || t.dirty) continue;
        ULONGLONG now = write_time(t.path);
        if (!now || now == t.fileTime) continue;
        std::string bytes;
        if (read_bytes(t.path, bytes) != Read::Ok) continue;
        TextFile f = text_decode(bytes);
        if (f.binary) continue;
        CHARRANGE cr = selection(t.edit);
        int top = (int)SendMessageW(t.edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        t.enc = f.enc;
        t.eol = f.eol;
        t.fileTime = now;
        set_text(n, t, f.text);
        select(t.edit, cr.cpMin, cr.cpMax);
        SendMessageW(t.edit, EM_LINESCROLL, 0, top - (int)SendMessageW(t.edit, EM_GETFIRSTVISIBLELINE, 0, 0));
    }
    invalidate_status(n);
}

// ------------------------------------------------------------------
// keeping the tabs (the session, and each unsaved text) in the notes folder
// ------------------------------------------------------------------
void keep_soon(Notepad& n) { if (n.wnd) SetTimer(n.wnd, TIMER_KEEP, kKeepDelayMs, nullptr); }

std::wstring new_cache_name(const std::wstring& dir) {
    static unsigned counter = 0;
    for (;;) {
        wchar_t b[64];
        swprintf_s(b, L"note-%llx-%u.txt", GetTickCount64(), ++counter);
        if (GetFileAttributesW((dir + L"\\" + b).c_str()) == INVALID_FILE_ATTRIBUTES) return b;
    }
}

bool keep_now(Notepad& n) {
    if (n.wnd) KillTimer(n.wnd, TIMER_KEEP);
    std::wstring dir = notes_dir();
    if (dir.empty()) return false;
    NoteSession s;
    s.active = std::max(0, n.active);
    s.wrap = n.wrap;
    s.statusBar = n.statusBar;
    s.zoom = n.zoom;
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
        NoteTabState st;
        st.path = t.path;
        st.untitled = t.path.empty() ? 1 : 0;
        st.dirty = t.dirty;
        st.enc = t.enc;
        st.eol = t.eol;
        st.fileTime = t.fileTime;
        CHARRANGE cr = selection(t.edit);
        st.selStart = cr.cpMin;
        st.selEnd = cr.cpMax;
        st.top = t.pendingTop >= 0 ? t.pendingTop : (int)SendMessageW(t.edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        if (t.dirty) {
            if (t.cache.empty()) {
                t.cache = new_cache_name(dir);
                t.cacheStale = true;
            }
            if (t.cacheStale) {
                if (write_own(dir + L"\\" + t.cache, text_encode(edit_text(t.edit), TextEncoding::Utf16Le, LineEnding::CrLf))) t.cacheStale = false;
                else ok = false;
            }
            st.cache = t.cache;
        } else if (!t.cache.empty()) {
            DeleteFileW((dir + L"\\" + t.cache).c_str());
            t.cache.clear();
        }
        s.tabs.push_back(std::move(st));
    }
    return write_own(dir + L"\\session.ini", text_encode(note_session_serialize(s), TextEncoding::Utf16Le, LineEnding::CrLf)) && ok;
}

// the tabs as they were left; the window's place and size into `s`
void restore(Notepad& n, NoteSession& s) {
    std::wstring dir = notes_dir();
    if (dir.empty()) return;
    std::string raw;
    if (read_bytes(dir + L"\\session.ini", raw) == Read::Ok) s = note_session_parse(ini_decode(raw));
    n.wrap = s.wrap;
    n.statusBar = s.statusBar;
    n.zoom = s.zoom;
    int active = 0;
    std::vector<std::wstring> caches;
    for (int i = 0; i < (int)s.tabs.size(); ++i) {
        const NoteTabState& st = s.tabs[i];
        // its unsaved text; else its file as it is now; else (an empty Untitled) nothing
        TextFile f;
        bool ok = false, kept = false;
        std::string bytes;
        if (st.dirty && !st.cache.empty() && read_bytes(dir + L"\\" + st.cache, bytes) == Read::Ok) {
            f = text_decode(bytes);
            f.enc = st.enc;
            f.eol = st.eol;
            ok = kept = true;
        } else if (!st.path.empty() && read_bytes(st.path, bytes) == Read::Ok) {
            f = text_decode(bytes);
            ok = !f.binary;
        } else if (st.path.empty() && !st.dirty) {
            ok = true;
        }
        if (!ok) continue;
        Tab* t = add_tab(n, (int)n.tabs.size());
        if (!t) break;
        t->path = st.path;
        t->enc = f.enc;
        t->eol = f.eol;
        set_text(n, *t, f.text);
        if (kept) {
            t->dirty = true;
            t->cache = st.cache;
            t->fileTime = st.fileTime;
            caches.push_back(st.cache);
        } else {
            t->fileTime = st.path.empty() ? 0 : write_time(st.path);
        }
        select(t->edit, st.selStart, st.selEnd);
        t->pendingTop = st.top;
        if (i <= s.active) active = (int)n.tabs.size() - 1;
    }
    // text kept for tabs no longer there
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\note-*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (std::none_of(caches.begin(), caches.end(), [&](const std::wstring& c) { return lstrcmpiW(c.c_str(), fd.cFileName) == 0; }))
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
    if (selection_is_found(n, *t)) SendMessageW(t->edit, EM_REPLACESEL, TRUE, (LPARAM)n.replaceText);
    find_next(n, true);
}

// every one, found by the control's own search, put in at once: one step to undo
void replace_all(Notepad& n) {
    Tab* t = cur(n);
    if (!t || !n.findText[0]) return;
    std::wstring text = edit_text(t->edit, false);          // (positions as the control counts them)
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
    select(t->edit, 0, -1);
    SendMessageW(t->edit, EM_REPLACESEL, TRUE, (LPARAM)out.c_str());
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
    caret_place(*t, d.line, col);
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
    SendMessageW(t.edit, EM_REPLACESEL, TRUE, (LPARAM)s.c_str());
}

void set_zoom(Notepad& n, int z) {
    n.zoom = std::clamp(z, 10, 500);
    for (auto& t : n.tabs) SendMessageW(t->edit, EM_SETZOOM, n.zoom, 100);
    invalidate_status(n);
    keep_soon(n);
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
    } else {
        HMENU zoom = CreatePopupMenu();
        AppendMenuW(zoom, MF_STRING, CMD_ZOOMIN, L"Zoom in\tCtrl+Plus");
        AppendMenuW(zoom, MF_STRING, CMD_ZOOMOUT, L"Zoom out\tCtrl+Minus");
        AppendMenuW(zoom, MF_STRING, CMD_ZOOMRESET, L"Restore default zoom\tCtrl+0");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)zoom, L"Zoom");
        add(CMD_STATUSBAR, L"Status bar", true, n.statusBar);
        add(CMD_WRAP, L"Word wrap", true, n.wrap);
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
    switch (id) {
        case CMD_NEW: new_untitled(n); break;
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
        case CMD_ZOOMIN: set_zoom(n, n.zoom < 100 ? n.zoom + 10 : n.zoom + 25); break;
        case CMD_ZOOMOUT: set_zoom(n, n.zoom <= 100 ? n.zoom - 10 : n.zoom - 25); break;
        case CMD_ZOOMRESET: set_zoom(n, 100); break;
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
        if (m.message != WM_SYSKEYDOWN || shift) return false;
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
    if (Tab* t = cur(n))
        MoveWindow(t->edit, n.editArea.left, n.editArea.top, n.editArea.right - n.editArea.left, n.editArea.bottom - n.editArea.top, TRUE);
    // the tabs share the row (each between a minimum and a maximum width); more than fit scroll,
    // the active one in view
    int count = (int)n.tabs.size(), plusW = px(n, 36);
    int left = px(n, 8), avail = std::max(px(n, 80), (int)rc.right - left - plusW - px(n, 16));
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
    caret_place(*t, line, col);
    CHARRANGE cr = selection(t->edit);
    long chars = text_length(t->edit);
    std::wstring text[SP_COUNT] = {
        L"Ln " + grouped(line) + L", Col " + grouped(col),
        cr.cpMax > cr.cpMin ? grouped(cr.cpMax - cr.cpMin) + L" of " + grouped(chars) + L" characters"
                            : grouped(chars) + (chars == 1 ? L" character" : L" characters"),
        std::to_wstring(n.zoom) + L"%",
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

void paint(Notepad& n) {
    BufferedPaint bp(n.wnd);
    HDC dc = bp.dc;
    const Palette& c = pal();
    RECT rc = bp.rc;
    fill_rect(dc, RECT{ 0, 0, rc.right, n.rowH }, c.caption);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, n.font);
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

void on_down(Notepad& n, POINT pt, bool dbl) {
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
    if (PtInRect(&n.plus, pt) || (dbl && pt.y < n.rowH)) { command(n, CMD_NEW); return; }
    for (int i = 0; i < 3; ++i)
        if (PtInRect(&n.menus[i], pt)) { show_menu(n, i); return; }
    if (Tab* t = cur(n)) {
        HMENU m = nullptr;
        if (PtInRect(&n.parts[SP_EOL], pt) || PtInRect(&n.parts[SP_ENC], pt)) {
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

LRESULT CALLBACK notepad_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* np = static_cast<Notepad*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        np->wnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)np);
    }
    auto* pn = reinterpret_cast<Notepad*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!pn) return DefWindowProcW(hwnd, msg, wp, lp);
    Notepad& n = *pn;
    if (msg == s_findMsg && s_findMsg) {
        auto* fr = (FINDREPLACEW*)lp;
        if (fr->Flags & FR_DIALOGTERM) { n.find = nullptr; return 0; }
        n.findFlags = (fr->Flags & (FR_MATCHCASE | FR_WHOLEWORD)) | (n.replacing || (fr->Flags & FR_DOWN) ? FR_DOWN : 0);
        if (fr->Flags & FR_FINDNEXT) find_next(n, (n.findFlags & FR_DOWN) != 0);
        else if (fr->Flags & FR_REPLACE) replace_one(n);
        else if (fr->Flags & FR_REPLACEALL) replace_all(n);
        return 0;
    }
    switch (msg) {
        case WM_CREATE:
            n.dpi = (int)GetDpiForWindow(hwnd);
            make_fonts(n);
            n.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                                    hwnd, nullptr, g_hinst, nullptr);
            SendMessageW(n.tip, TTM_SETMAXTIPWIDTH, 0, MulDiv(600, n.dpi, 96));
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
            on_down(n, POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }, msg == WM_LBUTTONDBLCLK);
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
            if (hdr->code == EN_SELCHANGE) { invalidate_status(n); return 0; }
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
                    }
                invalidate_status(n);
            }
            return 0;
        case WM_TIMER:
            if (wp == TIMER_KEEP) keep_now(n);
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
            if (n.find) DestroyWindow(n.find);
            for (auto& t : n.tabs)
                if (t->doc) t->doc->Release();
            for (HFONT f : { n.font, n.glyphs, n.glyphsSmall })
                if (f) DeleteObject(f);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            s_np = nullptr;
            delete pn;
            app_window_closed();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool create_window() {
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
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &wc.hIcon);
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &wc.hIconSm);
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
    if (s_np && s_np->active >= 0) {
        int a = s_np->active;
        s_np->active = -1;
        activate(*s_np, a);
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

void notepad_open(const std::vector<std::wstring>& files) {
    if (!s_np) {
        if (hand_over(files)) return;
        if (!create_window()) return;
    }
    Notepad& n = *s_np;
    for (auto& f : files) {
        if (!s_np) return;
        open_path(n, f);
    }
    if (!s_np) return;
    if (n.tabs.empty()) new_untitled(n);
    if (IsIconic(n.wnd)) ShowWindow(n.wnd, SW_RESTORE);
    SetForegroundWindow(n.wnd);
}

bool notepad_open_file(const std::wstring& path) {
    std::vector<std::wstring> files;
    if (!path.empty()) files.push_back(path);
    if (!s_np && hand_over(files)) return false;
    notepad_open(files);
    return s_np != nullptr;
}

bool notepad_text_file(const std::wstring& path) {
    const wchar_t* name = PathFindFileNameW(path.c_str());
    const wchar_t* ext = PathFindExtensionW(name);
    if (!*ext) return true;                 // (README, LICENSE, Makefile)
    static const wchar_t* const kText[] = {
        L".txt", L".log", L".ini", L".inf", L".cfg", L".conf", L".config", L".json", L".jsonc", L".xml", L".xaml",
        L".md", L".markdown", L".csv", L".tsv", L".yml", L".yaml", L".toml", L".reg", L".bat", L".cmd", L".ps1",
        L".psm1", L".psd1", L".vbs", L".js", L".mjs", L".cjs", L".ts", L".tsx", L".jsx", L".css", L".scss", L".less",
        L".html", L".htm", L".xhtml", L".c", L".h", L".cpp", L".hpp", L".cc", L".cxx", L".hh", L".cs", L".java",
        L".kt", L".py", L".rb", L".go", L".rs", L".php", L".pl", L".lua", L".sql", L".sh", L".bash", L".zsh",
        L".properties", L".gradle", L".cmake", L".mk", L".diff", L".patch", L".srt", L".vtt", L".ass", L".ssa",
        L".m3u", L".m3u8", L".pls", L".nfo", L".tex", L".rst", L".asm", L".gitignore", L".gitattributes",
        L".editorconfig", L".env", L".npmrc", L".manifest", L".rc", L".in", L".sln", L".vcxproj", L".csproj", L".props",
    };
    for (auto e : kText)
        if (lstrcmpiW(ext, e) == 0) return true;
    PERCEIVED type = PERCEIVED_TYPE_UNSPECIFIED;
    PERCEIVEDFLAG flag = 0;
    return SUCCEEDED(AssocGetPerceivedType(ext, &type, &flag, nullptr)) && type == PERCEIVED_TYPE_TEXT;
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
