#include "assoc.h"
#include "imageview.h"
#include "notepad.h"
#include "player.h"
#include "playlist.h"
#include "theme.h"
#include "userchoice.h"
#include <dwmapi.h>
#include <sddl.h>
#include <windowsx.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {

const wchar_t kApp[] = L"Explorer Plus";                       // its name in RegisteredApplications
const wchar_t kCaps[] = L"Software\\ExplorerPlus\\Capabilities";
const wchar_t kAppKey[] = L"Software\\ExplorerPlus";
const wchar_t kRegApps[] = L"Software\\RegisteredApplications";
const wchar_t kSettingsPage[] = L"ms-settings:defaultapps?registeredAppUser=Explorer%20Plus";

enum Kind { K_AUDIO, K_VIDEO, K_PICTURE, K_PLAYLIST, K_COUNT };
const wchar_t* const kKindNames[K_COUNT] = { L"Music and sound", L"Video", L"Pictures", L"Playlists" };

// the types offered: the common ones Windows has for each kind
const wchar_t* const kAudio[] = { L".mp3", L".m4a", L".aac", L".flac", L".wav", L".wma", L".ogg", L".oga", L".opus",
                                  L".mka", L".ac3", L".amr", L".aif", L".aiff", L".m4b", L".3ga", L".mid", L".midi", L".rmi", L".kar" };
const wchar_t* const kVideo[] = { L".mp4", L".m4v", L".mkv", L".webm", L".avi", L".wmv", L".mov", L".mpg", L".mpeg",
                                  L".m2ts", L".mts", L".ts", L".3gp", L".3g2", L".asf" };
const wchar_t* const kPictures[] = { L".jpg", L".jpeg", L".jfif", L".png", L".gif", L".bmp", L".tif", L".tiff", L".webp",
                                     L".heic", L".heif", L".avif", L".jxr", L".jxl", L".ico", L".apng", L".svg", L".emf",
                                     L".wmf", L".tga", L".qoi", L".hdr", L".psd", L".psb", L".pcx" };
const wchar_t* const kPlaylists[] = { L".m3u", L".m3u8", L".pls", L".wpl", L".zpl", L".asx", L".wax", L".wvx", L".xspf" };

struct Row {
    std::wstring ext;
    Kind kind;
    bool registered = false;
};

// ------------------------------------------------------------------
// the registry (HKEY_CURRENT_USER only)
// ------------------------------------------------------------------
std::wstring exe_path() {
    WCHAR b[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, b, ARRAYSIZE(b));
    return b;
}

std::wstring open_command() { return L"\"" + exe_path() + L"\" --play \"%1\""; }

std::wstring progid(const std::wstring& ext) { return L"ExplorerPlus." + ext.substr(1); }

bool reg_set(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                           (DWORD)((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

std::wstring reg_get(const std::wstring& key, const wchar_t* name) {
    WCHAR b[1024];
    DWORD n = sizeof(b);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, b, &n) != ERROR_SUCCESS)
        return {};
    return b;
}

std::wstring assoc_text(ASSOCSTR what, const std::wstring& ext) {
    WCHAR b[1024];
    DWORD n = ARRAYSIZE(b);
    if (FAILED(AssocQueryStringW(ASSOCF_NOTRUNCATE, what, ext.c_str(), nullptr, b, &n))) return {};
    return b;
}

bool registered(const std::wstring& ext) {
    return reg_get(std::wstring(kCaps) + L"\\FileAssociations", ext.c_str()) == progid(ext) &&
           !reg_get(L"Software\\Classes\\" + progid(ext) + L"\\shell\\open\\command", nullptr).empty();
}

void register_app() {
    std::wstring exe = exe_path();
    reg_set(kCaps, L"ApplicationName", kApp);
    reg_set(kCaps, L"ApplicationDescription", L"Plays music and video and shows pictures (Explorer Plus' media player)");
    reg_set(kCaps, L"ApplicationIcon", L"\"" + exe + L"\",0");
    reg_set(kRegApps, kApp, kCaps);
    // (the name "Open with" shows, short of the exe's whole description)
    reg_set(L"Software\\Classes\\Applications\\" + std::wstring(PathFindFileNameW(exe.c_str())), L"FriendlyAppName", kApp);
}

void unregister_app() {
    std::wstring exe = exe_path();
    RegDeleteTreeW(HKEY_CURRENT_USER, kCaps);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\ExplorerPlus\\PreviousApps");   // (each given back with its type)
    RegDeleteKeyW(HKEY_CURRENT_USER, kAppKey);              // (when nothing else is in it)
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegApps, kApp);
    RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\Classes\\Applications\\" + std::wstring(PathFindFileNameW(exe.c_str()))).c_str());
}

void register_type(const std::wstring& ext, Kind kind) {
    std::wstring cls = L"Software\\Classes\\" + progid(ext);
    // the type keeps its name and icon when Explorer Plus becomes its app (read before, while
    // they are another app's)
    if (reg_get(cls, nullptr).empty()) {
        std::wstring name = assoc_text(ASSOCSTR_FRIENDLYDOCNAME, ext);
        if (name.empty()) {
            std::wstring up = ext.substr(1);
            for (auto& c : up) c = (wchar_t)towupper(c);
            name = up + (kind == K_PICTURE ? L" picture" : kind == K_PLAYLIST ? L" playlist" : L" file");
        }
        reg_set(cls, nullptr, name);
    }
    if (reg_get(cls + L"\\DefaultIcon", nullptr).empty()) {
        std::wstring icon = assoc_text(ASSOCSTR_DEFAULTICON, ext);
        if (icon.empty() || icon.find(L"explorer-plus") != std::wstring::npos)
            icon = L"\"" + exe_path() + L"\",-" + std::to_wstring(IDI_PLAYER);
        reg_set(cls + L"\\DefaultIcon", nullptr, icon);
    }
    reg_set(cls + L"\\shell\\open\\command", nullptr, open_command());
    // offered under the type's Open with, and in Default apps
    RegSetKeyValueW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + ext + L"\\OpenWithProgids").c_str(), progid(ext).c_str(),
                    REG_NONE, nullptr, 0);
    reg_set(std::wstring(kCaps) + L"\\FileAssociations", ext.c_str(), progid(ext));
}

std::wstring user_sid() {
    std::wstring out;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return out;
    BYTE buf[256];
    DWORD n = 0;
    LPWSTR s = nullptr;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n) && ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &s)) {
        out = s;
        LocalFree(s);
    }
    CloseHandle(tok);
    return out;
}

std::wstring choice_key(const std::wstring& ext, const wchar_t* name = L"UserChoice") {
    return L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + ext + L"\\" + name;
}

FILETIME key_written(HKEY k) {
    FILETIME ft{};
    RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &ft);
    return ft;
}

// The type's default app chosen here as Settings would: the UserChoice key with the hash
// Windows checks it by (userchoice.cpp). The key is made anew: Windows denies the user setting
// its values, not deleting it. True when written (`verify`: and Windows now opens the type
// with `prog`).
bool write_choice(const std::wstring& ext, const std::wstring& prog, bool verify = true) {
    std::wstring sid = user_sid(), key = choice_key(ext);
    if (sid.empty()) return false;
    RegDeleteTreeW(HKEY_CURRENT_USER, choice_key(ext, L"UserChoiceLatest").c_str());   // (newer Windows: before UserChoice)
    // the hash goes by the minute the key was last written: written again should that turn over
    for (int tries = 0; tries < 3; ++tries) {
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
            return false;
        bool ok = RegSetValueExW(k, L"ProgId", 0, REG_SZ, (const BYTE*)prog.c_str(), (DWORD)((prog.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        FILETIME t = key_written(k);
        std::wstring hash = user_choice_hash(ext, sid, prog, t);
        ok = ok && !hash.empty() &&
             RegSetValueExW(k, L"Hash", 0, REG_SZ, (const BYTE*)hash.c_str(), (DWORD)((hash.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        FILETIME after = key_written(k);
        RegCloseKey(k);
        if (!ok) break;
        auto minute = [](const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32 | f.dwLowDateTime) / 600000000ull; };
        if (minute(t) == minute(after)) return !verify || assoc_text(ASSOCSTR_PROGID, ext) == prog;
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    return false;
}

// the app each type had before Explorer Plus (given back when the type is unticked)
const wchar_t kPrevious[] = L"Software\\ExplorerPlus\\PreviousApps";

bool make_default(const std::wstring& ext) {
    std::wstring now = reg_get(choice_key(ext), L"ProgId");
    if (!now.empty() && now != progid(ext)) reg_set(kPrevious, ext.c_str(), now);
    if (write_choice(ext, progid(ext))) return true;
    if (!now.empty() && now != progid(ext)) write_choice(ext, now, false);     // (refused: as it was)
    return false;
}

void unregister_type(const std::wstring& ext) {
    if (reg_get(choice_key(ext), L"ProgId") == progid(ext)) {
        // the app it had, where that is still there; else Windows' own choice (or it asks)
        std::wstring prev = reg_get(kPrevious, ext.c_str());
        HKEY k = nullptr;
        bool there = !prev.empty() && RegOpenKeyExW(HKEY_CLASSES_ROOT, prev.c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS;
        if (k) RegCloseKey(k);
        if (!there || !write_choice(ext, prev, false)) RegDeleteTreeW(HKEY_CURRENT_USER, choice_key(ext).c_str());
    }
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kPrevious, ext.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + progid(ext)).c_str());
    RegDeleteKeyValueW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + ext + L"\\OpenWithProgids").c_str(), progid(ext).c_str());
    RegDeleteKeyValueW(HKEY_CURRENT_USER, (std::wstring(kCaps) + L"\\FileAssociations").c_str(), ext.c_str());
}

// what opens the type now ("Explorer Plus" when it's this)
std::wstring opened_by(const std::wstring& ext) {
    if (assoc_text(ASSOCSTR_PROGID, ext) == progid(ext)) return std::wstring(kApp) + L"  \x2714";
    std::wstring app = assoc_text(ASSOCSTR_FRIENDLYAPPNAME, ext);
    return app.empty() ? L"(no app)" : app;
}

std::vector<Row> media_types() {
    std::vector<Row> rows;
    auto add = [&](const wchar_t* const* list, size_t n, Kind k) {
        for (size_t i = 0; i < n; ++i) {
            std::wstring ext = list[i];
            std::wstring probe = L"x" + ext;
            bool ok = k == K_PLAYLIST || (k == K_PICTURE ? image_file(probe) : media_file_playable(probe));
            if (ok) rows.push_back({ ext, k, registered(ext) });
        }
    };
    add(kAudio, ARRAYSIZE(kAudio), K_AUDIO);
    add(kVideo, ARRAYSIZE(kVideo), K_VIDEO);
    add(kPictures, ARRAYSIZE(kPictures), K_PICTURE);
    add(kPlaylists, ARRAYSIZE(kPlaylists), K_PLAYLIST);
    return rows;
}

// ------------------------------------------------------------------
// the dialog
// ------------------------------------------------------------------
enum { ID_LIST = 100, ID_ALL, ID_NONE };

struct Dlg {
    HWND wnd = nullptr, owner = nullptr, list = nullptr, all = nullptr, none = nullptr, apply = nullptr, close = nullptr;
    int dpi = 96;
    HFONT font = nullptr, bold = nullptr;
    std::vector<Row> rows;
    bool done = false;
    int textBottom = 0;
    RECT text{};
};
Dlg* s_dlg = nullptr;

int px(const Dlg& d, int v) { return MulDiv(v, d.dpi, 96); }

const wchar_t kHeading[] = L"Open media files with Explorer Plus";
const wchar_t kExplain[] =
    L"Tick the file types Explorer Plus should open - music, video and playlists in its player, pictures in "
    L"its viewer - and press Apply: Explorer Plus becomes their default app. Unticked types go back to the "
    L"app they had.\n"
    L"To pick an app for one type in Windows' own list, double-click that type.";

void fill_list(Dlg& d) {
    ListView_DeleteAllItems(d.list);
    for (int i = 0; i < (int)d.rows.size(); ++i) {
        const Row& r = d.rows[i];
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_GROUPID | LVIF_PARAM;
        it.iItem = i;
        it.iGroupId = r.kind;
        it.lParam = i;
        it.pszText = const_cast<wchar_t*>(r.ext.c_str());
        int at = ListView_InsertItem(d.list, &it);
        std::wstring name = assoc_text(ASSOCSTR_FRIENDLYDOCNAME, r.ext), by = opened_by(r.ext);
        ListView_SetItemText(d.list, at, 1, name.data());
        ListView_SetItemText(d.list, at, 2, by.data());
        ListView_SetCheckState(d.list, at, r.registered);
    }
}

// the row's index into `rows` (the list sorts by group)
int row_of(Dlg& d, int item) {
    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = item;
    return ListView_GetItem(d.list, &it) ? (int)it.lParam : -1;
}

void refresh_apps(Dlg& d) {
    int n = ListView_GetItemCount(d.list);
    for (int i = 0; i < n; ++i) {
        int r = row_of(d, i);
        if (r < 0) continue;
        std::wstring by = opened_by(d.rows[r].ext);
        ListView_SetItemText(d.list, i, 2, by.data());
    }
}

void apply(Dlg& d) {
    bool any = false;
    int n = ListView_GetItemCount(d.list);
    for (int i = 0; i < n; ++i) {
        int r = row_of(d, i);
        if (r < 0) continue;
        Row& row = d.rows[r];
        bool on = ListView_GetCheckState(d.list, i) != 0;
        if (on && !row.registered) register_type(row.ext, row.kind);
        if (!on && row.registered) unregister_type(row.ext);
        row.registered = on;
        any |= on;
    }
    if (any) register_app();
    else unregister_app();
    // Explorer Plus their default app at once; a type Windows keeps to itself (a newer Windows
    // may guard more of them) is left to Settings
    std::wstring refused;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    for (const Row& row : d.rows)
        if (row.registered && assoc_text(ASSOCSTR_PROGID, row.ext) != progid(row.ext) && !make_default(row.ext))
            refused += (refused.empty() ? L"" : L", ") + row.ext;
    SetCursor(old);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    refresh_apps(d);
    if (!refused.empty()) {
        std::wstring msg = L"Windows didn't let Explorer Plus become the app for " + refused +
                           L".\n\nSettings opens at Explorer Plus' page: pick it there for these types.";
        MessageBoxW(d.wnd, msg.c_str(), L"Media file types", MB_OK | MB_ICONINFORMATION);
        ShellExecuteW(d.wnd, nullptr, kSettingsPage, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// one type: registered (else it isn't among the choices), then Windows' "Open with" asks
void choose_one(Dlg& d, int item) {
    int r = row_of(d, item);
    if (r < 0) return;
    Row& row = d.rows[r];
    if (!row.registered) {
        register_app();
        register_type(row.ext, row.kind);
        row.registered = true;
        ListView_SetCheckState(d.list, item, TRUE);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
    std::wstring file = L"Explorer Plus" + row.ext;
    OPENASINFO oi{};
    oi.pcszFile = file.c_str();
    oi.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_REGISTER_EXT | OAIF_FORCE_REGISTRATION;
    SHOpenWithDialog(d.wnd, &oi);
    refresh_apps(d);
}

void make_fonts(Dlg& d) {
    if (d.font) DeleteObject(d.font);
    if (d.bold) DeleteObject(d.bold);
    d.font = theme_create_font(d.dpi, FW_REGULAR);
    d.bold = theme_create_font(d.dpi, FW_SEMIBOLD, 140);
    for (HWND h : { d.list, d.all, d.none, d.apply, d.close })
        if (h) SendMessageW(h, WM_SETFONT, (WPARAM)d.font, TRUE);
}

void layout(Dlg& d) {
    RECT rc{};
    GetClientRect(d.wnd, &rc);
    int m = px(d, 24);
    HDC dc = GetDC(d.wnd);
    HGDIOBJ of = SelectObject(dc, d.bold);
    RECT h{ m, m, rc.right - m, m };
    DrawTextW(dc, kHeading, -1, &h, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, d.font);
    RECT t{ m, h.bottom + px(d, 10), rc.right - m, 0 };
    DrawTextW(dc, kExplain, -1, &t, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, of);
    ReleaseDC(d.wnd, dc);
    t.right = rc.right - m;
    d.text = t;
    d.textBottom = t.bottom;
    int bh = px(d, 32), bw = px(d, 110), by = rc.bottom - m - bh;
    int listTop = t.bottom + px(d, 16);
    MoveWindow(d.list, m, listTop, rc.right - 2 * m, std::max(px(d, 60), by - px(d, 16) - listTop), TRUE);
    MoveWindow(d.all, m, by, bw, bh, TRUE);
    MoveWindow(d.none, m + bw + px(d, 8), by, bw, bh, TRUE);
    MoveWindow(d.close, rc.right - m - bw, by, bw, bh, TRUE);
    MoveWindow(d.apply, rc.right - m - 2 * bw - px(d, 8), by, bw, bh, TRUE);
    int w = rc.right - 2 * m - GetSystemMetricsForDpi(SM_CXVSCROLL, d.dpi) - px(d, 4);
    ListView_SetColumnWidth(d.list, 0, w * 18 / 100);
    ListView_SetColumnWidth(d.list, 1, w * 42 / 100);
    ListView_SetColumnWidth(d.list, 2, w - w * 18 / 100 - w * 42 / 100);
    InvalidateRect(d.wnd, nullptr, FALSE);
}

void apply_theme(Dlg& d) {
    BOOL dark = g_dark;
    DwmSetWindowAttribute(d.wnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    theme_style_list(d.list);
    for (HWND h : { d.all, d.none, d.apply, d.close }) theme_style_control(h);
    RedrawWindow(d.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

HBRUSH s_bg = nullptr;
COLORREF s_bgColor = CLR_INVALID;
HBRUSH bg_brush() {
    COLORREF c = pal().content;
    if (!s_bg || s_bgColor != c) {
        if (s_bg) DeleteObject(s_bg);
        s_bg = CreateSolidBrush(c);
        s_bgColor = c;
    }
    return s_bg;
}

// the header reports to the list: its text in the theme's colour (the dark header theme leaves
// it black)
LRESULT CALLBACK list_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NOTIFY) {
        auto* cd = (NMCUSTOMDRAW*)lp;
        if (cd->hdr.hwndFrom == ListView_GetHeader(h) && cd->hdr.code == NM_CUSTOMDRAW) {
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) SetTextColor(cd->hdc, pal().text);
            return CDRF_DODEFAULT;
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// a group's title: drawn here, legible on both themes (the list's own is dark blue on dark)
void draw_group(Dlg& d, NMLVCUSTOMDRAW* cd) {
    const Palette& c = pal();
    HDC dc = cd->nmcd.hdc;
    // (nmcd.rc: the whole group, its rows too; rcText: the title's row)
    RECT r{ cd->nmcd.rc.left, cd->rcText.top, cd->nmcd.rc.right, cd->rcText.bottom };
    fill_rect(dc, r, c.content);
    LVGROUP g{ sizeof(g) };
    g.mask = LVGF_HEADER;
    wchar_t buf[128] = L"";
    g.pszHeader = buf;
    g.cchHeader = ARRAYSIZE(buf);
    ListView_GetGroupInfo(d.list, (int)cd->nmcd.dwItemSpec, &g);
    HGDIOBJ of = SelectObject(dc, (HFONT)SendMessageW(d.list, WM_GETFONT, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c.text);
    RECT t{ r.left + px(d, 10), r.top, r.right, r.bottom };
    DrawTextW(dc, buf, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_CALCRECT);
    t.top = r.top;
    t.bottom = r.bottom;
    DrawTextW(dc, buf, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, of);
    int y = (r.top + r.bottom) / 2;
    RECT line{ t.right + px(d, 10), y, r.right - px(d, 10), y + 1 };
    if (line.right > line.left) fill_rect(dc, line, c.divider);
}

void on_create(Dlg& d, HWND hwnd) {
    d.wnd = hwnd;
    d.dpi = (int)GetDpiForWindow(hwnd);
    d.list = CreateWindowExW(0, WC_LISTVIEWW, L"File types", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT |
                             LVS_SHOWSELALWAYS | LVS_SINGLESEL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_LIST, g_hinst, nullptr);
    ListView_SetExtendedListViewStyle(d.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowSubclass(d.list, list_proc, 1, 0);
    const wchar_t* cols[] = { L"Type", L"Description", L"Opens with now" };
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(cols[i]);
        c.cx = 100;
        ListView_InsertColumn(d.list, i, &c);
    }
    ListView_EnableGroupView(d.list, TRUE);
    for (int k = 0; k < K_COUNT; ++k) {
        LVGROUP g{ sizeof(g) };
        g.mask = LVGF_HEADER | LVGF_GROUPID;
        g.pszHeader = const_cast<wchar_t*>(kKindNames[k]);
        g.iGroupId = k;
        ListView_InsertGroup(d.list, -1, &g);
    }
    auto button = [&](const wchar_t* text, int id, bool def = false) {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | (def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                               0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)id, g_hinst, nullptr);
    };
    d.all = button(L"Select all", ID_ALL);
    d.none = button(L"Select none", ID_NONE);
    d.apply = button(L"Apply", IDOK, true);
    d.close = button(L"Close", IDCANCEL);
    make_fonts(d);
    // (the first time: every type ticked, ready to apply)
    bool any = std::any_of(d.rows.begin(), d.rows.end(), [](const Row& r) { return r.registered; });
    fill_list(d);
    if (!any) ListView_SetCheckState(d.list, -1, TRUE);
    apply_theme(d);
    layout(d);
}

LRESULT CALLBACK dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Dlg* pd = s_dlg;
    if (msg == WM_CREATE && pd) { on_create(*pd, hwnd); return 0; }
    if (!pd || pd->wnd != hwnd) return DefWindowProcW(hwnd, msg, wp, lp);
    Dlg& d = *pd;
    switch (msg) {
        case WM_SIZE: layout(d); return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = { px(d, 520), px(d, 420) };
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, bg_brush());
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            const Palette& c = pal();
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, c.text);
            int m = px(d, 24);
            RECT h{ m, m, d.text.right, d.text.top };
            HGDIOBJ of = SelectObject(dc, d.bold);
            DrawTextW(dc, kHeading, -1, &h, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, d.font);
            SetTextColor(dc, c.textSecondary);
            RECT t = d.text;
            DrawTextW(dc, kExplain, -1, &t, DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, of);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORBTN:
            SetBkColor((HDC)wp, pal().content);
            return (LRESULT)bg_brush();
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_ALL: ListView_SetCheckState(d.list, -1, TRUE); return 0;
                case ID_NONE: ListView_SetCheckState(d.list, -1, FALSE); return 0;
                case IDOK: apply(d); return 0;
                case IDCANCEL: d.done = true; return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* hdr = (NMHDR*)lp;
            if (hdr->hwndFrom == d.list && hdr->code == NM_DBLCLK) {
                auto* ia = (NMITEMACTIVATE*)lp;
                if (ia->iItem >= 0) choose_one(d, ia->iItem);
                return 0;
            }
            if (hdr->hwndFrom == d.list && hdr->code == NM_CUSTOMDRAW) {
                auto* cd = (NMLVCUSTOMDRAW*)lp;
                if (cd->dwItemType == LVCDI_GROUP) {
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) { draw_group(d, cd); return CDRF_SKIPDEFAULT; }
                    return CDRF_DODEFAULT;
                }
                return cd->nmcd.dwDrawStage == CDDS_PREPAINT ? CDRF_NOTIFYITEMDRAW : CDRF_DODEFAULT;
            }
            break;
        }
        case WM_ACTIVATE:
            // (back from Settings or Open with: what each type opens with now)
            if (LOWORD(wp) != WA_INACTIVE) { refresh_apps(d); SetFocus(d.list); }
            return 0;
        case WM_SETTINGCHANGE:
            if (!((lp && lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) || wp == SPI_SETHIGHCONTRAST)) break;
            [[fallthrough]];
        case WM_SYSCOLORCHANGE:
            theme_reload();
            apply_theme(d);
            break;
        case WM_DPICHANGED: {
            d.dpi = HIWORD(wp);
            make_fonts(d);
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout(d);
            return 0;
        }
        case WM_CLOSE: d.done = true; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void assoc_dialog(HWND owner) {
    if (s_dlg) {
        SetForegroundWindow(s_dlg->wnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        registered = true;
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = g_hinst;
        wc.lpfnWndProc = dlg_proc;
        wc.lpszClassName = L"ExplorerPlusMediaTypes";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_PLAYER), LIM_SMALL, &wc.hIconSm);
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_PLAYER), LIM_LARGE, &wc.hIcon);
        RegisterClassExW(&wc);
    }
    Dlg d;
    d.owner = owner;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    d.rows = media_types();
    SetCursor(old);
    s_dlg = &d;
    // centred on the owner, at its DPI
    RECT orc{};
    GetWindowRect(owner, &orc);
    int dpi = (int)GetDpiForWindow(owner);
    int w = MulDiv(680, dpi, 96), h = MulDiv(640, dpi, 96);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    w = std::min(w, (int)(mi.rcWork.right - mi.rcWork.left));
    h = std::min(h, (int)(mi.rcWork.bottom - mi.rcWork.top));
    int x = std::clamp((int)(orc.left + orc.right - w) / 2, (int)mi.rcWork.left, (int)mi.rcWork.right - w);
    int y = std::clamp((int)(orc.top + orc.bottom - h) / 2, (int)mi.rcWork.top, (int)mi.rcWork.bottom - h);
    HWND wnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"ExplorerPlusMediaTypes", L"Media file types",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN, x, y, w, h, owner,
                               nullptr, g_hinst, nullptr);
    if (!wnd) { s_dlg = nullptr; return; }
    EnableWindow(owner, FALSE);
    ShowWindow(wnd, SW_SHOW);
    SetForegroundWindow(wnd);
    // modal: its own loop until closed (a WM_QUIT is passed on)
    MSG m{};
    bool quit = false;
    while (!d.done) {
        if (GetMessageW(&m, nullptr, 0, 0) <= 0) { quit = true; break; }
        if (player_owns(m.hwnd)) {
            if ((m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) && player_translate(&m)) continue;
        } else if (notepad_owns(m.hwnd)) {
            if (notepad_translate(&m)) continue;
        } else if (IsDialogMessageW(wnd, &m)) {
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    DestroyWindow(wnd);
    if (d.font) DeleteObject(d.font);
    if (d.bold) DeleteObject(d.bold);
    s_dlg = nullptr;
    if (quit) PostQuitMessage((int)m.wParam);
}

void assoc_follow_exe() {
    // (tests: never the user's registrations)
    WCHAR env[8];
    if (GetEnvironmentVariableW(L"EXPLORERPLUS_SETTINGS", env, ARRAYSIZE(env))) return;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(kCaps) + L"\\FileAssociations").c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    std::vector<std::wstring> exts;
    WCHAR name[256];
    for (DWORD i = 0;; ++i) {
        DWORD n = ARRAYSIZE(name);
        if (RegEnumValueW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (name[0] == L'.') exts.push_back(name);
    }
    RegCloseKey(k);
    // an exe that is gone (moved, or a new version unzipped elsewhere): its types to this one.
    // (One that is still there keeps them: another copy, a test build, mustn't take them.)
    std::wstring cmd = open_command();
    bool changed = false;
    for (auto& ext : exts) {
        std::wstring key = L"Software\\Classes\\" + progid(ext) + L"\\shell\\open\\command";
        std::wstring now = reg_get(key, nullptr);
        if (now.size() < 2 || now[0] != L'"' || lstrcmpiW(now.c_str(), cmd.c_str()) == 0) continue;
        std::wstring exe = now.substr(1, now.find(L'"', 1) - 1);
        if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        reg_set(key, nullptr, cmd);
        changed = true;
    }
    if (changed) {
        register_app();
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
}
