#include "settings.h"
#include <map>

// %APPDATA%\ExplorerPlus\settings.ini — simple INI persisted with Win32 profile APIs.

static std::wstring ini_path() {
    // tests point this at a scratch file so they never touch the user's settings
    WCHAR over[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"EXPLORERPLUS_SETTINGS", over, MAX_PATH) && over[0]) return over;
    WCHAR dir[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, dir))) return {};
    std::wstring p = dir;
    p += L"\\ExplorerPlus";
    CreateDirectoryW(p.c_str(), nullptr);
    p += L"\\settings.ini";
    // first start after the rename: carry the settings over (the old file stays as it was)
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring old = std::wstring(dir) + L"\\ExplorerClone\\settings.ini";
        CopyFileW(old.c_str(), p.c_str(), TRUE);
    }
    return p;
}

// The profile APIs write UTF-16 only when the file already starts with a UTF-16LE
// BOM; otherwise they write ANSI and non-ASCII folder names ("Stažené soubory")
// degrade to '?'. Create the file with a BOM / convert a legacy ANSI file once.
static void ensure_unicode_ini(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::string raw((size_t)size.QuadPart, '\0');
    DWORD got = 0;
    if (!raw.empty()) ReadFile(h, raw.data(), (DWORD)raw.size(), &got, nullptr);
    raw.resize(got);
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        CloseHandle(h);
        return;
    }
    std::wstring text;
    if (!raw.empty()) {
        int n = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), nullptr, 0);
        text.resize(n);
        MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), text.data(), n);
    }
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    WCHAR bom = 0xFEFF;
    DWORD wr = 0;
    WriteFile(h, &bom, sizeof(bom), &wr, nullptr);
    if (!text.empty()) WriteFile(h, text.data(), (DWORD)(text.size() * sizeof(WCHAR)), &wr, nullptr);
    SetEndOfFile(h);
    CloseHandle(h);
}

// INI keys cannot hold '=', ';', '[', ']' or leading/trailing blanks; '\' is
// stored as '|' (illegal in paths, keeps legacy files readable), the rest as %XX.
static std::wstring escape_key(const std::wstring& k) {
    std::wstring out;
    for (size_t i = 0; i < k.size(); ++i) {
        WCHAR c = k[i];
        bool edgeBlank = (c == L' ' || c == L'\t') && (i == 0 || i + 1 == k.size());
        if (c == L'\\') out += L'|';
        else if (c == L'=' || c == L';' || c == L'[' || c == L']' || c == L'%' || c == L'|' ||
                 c == L'"' || edgeBlank || c < 0x20) {
            WCHAR hex[8];
            swprintf(hex, 8, L"%%%02X", (unsigned)c & 0xFF);
            out += hex;
        } else out += c;
    }
    return out;
}

static std::wstring unescape_key(const std::wstring& k) {
    std::wstring out;
    for (size_t i = 0; i < k.size(); ++i) {
        if (k[i] == L'|') out += L'\\';
        else if (k[i] == L'%' && i + 2 < k.size() && iswxdigit(k[i + 1]) && iswxdigit(k[i + 2])) {
            out += (WCHAR)wcstoul(k.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else out += k[i];
    }
    return out;
}

// whole section, growing the buffer until it fits
static std::vector<WCHAR> read_section(const wchar_t* sec, const std::wstring& p) {
    std::vector<WCHAR> buf(16384);
    for (;;) {
        DWORD n = GetPrivateProfileSectionW(sec, buf.data(), (DWORD)buf.size(), p.c_str());
        if (n < buf.size() - 2) { buf.resize(n + 2); return buf; }
        if (buf.size() >= (1u << 24)) { buf.assign(2, 0); return buf; }
        buf.resize(buf.size() * 4);
    }
}

static View s_defaultView{ LV_VIEW_DETAILS, 0 };
static bool s_showHidden = false;
static bool s_detailsPane = true;
static SplitMode s_split{};
static std::map<std::wstring, View> s_byFolder;
static std::map<std::wstring, std::wstring> s_columns;     // folder -> serialized column set
static std::vector<std::wstring> s_tabLines;
static int s_activeTab = 0;
static std::map<std::wstring, int> s_prefs;

// groupBy * 100 + lvView * 10 + thumbsExtra (codes below 100 predate grouping = AUTO)
int view_to_int(const View& v) {
    return v.groupBy * 100 + (int)v.lvView * 10 + v.thumbsExtra;
}
View int_to_view(int x) {
    View v;
    if (x < 0) return v;
    v.groupBy = x / 100;
    v.lvView = (UINT)(x % 100 / 10);
    v.thumbsExtra = x % 10;
    // LV_VIEW_ICON is 0 — a valid value (Medium/Large icons), not "unset"
    if (v.lvView > LV_VIEW_MAX || v.thumbsExtra > 3) { v.lvView = LV_VIEW_DETAILS; v.thumbsExtra = 0; }
    if (v.groupBy > GB_MAX) v.groupBy = GB_AUTO;
    return v;
}

void settings_load_all() {
    std::wstring p = ini_path();
    if (p.empty()) return;
    ensure_unicode_ini(p);
    WCHAR buf[64]{};
    GetPrivateProfileStringW(L"main", L"defaultView", L"10", buf, 64, p.c_str());   // Details
    s_defaultView = int_to_view(_wtoi(buf));
    GetPrivateProfileStringW(L"main", L"showHidden", L"0", buf, 64, p.c_str());
    s_showHidden = _wtoi(buf) != 0;
    GetPrivateProfileStringW(L"main", L"detailsPane", L"1", buf, 64, p.c_str());
    s_detailsPane = _wtoi(buf) != 0;
    GetPrivateProfileStringW(L"main", L"splitter", L"280", buf, 64, p.c_str());
    s_split.width = _wtoi(buf);
    GetPrivateProfileStringW(L"main", L"winW", L"0", buf, 64, p.c_str()); s_split.winW = _wtoi(buf);
    GetPrivateProfileStringW(L"main", L"winH", L"0", buf, 64, p.c_str()); s_split.winH = _wtoi(buf);
    GetPrivateProfileStringW(L"main", L"winX", L"-2147483648", buf, 64, p.c_str()); s_split.x = _wtoi(buf);
    GetPrivateProfileStringW(L"main", L"winY", L"-2147483648", buf, 64, p.c_str()); s_split.y = _wtoi(buf);
    GetPrivateProfileStringW(L"main", L"maximized", L"0", buf, 64, p.c_str());
    s_split.maximized = _wtoi(buf) != 0;
    GetPrivateProfileStringW(L"main", L"treeVisible", L"1", buf, 64, p.c_str());
    s_split.treeVisible = _wtoi(buf) != 0;
    GetPrivateProfileStringW(L"main", L"splitterRight", L"260", buf, 64, p.c_str());
    s_split.widthRight = _wtoi(buf);
    s_split.detailsVisible = s_detailsPane;
    // tabs: [tabs] section with keys tab0..tabN, values "parse \x01 name \x01 viewcode"
    GetPrivateProfileStringW(L"tabs", L"active", L"0", buf, 64, p.c_str());
    s_activeTab = _wtoi(buf);
    s_tabLines.clear();
    for (int i = 0; i < 20; ++i) {
        WCHAR key[16]{}; swprintf(key, 16, L"tab%d", i);
        WCHAR big[2048]{};
        if (GetPrivateProfileStringW(L"tabs", key, L"", big, 2048, p.c_str()) && big[0]) {
            s_tabLines.push_back(big);
        } else break;
    }
    // per-folder views section: "escaped-key=viewcode\0...\0\0"
    std::vector<WCHAR> names = read_section(L"views", p);
    for (WCHAR* cur = names.data(); *cur; cur += wcslen(cur) + 1) {
        WCHAR* eq = wcsrchr(cur, L'=');      // values are numbers: the last '=' splits
        if (!eq) continue;
        *eq = 0;
        s_byFolder[unescape_key(cur)] = int_to_view(_wtoi(eq + 1));
    }
    // per-folder columns: escaped keys never contain '=', so the first one splits
    std::vector<WCHAR> cols = read_section(L"columns", p);
    for (WCHAR* cur = cols.data(); *cur; cur += wcslen(cur) + 1) {
        WCHAR* eq = wcschr(cur, L'=');
        if (!eq) continue;
        *eq = 0;
        s_columns[unescape_key(cur)] = eq + 1;
    }
    std::vector<WCHAR> prefs = read_section(L"prefs", p);
    for (WCHAR* cur = prefs.data(); *cur; cur += wcslen(cur) + 1) {
        WCHAR* eq = wcschr(cur, L'=');
        if (!eq) continue;
        *eq = 0;
        s_prefs[cur] = _wtoi(eq + 1);
    }
}

void settings_save_all() {
    std::wstring p = ini_path();
    if (p.empty()) return;
    WCHAR buf[128]{};
    swprintf(buf, 128, L"%d", view_to_int(s_defaultView));
    WritePrivateProfileStringW(L"main", L"defaultView", buf, p.c_str());
    WritePrivateProfileStringW(L"main", L"showHidden", s_showHidden ? L"1" : L"0", p.c_str());
    swprintf(buf, 128, L"%d", s_split.width);
    WritePrivateProfileStringW(L"main", L"splitter", buf, p.c_str());
    swprintf(buf, 128, L"%d", s_split.winW); WritePrivateProfileStringW(L"main", L"winW", buf, p.c_str());
    swprintf(buf, 128, L"%d", s_split.winH); WritePrivateProfileStringW(L"main", L"winH", buf, p.c_str());
    swprintf(buf, 128, L"%d", s_split.x);    WritePrivateProfileStringW(L"main", L"winX", buf, p.c_str());
    swprintf(buf, 128, L"%d", s_split.y);    WritePrivateProfileStringW(L"main", L"winY", buf, p.c_str());
    WritePrivateProfileStringW(L"main", L"maximized", s_split.maximized ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"main", L"treeVisible", s_split.treeVisible ? L"1" : L"0", p.c_str());
    swprintf(buf, 128, L"%d", s_split.widthRight);
    WritePrivateProfileStringW(L"main", L"splitterRight", buf, p.c_str());
    WritePrivateProfileStringW(L"main", L"detailsPane", s_detailsPane ? L"1" : L"0", p.c_str());
    // rewrite the views section in one call ("k=v\0k=v\0\0")
    std::wstring sec;
    for (auto& [k, v] : s_byFolder) {
        swprintf(buf, 128, L"%d", view_to_int(v));
        sec += escape_key(k) + L'=' + buf;
        sec += L'\0';
    }
    sec += L'\0';
    WritePrivateProfileSectionW(L"views", sec.c_str(), p.c_str());
    sec.clear();
    for (auto& [k, v] : s_columns) {
        sec += escape_key(k) + L'=' + v;
        sec += L'\0';
    }
    sec += L'\0';
    WritePrivateProfileSectionW(L"columns", sec.c_str(), p.c_str());
    sec.clear();
    for (auto& [k, v] : s_prefs) {
        sec += k + L'=' + std::to_wstring(v);
        sec += L'\0';
    }
    sec += L'\0';
    WritePrivateProfileSectionW(L"prefs", sec.c_str(), p.c_str());
}

// [contextmenu] promoteN=<item text or verb>[|<icon file>[,<index>]]: classic menu
// items the compact menu lists too. Written with defaults on first use so the
// file shows the format.
std::vector<std::wstring> settings_menu_promotions() {
    std::wstring p = ini_path();       // read each time: edits apply to the next menu
    std::vector<WCHAR> sec = read_section(L"contextmenu", p);
    std::vector<std::wstring> out;
    bool any = false;
    for (WCHAR* cur = sec.data(); *cur; cur += wcslen(cur) + 1) {
        any = true;
        WCHAR* eq = wcschr(cur, L'=');
        if (eq && _wcsnicmp(cur, L"promote", 7) == 0 && eq[1]) out.push_back(eq + 1);
    }
    if (!any) {
        out = { L"7-Zip", L"NotepadNext" };
        std::wstring def = L"; items from \"Show more options\" to list in the compact menu: text or verb, optional |icon file,index";
        def += L'\0';
        for (size_t i = 0; i < out.size(); ++i) { def += L"promote" + std::to_wstring(i + 1) + L'=' + out[i]; def += L'\0'; }
        def += L'\0';
        WritePrivateProfileSectionW(L"contextmenu", def.c_str(), p.c_str());
    }
    return out;
}

std::vector<std::wstring> settings_tree_pins() {
    std::wstring p = ini_path();
    std::vector<std::wstring> out;
    for (int i = 0; i < 100; ++i) {
        WCHAR key[16]{}; swprintf(key, 16, L"pin%d", i);
        WCHAR buf[2048]{};
        if (!GetPrivateProfileStringW(L"tree", key, L"", buf, 2048, p.c_str()) || !buf[0]) break;
        out.push_back(buf);
    }
    return out;
}

void settings_set_tree_pins(const std::vector<std::wstring>& pins) {
    std::wstring sec;
    for (size_t i = 0; i < pins.size() && i < 100; ++i) {
        sec += L"pin" + std::to_wstring(i) + L'=' + pins[i];
        sec += L'\0';
    }
    sec += L'\0';
    WritePrivateProfileSectionW(L"tree", sec.c_str(), ini_path().c_str());
}

int settings_pref(const wchar_t* key, int def) {
    auto it = s_prefs.find(key);
    return it == s_prefs.end() ? def : it->second;
}
void settings_set_pref(const wchar_t* key, int v) { s_prefs[key] = v; }

const std::wstring* settings_columns_for(const std::wstring& folderKey) {
    auto it = s_columns.find(folderKey);
    return it == s_columns.end() ? nullptr : &it->second;
}
void settings_set_columns_for(const std::wstring& folderKey, const std::wstring& cols) {
    if (cols.empty()) s_columns.erase(folderKey);
    else s_columns[folderKey] = cols;
}

View settings_default_view() { return s_defaultView; }
void settings_set_default_view(const View& v) { s_defaultView = v; }
const View* settings_view_for(const std::wstring& folderKey) {
    auto it = s_byFolder.find(folderKey);
    return it == s_byFolder.end() ? nullptr : &it->second;
}
void settings_set_view_for(const std::wstring& folderKey, const View& v) { s_byFolder[folderKey] = v; }
bool settings_show_hidden() { return s_showHidden; }
void settings_set_show_hidden(bool b) { s_showHidden = b; }
SplitMode settings_splitter() { return s_split; }
void settings_set_splitter(const SplitMode& s) { s_split = s; }

void settings_tabs(std::wstring& out, int& active) {
    out.clear();
    for (auto& l : s_tabLines) { out += l; out += L'\n'; }
    active = s_activeTab;
}

void settings_set_tabs(const std::vector<std::wstring>& lines, int active) {
    s_tabLines = lines;
    s_activeTab = active;
    std::wstring p = ini_path();
    if (p.empty()) return;
    WritePrivateProfileSectionW(L"tabs", nullptr, p.c_str());
    WCHAR key[16]{};
    swprintf(key, 16, L"%d", active);
    WritePrivateProfileStringW(L"tabs", L"active", key, p.c_str());
    for (size_t i = 0; i < lines.size() && i < 20; ++i) {
        swprintf(key, 16, L"tab%u", (unsigned)i);
        WritePrivateProfileStringW(L"tabs", key, lines[i].c_str(), p.c_str());
    }
}

bool settings_details_pane() { return s_detailsPane; }
void settings_set_details_pane(bool v) { s_detailsPane = v; s_split.detailsVisible = v; }
