#include "breadcrumb.h"
#include <algorithm>
using std::max;
using std::min;
#include "theme.h"
#include "navigation.h"
#include "listview.h"
#include "acc.h"
#include <algorithm>

struct Crumb {
    PIDLIST_ABSOLUTE abs = nullptr; // owned
    std::wstring name;
    RECT chevron{}, text{};         // layout (client coords); empty = hidden (overflow)
};

static HWND s_box = nullptr, s_edit = nullptr;
static WNDPROC s_editProc = nullptr;
static std::vector<Crumb> s_crumbs;
static RECT s_overflow{}, s_history{}, s_icon{};
static int s_firstVisible = 0;
static int s_hover = -1;            // 2*i = chevron of crumb i, 2*i+1 = text; -2 overflow; -3 history
static int s_pressed = -100;
static int s_iconIndex = -1;
static HBRUSH s_editBrush = nullptr;
static COLORREF s_editBrushColor = CLR_INVALID;

constexpr int kHitNone = -1, kHitOverflow = -2, kHitHistory = -3;

static void free_crumbs() {
    for (auto& c : s_crumbs) CoTaskMemFree(c.abs);
    s_crumbs.clear();
}

static void rebuild_crumbs() {
    free_crumbs();
    PIDLIST_ABSOLUTE p = current_clone();
    while (p && !pidl_is_desktop(p)) {
        Crumb c;
        c.abs = pidl_clone(p);
        c.name = pidl_abs_dispname(p);
        s_crumbs.insert(s_crumbs.begin(), std::move(c));
        ILRemoveLastID(p);
    }
    if (s_crumbs.empty()) {                        // the Desktop root itself
        Crumb c;
        c.abs = pidl_desktop();
        c.name = pidl_abs_dispname(c.abs);
        s_crumbs.push_back(std::move(c));
    }
    CoTaskMemFree(p);
    s_iconIndex = sys_icon_index(g_pane->current);
}

// measure with the DC we paint with (dc may be null: use a window DC)
static void layout_crumbs(HDC paintDc = nullptr) {
    if (!s_box) return;
    RECT rc{}; GetClientRect(s_box, &rc);
    HDC dc = paintDc ? paintDc : GetDC(s_box);
    HGDIOBJ of = SelectObject(dc, g_font);
    int h = rc.bottom;
    s_icon = { S(8), 0, S(8) + S(20), h };
    s_history = { rc.right - S(30), S(3), rc.right - S(3), h - S(3) };
    int left = s_icon.right + S(2), right = s_history.left - S(4);
    int chevW = S(18);
    std::vector<int> textW;
    for (auto& c : s_crumbs) {
        textW.push_back(min(text_width(dc, c.name.c_str(), (int)c.name.size()) + S(2), S(220)) + S(12));
    }
    SelectObject(dc, of);
    if (!paintDc) ReleaseDC(s_box, dc);
    // drop leading crumbs until the rest fits (always keep the last one)
    int n = (int)s_crumbs.size();
    s_firstVisible = 0;
    auto width_from = [&](int first) {
        int w = first > 0 ? chevW : 0;               // overflow button
        for (int i = first; i < n; ++i) w += chevW + textW[i];
        return w;
    };
    while (s_firstVisible < n - 1 && width_from(s_firstVisible) > right - left) ++s_firstVisible;
    int x = left;
    s_overflow = {};
    if (s_firstVisible > 0) { s_overflow = { x, S(3), x + chevW, h - S(3) }; x += chevW; }
    for (int i = 0; i < n; ++i) {
        Crumb& c = s_crumbs[i];
        if (i < s_firstVisible) { c.chevron = c.text = {}; continue; }
        c.chevron = { x, S(3), x + chevW, h - S(3) };
        x += chevW;
        int tw = min(textW[i], max(S(40), right - x));
        c.text = { x, S(3), x + tw, h - S(3) };
        x += tw;
    }
}

static int hit(POINT pt) {
    if (PtInRect(&s_history, pt)) return kHitHistory;
    if (PtInRect(&s_overflow, pt)) return kHitOverflow;
    for (int i = 0; i < (int)s_crumbs.size(); ++i) {
        if (PtInRect(&s_crumbs[i].chevron, pt)) return 2 * i;
        if (PtInRect(&s_crumbs[i].text, pt)) return 2 * i + 1;
    }
    return kHitNone;
}

static void paint(HWND hwnd) {
    BufferedPaint bp(hwnd);
    layout_crumbs(bp.dc);
    const Palette& p = pal();
    fill_rect(bp.dc, bp.rc, p.layer);
    bool editing = IsWindowVisible(s_edit);
    fill_rrect(bp.dc, bp.rc, S(4), p.boxFill, p.boxBorder);
    if (editing && GetFocus() == s_edit) {
        RECT ul{ bp.rc.left + S(1), bp.rc.bottom - S(2), bp.rc.right - S(1), bp.rc.bottom };
        fill_rect(bp.dc, ul, p.accent);
    }
    // location icon
    if (s_iconIndex >= 0) {
        int sz = 0, szH = 0;
        ImageList_GetIconSize(sys_imagelist(SHIL_SMALL), &sz, &szH);   // the list's size, not this monitor's metric
        ImageList_Draw(sys_imagelist(SHIL_SMALL), s_iconIndex, bp.dc,
                       s_icon.left + (s_icon.right - s_icon.left - sz) / 2, (bp.rc.bottom - sz) / 2, ILD_TRANSPARENT);
    }
    COLORREF hoverFill = blend(p.boxFill, p.text, 0.07), pressFill = blend(p.boxFill, p.text, 0.11);
    auto button_bg = [&](int id, const RECT& r) {
        if (s_pressed == id) fill_rrect(bp.dc, r, S(4), pressFill);
        else if (s_hover == id) fill_rrect(bp.dc, r, S(4), hoverFill);
    };
    if (!editing) {
        if (s_overflow.right > s_overflow.left) {
            button_bg(kHitOverflow, s_overflow);
            draw_glyph(bp.dc, L'\xE76B', s_overflow, p.textSecondary, 8);   // chevron left
            RECT o2 = s_overflow; OffsetRect(&o2, S(4), 0);
            draw_glyph(bp.dc, L'\xE76B', o2, p.textSecondary, 8);
        }
        SetTextColor(bp.dc, p.text);
        for (int i = s_firstVisible; i < (int)s_crumbs.size(); ++i) {
            const Crumb& c = s_crumbs[i];
            button_bg(2 * i, c.chevron);
            draw_glyph(bp.dc, L'\xE76C', c.chevron, p.textSecondary, 8);     // chevron right
            button_bg(2 * i + 1, c.text);
            RECT t = c.text;
            InflateRect(&t, -S(6), 0);
            DrawTextW(bp.dc, c.name.c_str(), -1, &t,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    button_bg(kHitHistory, s_history);
    draw_glyph(bp.dc, L'\xE70D', s_history, p.textSecondary, 10);           // chevron down
}

// ------------------------------------------------------------------
// drop-down menus

struct MenuEntry { PIDLIST_ABSOLUTE abs; int icon; };
static std::vector<MenuEntry> s_menuEntries;

static void clear_menu_entries() {
    for (auto& e : s_menuEntries) CoTaskMemFree(e.abs);
    s_menuEntries.clear();
}

static void append_entry(HMENU m, PIDLIST_ABSOLUTE abs, const std::wstring& name, bool bold) {
    UINT id = (UINT)s_menuEntries.size() + 1;
    s_menuEntries.push_back({ abs, sys_icon_index(abs) });
    MENUITEMINFOW mi{ sizeof(mi) };
    mi.fMask = MIIM_ID | MIIM_STRING | MIIM_BITMAP | MIIM_STATE;
    mi.wID = id;
    mi.dwTypeData = const_cast<LPWSTR>(name.c_str());
    mi.hbmpItem = HBMMENU_CALLBACK;                 // icon drawn in WM_DRAWITEM
    mi.fState = bold ? MFS_DEFAULT : 0;
    InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
}

// subfolders of `parent` (archives excluded), sorted like the tree
static void fill_children_menu(HMENU m, PCIDLIST_ABSOLUTE parent, PCIDLIST_ABSOLUTE current) {
    com_ptr<IShellFolder> psf;
    if (FAILED(bind_list(parent, IID_PPV_ARGS(psf.put()))) || !psf) return;
    com_ptr<IEnumIDList> en;
    SHCONTF f = SHCONTF_FOLDERS | (g_showHidden ? SHCONTF_INCLUDEHIDDEN : 0);
    if (psf->EnumObjects(g_main, f, en.put()) != S_OK || !en) return;
    std::vector<PITEMID_CHILD> kids;
    PITEMID_CHILD c = nullptr;
    while (kids.size() < 500 && en->Next(1, &c, nullptr) == S_OK) {
        SFGAOF a = SFGAO_FOLDER | SFGAO_STREAM;
        if (SUCCEEDED(psf->GetAttributesOf(1, (PCUITEMID_CHILD_ARRAY)&c, &a)) &&
            (a & SFGAO_FOLDER) && !(a & SFGAO_STREAM))
            kids.push_back(c);
        else
            CoTaskMemFree(c);
    }
    IShellFolder* raw = psf.get();
    std::sort(kids.begin(), kids.end(), [raw](PITEMID_CHILD a, PITEMID_CHILD b) {
        return (short)HRESULT_CODE(raw->CompareIDs(0, a, b)) < 0;
    });
    for (auto k : kids) {
        PIDLIST_ABSOLUTE abs = ILCombine(parent, k);
        std::wstring name = child_name(raw, k, SHGDN_NORMAL);
        bool isCurrent = current && ILIsParent(abs, current, FALSE);
        append_entry(m, abs, name, isCurrent);
        CoTaskMemFree(k);
    }
}

static void track_and_navigate(HMENU m, POINT screen) {
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                               screen.x, screen.y, s_box, nullptr);
    DestroyMenu(m);
    if (id >= 1 && id <= s_menuEntries.size()) navigate_to(s_menuEntries[id - 1].abs);
    clear_menu_entries();
}

static void show_chevron_menu(int crumb) {
    clear_menu_entries();
    HMENU m = CreatePopupMenu();
    PIDLIST_ABSOLUTE parent = crumb > 0 ? pidl_clone(s_crumbs[crumb - 1].abs) : pidl_desktop();
    fill_children_menu(m, parent, g_pane->current);
    CoTaskMemFree(parent);
    if (GetMenuItemCount(m) == 0) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"(empty)");
    POINT pt{ s_crumbs[crumb].chevron.left, s_crumbs[crumb].chevron.bottom + S(4) };
    ClientToScreen(s_box, &pt);
    track_and_navigate(m, pt);
}

static void show_overflow_menu() {
    clear_menu_entries();
    HMENU m = CreatePopupMenu();
    for (int i = s_firstVisible - 1; i >= 0; --i)
        append_entry(m, pidl_clone(s_crumbs[i].abs), s_crumbs[i].name, false);
    POINT pt{ s_overflow.left, s_overflow.bottom + S(4) };
    ClientToScreen(s_box, &pt);
    track_and_navigate(m, pt);
}

static void show_history_menu() {
    auto hist = address_history_list();
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < hist.size(); ++i)
        AppendMenuW(m, MF_STRING, (UINT)i + 1, hist[i].c_str());
    if (hist.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"(no history)");
    RECT rc{}; GetClientRect(s_box, &rc);
    POINT pt{ 0, rc.bottom + S(2) };
    ClientToScreen(s_box, &pt);
    TPMPARAMS tp{ sizeof(tp) };
    GetWindowRect(s_box, &tp.rcExclude);
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL,
                               pt.x, pt.y, s_box, &tp);
    DestroyMenu(m);
    if (id >= 1 && id <= hist.size()) navigate_path_text(hist[id - 1]);
}

// ------------------------------------------------------------------
// edit mode

static void end_edit(bool focusList) {
    if (!IsWindowVisible(s_edit)) return;
    // focus first: hiding the focused edit hands focus to the box, which would edit again
    if (focusList && g_pane->list) SetFocus(g_pane->list);
    ShowWindow(s_edit, SW_HIDE);
    InvalidateRect(s_box, nullptr, FALSE);
}

static void place_edit() {
    RECT rc{}; GetClientRect(s_box, &rc);
    HDC dc = GetDC(s_box);
    HGDIOBJ of = SelectObject(dc, g_font);
    TEXTMETRICW tm{}; GetTextMetricsW(dc, &tm);
    SelectObject(dc, of);
    ReleaseDC(s_box, dc);
    int eh = tm.tmHeight + 2;
    int x = s_icon.right + S(4);
    MoveWindow(s_edit, x, (rc.bottom - eh) / 2, s_history.left - S(4) - x, eh, TRUE);
}

void address_begin_edit() {
    if (!s_edit) return;
    if (IsWindowVisible(s_edit)) {      // already editing (F4 / Ctrl+L again): keep what is typed
        SetFocus(s_edit);
        SendMessageW(s_edit, EM_SETSEL, 0, -1);
        return;
    }
    // autocomplete on first use: setting it up costs ~10 ms at startup
    static bool autoComplete = false;
    if (!autoComplete) {
        autoComplete = true;
        SHAutoComplete(s_edit, SHACF_FILESYS_DIRS | SHACF_URLHISTORY);
    }
    SetWindowTextW(s_edit, current_display_path().c_str());
    place_edit();
    ShowWindow(s_edit, SW_SHOW);
    SetFocus(s_edit);
    SendMessageW(s_edit, EM_SETSEL, 0, -1);
    InvalidateRect(s_box, nullptr, FALSE);
}

bool address_editing() { return s_edit && IsWindowVisible(s_edit); }

static LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        int len = GetWindowTextLengthW(h);
        std::wstring buf((size_t)len + 1, L'\0');
        GetWindowTextW(h, buf.data(), len + 1);
        buf.resize(len);
        end_edit(true);
        navigate_path_text(buf);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        SetWindowTextW(h, current_display_path().c_str());
        end_edit(true);
        return 0;
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;   // no beep
    if (msg == WM_KILLFOCUS) {
        LRESULT r = CallWindowProcW(s_editProc, h, msg, wp, lp);
        // keep editing while the autocomplete drop-down (another window) has focus
        HWND to = (HWND)wp;
        WCHAR cls[64]{};
        if (to) GetClassNameW(to, cls, 64);
        if (lstrcmpW(cls, L"Auto-Suggest Dropdown") != 0) end_edit(false);
        return r;
    }
    return CallWindowProcW(s_editProc, h, msg, wp, lp);
}

// ------------------------------------------------------------------
// screen readers: "Address" with the path as its value; the visible crumbs (each its
// chevron, then its name), the overflow and the history buttons. Actions are posted
// back (kAccAction): the menus are modal.

static const UINT kAccAction = WM_APP + 50;
struct AccCrumb { int hit; };          // a hit code (see s_hover)
static std::vector<AccCrumb> acc_items() {
    std::vector<AccCrumb> v;
    if (!IsRectEmpty(&s_overflow)) v.push_back({ kHitOverflow });
    for (int i = s_firstVisible; i < (int)s_crumbs.size(); ++i) {
        v.push_back({ 2 * i });
        v.push_back({ 2 * i + 1 });
    }
    v.push_back({ kHitHistory });
    return v;
}
static const AccSource s_acc = [] {
    AccSource a;
    a.name = L"Address";
    a.role = ROLE_SYSTEM_TOOLBAR;
    a.value = [] { return current_display_path(); };
    a.count = [] { return (int)acc_items().size(); };
    a.child = [](int n) {
        int h = acc_items()[n].hit;
        AccChild c;
        if (h == kHitOverflow) {
            c.name = L"Earlier locations"; c.role = ROLE_SYSTEM_BUTTONMENU; c.action = L"Open"; c.rc = s_overflow;
        } else if (h == kHitHistory) {
            c.name = L"Recent locations"; c.role = ROLE_SYSTEM_BUTTONMENU; c.action = L"Open"; c.rc = s_history;
        } else if (h % 2 == 0) {
            int i = h / 2;
            c.name = L"Folders in " + (i > 0 ? s_crumbs[i - 1].name : std::wstring(L"Desktop"));
            c.role = ROLE_SYSTEM_BUTTONMENU; c.action = L"Open"; c.rc = s_crumbs[i].chevron;
        } else {
            int i = h / 2;
            c.name = s_crumbs[i].name;
            c.role = ROLE_SYSTEM_PUSHBUTTON; c.action = L"Open"; c.rc = s_crumbs[i].text;
        }
        if (s_hover == h) c.state |= STATE_SYSTEM_HOTTRACKED;
        return c;
    };
    a.action = [](int n) { PostMessageW(s_box, kAccAction, (WPARAM)acc_items()[n].hit, 0); };
    return a;
}();

static LRESULT CALLBACK box_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_GETOBJECT: return acc_get_object(hwnd, wp, lp, &s_acc);
        case WM_SETFOCUS:
            // reached with Tab / F6: type a path, like Explorer's address bar (but focus
            // falling back from the edit as it hides goes on to the list)
            if ((HWND)wp == s_edit) { if (g_pane->list) SetFocus(g_pane->list); }
            else address_begin_edit();
            return 0;
        case kAccAction: {
            int h = (int)wp;
            if (h == kHitHistory) show_history_menu();
            else if (h == kHitOverflow) show_overflow_menu();
            else if (h >= 0 && h / 2 < (int)s_crumbs.size()) {
                if (h % 2 == 0) show_chevron_menu(h / 2);
                else if (h / 2 == (int)s_crumbs.size() - 1) navigation_refresh();
                else navigate_to(s_crumbs[h / 2].abs);
            }
            return 0;
        }
        case WM_SETTEXT:
            // callers set the address text after navigating: resync the crumbs
            DefWindowProcW(hwnd, msg, wp, lp);
            rebuild_crumbs();
            layout_crumbs();
            if (s_edit && GetFocus() != s_edit) SetWindowTextW(s_edit, (LPCWSTR)lp);
            InvalidateRect(hwnd, nullptr, FALSE);
            acc_notify(hwnd, EVENT_OBJECT_VALUECHANGE, -1);
            acc_notify(hwnd, EVENT_OBJECT_REORDER, -1);
            return TRUE;
        case WM_SIZE:
            layout_crumbs();
            if (address_editing()) place_edit();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETFONT:
            if (s_edit) SendMessageW(s_edit, WM_SETFONT, wp, lp);
            layout_crumbs();
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(hwnd); return 0;
        case WM_CTLCOLOREDIT: {
            const Palette& p = pal();
            if (s_editBrushColor != p.boxFill) {
                if (s_editBrush) DeleteObject(s_editBrush);
                s_editBrush = CreateSolidBrush(p.boxFill);
                s_editBrushColor = p.boxFill;
            }
            SetTextColor((HDC)wp, p.text);
            SetBkColor((HDC)wp, p.boxFill);
            return (LRESULT)s_editBrush;
        }
        case WM_COMMAND:
            if ((HWND)lp == s_edit && (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS))
                InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            int h = hit({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (h != s_hover) { s_hover = h; InvalidateRect(hwnd, nullptr, FALSE); }
            TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&te);
            return 0;
        }
        case WM_MOUSELEAVE:
            s_hover = kHitNone;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CAPTURECHANGED:         // a crumb's press let go elsewhere (or taken away)
            if (s_pressed != -100) { s_pressed = -100; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        case WM_LBUTTONDOWN: {
            int h = hit({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (address_editing() && h != kHitHistory) return 0;
            s_pressed = h;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (h == kHitHistory) { show_history_menu(); }
            else if (h == kHitOverflow) { show_overflow_menu(); }
            else if (h >= 0 && h % 2 == 0) { show_chevron_menu(h / 2); }
            else if (h == kHitNone) { s_pressed = -100; address_begin_edit(); return 0; }
            else { SetCapture(hwnd); return 0; }   // crumb text: navigate on button-up (wherever it is)
            s_pressed = -100;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            int h = hit({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            int pressed = s_pressed;
            s_pressed = -100;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (h >= 0 && h % 2 == 1 && h == pressed) {
                int i = h / 2;
                if (i == (int)s_crumbs.size() - 1) navigation_refresh();
                else navigate_to(s_crumbs[i].abs);
            }
            return 0;
        }
        case WM_MEASUREITEM: {
            auto* mi = (MEASUREITEMSTRUCT*)lp;
            if (mi->CtlType != ODT_MENU) break;
            int w = 0, h = 0;
            ImageList_GetIconSize(sys_imagelist(SHIL_SMALL), &w, &h);   // what is drawn
            mi->itemWidth = w + S(4);
            mi->itemHeight = h;
            return TRUE;
        }
        case WM_DRAWITEM: {
            auto* di = (DRAWITEMSTRUCT*)lp;
            if (di->CtlType != ODT_MENU) break;
            UINT id = di->itemID;
            if (id >= 1 && id <= s_menuEntries.size() && s_menuEntries[id - 1].icon >= 0) {
                int w = 0, sz = 0;
                ImageList_GetIconSize(sys_imagelist(SHIL_SMALL), &w, &sz);
                ImageList_Draw(sys_imagelist(SHIL_SMALL), s_menuEntries[id - 1].icon, di->hDC,
                               di->rcItem.left, di->rcItem.top + (di->rcItem.bottom - di->rcItem.top - sz) / 2,
                               ILD_TRANSPARENT);
            }
            return TRUE;
        }
        case WM_DESTROY:
            free_crumbs();
            if (s_editBrush) { DeleteObject(s_editBrush); s_editBrush = nullptr; }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND address_create(HWND parent) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = g_hinst;
    wc.lpfnWndProc = box_proc;
    wc.lpszClassName = L"ExplorerPlusAddress";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    s_box = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_TABSTOP,
                            0, 0, 300, S(32), parent, (HMENU)IDC_ADDRESS, g_hinst, nullptr);
    s_edit = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | ES_AUTOHSCROLL,
                             0, 0, 100, 20, s_box, nullptr, g_hinst, nullptr);
    SendMessageW(s_edit, WM_SETFONT, (WPARAM)g_font, TRUE);
    s_editProc = (WNDPROC)SetWindowLongPtrW(s_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
    g_address = s_box;
    return s_box;
}
