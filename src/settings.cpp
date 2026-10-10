#include "settings.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>

// %APPDATA%\ExplorerPlus\settings.ini — a UTF-16 INI file, read and written here (not
// through the Win32 profile APIs: those rewrite the whole file for every key, can't
// write big sections, and write ANSI unless the file already has a BOM).
//
// Several windows (processes) share the file. Each keeps the settings in memory and
// remembers which keys it changed; saving re-reads the file and writes back only
// those, so one window doesn't undo another's changes. Writes go to a temporary file
// that then replaces the old one, under a mutex shared by all instances.

static std::wstring s_iniPath;
static bool s_iniPathKnown = false;

static std::wstring ini_path() {
    if (s_iniPathKnown) return s_iniPath;
    s_iniPathKnown = true;
    // tests point this at a scratch file so they never touch the user's settings; set
    // but unusable means no settings at all, never the real file
    DWORD need = GetEnvironmentVariableW(L"EXPLORERPLUS_SETTINGS", nullptr, 0);
    if (need) {
        std::wstring over(need, L'\0');
        DWORD n = GetEnvironmentVariableW(L"EXPLORERPLUS_SETTINGS", over.data(), need);
        over.resize(n < need ? n : 0);
        s_iniPath = over;
        return s_iniPath;
    }
    WCHAR dir[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, dir))) return s_iniPath;
    std::wstring p = dir;
    p += L"\\ExplorerPlus";
    CreateDirectoryW(p.c_str(), nullptr);
    p += L"\\settings.ini";
    // first start after the rename: carry the settings over (the old file stays as it was)
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring old = std::wstring(dir) + L"\\ExplorerClone\\settings.ini";
        CopyFileW(old.c_str(), p.c_str(), TRUE);
    }
    s_iniPath = p;
    return s_iniPath;
}

// ------------------------------------------------------------------
// the INI document: sections and lines in file order (comments kept)

static bool ieq(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

static std::wstring trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t')) --e;
    return s.substr(b, e - b);
}

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
        for (auto& s : sections) if (!s.name.empty() && ieq(s.name, name)) return &s;
        if (!create) return nullptr;
        sections.push_back({ name, {} });
        return &sections.back();
    }
    const IniSection* find(const std::wstring& name) const {
        return const_cast<IniDoc*>(this)->find(name, false);
    }
    const std::wstring* get(const std::wstring& sec, const std::wstring& key) const {
        if (const IniSection* s = find(sec))
            for (auto& l : s->lines) if (l.entry && ieq(l.key, key)) return &l.value;
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
                                      [&](const IniLine& l) { return l.entry && ieq(l.key, key); }),
                       s->lines.end());
    }
};

// UTF-16LE (with BOM, as written here), UTF-8 (a file edited in Notepad) or ANSI (very old)
static std::wstring decode(const std::string& raw) {
    auto widen = [&](UINT cp, size_t skip, DWORD flags, std::wstring& out) {
        int len = (int)(raw.size() - skip);
        if (len <= 0) { out.clear(); return true; }
        int n = MultiByteToWideChar(cp, flags, raw.data() + skip, len, nullptr, 0);
        if (n <= 0) return false;
        out.resize(n);
        MultiByteToWideChar(cp, flags, raw.data() + skip, len, out.data(), n);
        return true;
    };
    std::wstring text;
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        text.assign(reinterpret_cast<const wchar_t*>(raw.data() + 2), (raw.size() - 2) / sizeof(wchar_t));
    } else if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB &&
               (unsigned char)raw[2] == 0xBF) {
        widen(CP_UTF8, 3, 0, text);
    } else if (!widen(CP_UTF8, 0, MB_ERR_INVALID_CHARS, text)) {
        widen(CP_ACP, 0, 0, text);
    }
    return text;
}

static IniDoc parse(const std::wstring& text) {
    IniDoc d;
    d.sections.push_back({ L"", {} });
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? text.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring t = trim(line);
        if (t.size() >= 2 && t[0] == L'[' && t.find(L']') != std::wstring::npos) {
            d.sections.push_back({ trim(t.substr(1, t.find(L']') - 1)), {} });
            continue;
        }
        IniLine l;
        size_t eq = t.find(L'=');
        if (!t.empty() && t[0] != L';' && eq != std::wstring::npos && eq > 0) {
            l.entry = true;
            l.key = trim(t.substr(0, eq));
            l.value = trim(t.substr(eq + 1));
            // (the profile APIs dropped quotes around a value: files they wrote read the same)
            if (l.value.size() >= 2 && l.value.front() == L'"' && l.value.back() == L'"')
                l.value = l.value.substr(1, l.value.size() - 2);
        } else {
            if (t.empty() && pos > text.size()) break;    // (the final newline)
            l.raw = line;
        }
        d.sections.back().lines.push_back(std::move(l));
    }
    return d;
}

static std::wstring serialize(const IniDoc& d) {
    std::wstring out;
    for (auto& s : d.sections) {
        if (!s.name.empty()) out += L"[" + s.name + L"]\r\n";
        for (auto& l : s.lines) out += (l.entry ? l.key + L"=" + l.value : l.raw) + L"\r\n";
    }
    return out;
}

// the file as it is now (empty when there is none)
static IniDoc read_doc(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return parse(L"");
    LARGE_INTEGER size{};
    std::string raw;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (64LL << 20)) {
        raw.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, raw.data(), (DWORD)raw.size(), &got, nullptr)) got = 0;
        raw.resize(got);
    }
    CloseHandle(h);
    return parse(decode(raw));
}

// replace the file in one step: a crash mid-write leaves the old file, never half a one
static bool write_doc(const std::wstring& p, const IniDoc& d) {
    std::wstring tmp = p + L".tmp" + std::to_wstring(GetCurrentProcessId());
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::wstring text = serialize(d);
    WCHAR bom = 0xFEFF;
    DWORD wr = 0;
    bool ok = WriteFile(h, &bom, sizeof(bom), &wr, nullptr) &&
              WriteFile(h, text.data(), (DWORD)(text.size() * sizeof(WCHAR)), &wr, nullptr) &&
              wr == text.size() * sizeof(WCHAR);
    FlushFileBuffers(h);
    CloseHandle(h);
    for (int attempt = 0; ok && attempt < 10; ++attempt) {     // (a virus scanner may hold it a moment)
        if (MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        Sleep(20);
    }
    DeleteFileW(tmp.c_str());
    return false;
}

// read-modify-write of the file, one instance at a time
static bool update_file(const std::function<void(IniDoc&)>& change) {
    std::wstring p = ini_path();
    if (p.empty()) return false;
    HANDLE mx = CreateMutexW(nullptr, FALSE, L"Local\\ExplorerPlus.Settings");
    DWORD w = mx ? WaitForSingleObject(mx, 3000) : WAIT_FAILED;
    bool owned = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
    IniDoc d = read_doc(p);
    change(d);
    bool ok = write_doc(p, d);
    if (owned) ReleaseMutex(mx);
    if (mx) CloseHandle(mx);
    return ok;
}

// ------------------------------------------------------------------
// keys

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

// keys of file-system folders ("C:\...", "\\server\share\..."), not virtual ones
// ("::{...}", "::search-results")
static bool fs_key(const std::wstring& k) {
    return (k.size() >= 2 && iswalpha(k[0]) && k[1] == L':') || k.rfind(L"\\\\", 0) == 0;
}

// ------------------------------------------------------------------
// state

static View s_defaultView{ LV_VIEW_DETAILS, 0 };
static bool s_showHidden = false;
static bool s_detailsPane = true;
static SplitMode s_split{};
static std::map<std::wstring, View> s_byFolder;
static std::map<std::wstring, std::wstring> s_columns;     // folder -> serialized column set
static std::vector<std::wstring> s_tabLines;
static int s_activeTab = 0;
static std::map<std::wstring, int> s_prefs;

// what this process changed since loading / its last save
static std::set<std::wstring> s_viewsDirty, s_columnsDirty, s_prefsDirty;
static bool s_forgetFs = false;          // "Apply to all" / "Reset": every folder's own entry goes
struct MainValues {
    View defaultView;
    bool showHidden, detailsPane;
    SplitMode split;
};
static MainValues s_mainLoaded{};        // [main] as loaded / last saved

// per-folder entries kept (least recently written go first)
static constexpr size_t kMaxViews = 1500, kMaxColumns = 500;

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

static int get_int(const IniDoc& d, const wchar_t* sec, const wchar_t* key, int def) {
    const std::wstring* v = d.get(sec, key);
    return v && !v->empty() ? _wtoi(v->c_str()) : def;
}

static MainValues main_now() { return { s_defaultView, s_showHidden, s_detailsPane, s_split }; }

void settings_load_all() {
    std::wstring p = ini_path();
    if (p.empty()) return;
    IniDoc d = read_doc(p);
    s_defaultView = int_to_view(get_int(d, L"main", L"defaultView", 10));   // Details
    s_showHidden = get_int(d, L"main", L"showHidden", 0) != 0;
    s_detailsPane = get_int(d, L"main", L"detailsPane", 1) != 0;
    s_split.width = get_int(d, L"main", L"splitter", 280);
    s_split.winW = get_int(d, L"main", L"winW", 0);
    s_split.winH = get_int(d, L"main", L"winH", 0);
    s_split.x = get_int(d, L"main", L"winX", INT_MIN);
    s_split.y = get_int(d, L"main", L"winY", INT_MIN);
    s_split.maximized = get_int(d, L"main", L"maximized", 0) != 0;
    s_split.treeVisible = get_int(d, L"main", L"treeVisible", 1) != 0;
    s_split.widthRight = get_int(d, L"main", L"splitterRight", 260);
    s_split.detailsVisible = s_detailsPane;
    // tabs: [tabs] section with keys tab0..tabN, values "parse \x01 name \x01 viewcode \x01 pinned"
    s_activeTab = get_int(d, L"tabs", L"active", 0);
    s_tabLines.clear();
    for (int i = 0; i < 200; ++i) {
        const std::wstring* v = d.get(L"tabs", L"tab" + std::to_wstring(i));
        if (!v || v->empty()) break;
        s_tabLines.push_back(*v);
    }
    // per-folder views and columns ("escaped-key=value"), small integer preferences
    for (auto& [k, v] : d.entries(L"views")) s_byFolder[unescape_key(k)] = int_to_view(_wtoi(v.c_str()));
    for (auto& [k, v] : d.entries(L"columns")) s_columns[unescape_key(k)] = v;
    for (auto& [k, v] : d.entries(L"prefs")) s_prefs[k] = _wtoi(v.c_str());
    s_mainLoaded = main_now();
}

// an over-full per-folder section: first the folders on local disks that are gone, then
// the entries written longest ago
static void prune(IniDoc& d, const wchar_t* sec, size_t cap) {
    IniSection* s = d.find(sec);
    if (!s) return;
    auto count = [&] { return (size_t)std::count_if(s->lines.begin(), s->lines.end(), [](const IniLine& l) { return l.entry; }); };
    if (count() <= cap) return;
    auto gone = [](const IniLine& l) {
        if (!l.entry) return false;
        std::wstring k = unescape_key(l.key);
        if (k.size() < 3 || !iswalpha(k[0]) || k[1] != L':' || k[2] != L'\\') return false;
        WCHAR root[4] = { k[0], L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) return false;       // (never wait for a network drive)
        return GetFileAttributesW(k.c_str()) == INVALID_FILE_ATTRIBUTES &&
               (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND);
    };
    s->lines.erase(std::remove_if(s->lines.begin(), s->lines.end(), gone), s->lines.end());
    for (size_t n = count(); n > cap;) {
        auto it = std::find_if(s->lines.begin(), s->lines.end(), [](const IniLine& l) { return l.entry; });
        if (it == s->lines.end()) break;
        s->lines.erase(it);
        --n;
    }
}

void settings_save_all() {
    MainValues now = main_now();
    const MainValues& was = s_mainLoaded;
    update_file([&](IniDoc& d) {
        auto put = [&](const wchar_t* key, int v) { d.set(L"main", key, std::to_wstring(v)); };
        // [main]: only what changed here (another window may have saved its own since)
        if (view_to_int(now.defaultView) != view_to_int(was.defaultView)) put(L"defaultView", view_to_int(now.defaultView));
        if (now.showHidden != was.showHidden) put(L"showHidden", now.showHidden);
        if (now.detailsPane != was.detailsPane) put(L"detailsPane", now.detailsPane);
        if (now.split.width != was.split.width) put(L"splitter", now.split.width);
        if (now.split.widthRight != was.split.widthRight) put(L"splitterRight", now.split.widthRight);
        if (now.split.treeVisible != was.split.treeVisible) put(L"treeVisible", now.split.treeVisible);
        // the window's place: the last window closed is where the next one opens
        if (now.split.winW != was.split.winW || now.split.winH != was.split.winH ||
            now.split.x != was.split.x || now.split.y != was.split.y || now.split.maximized != was.split.maximized) {
            put(L"winW", now.split.winW); put(L"winH", now.split.winH);
            put(L"winX", now.split.x);    put(L"winY", now.split.y);
            put(L"maximized", now.split.maximized);
        }
        if (s_forgetFs)
            for (const wchar_t* sec : { L"views", L"columns" })
                for (auto& [k, v] : d.entries(sec))
                    if (fs_key(unescape_key(k))) d.erase(sec, k);
        for (auto& k : s_viewsDirty) {
            auto it = s_byFolder.find(k);
            if (it == s_byFolder.end()) d.erase(L"views", escape_key(k));
            else d.set(L"views", escape_key(k), std::to_wstring(view_to_int(it->second)));
        }
        for (auto& k : s_columnsDirty) {
            auto it = s_columns.find(k);
            if (it == s_columns.end()) d.erase(L"columns", escape_key(k));
            else d.set(L"columns", escape_key(k), it->second);
        }
        for (auto& k : s_prefsDirty) {
            auto it = s_prefs.find(k);
            if (it == s_prefs.end()) d.erase(L"prefs", k);
            else d.set(L"prefs", k, std::to_wstring(it->second));
        }
        prune(d, L"views", kMaxViews);
        prune(d, L"columns", kMaxColumns);
    });
    s_viewsDirty.clear();
    s_columnsDirty.clear();
    s_prefsDirty.clear();
    s_forgetFs = false;
    s_mainLoaded = now;
}

// [contextmenu] promoteN=<item text or verb>[|<icon file>[,<index>]]: classic menu
// items the compact menu lists too. Written with defaults when the section doesn't
// exist yet, so the file shows the format; an emptied section stays empty.
std::vector<std::wstring> settings_menu_promotions() {
    std::wstring p = ini_path();       // read each time: edits apply to the next menu
    std::vector<std::wstring> out;
    if (p.empty()) return out;
    IniDoc d = read_doc(p);
    if (const IniSection* s = d.find(std::wstring(L"contextmenu"))) {
        for (auto& l : s->lines)
            if (l.entry && _wcsnicmp(l.key.c_str(), L"promote", 7) == 0 && !l.value.empty()) out.push_back(l.value);
        return out;
    }
    out = { L"7-Zip", L"NotepadNext" };
    update_file([&](IniDoc& f) {
        if (f.find(std::wstring(L"contextmenu"))) return;      // another window wrote it meanwhile
        IniSection* s = f.find(L"contextmenu", true);
        IniLine c;
        c.raw = L"; items from \"Show more options\" to list in the compact menu: text or verb, optional |icon file,index";
        s->lines.push_back(c);
        for (size_t i = 0; i < out.size(); ++i) {
            IniLine l;
            l.entry = true; l.key = L"promote" + std::to_wstring(i + 1); l.value = out[i];
            s->lines.push_back(std::move(l));
        }
    });
    return out;
}

std::vector<std::wstring> settings_tree_pins() {
    std::vector<std::wstring> out;
    std::wstring p = ini_path();
    if (p.empty()) return out;
    IniDoc d = read_doc(p);
    for (int i = 0; i < 100; ++i) {
        const std::wstring* v = d.get(L"tree", L"pin" + std::to_wstring(i));
        if (!v || v->empty()) break;
        out.push_back(*v);
    }
    return out;
}

void settings_set_tree_pins(const std::vector<std::wstring>& pins) {
    KeyValues kv;
    for (size_t i = 0; i < pins.size() && i < 100; ++i) kv.push_back({ L"pin" + std::to_wstring(i), pins[i] });
    update_file([&](IniDoc& d) { d.replace(L"tree", kv); });
}

int settings_pref(const wchar_t* key, int def) {
    auto it = s_prefs.find(key);
    return it == s_prefs.end() ? def : it->second;
}
void settings_set_pref(const wchar_t* key, int v) {
    s_prefs[key] = v;
    s_prefsDirty.insert(key);
}

const std::wstring* settings_columns_for(const std::wstring& folderKey) {
    auto it = s_columns.find(folderKey);
    return it == s_columns.end() ? nullptr : &it->second;
}
void settings_set_columns_for(const std::wstring& folderKey, const std::wstring& cols) {
    if (cols.empty()) s_columns.erase(folderKey);
    else s_columns[folderKey] = cols;
    s_columnsDirty.insert(folderKey);
}

View settings_default_view() { return s_defaultView; }
void settings_set_default_view(const View& v) { s_defaultView = v; }

void settings_forget_fs_folders() {
    for (auto it = s_byFolder.begin(); it != s_byFolder.end();) it = fs_key(it->first) ? s_byFolder.erase(it) : std::next(it);
    for (auto it = s_columns.begin(); it != s_columns.end();) it = fs_key(it->first) ? s_columns.erase(it) : std::next(it);
    s_forgetFs = true;
}
const View* settings_view_for(const std::wstring& folderKey) {
    auto it = s_byFolder.find(folderKey);
    return it == s_byFolder.end() ? nullptr : &it->second;
}
void settings_set_view_for(const std::wstring& folderKey, const View& v) {
    s_byFolder[folderKey] = v;
    s_viewsDirty.insert(folderKey);
}
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
    KeyValues kv{ { L"active", std::to_wstring(active) } };
    for (size_t i = 0; i < lines.size(); ++i) kv.push_back({ L"tab" + std::to_wstring(i), lines[i] });
    update_file([&](IniDoc& d) { d.replace(L"tabs", kv); });    // written at once
}

bool settings_details_pane() { return s_detailsPane; }
void settings_set_details_pane(bool v) { s_detailsPane = v; s_split.detailsVisible = v; }
