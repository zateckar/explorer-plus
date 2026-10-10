#include "groups.h"
#include "theme.h"
#include <algorithm>

// ------------------------------------------------------------------
// IOwnerDataCallback: undocumented, but stable since Windows 7 — the only way to show
// groups in an owner-data (LVS_OWNERDATA) list view

static const IID kIID_IOwnerDataCallback =
    { 0x44C09D56, 0x8D3B, 0x419D, { 0xA4, 0x62, 0x7B, 0x95, 0x6B, 0x10, 0x5B, 0x47 } };
#ifndef LVM_SETOWNERDATACALLBACK
#define LVM_SETOWNERDATACALLBACK (LVM_FIRST + 187)
#endif

struct IOwnerDataCallback : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetItemPosition(int itemIndex, POINT* pos) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetItemPosition(int itemIndex, POINT pos) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetItemInGroup(int groupIndex, int groupWideIndex, int* totalIndex) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetItemGroup(int itemIndex, int occurrence, int* groupIndex) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetItemGroupCount(int itemIndex, int* occurrences) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnCacheHint(LVITEMINDEX first, LVITEMINDEX last) = 0;
};

struct GroupRange {
    std::wstring name;
    int first = 0, count = 0;
};

static HWND s_list = nullptr;
static std::vector<GroupRange> s_groups;       // shown groups, in list order (index = group id)
static std::vector<std::wstring> s_rankNames;  // name of each group rank of the last assign

enum Kind { KIND_NORMAL, KIND_HOME, KIND_THISPC };
static Kind s_kind = KIND_NORMAL;
// internal modes beyond GROUP_*
enum { MODE_HOME = 100, MODE_THISPC };

static int group_of_index(int item) {
    // ranges are sorted and contiguous: binary search
    int lo = 0, hi = (int)s_groups.size() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (item < s_groups[mid].first) hi = mid - 1;
        else if (item >= s_groups[mid].first + s_groups[mid].count) lo = mid + 1;
        else return mid;
    }
    return 0;
}

class OwnerData final : public IOwnerDataCallback {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == kIID_IOwnerDataCallback) { *ppv = this; return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }     // static lifetime
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetItemPosition(int, POINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetItemPosition(int, POINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetItemInGroup(int g, int n, int* total) override {
        if (!total || g < 0 || g >= (int)s_groups.size()) return E_INVALIDARG;
        *total = s_groups[g].first + n;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetItemGroup(int item, int, int* g) override {
        if (!g) return E_POINTER;
        *g = group_of_index(item);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetItemGroupCount(int, int* n) override {
        if (!n) return E_POINTER;
        *n = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnCacheHint(LVITEMINDEX, LVITEMINDEX) override { return S_OK; }
};
static OwnerData s_callback;

void groups_install(HWND list) {
    s_list = list;
    SendMessageW(list, LVM_SETOWNERDATACALLBACK, (WPARAM)static_cast<IOwnerDataCallback*>(&s_callback), 0);
    groups_theme_changed();
}

// (function statics: initialized once, thread-safely, on first use from any thread)
static PCIDLIST_ABSOLUTE home_pidl() {
    static PIDLIST_ABSOLUTE p = [] {
        PIDLIST_ABSOLUTE r = nullptr;
        SHParseDisplayName(L"shell:::{f874310e-b6b7-47dc-bc84-b9e6b38f5903}", nullptr, &r, 0, nullptr);
        return r;
    }();
    return p;
}

static PCIDLIST_ABSOLUTE this_pc_pidl() {
    static PIDLIST_ABSOLUTE p = [] {
        PIDLIST_ABSOLUTE r = nullptr;
        SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &r);
        return r;
    }();
    return p;
}

bool is_home_folder(PCIDLIST_ABSOLUTE abs)    { return abs && home_pidl() && pidl_equal(abs, home_pidl()); }
bool is_this_pc_folder(PCIDLIST_ABSOLUTE abs) { return abs && this_pc_pidl() && pidl_equal(abs, this_pc_pidl()); }

static int mode() {
    if (g_view.groupBy != GB_AUTO) return g_view.groupBy;
    if (s_kind == KIND_HOME) return MODE_HOME;
    if (s_kind == KIND_THISPC) return MODE_THISPC;
    return GB_NONE;
}

int groups_effective() { int m = mode(); return m >= MODE_HOME ? GB_AUTO : m; }
bool groups_keep_enum_order() { return mode() == MODE_HOME; }

// ------------------------------------------------------------------
// group keys

static const wchar_t* kDateNames[] = {
    L"Today", L"Yesterday", L"Earlier this week", L"Last week", L"Earlier this month",
    L"Last month", L"Earlier this year", L"A long time ago",
};

static int date_rank(const FILETIME& ft, const SYSTEMTIME& now, LONGLONG todayDay, int weekStartOffset) {
    if (!ft.dwLowDateTime && !ft.dwHighDateTime) return 7;
    FILETIME lft; SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &lft) || !FileTimeToSystemTime(&lft, &st)) return 7;
    LONGLONG day = (LONGLONG)((ULARGE_INTEGER{ { lft.dwLowDateTime, lft.dwHighDateTime } }.QuadPart) / 864000000000ULL);
    if (day >= todayDay) return 0;                  // today (and the future)
    if (day == todayDay - 1) return 1;
    LONGLONG weekStart = todayDay - weekStartOffset;
    if (day >= weekStart) return 2;
    if (day >= weekStart - 7) return 3;
    if (st.wYear == now.wYear && st.wMonth == now.wMonth) return 4;
    int prevMonth = now.wMonth == 1 ? 12 : now.wMonth - 1;
    int prevYear = now.wMonth == 1 ? now.wYear - 1 : now.wYear;
    if (st.wYear == prevYear && st.wMonth == prevMonth) return 5;
    if (st.wYear == now.wYear) return 6;
    return 7;
}

static const wchar_t* kSizeNames[] = {
    L"Empty (0 KB)", L"Tiny (0 \x2013 16 KB)", L"Small (16 KB \x2013 1 MB)", L"Medium (1 \x2013 128 MB)",
    L"Large (128 MB \x2013 1 GB)", L"Huge (1 \x2013 4 GB)", L"Gigantic (> 4 GB)", L"Unspecified",
};

static int size_rank(const Item* it) {
    if (it->isFolder) return 7;
    ULONGLONG b = it->bytes;
    if (b == 0) return 0;
    if (b < 16ULL << 10) return 1;
    if (b < 1ULL << 20) return 2;
    if (b < 128ULL << 20) return 3;
    if (b < 1ULL << 30) return 4;
    if (b < 4ULL << 30) return 5;
    return 6;
}

static const wchar_t* kNameNames[] = { L"0 \x2013 9", L"A \x2013 H", L"I \x2013 P", L"Q \x2013 Z", L"Other" };

static int name_rank(const Item* it) {
    if (it->name.empty()) return 4;
    wchar_t c = it->name[0];
    if (c >= L'0' && c <= L'9') return 0;
    // fold accents ("Š" -> "S") before bucketing
    wchar_t base[4]{};
    if (FoldStringW(MAP_COMPOSITE, &c, 1, base, 4) > 0) c = base[0];
    c = (wchar_t)(UINT_PTR)CharUpperW((LPWSTR)(UINT_PTR)c);
    if (c >= L'A' && c <= L'H') return 1;
    if (c >= L'I' && c <= L'P') return 2;
    if (c >= L'Q' && c <= L'Z') return 3;
    return 4;
}

void groups_assign(std::vector<Item*>& items) {
    s_kind = is_home_folder(g_current) ? KIND_HOME : is_this_pc_folder(g_current) ? KIND_THISPC : KIND_NORMAL;
    s_rankNames.clear();
    int m = mode();
    switch (m) {
        case GB_NAME:
            s_rankNames.assign(std::begin(kNameNames), std::end(kNameNames));
            for (Item* it : items) it->group = name_rank(it);
            break;
        case GB_SIZE:
            s_rankNames.assign(std::begin(kSizeNames), std::end(kSizeNames));
            for (Item* it : items) it->group = size_rank(it);
            break;
        case GB_DATE: {
            s_rankNames.assign(std::begin(kDateNames), std::end(kDateNames));
            SYSTEMTIME now; GetLocalTime(&now);
            FILETIME nowFt; SystemTimeToFileTime(&now, &nowFt);
            LONGLONG today = (LONGLONG)(ULARGE_INTEGER{ { nowFt.dwLowDateTime, nowFt.dwHighDateTime } }.QuadPart / 864000000000ULL);
            // days since the locale's first day of the week (LOCALE_IFIRSTDAYOFWEEK: 0 = Monday)
            WCHAR fd[4]{};
            GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, fd, 4);
            int first = (_wtoi(fd) + 1) % 7;                 // as SYSTEMTIME.wDayOfWeek (0 = Sunday)
            int offset = (now.wDayOfWeek - first + 7) % 7;
            for (Item* it : items) it->group = date_rank(it->modified, now, today, offset);
            break;
        }
        case GB_TYPE: {
            std::vector<std::wstring> types;
            for (Item* it : items) types.push_back(it->type.empty() ? L"Unspecified" : it->type);
            std::sort(types.begin(), types.end(), [](const std::wstring& a, const std::wstring& b) {
                return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.c_str(), -1,
                                       b.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
            });
            types.erase(std::unique(types.begin(), types.end()), types.end());
            s_rankNames = types;
            for (Item* it : items) {
                const std::wstring& t = it->type.empty() ? L"Unspecified" : it->type;
                it->group = (int)(std::lower_bound(types.begin(), types.end(), t,
                    [](const std::wstring& a, const std::wstring& b) {
                        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.c_str(), -1,
                                               b.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
                    }) - types.begin());
            }
            break;
        }
        case MODE_HOME:
            s_rankNames = { L"Quick access", L"Favorites", L"Recent" };
            for (Item* it : items) it->group = it->isFolder ? 0 : it->homePinned ? 1 : 2;
            break;
        case MODE_THISPC:
            s_rankNames = { L"Folders", L"Devices and drives", L"Network locations" };
            for (Item* it : items)
                it->group = it->driveType == DRIVE_REMOTE || (!it->driveType && (it->attrs & SFGAO_SHARE)) ? 2
                          : it->driveType ? 1 : 0;
            break;
        default:
            for (Item* it : items) it->group = 0;
            break;
    }
}

void groups_record(const std::vector<Item*>& items) {
    s_groups.clear();
    if (s_rankNames.empty()) return;
    int last = -1;
    for (int i = 0; i < (int)items.size(); ++i) {
        int r = items[i]->group;
        if (r != last) {
            GroupRange g;
            g.name = s_rankNames[r];
            g.first = i;
            s_groups.push_back(g);
            last = r;
        }
        ++s_groups.back().count;
    }
}

const wchar_t* groups_name(int id) {
    return id >= 0 && id < (int)s_groups.size() ? s_groups[id].name.c_str() : L"";
}

void groups_clear() {
    s_groups.clear();
    groups_apply();
}

void groups_apply() {
    if (!s_list) return;
    // a refresh rebuilds the groups: keep the ones the user collapsed collapsed
    static std::vector<std::wstring> s_shown, s_collapsed;    // header names on screen / collapsed
    static ULONGLONG s_shownFolder = 0;
    ULONGLONG folder = pidl_sig(g_current);
    s_collapsed.clear();
    if (folder == s_shownFolder) {
        int n = (int)ListView_GetGroupCount(s_list);
        for (int i = 0; i < n && i < (int)s_shown.size(); ++i)
            if (ListView_GetGroupState(s_list, i, LVGS_COLLAPSED) & LVGS_COLLAPSED) s_collapsed.push_back(s_shown[i]);
    }
    s_shownFolder = folder;
    s_shown.clear();
    for (auto& g : s_groups) s_shown.push_back(g.name);
    ListView_RemoveAllGroups(s_list);
    bool on = !s_groups.empty();
    ListView_EnableGroupView(s_list, on);
    if (!on) return;
    for (int i = 0; i < (int)s_groups.size(); ++i) {
        LVGROUP g{};
        g.cbSize = sizeof(g);
        g.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE | LVGF_ITEMS;
        g.pszHeader = const_cast<LPWSTR>(s_groups[i].name.c_str());
        g.iGroupId = i;
        g.stateMask = LVGS_COLLAPSIBLE | LVGS_COLLAPSED;
        g.state = LVGS_COLLAPSIBLE;
        if (std::find(s_collapsed.begin(), s_collapsed.end(), s_groups[i].name) != s_collapsed.end())
            g.state |= LVGS_COLLAPSED;
        g.cItems = s_groups[i].count;
        g.iFirstItem = s_groups[i].first;
        ListView_InsertGroup(s_list, -1, &g);
    }
}

void groups_theme_changed() {
    if (!s_list) return;
    LVGROUPMETRICS m{};
    m.cbSize = sizeof(m);
    m.mask = LVGMF_TEXTCOLOR;
    m.crHeader = pal().text;
    m.crFooter = pal().textSecondary;
    ListView_SetGroupMetrics(s_list, &m);
}
