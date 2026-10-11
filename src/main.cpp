// Explorer Plus — main window: Win11 frame with tabs in the caption, navigation
// row (buttons, breadcrumb, search), command bar, tree / list / details, status bar.
#include <algorithm>
#include <cstdlib>
using std::max;
using std::min;
#include "appstate.h"
#include "enumerate.h"
#include "groups.h"
#include "tree.h"
#include "crash.h"
#include "navigation.h"
#include "listview.h"
#include "columns.h"
#include "layout.h"
#include "settings.h"
#include "watcher.h"
#include "thumbs.h"
#include "dnd.h"
#include "contextmenu.h"
#include "searchbox.h"
#include "fileops.h"
#include "tabs.h"
#include "detailspane.h"
#include "navbar.h"
#include "theme.h"
#include "chrome.h"
#include "cmdbar.h"
#include "perf.h"
#include "workers.h"
#include "statusbar.h"
#include "breadcrumb.h"
#include "foldersizes.h"
#include <uxtheme.h>

static WNDPROC s_searchProc = nullptr;
static HBRUSH s_searchBrush = nullptr;
static COLORREF s_searchBrushColor = CLR_INVALID;
static DragKind s_dragKind = DragKind::None;

void exit_app() { DestroyWindow(g_main); }

// ------------------------------------------------------------------
// view mode plumbing (model lives in TabState::view)
void set_view_mode(const View& v, bool persist) {
    PerfScope ps("view.apply");
    bool same = (g_view.lvView == v.lvView && g_view.thumbsExtra == v.thumbsExtra);
    bool thumbSizeChanged = g_view.thumbsExtra != v.thumbsExtra;
    bool regroup = g_view.groupBy != v.groupBy;
    g_view = v;
    TabState* t = tabmodel_active();
    if (t) t->view = v;
    // remembered for this folder; other folders change only with "Apply to all folders"
    if (persist) settings_set_view_for(folder_settings_key(g_current), v);
    statusbar_refresh();
    // (navigation re-groups when its listing arrives; only a live change re-sorts here)
    if (regroup && persist) enum_sort_items();
    if (same) return;
    if (thumbSizeChanged) thumb_reset_for_new_folder();   // cached thumbs are per size
    list_apply_view();
    InvalidateRect(g_list, nullptr, TRUE);
}

// ------------------------------------------------------------------
// status bar
void status_update() {
    if (!g_status || !g_list) return;
    int count = 0;
    ULONGLONG selBytes = 0;
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        count = (int)g_items.size();
        int idx = -1;
        while ((idx = ListView_GetNextItem(g_list, idx, LVNI_SELECTED)) != -1)
            if (idx < (int)g_items.size() && !g_items[idx]->isFolder)
                selBytes += g_items[idx]->bytes;
    }
    int sel = ListView_GetSelectedCount(g_list);
    std::wstring a = std::to_wstring(count) + (count == 1 ? L" item" : L" items");
    std::wstring b;
    if (sel) {
        b = std::to_wstring(sel) + (sel == 1 ? L" item selected" : L" items selected");
        if (selBytes) b += L"  " + fmt_size(selBytes);
    }
    const std::wstring& err = enum_last_error();
    if (!err.empty()) { a = L"Can't open this location"; b = err; }

    statusbar_set(a, b);
    cmdbar_refresh();
}

// ------------------------------------------------------------------
// search box (borderless edit inside a painted Win11 frame)
static void search_frame_invalidate() {
    RECT r = layout_search_frame();
    InvalidateRect(g_main, &r, FALSE);
}

static LRESULT CALLBACK search_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) { search_submit(); return 0; }
        if (wp == VK_ESCAPE) {
            if (search_is_results(g_current)) search_cancel();     // back to the searched folder
            else SetWindowTextW(h, L"");
            SetFocus(g_list);
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
    return CallWindowProcW(s_searchProc, h, msg, wp, lp);
}

static void build_search() {
    g_search = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                               0, 0, 100, 20, g_main, (HMENU)IDC_SEARCH, g_hinst, nullptr);
    fix_font(g_search);
    s_searchProc = (WNDPROC)SetWindowLongPtrW(g_search, GWLP_WNDPROC, (LONG_PTR)search_proc);
}

// "Search <folder>" placeholder, like Explorer
void search_update_cue() {
    if (!g_search) return;
    TabState* t = tabmodel_active();
    PCIDLIST_ABSOLUTE scope = search_scope_of(g_current);
    std::wstring cue = L"Search " + (scope ? pidl_abs_dispname(scope) : t ? t->name : std::wstring());
    SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)cue.c_str());
}

// ------------------------------------------------------------------
// commands that need a little glue

// selected items' paths (or the folder's), quoted, one per line -> clipboard text
static void copy_path_to_clipboard() {
    std::wstring text;
    auto sel = list_selected_pidls();
    auto add = [&](PCIDLIST_ABSOLUTE abs) {
        std::wstring s = pidl_folder_path(abs);
        if (s.empty()) return;
        if (!text.empty()) text += L"\r\n";
        text += L"\"" + s + L"\"";
    };
    if (sel.empty()) add(g_current);
    for (auto c : sel) {
        PIDLIST_ABSOLUTE abs = ILCombine(g_current, c);
        if (abs) { add(abs); CoTaskMemFree(abs); }
    }
    if (text.empty() || !OpenClipboard(g_main)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(WCHAR);
    if (HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(hg), text.c_str(), bytes);
        GlobalUnlock(hg);
        if (!SetClipboardData(CF_UNICODETEXT, hg)) GlobalFree(hg);
    }
    CloseClipboard();
}

static void set_nav_pane_visible(bool v) {
    g_treeVisible = v;
    ShowWindow(g_tree, v ? SW_SHOWNA : SW_HIDE);
    layout_relayout();
    if (v) tree_sync_to_current();      // (not followed while hidden)
}

// text size of the navigation pane, the list and the details pane (remembered)
static void set_text_scale(int pct) {
    theme_set_text_scale(pct);
    settings_set_pref(L"textSize", text_scale());
    tree_apply_metrics();
    list_apply_view();                   // row spacing, tile size
    columns_rescale();
    InvalidateRect(g_tree, nullptr, TRUE);
    InvalidateRect(g_list, nullptr, TRUE);
    if (HWND pane = detailspane_hwnd()) InvalidateRect(pane, nullptr, FALSE);
}

// ------------------------------------------------------------------
// accelerator table
static HACCEL make_accels() {
    ACCEL a[] = {
        { FVIRTKEY | FALT,               VK_LEFT,   IDM_NAV_BACK },
        { FVIRTKEY | FALT,               VK_RIGHT,  IDM_NAV_FORWARD },
        { FVIRTKEY | FALT,               VK_UP,     IDM_NAV_UP },
        { FVIRTKEY,                      VK_BROWSER_BACK,    IDM_NAV_BACK },
        { FVIRTKEY,                      VK_BROWSER_FORWARD, IDM_NAV_FORWARD },
        { FVIRTKEY,                      VK_F5,     IDM_NAV_REFRESH },
        { FVIRTKEY | FCONTROL,           'R',       IDM_NAV_REFRESH },
        { FVIRTKEY,                      VK_F2,     IDM_EDIT_RENAME },
        { FVIRTKEY | FCONTROL,           'A',       IDM_EDIT_SELALL },
        { FVIRTKEY | FCONTROL,           'X',       IDM_EDIT_CUT },
        { FVIRTKEY | FCONTROL,           'C',       IDM_EDIT_COPY },
        { FVIRTKEY | FCONTROL,           'V',       IDM_EDIT_PASTE },
        { FVIRTKEY | FCONTROL | FSHIFT,  'N',       IDM_FILE_NEWFOLDER },
        { FVIRTKEY | FALT,               VK_RETURN, IDM_FILE_PROPERTIES },
        { FVIRTKEY | FCONTROL,           'T',       IDM_FILE_NEWTAB },
        { FVIRTKEY | FCONTROL,           'W',       IDM_FILE_CLOSETAB },
        { FVIRTKEY | FCONTROL,           VK_TAB,    IDM_TABNEXT },
        { FVIRTKEY | FCONTROL | FSHIFT,  VK_TAB,    IDM_TABPREV },
        { FVIRTKEY | FCONTROL,           VK_NEXT,   IDM_TABNEXT },
        { FVIRTKEY | FCONTROL,           VK_PRIOR,  IDM_TABPREV },
        { FVIRTKEY | FCONTROL,           'L',       IDM_FOCUS_ADDRESS },
        { FVIRTKEY | FALT,               'D',       IDM_FOCUS_ADDRESS },
        { FVIRTKEY,                      VK_F4,     IDM_FOCUS_ADDRESS },
        { FVIRTKEY | FCONTROL,           'E',       IDM_FOCUS_SEARCH },
        { FVIRTKEY | FCONTROL,           'F',       IDM_FOCUS_SEARCH },
        { FVIRTKEY,                      VK_F3,     IDM_FOCUS_SEARCH },
        { FVIRTKEY | FCONTROL,           'N',       IDM_NEW_WINDOW },
        { FVIRTKEY | FCONTROL | FSHIFT,  'C',       IDM_COPY_PATH },
        { FVIRTKEY | FALT | FSHIFT,      'P',       IDM_VIEW_DETAILSPANE },
        { FVIRTKEY | FALT,               'P',       IDM_VIEW_PREVIEWPANE },
        // Explorer's view shortcuts
        { FVIRTKEY | FCONTROL | FSHIFT,  '1',       IDM_VIEW_XLARGE },
        { FVIRTKEY | FCONTROL | FSHIFT,  '2',       IDM_VIEW_LARGE },
        { FVIRTKEY | FCONTROL | FSHIFT,  '3',       IDM_VIEW_MEDIUM },
        { FVIRTKEY | FCONTROL | FSHIFT,  '4',       IDM_VIEW_SMALL },
        { FVIRTKEY | FCONTROL | FSHIFT,  '5',       IDM_VIEW_LIST },
        { FVIRTKEY | FCONTROL | FSHIFT,  '6',       IDM_VIEW_DETAILS },
        { FVIRTKEY | FCONTROL | FSHIFT,  '7',       IDM_VIEW_TILES },
        { FVIRTKEY | FCONTROL,           '1',       IDM_TABSEL1 },
        { FVIRTKEY | FCONTROL,           '2',       IDM_TABSEL2 },
        { FVIRTKEY | FCONTROL,           '3',       IDM_TABSEL3 },
        { FVIRTKEY | FCONTROL,           '4',       IDM_TABSEL4 },
        { FVIRTKEY | FCONTROL,           '5',       IDM_TABSEL5 },
        { FVIRTKEY | FCONTROL,           '6',       IDM_TABSEL6 },
        { FVIRTKEY | FCONTROL,           '7',       IDM_TABSEL7 },
        { FVIRTKEY | FCONTROL,           '8',       IDM_TABSEL8 },
        { FVIRTKEY | FCONTROL,           '9',       IDM_TABSEL9 },
        // text size, like a browser's zoom
        { FVIRTKEY | FCONTROL,           VK_OEM_PLUS,  IDM_TEXT_LARGER },
        { FVIRTKEY | FCONTROL | FSHIFT,  VK_OEM_PLUS,  IDM_TEXT_LARGER },     // Ctrl++ on US layouts
        { FVIRTKEY | FCONTROL,           VK_ADD,       IDM_TEXT_LARGER },
        { FVIRTKEY | FCONTROL,           VK_OEM_MINUS, IDM_TEXT_SMALLER },
        { FVIRTKEY | FCONTROL,           VK_SUBTRACT,  IDM_TEXT_SMALLER },
        { FVIRTKEY | FCONTROL,           '0',          IDM_TEXT_RESET },
        { FVIRTKEY | FCONTROL,           VK_NUMPAD0,   IDM_TEXT_RESET },
    };
    return CreateAcceleratorTableW(a, ARRAYSIZE(a));
}

// Keys an edit control needs for itself (text clipboard, select-all, F2 does nothing
// harmful there): while the address bar, search box or a rename box has focus these
// must not become file commands.
static bool is_edit_focus() {
    HWND f = GetFocus();
    if (!f) return false;
    WCHAR cls[32]{};
    GetClassNameW(f, cls, 32);
    return lstrcmpiW(cls, WC_EDITW) == 0;
}
static bool edit_owns_key(const MSG& m) {
    if ((m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN) || !is_edit_focus()) return false;
    // renaming in the list or the pane: every key is the edit's (a refresh, navigation or
    // Alt+Enter would end the rename)
    HWND parent = GetParent(GetFocus());
    if (parent == g_list || parent == g_tree) return true;
    if (m.message != WM_KEYDOWN) return false;
    bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
    switch (m.wParam) {
        case 'A': case 'C': case 'X': case 'V': case 'Z': case 'Y': return ctrl;
        case VK_F2: case VK_DELETE: case VK_BACK: case VK_RETURN: return true;
        default: return false;
    }
}

// Tab / F6 (Shift: backwards) move the keyboard focus around the panes like Explorer:
// address bar, search box, navigation pane, file list. A rename box keeps its keys.
static bool cycle_focus(const MSG& m) {
    if (m.message != WM_KEYDOWN || (m.wParam != VK_TAB && m.wParam != VK_F6)) return false;
    if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) return false;   // Ctrl+Tab: tabs
    HWND focus = GetFocus();
    if (!focus || GetAncestor(focus, GA_ROOT) != g_main) return false;
    HWND parent = GetParent(focus);
    if (is_edit_focus() && (parent == g_list || parent == g_tree)) return false;
    HWND panes[] = { g_address, g_search, g_tree, g_list };
    constexpr int n = (int)(sizeof(panes) / sizeof(panes[0]));
    int cur = -1;
    for (HWND h = focus; h && h != g_main && cur < 0; h = GetParent(h))
        for (int i = 0; i < n; ++i)
            if (panes[i] == h) cur = i;
    int dir = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
    for (int k = 1; k <= n; ++k) {
        int i = cur < 0 ? (dir > 0 ? k - 1 : n - k) : ((cur + dir * k) % n + n) % n;
        if (panes[i] && IsWindowVisible(panes[i]) && IsWindowEnabled(panes[i])) {
            SetFocus(panes[i]);                // (the address bar starts editing)
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------
// commands
static void set_sort(int column, int asc /* -1 keep */) {
    if (column >= 0) g_sortColumn = column;
    if (asc >= 0) g_sortAsc = asc != 0;
    enum_sort_items();
}

static void on_command(WORD id) {
    switch (id) {
        case IDM_NAV_BACK:    navigation_go_back(); break;
        case IDM_NAV_FORWARD: navigation_go_forward(); break;
        case IDM_NAV_UP:      navigation_go_up(); break;
        case IDM_NAV_REFRESH:
        case IDM_VIEW_REFRESH:
            navigation_refresh();
            break;

        case IDM_VIEW_DETAILS: set_view_mode({ LV_VIEW_DETAILS, 0, g_view.groupBy }, true); break;
        case IDM_VIEW_TILES:   set_view_mode({ LV_VIEW_TILE, 0, g_view.groupBy }, true); break;
        case IDM_VIEW_XLARGE:  set_view_mode({ LV_VIEW_ICON, 2, g_view.groupBy }, true); break;
        case IDM_VIEW_LARGE:   set_view_mode({ LV_VIEW_ICON, 1, g_view.groupBy }, true); break;
        case IDM_VIEW_MEDIUM:  set_view_mode({ LV_VIEW_ICON, 3, g_view.groupBy }, true); break;
        case IDM_VIEW_SMALL:   set_view_mode({ LV_VIEW_SMALLICON, 0, g_view.groupBy }, true); break;
        case IDM_VIEW_LIST:    set_view_mode({ LV_VIEW_LIST, 0, g_view.groupBy }, true); break;
        case IDM_VIEW_HIDDEN:
            g_showHidden = !g_showHidden;
            settings_set_show_hidden(g_showHidden);
            navigation_refresh();
            tree_refresh_all();
            break;
        case IDM_VIEW_DETAILSPANE:
        case IDM_VIEW_PREVIEWPANE:
            // one right-hand pane: Details or Preview (the other's command switches it)
            detailspane_toggle(id == IDM_VIEW_PREVIEWPANE ? PANE_PREVIEW : PANE_DETAILS);
            settings_set_details_pane(detailspane_visible());
            settings_set_pref(L"paneMode", detailspane_mode());
            layout_relayout();
            cmdbar_refresh();
            break;
        case IDM_VIEW_NAVPANE: set_nav_pane_visible(!g_treeVisible); break;
        case IDM_VIEW_CHECKBOXES:
            g_checkBoxes = !g_checkBoxes;
            settings_set_pref(L"checkBoxes", g_checkBoxes);
            list_apply_view();
            InvalidateRect(g_list, nullptr, TRUE);
            break;
        case IDM_TOOLS_MENU: cmdbar_show_tools(); break;
        case IDM_PERF_MARK: perf_flush(); perf_event("mark"); break;   // tools/perf.py: end of a scenario
        case IDM_VIEW_CMDBAR:
            g_cmdBarVisible = !g_cmdBarVisible;
            settings_set_pref(L"commandBar", g_cmdBarVisible);
            navbar_refresh();
            layout_relayout();
            break;
        case IDM_VIEW_COMPACT:
            g_compactView = !g_compactView;
            settings_set_pref(L"compactView", g_compactView);
            list_apply_view();
            tree_apply_metrics();
            InvalidateRect(g_list, nullptr, TRUE);
            InvalidateRect(g_tree, nullptr, TRUE);
            break;
        case IDM_VIEW_FOLDERSIZES:
            foldersizes_set_enabled(!foldersizes_enabled());
            settings_set_pref(L"folderSizes", foldersizes_enabled());
            break;
        case IDM_VIEW_EXTENSIONS:
            // our own choice from now on; Windows' setting stays as it is
            g_showExtensions = show_extensions_effective() ? 0 : 1;
            settings_set_pref(L"showExtensions", g_showExtensions);
            navigation_refresh();
            break;
        case IDM_TEXT_LARGER:  set_text_scale(text_scale_step(text_scale(), +1)); break;
        case IDM_TEXT_SMALLER: set_text_scale(text_scale_step(text_scale(), -1)); break;
        case IDM_TEXT_RESET:   set_text_scale(100); break;
        case IDM_VIEW_APPLY_ALL: views_apply_to_all(); break;
        case IDM_VIEW_RESET_ALL: views_reset_all(); break;
        case IDM_TREE_DELETE:      tree_delete_selected(false); break;
        case IDM_TREE_DELETE_PERM: tree_delete_selected(true); break;

        case IDM_NEW_MENU: case IDM_SORT_MENU: case IDM_VIEW_MENU: case IDM_MORE_MENU:
            cmdbar_show_menu(id);
            break;
        case IDM_SHARE:
            if (!contextmenu_invoke_verb(g_main, "Windows.ModernShare"))
                contextmenu_invoke_verb(g_main, "Windows.Share");
            break;
        case IDM_COPY_PATH: copy_path_to_clipboard(); break;
        case IDM_NEW_WINDOW: navigation_open_new_window(g_current); break;
        case IDM_OPTIONS:
            ShellExecuteW(g_main, nullptr, L"rundll32.exe", L"shell32.dll,Options_RunDLL 0", nullptr, SW_SHOWNORMAL);
            break;

        case IDM_SORT_NAME: set_sort(0, -1); break;
        case IDM_SORT_DATE: set_sort(3, -1); break;
        case IDM_SORT_TYPE: set_sort(2, -1); break;
        case IDM_SORT_SIZE: set_sort(1, -1); break;
        case IDM_SORT_ASC:  set_sort(-1, 1); break;
        case IDM_SORT_DESC: set_sort(-1, 0); break;
        case IDM_SORT_COL_BASE ... IDM_SORT_COL_LAST: columns_sort_by(id - IDM_SORT_COL_BASE); break;
        case IDM_GROUP_BASE + GB_NONE: case IDM_GROUP_BASE + GB_NAME: case IDM_GROUP_BASE + GB_DATE:
        case IDM_GROUP_BASE + GB_TYPE: case IDM_GROUP_BASE + GB_SIZE: {
            View v = g_view;
            v.groupBy = id - IDM_GROUP_BASE;
            set_view_mode(v, true);
            break;
        }

        case IDM_EDIT_CUT:      list_copy_selection(true); break;
        case IDM_EDIT_COPY:     list_copy_selection(false); break;
        case IDM_EDIT_PASTE:    fileops_paste_into_current(); break;
        case IDM_EDIT_DELETE:   list_delete_selection(false); break;
        case IDM_EDIT_RENAME:
            if (GetFocus() == g_tree) tree_rename_selected(); else list_rename_selected();
            break;
        case IDM_EDIT_SELALL:   list_select_all(); break;
        case IDM_EDIT_SELNONE:  list_select_none(); break;
        case IDM_EDIT_INVERT:   list_invert_selection(); break;

        case IDM_FILE_NEWFOLDER:   fileops_new_folder(); break;
        case IDM_FILE_NEWTAB:      tabmodel_new_tab(); break;
        case IDM_FILE_CLOSETAB:    tabmodel_close_tab(-1); break;
        case IDM_FILE_OPEN:        navigation_open_selected(); break;
        case IDM_FILE_RENAME:      list_rename_selected(); break;
        case IDM_FILE_DELETE:      list_delete_selection(false); break;
        case IDM_FILE_PROPERTIES:  list_show_properties(); break;
        case IDM_FILE_EXIT:        exit_app(); break;

        case IDM_FOCUS_ADDRESS: address_begin_edit(); break;
        case IDM_FOCUS_SEARCH:
            SetFocus(g_search);
            SendMessageW(g_search, EM_SETSEL, 0, -1);
            break;

        case IDM_TABNEXT: tabmodel_next_tab(+1); break;
        case IDM_TABPREV: tabmodel_next_tab(-1); break;
        case IDM_TABSEL1: case IDM_TABSEL2: case IDM_TABSEL3: case IDM_TABSEL4:
        case IDM_TABSEL5: case IDM_TABSEL6: case IDM_TABSEL7: case IDM_TABSEL8:
        case IDM_TABSEL9:
            tabmodel_switch_index_1_based(id - IDM_TABSEL1 + 1);
            break;

        default: break;
    }
}

static void on_command_ex(WPARAM wp, LPARAM) {
    WORD id = LOWORD(wp);
    WORD code = HIWORD(wp);
    if (id == IDC_SEARCH) {                    // EN_* notifications
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) search_frame_invalidate();
        else if (code == EN_CHANGE) search_text_changed();
        return;
    }
    on_command(id);
}
static void tree_context_menu(HWND hwnd, POINT pt) {
    HTREEITEM h = nullptr;
    if (pt.x == -1 && pt.y == -1) {
        // keyboard context menu: at the selected node
        h = TreeView_GetSelection(g_tree);
        RECT ir{};
        if (h && TreeView_GetItemRect(g_tree, h, &ir, TRUE)) pt = { ir.left, ir.bottom };
        else pt = { 8, 8 };
        ClientToScreen(g_tree, &pt);
    } else {
        POINT c = pt; ScreenToClient(g_tree, &c);
        TVHITTESTINFO ht{}; ht.pt = c;
        h = TreeView_HitTest(g_tree, &ht);
        if (!(ht.flags & TVHT_ONITEM)) h = nullptr;
    }
    if (!h) return;
    // highlight the node for the menu's lifetime without navigating to it
    TreeView_SelectDropTarget(g_tree, h);
    PITEMID_CHILD p = tree_get_pidl_of_item(h);
    if (p) { show_tree_context_menu(hwnd, pt, p); CoTaskMemFree(p); }
    TreeView_SelectDropTarget(g_tree, nullptr);
}

// ------------------------------------------------------------------
// window proc
static UINT s_watchMsg = 0;

// persist the window's place, the panes, the tabs and the settings
static void save_window_state(HWND hwnd) {
    SplitMode s = settings_splitter();
    s.width = g_splitterX;
    s.widthRight = g_splitterRight;
    s.detailsVisible = detailspane_visible();
    s.treeVisible = g_treeVisible;
    WINDOWPLACEMENT wpl{ sizeof(wpl) };
    if (GetWindowPlacement(hwnd, &wpl)) {
        // the normal position is in workspace coordinates (relative to the work area);
        // the restore at startup works in screen coordinates
        RECT r = wpl.rcNormalPosition;
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi))
            OffsetRect(&r, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
        s.maximized = wpl.showCmd == SW_SHOWMAXIMIZED;
        s.x = (int)r.left;
        s.y = (int)r.top;
        s.winW = (int)(r.right - r.left);
        s.winH = (int)(r.bottom - r.top);
    }
    settings_set_splitter(s);
    tabs_persist_now();
    settings_save_all();
}

static void on_theme_changed(HWND hwnd) {
    theme_apply_controls();
    groups_theme_changed();
    detailspane_theme_changed();
    list_apply_view();               // check box images follow the theme
    chrome_theme_changed(hwnd);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == s_watchMsg && s_watchMsg) {
        watcher_on_message(wp, lp);
        return 0;
    }
    // owner-drawn items and lazily-built submenus of a tracked shell menu
    if (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM || msg == WM_MENUCHAR) {
        LRESULT r = 0;
        if (contextmenu_handle_menu_msg(msg, wp, lp, &r)) return r;
    }
    {   // custom frame: caption row, caption buttons, resize borders
        LRESULT r = 0;
        if (chrome_handle(hwnd, msg, wp, lp, &r)) return r;
    }
    switch (msg) {
        case WM_CREATE: {
            g_main = hwnd;
            theme_set_dpi((int)GetDpiForWindow(hwnd));
            chrome_init(hwnd);
            layout_create_splitters();
            if (!g_treeVisible) ShowWindow(g_tree, SW_HIDE);   // hidden when the app was closed
            address_create(hwnd);
            build_search();
            g_navbar = navbar_create(hwnd);
            tabstrip_create(hwnd);
            cmdbar_create(hwnd);
            statusbar_create(hwnd);
            list_create();
            detailspane_create(hwnd);
            theme_apply_controls();
            perf_event("create.controls");
            initialize_tree(g_tree);
            perf_event("create.tree");
            watcher_init(hwnd);
            s_watchMsg = watcher_change_msg();
            dnd_register_sources();
            AddClipboardFormatListener(hwnd);       // Paste button state
            perf_event("create.watcher+dnd");
            // apply active tab (its own folder + view)
            navigate_replace(tabmodel_active());
            tree_sync_to_current();     // starts the tree's listings now (the posted sync would wait for ShowWindow)
            perf_event("create.navigate");
            address_history_add(current_display_path());
            layout_relayout();
            status_update();
            SetFocus(g_list);
            return 0;
        }

        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            double t0 = perf_on() ? perf_now() : 0;
            {
                BufferedPaint bp(hwnd);
                chrome_paint(bp.dc, bp.rc);
                layout_paint(bp.dc, bp.rc);
            }
            if (perf_on()) {
                static bool first = true;
                if (first) { first = false; perf_event("main.first_paint", perf_now() - t0); }
                perf_paint("main", perf_now() - t0);
            }
            return 0;
        }
        case WM_CTLCOLOREDIT:
            if ((HWND)lp == g_search) {
                const Palette& p = pal();
                if (s_searchBrushColor != p.boxFill) {
                    if (s_searchBrush) DeleteObject(s_searchBrush);
                    s_searchBrush = CreateSolidBrush(p.boxFill);
                    s_searchBrushColor = p.boxFill;
                }
                SetTextColor((HDC)wp, p.text);
                SetBkColor((HDC)wp, p.boxFill);
                return (LRESULT)s_searchBrush;
            }
            break;

        case WM_SIZE:
            layout_resize(hwnd, LOWORD(lp), HIWORD(lp));
            InvalidateRect(hwnd, nullptr, FALSE);
            status_update();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = { S(500), S(360) };
            return 0;
        }
        case WM_COMMAND: on_command_ex(wp, lp); return 0;
        case WM_CLIPBOARDUPDATE: cmdbar_refresh(); return 0;
        case WM_TREE_SYNC: tree_sync_to_current(); return 0;
        case WM_SELECTION_CHANGED: list_on_selection_changed(); cmdbar_refresh(); return 0;
        case WM_ASYNC_WAKE: drain_async(); return 0;
        case WM_ENUM_DONE: enum_apply_op((HeapEnumOp*)lp); return 0;
        case WM_TIMER:
            if (wp == TIMER_WATCH_REFRESH) { watcher_on_timer(); return 0; }
            if (wp == TIMER_SEARCH) { search_on_timer(); return 0; }
            if (wp == TIMER_ENUM_STREAM) { enum_on_stream_timer(); return 0; }
            break;
        case WM_SETTINGCHANGE:
            if (lp && lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") == 0 && theme_reload())
                on_theme_changed(hwnd);
            break;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            if (theme_reload()) on_theme_changed(hwnd);
            break;

        case WM_SETCURSOR: {
            if (s_dragKind != DragKind::None) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            DWORD mp = GetMessagePos();
            POINT pt{ GET_X_LPARAM(mp), GET_Y_LPARAM(mp) };
            ScreenToClient(hwnd, &pt);
            if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT && layout_splitter_kind(hwnd, pt) != DragKind::None) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            DragKind k = layout_splitter_kind(hwnd, pt);
            if (k != DragKind::None) { s_dragKind = k; SetCapture(hwnd); return 0; }
            break;
        }
        case WM_LBUTTONUP:
            if (s_dragKind != DragKind::None) { s_dragKind = DragKind::None; ReleaseCapture(); }
            break;
        case WM_CAPTURECHANGED:
            s_dragKind = DragKind::None;
            break;
        case WM_MOUSEMOVE:
            if (s_dragKind != DragKind::None) {
                POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                layout_splitter_drag(hwnd, pt, s_dragKind);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_XBUTTONUP:   // mouse back/forward buttons
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) navigation_go_back();
            else if (GET_XBUTTON_WPARAM(wp) == XBUTTON2) navigation_go_forward();
            return TRUE;
        case WM_APPCOMMAND:  // the same buttons over a child window (list, tree, ...) arrive as this
            switch (GET_APPCOMMAND_LPARAM(lp)) {
                case APPCOMMAND_BROWSER_BACKWARD: navigation_go_back(); return TRUE;
                case APPCOMMAND_BROWSER_FORWARD:  navigation_go_forward(); return TRUE;
                case APPCOMMAND_BROWSER_REFRESH:  navigation_refresh(); return TRUE;
            }
            break;

        case WM_NOTIFY: {
            NMHDR* hdr = (NMHDR*)lp;
            if (hdr->idFrom == IDC_TREE) return tree_notify(hwnd, wp, lp);
            if (hdr->idFrom == IDC_LIST) return list_notify(hwnd, wp, lp);
            break;
        }

        case WM_CONTEXTMENU: {
            HWND child = (HWND)wp;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            // mouse right-clicks on the list are handled by NM_RCLICK (which suppresses
            // this message); what arrives here is Shift+F10 / the Apps key
            if (child == g_list) { list_context_menu(pt); return 0; }
            if (child == g_tree) { tree_context_menu(hwnd, pt); return 0; }
            break;
        }

        case WM_DPICHANGED: {
            theme_set_dpi(HIWORD(wp));
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            tree_apply_metrics();
            list_apply_view();           // row heights, tile size
            columns_rescale();
            layout_relayout();
            return 0;
        }

        case WM_CLOSE: DestroyWindow(hwnd); return 0;

        case WM_ENDSESSION:
            // Windows is shutting down / logging off: the process ends without WM_DESTROY
            if (wp) save_window_state(hwnd);
            return 0;

        case WM_DESTROY: {
            perf_flush();
            workers_shutdown();             // no new background work, results dropped
            RemoveClipboardFormatListener(hwnd);
            save_window_state(hwnd);
            dnd_unregister_all();
            deregister_current_watch();
            if (s_searchBrush) { DeleteObject(s_searchBrush); s_searchBrush = nullptr; }
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------
// init & run

static ATOM register_class(HICON hIcon, HICON hIconSm) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = g_hinst;
    wc.lpfnWndProc = main_proc;
    wc.lpszClassName = APP_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = hIcon;
    wc.hIconSm = hIconSm;
    wc.style = CS_DBLCLKS;
    return RegisterClassExW(&wc);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    crash_init();
    perf_init();
    perf_event("winmain");
    g_hinst = hInst;
    OleInit ole;            // also initializes COM (STA) for this thread
    if (!ole.ok()) return 1;

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_TREEVIEW_CLASSES |
                              ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    perf_event("ole+commctrl");
    theme_init();
    theme_set_dpi(theme_dpi());     // fonts for the primary monitor; WM_CREATE adjusts
    perf_event("theme");

    settings_load_all();
    g_view = settings_default_view();
    g_showHidden = settings_show_hidden();
    g_checkBoxes = settings_pref(L"checkBoxes", 0) != 0;
    g_cmdBarVisible = settings_pref(L"commandBar", 0) != 0;
    // like Explorer until chosen here
    int compact = settings_pref(L"compactView", -1);
    g_compactView = compact < 0 ? windows_compact_view() : compact != 0;
    g_showExtensions = std::clamp(settings_pref(L"showExtensions", -1), -1, 1);
    foldersizes_set_enabled(settings_pref(L"folderSizes", 1) != 0);
    theme_set_text_scale(settings_pref(L"textSize", 100));
    SplitMode sp = settings_splitter();
    if (sp.width >= 120 && sp.width <= 1200) g_splitterX = sp.width;
    if (sp.widthRight >= 140 && sp.widthRight <= 480) g_splitterRight = sp.widthRight;
    g_treeVisible = sp.treeVisible;

    // explorer-plus.exe [--new-window] [folder]
    std::wstring openPath;
    bool newWindow = false;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            if (lstrcmpiW(argv[i], L"--new-window") == 0) newWindow = true;
            else openPath += (openPath.empty() ? L"" : L" ") + std::wstring(argv[i]);   // an unquoted path with spaces
        }
        if (argv) LocalFree(argv);
        // "explorer-plus.exe ..\docs": relative to the working directory
        if (!openPath.empty() && PathIsRelativeW(openPath.c_str()) && openPath.find(L':') == std::wstring::npos &&
            openPath.rfind(L"\\\\", 0) != 0) {
            WCHAR full[MAX_PATH * 4]{};
            DWORD n = GetFullPathNameW(openPath.c_str(), ARRAYSIZE(full), full, nullptr);
            if (n && n < ARRAYSIZE(full) && PathIsDirectoryW(full)) openPath = full;
        }
    }
    perf_event("settings");
    tabmodel_init(openPath, !newWindow);          // restored tabs (+ command-line folder)
    // the system image list is set up here, once: workers asking for icons while it is
    // still being initialized get no index (and the tree showed "unknown file" icons)
    sys_imagelist_init();
    if (TabState* t = tabmodel_active(); t && g_treeVisible) tree_prefetch(t->current);   // ready by WM_CREATE
    perf_event("tabs");
    detailspane_set_mode(settings_pref(L"paneMode", PANE_DETAILS) == PANE_PREVIEW ? PANE_PREVIEW : PANE_DETAILS);
    detailspane_set_visible(sp.detailsVisible);   // sets flags only (window created later)

    // the exe's own icon (taskbar, Alt+Tab, title bar), at the sizes the system uses
    HICON appIcon = nullptr, appIconSm = nullptr;
    LoadIconMetric(hInst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &appIcon);
    LoadIconMetric(hInst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &appIconSm);
    if (!appIcon) appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    register_class(appIcon, appIconSm ? appIconSm : appIcon);

    // restore window rect, clamped to the work area of the monitor nearest the saved rect
    int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT;
    int ww = 1200, wh = 760;
    bool haveRect = (sp.winW > 500 && sp.winH > 350 && sp.x != INT_MIN && sp.y != INT_MIN);
    if (newWindow && haveRect) { sp.x += 32; sp.y += 32; }      // cascade
    if (haveRect) {
        RECT saved{ sp.x, sp.y, sp.x + sp.winW, sp.y + sp.winH };
        HMONITOR hm = MonitorFromRect(&saved, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (hm && GetMonitorInfoW(hm, &mi)) {
            RECT work = mi.rcWork;
            ww = min((int)sp.winW, (int)(work.right - work.left));
            wh = min((int)sp.winH, (int)(work.bottom - work.top));
            wx = max((int)work.left, min((int)sp.x, (int)(work.right  - 60)));
            wy = max((int)work.top,  min((int)sp.y, (int)(work.bottom - 60)));
            if (wx + ww > work.right)  ww = work.right - wx;
            if (wy + wh > work.bottom) wh = work.bottom - wy;
            if (ww < 500 || wh < 350) {  // fallback centered on that monitor
                ww = min(1200, (int)(work.right - work.left));
                wh = min(760,  (int)(work.bottom - work.top));
                wx = work.left + (work.right - work.left - ww) / 2;
                wy = work.top  + (work.bottom - work.top - wh) / 2;
            }
        }
    }
    HWND wnd = CreateWindowExW(0, APP_CLASS, L"Explorer Plus",
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               wx, wy, ww, wh, nullptr, nullptr, g_hinst, nullptr);
    if (!wnd) return 2;
    perf_event("window.created");

    ShowWindow(wnd, sp.maximized && !newWindow ? SW_SHOWMAXIMIZED : nCmdShow);
    UpdateWindow(wnd);
    perf_event("window.shown");

    HACCEL acc = make_accels();
    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (cycle_focus(m)) continue;
        if ((m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) && !edit_owns_key(m) &&
            TranslateAcceleratorW(g_main, acc, &m))
            continue;
        TranslateMessage(&m);
        if (!perf_on()) {
            DispatchMessageW(&m);
            continue;
        }
        // profiling: report messages that stall the UI for more than a frame
        double t0 = perf_now();
        DispatchMessageW(&m);
        double dur = perf_now() - t0;
        if (dur > 16) {
            WCHAR cls[64]{}, detail[160]{};
            if (m.hwnd) GetClassNameW(m.hwnd, cls, 64);
            swprintf_s(detail, L"msg=0x%04X wp=0x%llX %ls", m.message, (unsigned long long)m.wParam, cls);
            perf_event("slow.msg", dur, -1, detail);
        }
    }
    DestroyAcceleratorTable(acc);
    theme_shutdown();
    if (g_font) DeleteObject(g_font);
    if (g_iconFont) DeleteObject(g_iconFont);
    if (g_mdl2Font) DeleteObject(g_mdl2Font);
    if (appIcon) DestroyIcon(appIcon);
    if (appIconSm) DestroyIcon(appIconSm);
    // Detached workers (listing, thumbnails, icons, previews) may still be running:
    // returning would run the static destructors under them. Everything is saved by
    // now and the clipboard is flushed at copy time, so end the process here.
    std::_Exit((int)m.wParam);
}