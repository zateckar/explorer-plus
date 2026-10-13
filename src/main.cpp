// Explorer Plus — main window: Win11 frame with tabs in the caption, navigation
// row (buttons, breadcrumb, search), command bar, tree / list / details, status bar.
#include <algorithm>
#include <cstdlib>
using std::max;
using std::min;
#include "appstate.h"
#include "cmdline.h"
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
#include "preview.h"
#include "player.h"
#include <uxtheme.h>
#include <oleacc.h>

static WNDPROC s_searchProc = nullptr;
static HBRUSH s_searchBrush = nullptr;
static COLORREF s_searchBrushColor = CLR_INVALID;
static DragKind s_dragKind = DragKind::None;

void exit_app() { DestroyWindow(g_main); }

// ------------------------------------------------------------------
// view mode plumbing (model lives in TabState::view)
void set_view_mode(const View& v, bool persist) {
    PerfScope ps("view.apply");
    bool same = (g_pane->view.lvView == v.lvView && g_pane->view.thumbsExtra == v.thumbsExtra);
    bool thumbSizeChanged = g_pane->view.thumbsExtra != v.thumbsExtra;
    bool regroup = g_pane->view.groupBy != v.groupBy;
    g_pane->view = v;
    TabState* t = tabmodel_active();
    if (t) t->nav().view = v;
    // remembered for this folder; other folders change only with "Apply to all folders"
    if (persist) settings_set_view_for(folder_settings_key(g_pane->current), v);
    if (pane_is_active()) statusbar_refresh();
    // (navigation re-groups when its listing arrives; only a live change re-sorts here)
    if (regroup && persist) enum_sort_items();
    if (same) return;
    if (thumbSizeChanged) thumb_reset_for_new_folder();   // cached thumbs are per size
    list_apply_view();
    InvalidateRect(g_pane->list, nullptr, TRUE);
}

// ------------------------------------------------------------------
// status bar
void status_update() {
    if (!g_status || !g_pane->list || !pane_is_active()) return;    // (the bar shows the active list)
    int count = 0;
    ULONGLONG selBytes = 0;
    {
        std::lock_guard<std::mutex> lk(g_pane->itemsMx);
        count = (int)g_pane->items.size();
        int idx = -1;
        while ((idx = ListView_GetNextItem(g_pane->list, idx, LVNI_SELECTED)) != -1)
            if (idx < (int)g_pane->items.size() && !g_pane->items[idx]->isFolder)
                selBytes += g_pane->items[idx]->bytes;
    }
    int sel = ListView_GetSelectedCount(g_pane->list);
    std::wstring a = std::to_wstring(count) + (count == 1 ? L" item" : L" items");
    std::wstring b;
    if (sel) {
        b = std::to_wstring(sel) + (sel == 1 ? L" item selected" : L" items selected");
        if (selBytes) b += L"  " + fmt_size(selBytes);
    }
    const std::wstring& err = enum_last_error();
    if (!err.empty()) {
        // nothing listed, or a listing that stopped part way (some items are missing)
        a = count ? a + L" (listing stopped)" : L"Can't open this location";
        b = err;
    }

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
            if (search_is_results(g_pane->current)) search_cancel();     // back to the searched folder
            else SetWindowTextW(h, L"");
            SetFocus(g_pane->list);
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

// a screen reader's name for a control without a label (the edits: their placeholder
// isn't one)
static void set_accessible_name(HWND h, const std::wstring& name) {
    // (the GUIDs here: oleacc.h only declares them)
    static const CLSID kAccPropServices = { 0xb5f8350b, 0x0548, 0x48b1, { 0xa6, 0xee, 0x88, 0xbd, 0x00, 0xb4, 0xa5, 0xe7 } };
    static const GUID kAccName = { 0x608d3df8, 0x8128, 0x4aa7, { 0xa4, 0x28, 0xf5, 0x5e, 0x49, 0x26, 0x72, 0x91 } };
    static const IID kIAccPropServices = { 0x6e26e776, 0x04f0, 0x495d, { 0x80, 0xe4, 0x33, 0x30, 0x35, 0x2e, 0x31, 0x69 } };
    static com_ptr<IAccPropServices> s_props;
    if (!s_props && FAILED(CoCreateInstance(kAccPropServices, nullptr, CLSCTX_INPROC_SERVER, kIAccPropServices, (void**)s_props.put())))
        return;
    if (s_props) s_props->SetHwndPropStr(h, (DWORD)OBJID_CLIENT, CHILDID_SELF, kAccName, name.c_str());
}

// "Search <folder>" placeholder, like Explorer
void search_update_cue() {
    if (!g_search) return;
    TabState* t = tabmodel_active();
    PCIDLIST_ABSOLUTE scope = search_scope_of(g_pane->current);
    std::wstring cue = L"Search " + (scope ? pidl_abs_dispname(scope) : t ? t->nav().name : std::wstring());
    SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)cue.c_str());
    set_accessible_name(g_search, cue);
}

// ------------------------------------------------------------------
// commands that need a little glue

// selected items' paths, or the folder's
static void copy_path_to_clipboard() {
    std::vector<PIDLIST_ABSOLUTE> own;
    for (auto c : list_selected_pidls())
        if (PIDLIST_ABSOLUTE abs = ILCombine(g_pane->current, c)) own.push_back(abs);
    std::vector<PCIDLIST_ABSOLUTE> items(own.begin(), own.end());
    if (items.empty()) items.push_back(g_pane->current);
    fileops_copy_paths(items);
    for (auto p : own) CoTaskMemFree(p);
}

// Edit shortcuts typed in the navigation pane act on its folder, not on the list's
// selection, which can't be seen from there (Ctrl+X would cut files the user isn't
// looking at). True when the command was the tree's.
static bool tree_key_command(WORD id) {
    switch (id) {
        case IDM_EDIT_CUT: case IDM_EDIT_COPY: case IDM_EDIT_PASTE:
        case IDM_FILE_PROPERTIES: case IDM_COPY_PATH:
            break;
        case IDM_EDIT_SELALL: return true;      // nothing to select there
        default: return false;
    }
    PIDLIST_ABSOLUTE abs = tree_get_pidl_of_item(TreeView_GetSelection(g_tree));
    if (!abs) { MessageBeep(MB_ICONASTERISK); return true; }
    switch (id) {
        case IDM_EDIT_CUT:
        case IDM_EDIT_COPY: {
            com_ptr<IShellFolder> parent;
            PCUITEMID_CHILD child = nullptr;
            if (SUCCEEDED(bind_parent(abs, parent.put(), &child)) && child)
                fileops_set_clipboard(parent.get(), { child }, id == IDM_EDIT_CUT);
            else
                MessageBeep(MB_ICONASTERISK);      // a root (Home, This PC) can't be copied
            break;
        }
        case IDM_EDIT_PASTE:      fileops_paste_into(abs); break;
        case IDM_FILE_PROPERTIES: contextmenu_properties_of(g_main, abs); break;
        case IDM_COPY_PATH:       fileops_copy_paths({ abs }); break;
    }
    CoTaskMemFree(abs);
    return true;
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
    for_each_pane([] {
        list_apply_view();               // row spacing, tile size
        columns_rescale();
        InvalidateRect(g_pane->list, nullptr, TRUE);
    });
    InvalidateRect(g_tree, nullptr, TRUE);
    if (HWND pane = detailspane_hwnd()) InvalidateRect(pane, nullptr, FALSE);
}

// ------------------------------------------------------------------
// accelerator table
static HACCEL s_accel = nullptr;
static UINT s_accelCount = 0;

HACCEL app_accelerators(UINT* count) {
    if (count) *count = s_accelCount;
    return s_accel;
}

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
        { FVIRTKEY | FCONTROL | FSHIFT,  'D',       IDM_VIEW_DUALPANE },
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
    s_accelCount = ARRAYSIZE(a);
    return CreateAcceleratorTableW(a, ARRAYSIZE(a));
}

// Ctrl with the key that types "+" or "-" on the user's layout (German, Nordic, Japanese,
// ... have them on other keys than US): text size. The digit keys stay Ctrl+1..9 (tabs),
// as in browsers, also where a digit key types "+" unshifted (Czech, Slovak).
static bool text_size_key(const MSG& m) {
    if (m.message != WM_KEYDOWN || GetKeyState(VK_CONTROL) >= 0 || GetKeyState(VK_MENU) < 0) return false;
    UINT vk = (UINT)m.wParam;
    if ((vk >= '0' && vk <= '9') || (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE)) return false;   // (the table has the numpad)
    BYTE state[256];
    if (!GetKeyboardState(state)) return false;
    state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_RCONTROL] = 0;    // the character without Ctrl
    WCHAR ch[4]{};
    UINT scan = (UINT)((m.lParam >> 16) & 0xFF);
    // (flag 4: leaves the keyboard's dead-key state alone)
    if (ToUnicodeEx(vk, scan, state, ch, ARRAYSIZE(ch), 4, GetKeyboardLayout(0)) != 1) return false;
    UINT cmd = ch[0] == L'+' ? IDM_TEXT_LARGER : ch[0] == L'-' ? IDM_TEXT_SMALLER : 0;
    if (!cmd) return false;
    PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(cmd, 1), 0);
    return true;
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
    if (is_pane_list(parent) || parent == g_tree) return true;
    if (m.message != WM_KEYDOWN) return false;
    bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
    switch (m.wParam) {
        case 'A': case 'C': case 'X': case 'V': case 'Z': case 'Y': return ctrl;
        case VK_F2: case VK_DELETE: case VK_BACK: case VK_RETURN: return true;
        default: return false;
    }
}

// Tab / F6 (Shift: backwards) move the keyboard focus around the panes like Explorer:
// tabs, navigation buttons, address bar, search box, command bar, navigation pane, file
// list, preview. A rename box keeps its keys.
static bool cycle_focus(const MSG& m) {
    if (m.message != WM_KEYDOWN || (m.wParam != VK_TAB && m.wParam != VK_F6)) return false;
    if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) return false;   // Ctrl+Tab: tabs
    HWND focus = GetFocus();
    if (!focus || GetAncestor(focus, GA_ROOT) != g_main) return false;
    HWND parent = GetParent(focus);
    if (is_edit_focus() && (is_pane_list(parent) || parent == g_tree)) return false;
    // (the preview pane, while a handler shows a file: the handler takes the focus itself)
    HWND preview = preview_active() ? detailspane_hwnd() : nullptr;
    // (in the order they are on screen, as in Explorer; the painted rows take arrow keys)
    HWND panes[] = { tabstrip_hwnd(), g_navbar, g_address, g_search, cmdbar_hwnd(), g_tree, g_panes[0].list, g_panes[1].list, preview };
    constexpr int n = (int)(sizeof(panes) / sizeof(panes[0]));
    int cur = -1;
    for (HWND h = focus; h && h != g_main && cur < 0; h = GetParent(h))
        for (int i = 0; i < n; ++i)
            if (panes[i] == h) cur = i;
    int dir = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
    for (int k = 1; k <= n; ++k) {
        int i = cur < 0 ? (dir > 0 ? k - 1 : n - k) : ((cur + dir * k) % n + n) % n;
        if (!panes[i] || !IsWindowVisible(panes[i]) || !IsWindowEnabled(panes[i])) continue;
        if (panes[i] == preview) {
            if (preview_focus()) return true;
            continue;
        }
        SetFocus(panes[i]);                    // (the address bar starts editing)
        return true;
    }
    return false;
}

// ------------------------------------------------------------------
// commands
static void set_sort(int column, int asc /* -1 keep */) {
    if (column >= 0) g_pane->sortColumn = column;
    if (asc >= 0) g_pane->sortAsc = asc != 0;
    columns_save_sort();
    enum_sort_items();
}

static void on_command(WORD id) {
    switch (id) {
        case IDM_NAV_BACK:    navigation_go_back(); break;
        case IDM_NAV_FORWARD: navigation_go_forward(); break;
        case IDM_NAV_UP:      navigation_go_up(); break;
        case IDM_NAV_REFRESH:
        case IDM_VIEW_REFRESH:
            foldersizes_forget_shown();         // a refresh asked for: sizes too, not the ones kept
            navigation_refresh();
            break;

        case IDM_VIEW_DETAILS: set_view_mode({ LV_VIEW_DETAILS, 0, g_pane->view.groupBy }, true); break;
        case IDM_VIEW_TILES:   set_view_mode({ LV_VIEW_TILE, 0, g_pane->view.groupBy }, true); break;
        case IDM_VIEW_XLARGE:  set_view_mode({ LV_VIEW_ICON, 2, g_pane->view.groupBy }, true); break;
        case IDM_VIEW_LARGE:   set_view_mode({ LV_VIEW_ICON, 1, g_pane->view.groupBy }, true); break;
        case IDM_VIEW_MEDIUM:  set_view_mode({ LV_VIEW_ICON, 3, g_pane->view.groupBy }, true); break;
        case IDM_VIEW_SMALL:   set_view_mode({ LV_VIEW_SMALLICON, 0, g_pane->view.groupBy }, true); break;
        case IDM_VIEW_LIST:    set_view_mode({ LV_VIEW_LIST, 0, g_pane->view.groupBy }, true); break;
        case IDM_VIEW_HIDDEN:
            g_showHidden = !g_showHidden;
            settings_set_show_hidden(g_showHidden);
            for_each_pane(navigation_refresh, true);
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
            for_each_pane([] {
                list_apply_view();
                InvalidateRect(g_pane->list, nullptr, TRUE);
            });
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
            for_each_pane([] {
                list_apply_view();
                InvalidateRect(g_pane->list, nullptr, TRUE);
            });
            tree_apply_metrics();
            InvalidateRect(g_tree, nullptr, TRUE);
            break;
        case IDM_VIEW_DUALPANE: navigation_toggle_dual(); break;
        case IDM_VIEW_FOLDERSIZES:
            foldersizes_set_enabled(!foldersizes_enabled());
            settings_set_pref(L"folderSizes", foldersizes_enabled());
            break;
        case IDM_VIEW_EXTENSIONS:
            // our own choice from now on; Windows' setting stays as it is
            g_showExtensions = show_extensions_effective() ? 0 : 1;
            settings_set_pref(L"showExtensions", g_showExtensions);
            for_each_pane(navigation_refresh, true);
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
        case IDM_NEW_WINDOW: navigation_open_new_window(g_pane->current); break;
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
            View v = g_pane->view;
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
        case IDM_FOCUS_LIST: SetFocus(active_pane()->list); break;

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
    // (code 1: an accelerator, typed where the focus is; menus keep acting on the list)
    if (code == 1 && GetFocus() == g_tree && tree_key_command(id)) return;
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
    PIDLIST_ABSOLUTE p = tree_get_pidl_of_item(h);
    if (p) { show_tree_context_menu(hwnd, pt, p); CoTaskMemFree(p); }
    TreeView_SelectDropTarget(g_tree, nullptr);
}

// ------------------------------------------------------------------
// window proc

// persist the window's place, the panes, the tabs and the settings
static void save_window_state(HWND hwnd) {
    SplitMode s = settings_splitter();
    s.width = g_splitterX;
    s.widthRight = g_splitterRight;
    s.detailsVisible = detailspane_visible();
    s.treeVisible = g_treeVisible;
    settings_set_pref(L"dualSplit", g_dualSplit);
    WINDOWPLACEMENT wpl{ sizeof(wpl) };
    if (GetWindowPlacement(hwnd, &wpl)) {
        // the normal position is in workspace coordinates (relative to the work area);
        // the restore at startup works in screen coordinates
        RECT r = wpl.rcNormalPosition;
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi))
            OffsetRect(&r, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
        // (minimized from maximized: it comes back maximized)
        s.maximized = wpl.showCmd == SW_SHOWMAXIMIZED ||
                      (wpl.showCmd == SW_SHOWMINIMIZED && (wpl.flags & WPF_RESTORETOMAXIMIZED));
        s.x = (int)r.left;
        s.y = (int)r.top;
        s.winW = (int)(r.right - r.left);
        s.winH = (int)(r.bottom - r.top);
    }
    settings_set_splitter(s);
    tabs_persist_now();
    // a scanner or sync client can hold the file a moment: a few tries, not the session's changes lost
    for (int attempt = 0; attempt < 5 && !settings_save_all(); ++attempt) Sleep(200);
}

// the theme this window shows (the media player's window may read a change first)
static unsigned s_themeSeen = 1;

static void on_theme_changed(HWND hwnd) {
    s_themeSeen = theme_generation();
    theme_apply_controls();
    for_each_pane([] {
        groups_theme_changed();
        list_apply_view();           // check box images follow the theme
    });
    detailspane_theme_changed();
    chrome_theme_changed(hwnd);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg >= 0xC000) {                     // (registered messages: a list's change notifications)
        int pane = watcher_pane_of_msg(msg);
        if (pane >= 0) {
            PaneScope ps(&g_panes[pane]);
            watcher_on_message(wp, lp);
            return 0;
        }
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
            for (Pane& p : g_panes) {        // (the second list shows in dual-pane tabs)
                PaneScope ps(&p);
                list_create();
            }
            detailspane_create(hwnd);
            theme_apply_controls();
            perf_event("create.controls");
            initialize_tree(g_tree);
            perf_event("create.tree");
            watcher_init(hwnd);
            dnd_register_sources();
            AddClipboardFormatListener(hwnd);       // Paste button state
            perf_event("create.watcher+dnd");
            // apply active tab (its own folders + views)
            tabmodel_show_active();
            tree_sync_to_current();     // starts the tree's listings now (the posted sync would wait for ShowWindow)
            perf_event("create.navigate");
            address_history_add(current_display_path());
            layout_relayout();
            status_update();
            SetFocus(active_pane()->list);
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
        case WM_SELECTION_CHANGED: {
            if (wp >= (WPARAM)kPanes) return 0;
            PaneScope ps(&g_panes[wp]);
            list_on_selection_changed();
            cmdbar_refresh();
            return 0;
        }
        case WM_ASYNC_WAKE: drain_async(); return 0;
        case WM_ENUM_DONE: {
            auto* op = (HeapEnumOp*)lp;
            PaneScope ps(op && op->pane >= 0 && op->pane < kPanes ? &g_panes[op->pane] : nullptr);
            enum_apply_op(op);
            return 0;
        }
        case WM_TIMER: {
            if (wp == TIMER_SEARCH) { search_on_timer(); return 0; }
            if (wp == TIMER_TABS_PERSIST) { tabs_persist_now(); return 0; }
            // a list's own timers: the base id + 100 * its index (pane_timer)
            if (wp / 100 >= (WPARAM)kPanes) break;
            PaneScope ps(&g_panes[wp / 100]);
            switch (wp % 100) {
                case TIMER_WATCH_REFRESH: watcher_on_timer(); return 0;
                case TIMER_ENUM_STREAM:   enum_on_stream_timer(); return 0;
                case TIMER_ENUM_HELD:     enum_on_held_timer(); return 0;
                case TIMER_FOLDER_SIZES:  foldersizes_on_timer(); return 0;
            }
            break;
        }
        case WM_SETTINGCHANGE:
            if (!((lp && lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) || wp == SPI_SETHIGHCONTRAST)) break;
            [[fallthrough]];
        case WM_SYSCOLORCHANGE:          // (High Contrast themes: the palette is these colours)
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            theme_reload();
            if (s_themeSeen != theme_generation()) on_theme_changed(hwnd);
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
            RECT sf = layout_search_frame();
            if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT && g_search && PtInRect(&sf, pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            DragKind k = layout_splitter_kind(hwnd, pt);
            if (k != DragKind::None) { s_dragKind = k; SetCapture(hwnd); return 0; }
            // a list's caption (two lists): that list
            if (int i = layout_caption_pane_at(pt); i >= 0) {
                SetFocus(g_panes[i].list);
                navigation_activate_pane(&g_panes[i]);
                return 0;
            }
            // the search box's frame (around the edit) is the box too
            RECT sf = layout_search_frame();
            if (g_search && PtInRect(&sf, pt)) {
                if (GetFocus() != g_search) {
                    SetFocus(g_search);
                    int n = GetWindowTextLengthW(g_search);
                    SendMessageW(g_search, EM_SETSEL, n, n);
                }
                return 0;
            }
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
                layout_splitter_drag(hwnd, pt, s_dragKind);     // (repaints the panes' row)
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
            if (player_app_command(GET_APPCOMMAND_LPARAM(lp))) return TRUE;   // media keys: the player's
            break;

        case WM_NOTIFY: {
            NMHDR* hdr = (NMHDR*)lp;
            if (hdr->idFrom == IDC_TREE) return tree_notify(hwnd, wp, lp);
            if (hdr->idFrom == IDC_LIST || hdr->idFrom == IDC_LIST2) {
                Pane* p = pane_of_list(hdr->hwndFrom);
                if (!p) break;
                PaneScope ps(p);
                if (hdr->code == NM_SETFOCUS) navigation_activate_pane(p);   // commands go to this list now
                return list_notify(hwnd, wp, lp);
            }
            break;
        }

        case WM_CONTEXTMENU: {
            HWND child = (HWND)wp;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            // mouse right-clicks on the list are handled by NM_RCLICK (which suppresses
            // this message); what arrives here is Shift+F10 / the Apps key
            if (Pane* p = pane_of_list(child)) {
                PaneScope ps(p);
                list_context_menu(pt);
                return 0;
            }
            if (child == g_tree) { tree_context_menu(hwnd, pt); return 0; }
            break;
        }

        case WM_DPICHANGED: {
            theme_set_dpi(HIWORD(wp));
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            tree_apply_metrics();
            for_each_pane([] {
                list_apply_view();       // row heights, tile size
                columns_rescale();
            });
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
            for_each_pane(deregister_current_watch);
            if (s_searchBrush) { DeleteObject(s_searchBrush); s_searchBrush = nullptr; }
            // (a media player still open keeps the process: it ends when that closes)
            if (!player_main_closed()) PostQuitMessage(0);
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
    for (Pane& p : g_panes) {
        PaneScope ps(&p);
        p.view = settings_default_view();
        columns_load_sort();             // the order chosen last
    }
    g_dualSplit = std::clamp(settings_pref(L"dualSplit", 500), 150, 850);
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

    // explorer-plus.exe [--new-window] [folder or file]
    CommandLine cl = parse_command_line(GetCommandLineW());
    std::wstring openPath = cl.path;
    bool newWindow = cl.newWindow;
    {
        // "explorer-plus.exe ..\docs": relative to the working directory
        if (!openPath.empty() && PathIsRelativeW(openPath.c_str()) && openPath.find(L':') == std::wstring::npos &&
            openPath.rfind(L"\\\\", 0) != 0) {
            WCHAR full[MAX_PATH * 4]{};
            DWORD n = GetFullPathNameW(openPath.c_str(), ARRAYSIZE(full), full, nullptr);
            if (n && n < ARRAYSIZE(full) && PathFileExistsW(full)) openPath = full;   // (a file: its folder)
        }
    }
    perf_event("settings");
    tabmodel_init(openPath, !newWindow);          // restored tabs (+ command-line folder)
    // the system image list is set up here, once: workers asking for icons while it is
    // still being initialized get no index (and the tree showed "unknown file" icons)
    sys_imagelist_init();
    if (TabState* t = tabmodel_active(); t && g_treeVisible) tree_prefetch(t->side[t->dual ? t->activeSide : 0].current);   // ready by WM_CREATE
    perf_event("tabs");
    detailspane_set_mode(settings_pref(L"paneMode", PANE_DETAILS) == PANE_PREVIEW ? PANE_PREVIEW : PANE_DETAILS);
    detailspane_set_visible(sp.detailsVisible);   // sets flags only (window created later)

    // the exe's own icon (taskbar, Alt+Tab, title bar), at the sizes the system uses
    HICON appIcon = nullptr, appIconSm = nullptr;
    LoadIconMetric(hInst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &appIcon);
    LoadIconMetric(hInst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &appIconSm);
    if (!appIcon) appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    register_class(appIcon, appIconSm ? appIconSm : appIcon);
    s_accel = make_accels();        // (before the window: a preview handler may ask for it)

    // restore window rect, clamped to the work area of the monitor nearest the saved rect
    // (no saved rect: 1200 x 760 at 96 dpi, centered on the primary monitor and within it)
    int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT;
    UINT sysDpi = GetDpiForSystem();
    int defW = MulDiv(1200, (int)sysDpi, 96), defH = MulDiv(760, (int)sysDpi, 96);
    int ww = defW, wh = defH;
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
                ww = min(defW, (int)(work.right - work.left));
                wh = min(defH, (int)(work.bottom - work.top));
                wx = work.left + (work.right - work.left - ww) / 2;
                wy = work.top  + (work.bottom - work.top - wh) / 2;
            }
        }
    } else {
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            RECT work = mi.rcWork;
            ww = min(defW, (int)(work.right - work.left));
            wh = min(defH, (int)(work.bottom - work.top));
            wx = work.left + (work.right - work.left - ww) / 2;
            wy = work.top  + (work.bottom - work.top - wh) / 2;
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

    HACCEL acc = s_accel;
    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        bool key = m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN;
        if (player_owns(m.hwnd)) {          // the media player's window: its keys, not the main window's
            if (key && player_translate(&m)) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
            continue;
        }
        if (key && preview_translate_accelerator(&m)) continue;     // (an in-process handler's keys)
        if (cycle_focus(m)) continue;
        if (key && !edit_owns_key(m) && (text_size_key(m) || TranslateAcceleratorW(g_main, acc, &m)))
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