#include "tabs.h"
#include <algorithm>
using std::max;
using std::min;
#include "navigation.h"
#include "settings.h"
#include "theme.h"
#include "chrome.h"
#include "layout.h"
#include "searchbox.h"
#include <unordered_map>

// ------------------------------------------------------------------
// model

static std::vector<TabState> s_tabs;
static int s_nextTabId = 1;
static bool s_persist = true;        // this window owns the saved tab set
int g_activeTab = 0;

static void free_tab(TabState& t) {
    if (t.current) { CoTaskMemFree(t.current); t.current = nullptr; }
    for (auto p : t.back) CoTaskMemFree(p);
    for (auto p : t.fwd) CoTaskMemFree(p);
    t.back.clear(); t.fwd.clear();
}

static std::wstring name_of(PCIDLIST_ABSOLUTE p) {
    std::wstring n = pidl_abs_dispname(p);
    if (n.empty()) n = pidl_folder_path(p);
    if (n.empty()) n = L"?";
    return n;
}

static TabState make_tab(PITEMID_CHILD adoptCurrent) {
    TabState t;
    t.id = s_nextTabId++;
    t.current = adoptCurrent ? adoptCurrent : pidl_desktop();
    t.name = name_of(t.current);
    return t;
}

static int index_of_id(int id) {
    for (int i = 0; i < (int)s_tabs.size(); ++i) if (s_tabs[i].id == id) return i;
    return -1;
}

TabState* tabmodel_active() {
    if (s_tabs.empty()) return nullptr;
    if (g_activeTab < 0 || g_activeTab >= (int)s_tabs.size()) g_activeTab = 0;
    return &s_tabs[g_activeTab];
}

void tabmodel_on_navigate(PITEMID_CHILD newAbs, bool pushHistory) {
    TabState* t = tabmodel_active();
    if (!t) { CoTaskMemFree(newAbs); return; }
    if (!newAbs) newAbs = pidl_desktop();
    if (t->current) {
        if (pushHistory) t->back.push_back(t->current);   // adopt old
        else CoTaskMemFree(t->current);
    }
    t->current = newAbs;                                    // adopt new
    if (pushHistory) { for (auto p : t->fwd) CoTaskMemFree(p); t->fwd.clear(); }
    t->name = name_of(newAbs);
    if (t->back.size() > 200) { CoTaskMemFree(t->back.front()); t->back.erase(t->back.begin()); }
}

bool tabmodel_can_back() { TabState* t = tabmodel_active(); return t && !t->back.empty(); }
bool tabmodel_can_forward() { TabState* t = tabmodel_active(); return t && !t->fwd.empty(); }

bool tabmodel_go_back() {
    TabState* t = tabmodel_active();
    if (!t || t->back.empty()) return false;
    t->fwd.push_back(t->current);
    t->current = t->back.back();
    t->back.pop_back();
    t->name = name_of(t->current);
    return true;
}
bool tabmodel_go_forward() {
    TabState* t = tabmodel_active();
    if (!t || t->fwd.empty()) return false;
    t->back.push_back(t->current);
    t->current = t->fwd.back();
    t->fwd.pop_back();
    t->name = name_of(t->current);
    return true;
}

void tabmodel_switch_to(int idx) {
    if (idx < 0 || idx >= (int)s_tabs.size() || idx == g_activeTab) return;
    g_activeTab = idx;
    navigate_replace(tabmodel_active());
}

void tabmodel_new_tab() {
    TabState t = make_tab(current_clone());
    t.view = g_view;
    s_tabs.push_back(std::move(t));
    g_activeTab = (int)s_tabs.size() - 1;
    navigate_replace(tabmodel_active());
    tabs_persist_now();
    tabstrip_refresh();
}

bool tabmodel_is_pinned(int idx) {
    return idx >= 0 && idx < (int)s_tabs.size() && s_tabs[idx].pinned;
}

void tabmodel_close_tab(int idx) {
    if (idx < 0) idx = g_activeTab;
    if (idx < 0 || idx >= (int)s_tabs.size()) return;
    if (s_tabs[idx].pinned) return;              // pinned tabs can't be closed (Ctrl+W safe)
    if (s_tabs.size() <= 1) { PostMessageW(g_main, WM_CLOSE, 0, 0); return; }
    bool wasActive = idx == g_activeTab;
    free_tab(s_tabs[idx]);
    s_tabs.erase(s_tabs.begin() + idx);
    if (idx < g_activeTab) --g_activeTab;        // keep the same tab active
    if (g_activeTab >= (int)s_tabs.size()) g_activeTab = (int)s_tabs.size() - 1;
    if (wasActive) navigate_replace(tabmodel_active());
    tabs_persist_now();
    tabstrip_refresh();
}

// keep pinned tabs first (stable), keeping the active tab active
static void sort_pinned_first() {
    int activeId = tabmodel_active() ? tabmodel_active()->id : 0;
    std::stable_sort(s_tabs.begin(), s_tabs.end(),
                     [](const TabState& a, const TabState& b) { return a.pinned && !b.pinned; });
    g_activeTab = max(0, index_of_id(activeId));
}

void tabmodel_toggle_pin(int idx) {
    if (idx < 0 || idx >= (int)s_tabs.size()) return;
    s_tabs[idx].pinned = !s_tabs[idx].pinned;
    sort_pinned_first();
    tabstrip_refresh();
    tabs_persist_now();
}

void tabmodel_duplicate_tab(int idx) {
    if (idx < 0 || idx >= (int)s_tabs.size()) return;
    TabState t = make_tab(pidl_clone(s_tabs[idx].current));
    t.view = s_tabs[idx].view;
    s_tabs.insert(s_tabs.begin() + idx + 1, std::move(t));
    g_activeTab = idx + 1;
    navigate_replace(tabmodel_active());
    tabs_persist_now();
    tabstrip_refresh();
}

void tabmodel_close_others(int idx) {
    if (idx < 0 || idx >= (int)s_tabs.size()) return;
    int keepId = s_tabs[idx].id;
    int activeId = tabmodel_active()->id;
    for (int i = (int)s_tabs.size() - 1; i >= 0; --i) {
        if (i != idx && !s_tabs[i].pinned) {
            free_tab(s_tabs[i]);
            s_tabs.erase(s_tabs.begin() + i);
        }
    }
    int stillActive = index_of_id(activeId);
    g_activeTab = stillActive >= 0 ? stillActive : max(0, index_of_id(keepId));
    if (stillActive < 0) navigate_replace(tabmodel_active());
    tabs_persist_now();
    tabstrip_refresh();
}

// move tab `from` so it lands before the tab currently at `to` (to == size: append).
// Returns the tab's new index.
int tabmodel_move_tab(int from, int to) {
    if (from < 0 || from >= (int)s_tabs.size()) return from;
    to = max(0, min(to, (int)s_tabs.size()));
    if (to == from || to == from + 1) return from;
    int activeId = tabmodel_active()->id;
    TabState t = std::move(s_tabs[from]);
    s_tabs.erase(s_tabs.begin() + from);
    if (to > from) --to;
    s_tabs.insert(s_tabs.begin() + to, std::move(t));
    g_activeTab = max(0, index_of_id(activeId));
    tabs_persist_now();
    tabstrip_refresh();
    return to;
}

void tabmodel_next_tab(int delta) {
    if (s_tabs.size() < 2) return;
    int n = (int)s_tabs.size();
    int idx = (g_activeTab + delta + n) % n;
    tabmodel_switch_to(idx);
}

void tabmodel_switch_index_1_based(int one) {
    int idx = one - 1;
    if (one == 9 || idx >= (int)s_tabs.size()) idx = (int)s_tabs.size() - 1;   // Ctrl+9 = last
    tabmodel_switch_to(idx);
}

std::wstring tabmodel_active_name() {
    TabState* t = tabmodel_active();
    return t ? t->name : L"";
}

// path used for tab persistence: prefer a plain fs path so it survives across
// runs (round-trips through SHParseDisplayName even for system folders);
// fall back to the shell parsing name for pure shell locations.
static std::wstring tab_persist_path(PCIDLIST_ABSOLUTE p) {
    if (pidl_is_desktop(p)) return L"::desktop";
    std::wstring s = pidl_folder_path(p);      // fs path when possible
    if (s.empty()) s = pidl_parsingname(p);     // shell parsing name
    if (!s.empty() && s.back() == L'\\' && s.size() > 3) s.pop_back();
    return s;
}

void tabs_persist_now() {
    if (s_tabs.empty() || !s_persist) return;      // extra windows don't own the saved tab set
    std::vector<std::wstring> lines;
    int active = 0;
    tabs_serialize_lines(lines, active);
    settings_set_tabs(lines, active);        // writes the ini immediately
}

static PITEMID_CHILD parse_persisted(const std::wstring& path) {
    if (path.empty() || path == L"::desktop") return pidl_desktop();
    PIDLIST_ABSOLUTE p = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &p, 0, nullptr)) && p)
        return reinterpret_cast<PITEMID_CHILD>(p);
    // path no longer exists: walk to parent until it parses, else Desktop
    std::wstring fb = path;
    while (!fb.empty()) {
        size_t s1 = fb.find_last_of(L"\\/");
        if (s1 == std::wstring::npos) break;
        fb.erase(s1);
        if (!fb.empty() && fb.back() == L':') fb += L'\\';
        if (SUCCEEDED(SHParseDisplayName(fb.c_str(), nullptr, &p, 0, nullptr)) && p)
            return reinterpret_cast<PITEMID_CHILD>(p);
    }
    return pidl_desktop();
}

void tabs_deserialize(const std::wstring& tabs, int active) {
    for (auto& t : s_tabs) free_tab(t);
    s_tabs.clear();
    size_t pos = 0;
    while (pos < tabs.size()) {
        size_t nl = tabs.find(L'\n', pos);
        std::wstring line = nl == std::wstring::npos ? tabs.substr(pos) : tabs.substr(pos, nl - pos);
        pos = nl == std::wstring::npos ? tabs.size() : nl + 1;
        if (line.empty()) continue;
        std::vector<std::wstring> f;
        size_t p2 = 0;
        while (true) {
            size_t s = line.find(L'\x1', p2);   // \x01 separator
            f.push_back(s == std::wstring::npos ? line.substr(p2) : line.substr(p2, s - p2));
            if (s == std::wstring::npos) break;
            p2 = s + 1;
        }
        while (f.size() < 4) f.push_back(L"");
        TabState t = make_tab(parse_persisted(f[0]));
        t.view = int_to_view(_wtoi(f[2].c_str()));
        t.pinned = (f[3] == L"1");
        s_tabs.push_back(std::move(t));
    }
    if (s_tabs.empty()) {
        // first run: Home, like Windows 11 (older systems: the user's profile)
        PIDLIST_ABSOLUTE dp = nullptr;
        if (FAILED(SHParseDisplayName(L"shell:::{f874310e-b6b7-47dc-bc84-b9e6b38f5903}", nullptr, &dp, 0, nullptr)) &&
            FAILED(SHGetKnownFolderIDList(FOLDERID_Profile, 0, nullptr, &dp)))
            dp = nullptr;
        s_tabs.push_back(make_tab(reinterpret_cast<PITEMID_CHILD>(dp)));
    }
    sort_pinned_first();
    g_activeTab = max(0, min(active, (int)s_tabs.size() - 1));
    current_replace(pidl_clone(tabmodel_active()->current));
}

void tabs_serialize_lines(std::vector<std::wstring>& lines, int& outActive) {
    lines.clear();
    for (auto& t : s_tabs) {
        // search results live only in this session: such a tab reopens on the searched folder
        PCIDLIST_ABSOLUTE scope = search_scope_of(t.current);
        std::wstring name = scope ? pidl_abs_dispname(scope) : t.name;
        lines.push_back(tab_persist_path(scope ? scope : t.current) + L"\x1" + name + L"\x1" +
                        std::to_wstring(view_to_int(t.view)) + L"\x1" + (t.pinned ? L"1" : L"0"));
    }
    outActive = g_activeTab;
}

void tabmodel_init(const std::wstring& openPath, bool restore) {
    std::wstring saved; int active = 0;
    s_persist = restore;
    if (restore) settings_tabs(saved, active);
    if (restore || openPath.empty()) tabs_deserialize(saved, active);
    if (openPath.empty()) return;
    // "explorer-plus.exe <folder>": open it in a new, active tab
    PIDLIST_ABSOLUTE p = nullptr;
    if (openPath == L"::desktop") p = reinterpret_cast<PIDLIST_ABSOLUTE>(pidl_desktop());
    else if (FAILED(parse_typed(openPath, &p))) p = nullptr;
    if (!p) {
        if (s_tabs.empty()) tabs_deserialize(L"", 0);      // fresh window, bad path: Home
        return;
    }
    for (size_t i = 0; i < s_tabs.size(); ++i) {           // already open: just activate it
        if (pidl_equal(s_tabs[i].current, p)) {
            CoTaskMemFree(p);
            g_activeTab = (int)i;
            current_replace(pidl_clone(s_tabs[i].current));
            return;
        }
    }
    TabState t = make_tab(reinterpret_cast<PITEMID_CHILD>(p));
    t.view = view_for_folder(t.current);
    s_tabs.push_back(std::move(t));
    g_activeTab = (int)s_tabs.size() - 1;
    current_replace(pidl_clone(tabmodel_active()->current));
}

// ------------------------------------------------------------------
// tab strip window: Win11 tabs in the caption row (custom drawn)

static HWND s_strip = nullptr;
static int s_hoverTab = -1;
static bool s_hoverClose = false;
static bool s_hoverPlus = false;
static int s_draggingTab = -1;   // index of tab being drag-reordered
static bool s_dragMoved = false;
static POINT s_dragDownPt{};
static std::unordered_map<ULONGLONG, int> s_iconCache;   // pidl signature -> system icon

struct TabUI { RECT rect; RECT close; };

static int tab_icon(PCIDLIST_ABSOLUTE p) {
    ULONGLONG sig = pidl_sig(p);
    auto it = s_iconCache.find(sig);
    if (it != s_iconCache.end()) return it->second;
    if (s_iconCache.size() > 256) s_iconCache.clear();
    return s_iconCache[sig] = sys_icon_index(p);
}

static int tab_top() { return S(8); }

static void layout_tabs(int stripW, std::vector<TabUI>& out, RECT* plus) {
    out.clear();
    HDC hdc = s_strip ? GetDC(s_strip) : nullptr;
    HGDIOBJ of = hdc ? SelectObject(hdc, g_font) : nullptr;
    int h = layout_caption_h();
    int left = S(12);
    int avail = max((int)S(100), stripW - left - S(48) - S(40));   // "+" and a drag area stay visible
    std::vector<int> widths;
    int total = 0;
    for (auto& t : s_tabs) {
        int tw = hdc ? text_width(hdc, t.name.c_str(), (int)t.name.size()) : 0;
        int w = S(12) + S(16) + S(10) + tw + S(8) + (t.pinned ? S(4) : S(28)) + S(4);
        widths.push_back(max((int)S(96), min(w, (int)S(240))));
        total += widths.back();
    }
    if (hdc) { SelectObject(hdc, of); ReleaseDC(s_strip, hdc); }
    int n = (int)s_tabs.size();
    if (total > avail && n > 0) {
        int each = max((int)S(56), avail / n);
        for (auto& w : widths) w = min(w, each);
    }
    int x = left;
    for (int i = 0; i < n; ++i) {
        TabUI ui;
        ui.rect = { x, tab_top(), x + widths[i], h };
        int cs = S(24), cy = tab_top() + (h - tab_top() - cs) / 2;
        ui.close = { ui.rect.right - S(6) - cs, cy, ui.rect.right - S(6), cy + cs };
        out.push_back(ui);
        x += widths[i];
    }
    if (plus) {
        int ps = S(32), py = tab_top() + (h - tab_top() - ps) / 2;
        *plus = { x + S(6), py, x + S(6) + ps, py + ps };
    }
}

static int hit_tab(POINT pt, bool* onClose, bool* onPlus) {
    std::vector<TabUI> uis;
    RECT rc{}; if (s_strip) GetClientRect(s_strip, &rc);
    RECT plus{};
    layout_tabs(rc.right, uis, &plus);
    *onClose = false;
    if (onPlus) *onPlus = PtInRect(&plus, pt) != 0;
    for (size_t i = 0; i < uis.size(); ++i) {
        if (PtInRect(&uis[i].rect, pt)) {
            *onClose = !s_tabs[i].pinned && PtInRect(&uis[i].close, pt);
            return (int)i;
        }
    }
    return -1;
}

static void paint_strip(HWND hwnd) {
    BufferedPaint bp(hwnd);
    const Palette& p = pal();
    bool activeWnd = chrome_active();
    COLORREF bg = activeWnd ? p.caption : p.captionInactive;
    fill_rect(bp.dc, bp.rc, bg);
    std::vector<TabUI> uis;
    RECT plus{};
    layout_tabs(bp.rc.right, uis, &plus);
    COLORREF sepColor = blend(bg, p.text, 0.22);
    HIMAGELIST il = sys_imagelist(SHIL_SMALL);
    int iconSz = GetSystemMetrics(SM_CXSMICON);
    for (size_t i = 0; i < uis.size(); ++i) {
        const TabUI& u = uis[i];
        bool active = (int)i == g_activeTab;
        bool hover = (int)i == s_hoverTab;
        if (active) {
            fill_tab_shape(bp.dc, u.rect, S(8), p.layer);
        } else if (hover) {
            RECT hr{ u.rect.left + S(2), u.rect.top + S(2), u.rect.right - S(2), u.rect.bottom - S(4) };
            fill_rrect(bp.dc, hr, S(6), p.tabHover);
        }
        // separator after an inactive tab unless a highlighted tab follows
        bool nextHighlighted = (int)i + 1 == g_activeTab || (int)i + 1 == s_hoverTab;
        if (!active && !hover && !nextHighlighted && i + 1 < uis.size()) {
            int cy = (u.rect.top + u.rect.bottom) / 2;
            RECT s{ u.rect.right - 1, cy - S(8), u.rect.right, cy + S(8) };
            fill_rect(bp.dc, s, sepColor);
        }
        int x = u.rect.left + S(12);
        int cy = (u.rect.top + u.rect.bottom) / 2;
        if (s_tabs[i].pinned) {
            RECT pr{ x, u.rect.top, x + S(16), u.rect.bottom };
            draw_glyph(bp.dc, L'\xE718', pr, active ? p.accent : p.textSecondary, 12);
        } else {
            int icon = tab_icon(s_tabs[i].current);
            if (icon >= 0) ImageList_Draw(il, icon, bp.dc, x, cy - iconSz / 2, ILD_TRANSPARENT);
        }
        x += S(16) + S(10);
        RECT tr{ x, u.rect.top, (s_tabs[i].pinned ? u.rect.right - S(8) : u.close.left - S(4)), u.rect.bottom };
        SetTextColor(bp.dc, activeWnd ? p.text : p.textSecondary);
        DrawTextW(bp.dc, s_tabs[i].name.c_str(), -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (!s_tabs[i].pinned) {
            if (hover && s_hoverClose) fill_rrect(bp.dc, u.close, S(4), blend(active ? p.layer : p.tabHover, p.text, 0.08));
            draw_glyph(bp.dc, L'\xE711', u.close, p.text, 10);
        }
    }
    if (s_hoverPlus) fill_rrect(bp.dc, plus, S(4), blend(bg, p.text, 0.07));
    draw_glyph(bp.dc, L'\xE710', plus, p.text, 12);
}

static LRESULT CALLBACK strip_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: {
            // empty strip area belongs to the title bar (drag, double-click, snap)
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            bool oc = false, op = false;
            if (s_draggingTab < 0 && hit_tab(pt, &oc, &op) < 0 && !op) return HTTRANSPARENT;
            return HTCLIENT;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint_strip(hwnd); return 0;
        case WM_MOUSEMOVE: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (s_draggingTab >= 0) {
                // small threshold so a plain click doesn't reorder
                if (!s_dragMoved) {
                    if (abs(pt.x - s_dragDownPt.x) < GetSystemMetrics(SM_CXDRAG) &&
                        abs(pt.y - s_dragDownPt.y) < GetSystemMetrics(SM_CYDRAG))
                        return 0;
                    s_dragMoved = true;
                }
                RECT rc{}; GetClientRect(hwnd, &rc);
                std::vector<TabUI> uis;
                layout_tabs(rc.right, uis, nullptr);
                int to = s_draggingTab;
                for (size_t i = 0; i < uis.size(); ++i) {
                    if (pt.x < (uis[i].rect.left + uis[i].rect.right) / 2) { to = (int)i; break; }
                    to = (int)i + 1;
                }
                if (to != s_draggingTab && to != s_draggingTab + 1)
                    s_draggingTab = tabmodel_move_tab(s_draggingTab, to);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            bool oc = false, op = false;
            int t = hit_tab(pt, &oc, &op);
            if (t != s_hoverTab || oc != s_hoverClose || op != s_hoverPlus) {
                s_hoverTab = t; s_hoverClose = oc; s_hoverPlus = op;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&te);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (s_draggingTab < 0) {
                s_hoverTab = -1; s_hoverClose = false; s_hoverPlus = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool oc = false, op = false;
            int t = hit_tab(pt, &oc, &op);
            if (op) { tabmodel_new_tab(); return 0; }
            if (t >= 0) {
                if (oc) { tabmodel_close_tab(t); return 0; }
                if (t != g_activeTab) tabmodel_switch_to(t);
                s_draggingTab = t;         // potential drag-reorder; released on LBUTTONUP without move
                s_dragMoved = false;
                s_dragDownPt = pt;
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (s_draggingTab >= 0) {
                s_draggingTab = -1;
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            s_draggingTab = -1;
            return 0;
        case WM_RBUTTONUP: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool oc = false;
            int t = hit_tab(pt, &oc, nullptr);
            if (t < 0) return 0;
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"New tab\tCtrl+T");
            AppendMenuW(m, MF_STRING, 2, L"Duplicate tab");
            AppendMenuW(m, MF_STRING, 3, s_tabs[t].pinned ? L"Unpin tab" : L"Pin tab");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING | (s_tabs[t].pinned ? MF_GRAYED : 0), 4, L"Close tab\tCtrl+W");
            AppendMenuW(m, MF_STRING, 5, L"Close other tabs");
            POINT sp = pt; ClientToScreen(hwnd, &sp);
            UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTBUTTON, sp.x, sp.y, hwnd, nullptr);
            DestroyMenu(m);
            switch (cmd) {
                case 1: tabmodel_new_tab(); break;
                case 2: tabmodel_duplicate_tab(t); break;
                case 3: tabmodel_toggle_pin(t); break;
                case 4: tabmodel_close_tab(t); break;
                case 5: tabmodel_close_others(t); break;
            }
            return 0;
        }
        case WM_MBUTTONUP: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool oc = false;
            int t = hit_tab(pt, &oc, nullptr);
            if (t >= 0) tabmodel_close_tab(t);   // close_tab itself ignores pinned
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND tabstrip_create(HWND parent) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = g_hinst;
    wc.lpfnWndProc = strip_proc;
    wc.lpszClassName = L"ExplorerPlusTabStrip";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    s_strip = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
                              0, 0, 100, layout_caption_h(), parent, (HMENU)IDC_TABSTRIP, g_hinst, nullptr);
    return s_strip;
}

HWND tabstrip_hwnd() { return s_strip; }

void tabstrip_refresh() {
    if (s_strip) InvalidateRect(s_strip, nullptr, FALSE);
}