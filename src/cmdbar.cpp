#include "cmdbar.h"
#include <algorithm>
using std::max;
using std::min;
#include "theme.h"
#include "fileops.h"
#include "contextmenu.h"
#include "detailspane.h"
#include "groups.h"
#include "columns.h"
#include "flyout.h"
#include "navbar.h"
#include "acc.h"
#include <vector>

enum class Kind { Button, Separator, Spacer };
struct CmdItem {
    Kind kind;
    int cmd = 0;
    wchar_t glyph = 0;
    const wchar_t* label = nullptr;    // nullptr = icon-only button
    bool dropdown = false;
    const wchar_t* tip = nullptr;
};

static const CmdItem kItems[] = {
    { Kind::Button, IDM_NEW_MENU,     L'\xE710', L"New",  true,  L"Create a new item in the current location." },
    { Kind::Separator },
    { Kind::Button, IDM_EDIT_CUT,     L'\xE8C6', nullptr, false, L"Cut (Ctrl+X)" },
    { Kind::Button, IDM_EDIT_COPY,    L'\xE8C8', nullptr, false, L"Copy (Ctrl+C)" },
    { Kind::Button, IDM_EDIT_PASTE,   L'\xE77F', nullptr, false, L"Paste (Ctrl+V)" },
    { Kind::Button, IDM_EDIT_RENAME,  L'\xE8AC', nullptr, false, L"Rename (F2)" },
    { Kind::Button, IDM_SHARE,        L'\xE72D', nullptr, false, L"Share" },
    { Kind::Button, IDM_EDIT_DELETE,  L'\xE74D', nullptr, false, L"Delete (Del)" },
    { Kind::Separator },
    { Kind::Button, IDM_SORT_MENU,    L'\xE8CB', L"Sort", true,  L"Sort and group options" },
    { Kind::Button, IDM_VIEW_MENU,    L'\xE8FD', L"View", true,  L"Layout and view options" },
    { Kind::Separator },
    { Kind::Button, IDM_MORE_MENU,    L'\xE712', nullptr, false, L"See more" },
    { Kind::Spacer },
    { Kind::Button, IDM_VIEW_DETAILSPANE, L'\xE946', L"Details", false, L"Show the details pane (Alt+Shift+P)" },
};
constexpr int kCount = (int)(sizeof(kItems) / sizeof(kItems[0]));

static HWND s_bar = nullptr, s_tip = nullptr;
static int s_hover = -1, s_pressed = -1;
static std::vector<RECT> s_rects;

HWND cmdbar_hwnd() { return s_bar; }


static void layout(HWND hwnd) {
    RECT rc{}; GetClientRect(hwnd, &rc);
    s_rects.assign(kCount, RECT{});
    HDC dc = GetDC(hwnd);
    HGDIOBJ of = SelectObject(dc, g_font);
    int h = S(32), y = (rc.bottom - h) / 2;
    std::vector<int> widths(kCount, 0);
    int spacer = -1, total = 0;
    for (int i = 0; i < kCount; ++i) {
        const CmdItem& it = kItems[i];
        if (it.kind == Kind::Separator) widths[i] = S(17);
        else if (it.kind == Kind::Spacer) spacer = i;
        else if (!it.label) widths[i] = S(40);
        else widths[i] = S(12) + S(16) + S(8) + text_width(dc, it.label) + (it.dropdown ? S(18) : 0) + S(12);
        total += widths[i];
    }
    SelectObject(dc, of);
    ReleaseDC(hwnd, dc);
    if (spacer >= 0) widths[spacer] = max(0, (int)rc.right - S(16) - total);
    int x = S(8);
    for (int i = 0; i < kCount; ++i) {
        s_rects[i] = { x, y, x + widths[i], y + h };
        x += widths[i];
    }
    if (s_tip) {
        for (int i = 0; i < kCount; ++i) {
            TTTOOLINFOW ti{ sizeof(ti) };
            ti.hwnd = hwnd; ti.uId = (UINT_PTR)i; ti.rect = s_rects[i];
            SendMessageW(s_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        }
    }
}

static bool enabled(int cmd) {
    int sel = g_list ? ListView_GetSelectedCount(g_list) : 0;
    switch (cmd) {
        case IDM_EDIT_CUT: case IDM_EDIT_COPY: case IDM_EDIT_DELETE:
        case IDM_EDIT_RENAME: case IDM_SHARE:
            return sel > 0;
        case IDM_EDIT_PASTE:
            return clipboard_has_files();
        default:
            return true;
    }
}

static bool toggled(int cmd) {
    return cmd == IDM_VIEW_DETAILSPANE && detailspane_visible() && detailspane_mode() == PANE_DETAILS;
}

static int hit(POINT pt) {
    for (int i = 0; i < (int)s_rects.size(); ++i)
        if (kItems[i].kind == Kind::Button && PtInRect(&s_rects[i], pt)) return i;
    return -1;
}

static void paint(HWND hwnd) {
    BufferedPaint bp(hwnd);
    const Palette& p = pal();
    fill_rect(bp.dc, bp.rc, p.layer);
    RECT line{ 0, bp.rc.bottom - 1, bp.rc.right, bp.rc.bottom };
    fill_rect(bp.dc, line, p.divider);
    for (int i = 0; i < kCount && i < (int)s_rects.size(); ++i) {
        const CmdItem& it = kItems[i];
        RECT r = s_rects[i];
        if (it.kind == Kind::Separator) {
            int x = (r.left + r.right) / 2;
            RECT s{ x, r.top + S(6), x + 1, r.bottom - S(6) };
            fill_rect(bp.dc, s, p.divider);
            continue;
        }
        if (it.kind != Kind::Button) continue;
        bool en = enabled(it.cmd);
        if (en && (s_pressed == i || toggled(it.cmd))) fill_rrect(bp.dc, r, S(4), p.pressed);
        else if (en && s_hover == i) fill_rrect(bp.dc, r, S(4), p.hover);
        COLORREF fg = en ? p.text : p.textDisabled;
        if (!it.label) {
            draw_glyph(bp.dc, it.glyph, r, fg, 16);
            continue;
        }
        RECT g{ r.left + S(12), r.top, r.left + S(12) + S(16), r.bottom };
        // the New button's plus is drawn in the accent color, as in Explorer
        draw_glyph(bp.dc, it.glyph, g, it.cmd == IDM_NEW_MENU && en ? p.accent : fg, 16);
        RECT t{ g.right + S(8), r.top, r.right - (it.dropdown ? S(18) : 0) - S(8), r.bottom };
        SetTextColor(bp.dc, fg);
        DrawTextW(bp.dc, it.label, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (it.dropdown) {
            RECT c{ r.right - S(12) - S(16), r.top, r.right - S(8), r.bottom };
            draw_glyph(bp.dc, L'\xE70D', c, fg, 10);                 // chevron down
        }
    }
}

// ------------------------------------------------------------------
// drop-down menus

static UINT menu_flag(bool checked) { return checked ? MF_CHECKED : 0u; }

static void append_radio(HMENU m, UINT id, const wchar_t* label, bool checked) {
    AppendMenuW(m, MF_STRING | menu_flag(checked), id, label);
    MENUITEMINFOW mi{ sizeof(mi) };
    mi.fMask = MIIM_FTYPE;
    mi.fType = MFT_STRING | MFT_RADIOCHECK;
    SetMenuItemInfoW(m, id, FALSE, &mi);
}

HMENU menu_group_by() {
    HMENU group = CreatePopupMenu();
    int g = groups_effective();          // GB_AUTO: the folder's own grouping (Home, This PC)
    static const struct { int by; const wchar_t* label; } kGroups[] = {
        { GB_NAME, L"Name" }, { GB_DATE, L"Date modified" }, { GB_TYPE, L"Type" },
        { GB_SIZE, L"Size" }, { GB_NONE, L"(None)" },
    };
    for (auto& k : kGroups) {
        if (k.by == GB_NONE) AppendMenuW(group, MF_SEPARATOR, 0, nullptr);
        append_radio(group, IDM_GROUP_BASE + k.by, k.label, g == k.by);
    }
    return group;
}

// like Explorer: the folder's own columns (Recycle Bin sorts by Date deleted, ...)
HMENU menu_sort_by() {
    HMENU m = CreatePopupMenu();
    int sorted = columns_sort_display_index();
    for (int i = 0; i < (int)g_columns.size() && IDM_SORT_COL_BASE + i <= IDM_SORT_COL_LAST; ++i)
        append_radio(m, IDM_SORT_COL_BASE + i, g_columns[i].title.c_str(), i == sorted);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    append_radio(m, IDM_SORT_ASC, L"Ascending", g_sortAsc);
    append_radio(m, IDM_SORT_DESC, L"Descending", !g_sortAsc);
    return m;
}

HMENU menu_view_modes() {
    HMENU m = CreatePopupMenu();
    const View& v = g_view;
    bool icons = v.lvView == LV_VIEW_ICON;
    append_radio(m, IDM_VIEW_XLARGE,  L"Extra large icons\tCtrl+Shift+1", icons && v.thumbsExtra == 2);
    append_radio(m, IDM_VIEW_LARGE,   L"Large icons\tCtrl+Shift+2",       icons && v.thumbsExtra == 1);
    append_radio(m, IDM_VIEW_MEDIUM,  L"Medium icons\tCtrl+Shift+3",      icons && (v.thumbsExtra == 3 || v.thumbsExtra == 0));
    append_radio(m, IDM_VIEW_SMALL,   L"Small icons\tCtrl+Shift+4",       v.lvView == LV_VIEW_SMALLICON);
    append_radio(m, IDM_VIEW_LIST,    L"List\tCtrl+Shift+5",              v.lvView == LV_VIEW_LIST);
    append_radio(m, IDM_VIEW_DETAILS, L"Details\tCtrl+Shift+6",           v.lvView == LV_VIEW_DETAILS);
    append_radio(m, IDM_VIEW_TILES,   L"Tiles\tCtrl+Shift+7",             v.lvView == LV_VIEW_TILE);
    return m;
}

static HMENU build_sort_menu() {
    HMENU m = menu_sort_by();
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)menu_group_by(), L"Group by");
    return m;
}

static HMENU build_text_size_menu() {
    HMENU m = CreatePopupMenu();
    int pct = text_scale();
    AppendMenuW(m, MF_STRING | (text_scale_step(pct, +1) == pct ? MF_GRAYED : 0), IDM_TEXT_LARGER, L"Larger\tCtrl++");
    AppendMenuW(m, MF_STRING | (text_scale_step(pct, -1) == pct ? MF_GRAYED : 0), IDM_TEXT_SMALLER, L"Smaller\tCtrl+-");
    std::wstring reset = L"Reset (" + std::to_wstring(pct) + L"%)\tCtrl+0";
    AppendMenuW(m, MF_STRING | (pct == 100 ? MF_GRAYED : 0), IDM_TEXT_RESET, reset.c_str());
    return m;
}

static HMENU build_view_menu() {
    HMENU m = menu_view_modes();
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | menu_flag(g_compactView), IDM_VIEW_COMPACT, L"Compact view");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)build_text_size_menu(), L"Text size");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (columns_can_apply_to_all() ? 0 : MF_GRAYED), IDM_VIEW_APPLY_ALL,
                L"Apply this view to all folders");
    AppendMenuW(m, MF_STRING, IDM_VIEW_RESET_ALL, L"Reset all folders");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU show = CreatePopupMenu();
    AppendMenuW(show, MF_STRING | menu_flag(g_cmdBarVisible), IDM_VIEW_CMDBAR, L"Command bar");
    AppendMenuW(show, MF_STRING | menu_flag(g_treeVisible), IDM_VIEW_NAVPANE, L"Navigation pane");
    bool pane = detailspane_visible();
    AppendMenuW(show, MF_STRING | menu_flag(pane && detailspane_mode() == PANE_DETAILS), IDM_VIEW_DETAILSPANE, L"Details pane\tAlt+Shift+P");
    AppendMenuW(show, MF_STRING | menu_flag(pane && detailspane_mode() == PANE_PREVIEW), IDM_VIEW_PREVIEWPANE, L"Preview pane\tAlt+P");
    AppendMenuW(show, MF_STRING | menu_flag(g_checkBoxes), IDM_VIEW_CHECKBOXES, L"Item check boxes");
    AppendMenuW(show, MF_STRING | menu_flag(show_extensions_effective()), IDM_VIEW_EXTENSIONS, L"File name extensions");
    AppendMenuW(show, MF_STRING | menu_flag(g_showHidden), IDM_VIEW_HIDDEN, L"Hidden items");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)show, L"Show");
    return m;
}

static HMENU build_more_menu() {
    HMENU m = CreatePopupMenu();
    bool sel = g_list && ListView_GetSelectedCount(g_list) > 0;
    AppendMenuW(m, MF_STRING, IDM_EDIT_SELALL, L"Select all\tCtrl+A");
    AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_EDIT_SELNONE, L"Select none");
    AppendMenuW(m, MF_STRING, IDM_EDIT_INVERT, L"Invert selection");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_COPY_PATH, L"Copy path\tCtrl+Shift+C");
    AppendMenuW(m, MF_STRING, IDM_FILE_PROPERTIES, L"Properties\tAlt+Enter");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_NEW_WINDOW, L"New window\tCtrl+N");
    AppendMenuW(m, MF_STRING, IDM_OPTIONS, L"Options");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_VIEW_CMDBAR, L"Hide command bar");   // back into the nav row's "..."
    return m;
}

void cmdbar_show_menu(int menuCmd) {
    int idx = -1;
    for (int i = 0; i < kCount; ++i) if (kItems[i].cmd == menuCmd) idx = i;
    POINT pt{ 0, 0 };
    UINT align = TPM_LEFTALIGN | TPM_TOPALIGN;
    if (!g_cmdBarVisible) {
        pt = navbar_menu_point();           // the bar is folded into the nav row's "..."
        idx = -1;
    } else if (idx >= 0 && idx < (int)s_rects.size()) {
        pt = { s_rects[idx].left, s_rects[idx].bottom + S(2) };
        ClientToScreen(s_bar, &pt);
    } else {
        GetCursorPos(&pt);
    }
    s_pressed = idx;
    InvalidateRect(s_bar, nullptr, FALSE);
    if (menuCmd == IDM_NEW_MENU) {
        show_new_menu(g_main, pt, align);
    } else {
        HMENU m = menuCmd == IDM_SORT_MENU ? build_sort_menu()
                : menuCmd == IDM_VIEW_MENU ? build_view_menu() : build_more_menu();
        UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTBUTTON | align, pt.x, pt.y, g_main, nullptr);
        DestroyMenu(m);
        if (id) SendMessageW(g_main, WM_COMMAND, MAKEWPARAM(id, 0), 0);
    }
    s_pressed = -1;
    InvalidateRect(s_bar, nullptr, FALSE);
}

// The nav row's "..." while the command bar is hidden: the bar's buttons as the
// flyout's icon row, its menus and toggles as entries.
void cmdbar_show_tools() {
    POINT pt = navbar_menu_point();
    std::vector<FlyButton> row;
    for (const CmdItem& it : kItems)
        if (it.kind == Kind::Button && !it.label && it.cmd != IDM_MORE_MENU)
            row.push_back({ (UINT)it.cmd, it.glyph, it.tip, !enabled(it.cmd) });
    HMENU sort = build_sort_menu(), view = build_view_menu();
    bool pane = detailspane_visible();
    bool sel = g_list && ListView_GetSelectedCount(g_list) > 0;
    std::vector<FlyEntry> list;
    auto item = [&](UINT id, const wchar_t* text, wchar_t glyph, const wchar_t* accel = L"",
                    bool checked = false, bool disabled = false) {
        FlyEntry e;
        e.id = id; e.text = text; e.glyph = glyph; e.accel = accel;
        e.checked = checked; e.disabled = disabled;
        list.push_back(e);
    };
    auto sub = [&](HMENU h, const wchar_t* text, wchar_t glyph) {
        FlyEntry e;
        e.kind = FlyEntry::Submenu; e.sub = h; e.text = text; e.glyph = glyph;
        list.push_back(e);
    };
    auto sep = [&] { FlyEntry e; e.kind = FlyEntry::Separator; list.push_back(e); };
    item(IDM_NEW_MENU, L"&New", L'\xE710');
    item(IDM_FILE_NEWFOLDER, L"New &folder", L'\xE8F4', L"Ctrl+Shift+N");
    sep();
    sub(sort, L"&Sort", L'\xE8CB');
    sub(view, L"&View", L'\xE8FD');
    item(IDM_VIEW_DETAILSPANE, L"&Details pane", L'\xE946', L"Alt+Shift+P", pane && detailspane_mode() == PANE_DETAILS);
    item(IDM_VIEW_PREVIEWPANE, L"Pre&view pane", L'\xE8FF', L"Alt+P", pane && detailspane_mode() == PANE_PREVIEW);
    sep();
    item(IDM_EDIT_SELALL, L"Select &all", L'\xE8B3', L"Ctrl+A");
    item(IDM_EDIT_SELNONE, L"Select n&one", L'\xE8E6', L"", false, !sel);
    item(IDM_EDIT_INVERT, L"&Invert selection", 0);
    sep();
    item(IDM_COPY_PATH, L"Copy &path", L'\xE71B', L"Ctrl+Shift+C");
    item(IDM_FILE_PROPERTIES, L"P&roperties", L'\xE90F', L"Alt+Enter");
    sep();
    item(IDM_NEW_WINDOW, L"New &window", L'\xE8A7', L"Ctrl+N");
    item(IDM_OPTIONS, L"Op&tions", L'\xE713');
    item(IDM_VIEW_CMDBAR, L"Show command &bar", 0, L"", g_cmdBarVisible);
    UINT id = flyout_track(g_main, pt, row, list);
    DestroyMenu(sort);
    DestroyMenu(view);
    if (id == IDM_NEW_MENU) show_new_menu(g_main, pt, TPM_LEFTALIGN | TPM_TOPALIGN);
    else if (id) SendMessageW(g_main, WM_COMMAND, MAKEWPARAM(id, 0), 0);
}

// ------------------------------------------------------------------
// screen readers: the buttons (not separators / the spacer), named by their label or the
// tip's text before "(shortcut)"

static std::vector<int> acc_buttons() {
    std::vector<int> v;
    for (int i = 0; i < kCount; ++i)
        if (kItems[i].kind == Kind::Button) v.push_back(i);
    return v;
}
static const AccSource s_acc = [] {
    AccSource a;
    a.name = L"Commands";
    a.role = ROLE_SYSTEM_TOOLBAR;
    a.count = [] { return (int)acc_buttons().size(); };
    a.child = [](int n) {
        int i = acc_buttons()[n];
        const CmdItem& it = kItems[i];
        AccChild c;
        std::wstring tip = it.tip ? it.tip : L"";
        size_t paren = tip.find(L" (");
        c.name = it.label ? it.label : tip.substr(0, paren);
        if (paren != std::wstring::npos && tip.back() == L')')
            c.shortcut = tip.substr(paren + 2, tip.size() - paren - 3);
        bool menu = it.dropdown || it.cmd == IDM_MORE_MENU;
        c.role = menu ? ROLE_SYSTEM_BUTTONMENU : ROLE_SYSTEM_PUSHBUTTON;
        c.action = menu ? L"Open" : L"Press";
        if (i < (int)s_rects.size()) c.rc = s_rects[i];
        if (!enabled(it.cmd)) c.state |= STATE_SYSTEM_UNAVAILABLE;
        if (toggled(it.cmd)) c.state |= STATE_SYSTEM_CHECKED;
        if (s_hover == i) c.state |= STATE_SYSTEM_HOTTRACKED;
        return c;
    };
    // (posted: a menu opened from inside the screen reader's call would block it)
    a.action = [](int n) { PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(kItems[acc_buttons()[n]].cmd, 0), 0); };
    return a;
}();

static LRESULT CALLBACK bar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_GETOBJECT: return acc_get_object(hwnd, wp, lp, &s_acc);
        case WM_SIZE: layout(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_SETFONT: layout(hwnd); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(hwnd); return 0;
        case WM_MOUSEMOVE: {
            int h = hit({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (h != s_hover) { s_hover = h; InvalidateRect(hwnd, nullptr, FALSE); }
            TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&te);
            return 0;
        }
        case WM_MOUSELEAVE:
            s_hover = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            int h = hit({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (h < 0 || !enabled(kItems[h].cmd)) return 0;
            if (kItems[h].dropdown || kItems[h].cmd == IDM_MORE_MENU) {
                cmdbar_show_menu(kItems[h].cmd);       // menus open on press, like Explorer
                return 0;
            }
            s_pressed = h;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            int h = hit({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            int pressed = s_pressed;
            s_pressed = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (h >= 0 && h == pressed && enabled(kItems[h].cmd))
                SendMessageW(g_main, WM_COMMAND, MAKEWPARAM(kItems[h].cmd, 0), 0);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (s_pressed >= 0) { s_pressed = -1; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        case WM_CONTEXTMENU: {
            // right-click on the bar: hide it again
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x == -1 && pt.y == -1) { pt = { S(8), S(8) }; ClientToScreen(hwnd, &pt); }
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, IDM_VIEW_CMDBAR, L"Hide command bar");
            UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, g_main, nullptr);
            DestroyMenu(m);
            if (id) SendMessageW(g_main, WM_COMMAND, MAKEWPARAM(id, 0), 0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND cmdbar_create(HWND parent) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = g_hinst;
    wc.lpfnWndProc = bar_proc;
    wc.lpszClassName = L"ExplorerPlusCmdBar";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc);
    s_bar = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
                            0, 0, 100, S(48), parent, (HMENU)IDC_CMDBAR, g_hinst, nullptr);
    s_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            0, 0, 0, 0, s_bar, nullptr, g_hinst, nullptr);
    for (int i = 0; s_tip && i < kCount; ++i) {
        if (kItems[i].kind != Kind::Button) continue;
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = s_bar;
        ti.uId = (UINT_PTR)i;
        ti.lpszText = const_cast<LPWSTR>(kItems[i].tip);
        SendMessageW(s_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    layout(s_bar);
    return s_bar;
}

void cmdbar_refresh() {
    if (!s_bar) return;
    InvalidateRect(s_bar, nullptr, FALSE);
    acc_notify(s_bar, EVENT_OBJECT_STATECHANGE, -1);    // what can be used follows the selection
}
