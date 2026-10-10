#include "columns.h"
#include "settings.h"
#include "enumerate.h"
#include "navigation.h"
#include "theme.h"
#include "searchbox.h"
#include <propsys.h>
#include <propvarutil.h>
#include <algorithm>

ColumnSet g_columns;

// ------------------------------------------------------------------
// column definitions (any thread)

static bool same_key(const PROPERTYKEY& a, const PROPERTYKEY& b) {
    return a.pid == b.pid && IsEqualGUID(a.fmtid, b.fmtid);
}

static int kind_of(const PROPERTYKEY& k) {
    if (same_key(k, PKEY_ItemNameDisplay)) return COL_NAME;
    if (same_key(k, PKEY_Size))            return COL_SIZE;
    if (same_key(k, PKEY_ItemTypeText))    return COL_TYPE;
    if (same_key(k, PKEY_DateModified))    return COL_DATE;
    return COL_EXTRA;
}

static std::wstring title_for(const PROPERTYKEY& k) {
    std::wstring out;
    com_ptr<IPropertyDescription> pd;
    if (SUCCEEDED(PSGetPropertyDescription(k, IID_PPV_ARGS(pd.put()))) && pd) {
        PWSTR s = nullptr;
        if (SUCCEEDED(pd->GetDisplayName(&s)) && s) { out = s; CoTaskMemFree(s); }
        if (out.empty() && SUCCEEDED(pd->GetCanonicalName(&s)) && s) { out = s; CoTaskMemFree(s); }
    }
    return out.empty() ? L"?" : out;
}

static Column make_column(const PROPERTYKEY& k, std::wstring title, int fmt, int cxChar) {
    static const int kBuiltinW[] = { 280, 90, 150, 160 };     // name, size, type, date
    Column c;
    c.key = k;
    c.kind = kind_of(k);
    c.title = title.empty() ? title_for(k) : std::move(title);
    c.fmt = fmt & LVCFMT_JUSTIFYMASK;
    if (c.kind == COL_SIZE) c.fmt = LVCFMT_RIGHT;
    if (c.kind == COL_NAME) c.fmt = LVCFMT_LEFT;     // the label column is always left-aligned
    c.width96 = c.kind != COL_EXTRA ? kBuiltinW[c.kind] : std::clamp(cxChar * 7 + 16, 80, 360);
    return c;
}

// Name first (it is the item label), extra columns numbered in order
static void normalize(ColumnSet& cols) {
    auto name = std::find_if(cols.begin(), cols.end(), [](const Column& c) { return c.kind == COL_NAME; });
    if (name == cols.end()) cols.insert(cols.begin(), make_column(PKEY_ItemNameDisplay, L"Name", LVCFMT_LEFT, 0));
    else if (name != cols.begin()) std::rotate(cols.begin(), name, name + 1);
    int n = 0;
    for (auto& c : cols) c.extra = c.kind == COL_EXTRA ? n++ : -1;
}

static std::wstring strret_text(STRRET& s) {
    std::wstring out;
    PWSTR p = nullptr;
    if (SUCCEEDED(StrRetToStrW(&s, nullptr, &p)) && p) { out = p; CoTaskMemFree(p); }
    return out;
}

static ColumnSet fs_defaults() {
    ColumnSet cols{
        make_column(PKEY_ItemNameDisplay, L"Name", LVCFMT_LEFT, 0),
        make_column(PKEY_DateModified, L"Date modified", LVCFMT_LEFT, 0),
        make_column(PKEY_ItemTypeText, L"Type", LVCFMT_LEFT, 0),
        make_column(PKEY_Size, L"Size", LVCFMT_RIGHT, 0),
    };
    normalize(cols);
    return cols;
}

ColumnSet columns_defaults(IShellFolder* folder, PCIDLIST_ABSOLUTE abs) {
    // file-system folders: Explorer's generic template (Name, Date modified, Type, Size),
    // not the shell folder's raw column order
    WCHAR path[MAX_PATH];
    if (!abs || ILIsEmpty(abs) || SHGetPathFromIDListW(abs, path)) return fs_defaults();
    ColumnSet cols;
    com_ptr<IShellFolder2> sf2;
    if (folder) folder->QueryInterface(IID_PPV_ARGS(sf2.put()));
    if (sf2) {
        for (UINT i = 0; i < 512; ++i) {
            SHELLDETAILS sd{};
            if (FAILED(sf2->GetDetailsOf(nullptr, i, &sd))) break;
            std::wstring title = strret_text(sd.str);
            SHCOLSTATEF st = 0;
            if (FAILED(sf2->GetDefaultColumnState(i, &st))) st = 0;
            if (!(st & SHCOLSTATE_ONBYDEFAULT) || (st & SHCOLSTATE_HIDDEN)) continue;
            PROPERTYKEY k{};
            if (FAILED(sf2->MapColumnToSCID(i, &k))) continue;
            if (std::any_of(cols.begin(), cols.end(), [&](const Column& c) { return same_key(c.key, k); })) continue;
            cols.push_back(make_column(k, title, sd.fmt, sd.cxChar));
        }
    }
    if (cols.size() < 2) return fs_defaults();
    normalize(cols);
    return cols;
}

// "{fmtid}/pid/width;..." in display order
std::wstring columns_serialize(const ColumnSet& cols) {
    std::wstring s;
    for (auto& c : cols) {
        WCHAR g[64]{};
        StringFromGUID2(c.key.fmtid, g, 64);
        if (!s.empty()) s += L';';
        s += g + (L"/" + std::to_wstring(c.key.pid) + L"/" + std::to_wstring(c.width96));
    }
    return s;
}

ColumnSet columns_parse(const std::wstring& s) {
    ColumnSet cols;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(L';', pos);
        std::wstring part = s.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
        pos = end == std::wstring::npos ? s.size() : end + 1;
        size_t a = part.find(L'/'), b = part.find(L'/', a + 1);
        if (a == std::wstring::npos || b == std::wstring::npos) continue;
        PROPERTYKEY k{};
        if (FAILED(CLSIDFromString(part.substr(0, a).c_str(), &k.fmtid))) continue;
        k.pid = (DWORD)wcstoul(part.c_str() + a + 1, nullptr, 10);
        Column c = make_column(k, L"", LVCFMT_LEFT, 0);
        if (c.kind == COL_EXTRA) {
            // numbers right-aligned like Explorer
            com_ptr<IPropertyDescription> pd;
            VARTYPE vt = VT_EMPTY;
            if (SUCCEEDED(PSGetPropertyDescription(k, IID_PPV_ARGS(pd.put()))) && pd &&
                SUCCEEDED(pd->GetPropertyType(&vt)) &&
                (vt == VT_UI8 || vt == VT_UI4 || vt == VT_I4 || vt == VT_I8 || vt == VT_R8))
                c.fmt = LVCFMT_RIGHT;
        }
        c.width96 = std::clamp(_wtoi(part.c_str() + b + 1), 20, 2000);
        cols.push_back(c);
    }
    if (cols.empty()) return cols;
    normalize(cols);
    return cols;
}

void columns_fill(IShellItem2* si, const ColumnSet& cols, Item* it) {
    int n = 0;
    for (auto& c : cols) n = std::max(n, c.extra + 1);
    it->extra.assign(n, std::wstring());
    it->extraRaw.resize(n);
    for (auto& v : it->extraRaw) PropVariantInit(&v);
    // network drives are not queried (a dead server would stall the listing)
    if (!si || it->driveType == DRIVE_REMOTE) return;
    for (auto& c : cols) {
        if (c.kind != COL_EXTRA || c.extra < 0) continue;
        PROPVARIANT& pv = it->extraRaw[c.extra];
        if (FAILED(si->GetProperty(c.key, &pv))) { PropVariantInit(&pv); continue; }
        if (pv.vt == VT_EMPTY) continue;
        WCHAR buf[512]{};
        if (SUCCEEDED(PSFormatForDisplay(c.key, pv, PDFF_DEFAULT, buf, ARRAYSIZE(buf)))) it->extra[c.extra] = buf;
    }
}

// ------------------------------------------------------------------
// UI thread

ColumnSet columns_for_folder(PCIDLIST_ABSOLUTE folder) {
    const std::wstring* s = settings_columns_for(folder_settings_key(folder));
    if (s) return columns_parse(*s);
    if (search_is_results(folder)) {
        // results come from many folders: the file template plus where each one is
        ColumnSet cols = fs_defaults();
        cols.push_back(make_column(PKEY_ItemFolderPathDisplay, L"Folder", LVCFMT_LEFT, 40));
        normalize(cols);
        return cols;
    }
    return {};
}

static bool same_set(const ColumnSet& a, const ColumnSet& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!same_key(a[i].key, b[i].key) || a[i].width96 != b[i].width96 || a[i].extra != b[i].extra) return false;
    return true;
}

void columns_apply(const ColumnSet& cols) {
    if (cols.empty() || !g_list) return;
    // a sort by a column this folder doesn't have falls back to Name
    if (g_sortColumn == COL_EXTRA &&
        std::none_of(cols.begin(), cols.end(), [](const Column& c) { return same_key(c.key, g_sortKey); })) {
        g_sortColumn = COL_NAME;
        g_sortAsc = true;
    }
    if (same_set(cols, g_columns)) return;
    g_columns = cols;
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    HWND hdr = ListView_GetHeader(g_list);
    for (int n = hdr ? Header_GetItemCount(hdr) : 0; n > 0; --n) ListView_DeleteColumn(g_list, n - 1);
    for (int i = 0; i < (int)g_columns.size(); ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        c.fmt = g_columns[i].fmt;
        c.cx = S(g_columns[i].width96);
        c.pszText = const_cast<LPWSTR>(g_columns[i].title.c_str());
        c.iSubItem = i;
        ListView_InsertColumn(g_list, i, &c);
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    enum_update_header_arrows();
    InvalidateRect(g_list, nullptr, TRUE);
}

const wchar_t* columns_text(const Item* it, int sub) {
    if (sub < 0 || sub >= (int)g_columns.size()) return L"";
    const Column& c = g_columns[sub];
    switch (c.kind) {
        case COL_NAME: return it->name.c_str();
        case COL_SIZE: return it->size.c_str();
        case COL_TYPE: return it->type.c_str();
        case COL_DATE: return it->date.c_str();
    }
    return c.extra >= 0 && c.extra < (int)it->extra.size() ? it->extra[c.extra].c_str() : L"";
}

const UINT* columns_tile_lines(UINT* count) {
    static UINT lines[2];
    UINT n = 0;
    for (int want : { COL_TYPE, COL_SIZE })
        for (int i = 1; i < (int)g_columns.size() && n < 2; ++i)
            if (g_columns[i].kind == want) lines[n++] = i;
    for (int i = 1; i < (int)g_columns.size() && n < 2; ++i)
        if (n == 0 || lines[0] != (UINT)i) lines[n++] = i;
    *count = n;
    return lines;
}

int columns_sort_display_index() {
    for (int i = 0; i < (int)g_columns.size(); ++i) {
        const Column& c = g_columns[i];
        if (g_sortColumn == COL_EXTRA ? (c.kind == COL_EXTRA && same_key(c.key, g_sortKey)) : c.kind == g_sortColumn)
            return i;
    }
    return -1;
}

void columns_header_click(int sub) {
    if (sub < 0 || sub >= (int)g_columns.size()) return;
    const Column& c = g_columns[sub];
    if (c.kind != COL_EXTRA) {
        if (g_sortColumn == c.kind) g_sortAsc = !g_sortAsc;
        else { g_sortColumn = c.kind; g_sortAsc = c.kind != COL_DATE && c.kind != COL_SIZE; }  // newest/largest first
    } else if (g_sortColumn == COL_EXTRA && same_key(g_sortKey, c.key)) {
        g_sortAsc = !g_sortAsc;
    } else {
        g_sortColumn = COL_EXTRA;
        g_sortKey = c.key;
        g_sortAsc = true;
    }
    enum_sort_items();
}

void columns_sort_by(int sub) {
    if (sub < 0 || sub >= (int)g_columns.size() || sub == columns_sort_display_index()) return;
    columns_header_click(sub);              // a column not sorted by yet: its default direction
}

static int s_sortExtra = -1;

void columns_prepare_sort() {
    s_sortExtra = -1;
    if (g_sortColumn != COL_EXTRA) return;
    for (auto& c : g_columns)
        if (c.kind == COL_EXTRA && same_key(c.key, g_sortKey)) s_sortExtra = c.extra;
}

int columns_compare_extra(const Item* a, const Item* b) {
    int i = s_sortExtra;
    if (i < 0) return 0;
    bool ha = i < (int)a->extraRaw.size() && a->extraRaw[i].vt != VT_EMPTY;
    bool hb = i < (int)b->extraRaw.size() && b->extraRaw[i].vt != VT_EMPTY;
    if (ha != hb) return ha ? -1 : 1;                 // empty values last
    if (!ha) return 0;
    return PropVariantCompareEx(a->extraRaw[i], b->extraRaw[i], PVCU_DEFAULT, PVCHF_DEFAULT);
}

// ------------------------------------------------------------------
// persistence of the shown set (display order, current widths)

static void save_current() {
    if (g_columns.empty()) return;
    int n = (int)g_columns.size();
    std::vector<int> order(n);
    if (!ListView_GetColumnOrderArray(g_list, n, order.data()))
        for (int i = 0; i < n; ++i) order[i] = i;
    ColumnSet ordered;
    for (int i : order) {
        if (i < 0 || i >= n) continue;
        Column c = g_columns[i];
        int w = ListView_GetColumnWidth(g_list, i);
        if (w > 0) c.width96 = MulDiv(w, 96, theme_dpi());
        ordered.push_back(c);
    }
    settings_set_columns_for(folder_settings_key(g_current), columns_serialize(ordered));
}

void columns_widths_changed() {
    int dpi = theme_dpi();
    for (int i = 0; i < (int)g_columns.size(); ++i) {
        int w = ListView_GetColumnWidth(g_list, i);
        if (w > 0) g_columns[i].width96 = MulDiv(w, 96, dpi);
    }
    save_current();
}

// ------------------------------------------------------------------
// column chooser (header right-click)

static bool commonly_offered(const PROPERTYKEY& k) {
    static const PROPERTYKEY* keys[] = {
        &PKEY_DateCreated, &PKEY_DateAccessed, &PKEY_FileAttributes, &PKEY_Author, &PKEY_Keywords,
        &PKEY_Title, &PKEY_ItemFolderPathDisplay, &PKEY_Image_Dimensions, &PKEY_Media_Duration,
        &PKEY_DateModified, &PKEY_ItemTypeText, &PKEY_Size,
    };
    for (auto* p : keys) if (same_key(*p, k)) return true;
    return false;
}

void columns_header_menu(POINT screen) {
    struct Cand { PROPERTYKEY key; std::wstring title; int fmt, cxChar; bool shown; };
    std::vector<Cand> cands;
    for (auto& c : g_columns) cands.push_back({ c.key, c.title, c.fmt, 0, true });
    com_ptr<IShellFolder2> sf2;
    if (SUCCEEDED(bind_list(g_current, IID_PPV_ARGS(sf2.put()))) && sf2) {
        for (UINT i = 0; i < 1024; ++i) {
            SHELLDETAILS sd{};
            if (FAILED(sf2->GetDetailsOf(nullptr, i, &sd))) break;
            std::wstring title = strret_text(sd.str);
            SHCOLSTATEF st = 0;
            if (FAILED(sf2->GetDefaultColumnState(i, &st))) st = 0;
            PROPERTYKEY k{};
            if ((st & SHCOLSTATE_HIDDEN) || FAILED(sf2->MapColumnToSCID(i, &k)) || title.empty()) continue;
            if (!(st & SHCOLSTATE_ONBYDEFAULT) && !commonly_offered(k)) continue;
            if (std::any_of(cands.begin(), cands.end(), [&](const Cand& c) { return same_key(c.key, k); })) continue;
            cands.push_back({ k, title, sd.fmt, sd.cxChar, false });
        }
    }
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Size all columns to fit");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < cands.size(); ++i) {
        UINT flags = MF_STRING | (cands[i].shown ? MF_CHECKED : 0);
        if (kind_of(cands[i].key) == COL_NAME) flags |= MF_GRAYED;      // the label column stays
        AppendMenuW(m, flags, 100 + (UINT)i, cands[i].title.c_str());
    }
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, g_main, nullptr);
    DestroyMenu(m);
    if (id == 1) {
        for (int i = 0; i < (int)g_columns.size(); ++i) ListView_SetColumnWidth(g_list, i, LVSCW_AUTOSIZE_USEHEADER);
        columns_widths_changed();
        return;
    }
    if (id < 100 || id - 100 >= cands.size()) return;
    const Cand& pick = cands[id - 100];
    columns_widths_changed();                  // keep the widths the user has now
    ColumnSet next = g_columns;
    bool fetch = false;
    if (pick.shown) {
        next.erase(std::remove_if(next.begin(), next.end(),
                                  [&](const Column& c) { return same_key(c.key, pick.key); }), next.end());
    } else {
        Column c = make_column(pick.key, pick.title, pick.fmt, pick.cxChar);
        if (c.kind == COL_EXTRA) {
            // keep the other columns' value slots: the shown items still carry them
            int n = 0;
            for (auto& e : next) n = std::max(n, e.extra + 1);
            c.extra = n;
            fetch = true;
        }
        next.push_back(c);
    }
    columns_apply(next);
    save_current();
    if (fetch) navigation_refresh();           // read the new column's values
}
