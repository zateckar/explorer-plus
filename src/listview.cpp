#include "listview.h"
#include "enumerate.h"
#include "navigation.h"
#include "detailspane.h"
#include "contextmenu.h"
#include "fileops.h"
#include "dnd.h"
#include "thumbs.h"
#include "groups.h"
#include "theme.h"
#include <uxtheme.h>
#include <vssym32.h>
#include <windowsx.h>
#include "columns.h"
#include "searchbox.h"
#include "perf.h"

static const UINT WM_LIST_COLUMNS_CHANGED = WM_APP + 60;   // posted to the list after header edits

// rename pending state
static int s_editIndex = -1;                 // row when the edit began (for the base-name select)
static PITEMID_CHILD s_editPidl = nullptr;   // the item itself
static std::wstring s_editName;              // as shown when the edit began
static std::wstring s_editHiddenExt;         // the extension the shown name leaves out (ours or Windows')
static bool s_editExtForced = false;
static PITEMID_CHILD s_editFolder = nullptr;  // absolute pidl of the folder the renamed item is in
static bool s_selChangePosted = false;

// ---------------------------------------------------------------
// view application

// Row spacing: Win11 spaces Details, List and Small icons rows out unless "Compact
// view" is on. These views make rows as tall as their small image list, but draw an
// image with the row's height (so the next image of the strip shows under a short
// icon). They therefore get blank image lists of the row height, which only reserve
// the icon and check box slots; icons, overlays and boxes are drawn in item post-paint.
static HIMAGELIST s_padSmall = nullptr, s_padState = nullptr;
static int s_padKey = -1;
static HTHEME s_boxTheme = nullptr;      // "Button" theme of the list (check boxes)
static int s_boxW = 0;

static bool padded_view(UINT v) { return v == LV_VIEW_DETAILS || v == LV_VIEW_LIST || v == LV_VIEW_SMALLICON; }
static bool padded_rows() { return padded_view(g_view.lvView); }

static void apply_image_lists(UINT v) {
    HIMAGELIST small = sys_imagelist(SHIL_SMALL);
    if (s_boxTheme) { CloseThemeData(s_boxTheme); s_boxTheme = nullptr; }
    if (!padded_view(v)) {
        // the control's own drawing: its check boxes (if any) and the shell icons
        if (s_padState && ListView_GetImageList(g_list, LVSIL_STATE) == s_padState) {
            ListView_SetImageList(g_list, nullptr, LVSIL_STATE);
            if (g_checkBoxes) {        // re-setting the style recreates the control's box images
                ListView_SetExtendedListViewStyleEx(g_list, LVS_EX_CHECKBOXES, 0);
                ListView_SetExtendedListViewStyleEx(g_list, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);
            }
        }
        ListView_SetImageList(g_list, small, LVSIL_SMALL);
        return;
    }
    s_boxTheme = OpenThemeData(g_list, L"Button");
    int cx = 16, cy = 16;
    ImageList_GetIconSize(small, &cx, &cy);
    int h = std::max(cy, SC(g_compactView ? 21 : 29));
    s_boxW = S(16);
    int key = (h << 16) ^ (cx << 8) ^ s_boxW;
    if (key != s_padKey || !s_padSmall) {
        if (s_padSmall) ImageList_Destroy(s_padSmall);     // (LVS_SHAREIMAGELISTS: ours to free)
        if (s_padState) ImageList_Destroy(s_padState);
        s_padSmall = ImageList_Create(cx, h, ILC_COLOR32, 0, 1);
        s_padState = ImageList_Create(s_boxW, h, ILC_COLOR32, 0, 1);
        s_padKey = key;
    }
    // state list first: setting the small list is what makes the control re-measure rows
    ListView_SetImageList(g_list, g_checkBoxes ? s_padState : nullptr, LVSIL_STATE);
    ListView_SetImageList(g_list, nullptr, LVSIL_SMALL);
    ListView_SetImageList(g_list, s_padSmall, LVSIL_SMALL);
}

void list_apply_view() {
    if (!g_list) return;
    UINT v = g_view.lvView;
    ListView_SetView(g_list, v);
    if (v == LV_VIEW_TILE) {
        LVTILEVIEWINFO tv{};
        tv.cbSize = sizeof(tv);
        tv.dwMask = LVTVIM_TILESIZE | LVTVIM_COLUMNS;
        tv.dwFlags = LVTVIF_FIXEDSIZE;
        tv.sizeTile.cx = SC(280);
        tv.sizeTile.cy = SC(68);
        tv.cLines = 2;
        ListView_SetTileViewInfo(g_list, &tv);
    }
    // normal (large) imagelist: thumbnails for the icon views, shell icons otherwise
    HIMAGELIST normal = nullptr;
    if (g_view.thumbsExtra && v == LV_VIEW_ICON) normal = thumb_imagelist_for_current_view();
    if (!normal) normal = sys_imagelist(v == LV_VIEW_TILE ? SHIL_EXTRALARGE : SHIL_LARGE);
    ListView_SetImageList(g_list, normal, LVSIL_NORMAL);
    // item check boxes mirror the selection; owner-data lists keep no per-item state
    // but selection/focus, so overlays, "cut" ghosting and the boxes come from
    // LVN_GETDISPINFO. The Name header gets a select-all box.
    ListView_SetExtendedListViewStyleEx(g_list, LVS_EX_CHECKBOXES, g_checkBoxes ? LVS_EX_CHECKBOXES : 0);
    ListView_SetCallbackMask(g_list, LVIS_OVERLAYMASK | LVIS_CUT | (g_checkBoxes ? LVIS_STATEIMAGEMASK : 0));
    // after the style: setting LVS_EX_CHECKBOXES installs the control's own box images
    apply_image_lists(v);
    if (HWND hdr = ListView_GetHeader(g_list)) {
        LONG_PTR st = GetWindowLongPtrW(hdr, GWL_STYLE);
        LONG_PTR want = g_checkBoxes ? (st | HDS_CHECKBOXES) : (st & ~(LONG_PTR)HDS_CHECKBOXES);
        if (want != st) SetWindowLongPtrW(hdr, GWL_STYLE, want);
    }
    enum_update_header_arrows();
}

// header select-all box: checked when every item is selected
bool list_all_selected() {
    int n = ListView_GetItemCount(g_list);
    return n > 0 && (int)ListView_GetSelectedCount(g_list) == n;
}

// a press on an item's check box toggles just that item, like Ctrl+click
static bool toggle_check_at(POINT pt) {
    if (!g_checkBoxes) return false;
    LVHITTESTINFO ht{};
    ht.pt = pt;
    int i = ListView_HitTest(g_list, &ht);
    if (i < 0 || !(ht.flags & LVHT_ONITEMSTATEICON)) return false;
    bool sel = ListView_GetItemState(g_list, i, LVIS_SELECTED) & LVIS_SELECTED;
    ListView_SetItemState(g_list, i, sel ? 0 : LVIS_SELECTED, LVIS_SELECTED);
    ListView_SetItemState(g_list, i, LVIS_FOCUSED, LVIS_FOCUSED);
    ListView_SetSelectionMark(g_list, i);
    SetFocus(g_list);
    return true;
}

// ---------------------------------------------------------------
// creation

static bool toggle_group_at(POINT pt);

// no item and no group header under the point (the list's background, also between
// a group's items)
static bool on_empty_space(POINT pt) {
    LVHITTESTINFO ht{};
    ht.pt = pt;
    int i = (int)SendMessageW(g_list, LVM_HITTEST, (WPARAM)-1, (LPARAM)&ht);   // -1: group hits too
    const UINT groupParts = LVHT_EX_GROUP_HEADER | LVHT_EX_GROUP_FOOTER | LVHT_EX_GROUP_COLLAPSE |
                            LVHT_EX_GROUP_STATEICON | LVHT_EX_GROUP_SUBSETLINK;
    return i < 0 && !(ht.flags & (groupParts | LVHT_ONITEM));
}

// a press on a group header's chevron toggles the group instead of selecting its items
static LRESULT CALLBACK list_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    static bool swallowUp = false;
    switch (msg) {
        case WM_PAINT:
            if (perf_on()) {
                double t0 = perf_now();
                LRESULT r = DefSubclassProc(h, msg, wp, lp);
                perf_paint("list", perf_now() - t0);
                return r;
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (toggle_group_at(pt) || toggle_check_at(pt)) { swallowUp = true; return 0; }
            if (msg == WM_LBUTTONDBLCLK && on_empty_space(pt)) {
                // a double-click on the background goes up a level
                LRESULT r = DefSubclassProc(h, msg, wp, lp);
                PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(IDM_NAV_UP, 0), 0);
                return r;
            }
            break;
        }
        case WM_LBUTTONUP:
            if (swallowUp) { swallowUp = false; return 0; }   // (the up would select the group)
            break;
        case WM_NOTIFY: {
            // the header reports to the list, not to the main window
            NMHDR* nm = (NMHDR*)lp;
            if (nm->hwndFrom != ListView_GetHeader(h)) break;
            if (nm->code == NM_CUSTOMDRAW) {
                // the dark header theme leaves the text color to us (black otherwise)
                auto* cd = (NMCUSTOMDRAW*)lp;
                if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->dwDrawStage == CDDS_ITEMPREPAINT) SetTextColor(cd->hdc, pal().text);
                return CDRF_DODEFAULT;
            }
            if (nm->code == NM_RCLICK) {
                POINT pt; GetCursorPos(&pt);
                columns_header_menu(pt);
                return TRUE;
            }
            if (nm->code == HDN_ITEMSTATEICONCLICK) {      // the select-all box
                if (list_all_selected()) list_select_none(); else list_select_all();
                return 0;
            }
            if (nm->code == HDN_ENDTRACKW || nm->code == HDN_DIVIDERDBLCLICKW || nm->code == HDN_ENDDRAG) {
                // (widths / order are final only after the list handled the notification)
                LRESULT r = DefSubclassProc(h, msg, wp, lp);
                PostMessageW(h, WM_LIST_COLUMNS_CHANGED, 0, 0);
                return r;
            }
            break;
        }
        case WM_LIST_COLUMNS_CHANGED:
            columns_widths_changed();
            return 0;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void list_create() {
    // LVS_SHAREIMAGELISTS: the system image lists must never be destroyed by the control
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                  LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA | LVS_EDITLABELS | LVS_SHAREIMAGELISTS;
    g_list = CreateWindowExW(0, WC_LISTVIEWW, L"", style,
                             0, 0, 100, 100, g_main, (HMENU)IDC_LIST, g_hinst, nullptr);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)content_font(), FALSE);
    ListView_SetExtendedListViewStyle(g_list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP | LVS_EX_HEADERDRAGDROP);
    columns_apply(columns_defaults(nullptr, nullptr));   // Name / Date modified / Type / Size until a folder lists
    groups_install(g_list);
    SetWindowSubclass(g_list, list_subclass, 1, 0);
    // no dotted focus rectangle (Explorer shows none)
    SendMessageW(g_list, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    list_apply_view();
}

// ---------------------------------------------------------------
// selection helpers

// Child pidls of the selected rows. The pointers stay owned by g_items and are valid
// until the next listing is applied on the UI thread — use them synchronously.
std::vector<PCITEMID_CHILD> list_selected_pidls() {
    std::vector<PCITEMID_CHILD> out;
    std::lock_guard<std::mutex> lk(g_itemsMx);
    int idx = -1;
    while ((idx = ListView_GetNextItem(g_list, idx, LVNI_SELECTED)) != -1)
        if (idx < (int)g_items.size() && g_items[idx] && g_items[idx]->pidl)
            out.push_back(g_items[idx]->pidl);
    return out;
}

int list_first_selected() { return ListView_GetNextItem(g_list, -1, LVNI_SELECTED); }

void navigation_open_selected() {
    std::vector<PITEMID_CHILD> full;
    for (PCITEMID_CHILD c : list_selected_pidls())
        if (PITEMID_CHILD f = static_cast<PITEMID_CHILD>(ILCombine(g_current, c))) full.push_back(f);
    // a single folder (or folder shortcut) opens in place; multiple items each get
    // their default verb, folders among them navigate to the first one
    PITEMID_CHILD navTarget = nullptr;
    for (PITEMID_CHILD f : full) {
        com_ptr<IShellItem> si;
        SFGAOF a = 0;
        if (SUCCEEDED(SHCreateItemFromIDList(f, IID_PPV_ARGS(si.put()))) && si)
            si->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &a);
        bool folder = (a & SFGAO_FOLDER) && !(a & SFGAO_STREAM);
        if (folder) { if (!navTarget) navTarget = f; continue; }
        if (full.size() == 1) navigation_open_abs(f);     // resolves folder shortcuts
        else shell_open_abs(g_main, f);
    }
    if (navTarget) {
        // a folder found by a search opens at its real location, not inside the result set
        PITEMID_CHILD real = search_is_results(g_current) ? search_real_location(navTarget) : nullptr;
        if (real) navigate_owned(real);
        else navigate_to(navTarget);
    }
    for (PITEMID_CHILD f : full) CoTaskMemFree(f);
}

// ---------------------------------------------------------------
// file ops glue

void list_copy_selection(bool cut) {
    auto sel = list_selected_pidls();
    if (sel.empty()) return;
    com_ptr<IShellFolder> psf;
    if (FAILED(bind_list(g_current, IID_PPV_ARGS(psf.put()))) || !psf) return;
    com_ptr<IDataObject> pdo;
    if (FAILED(dataobject_from_children(g_main, psf.get(), sel, pdo.put())) || !pdo) return;
    FORMATETC fe{ (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT),
                  nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM sm{};
    sm.tymed = TYMED_HGLOBAL;
    sm.hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (sm.hGlobal) {
        if (void* p = GlobalLock(sm.hGlobal)) {
            *(DWORD*)p = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            GlobalUnlock(sm.hGlobal);
        }
        if (FAILED(pdo->SetData(&fe, &sm, TRUE))) GlobalFree(sm.hGlobal);
    }
    // (another program may hold the clipboard: then nothing is cut, and no row is ghosted)
    bool onClipboard = SUCCEEDED(OleSetClipboard(pdo.get()));
    if (onClipboard) OleFlushClipboard();
    else MessageBeep(MB_ICONASTERISK);
    fileops_set_cut_items(cut && onClipboard ? sel : std::vector<PCITEMID_CHILD>{});
    InvalidateRect(g_list, nullptr, FALSE);
}

void list_delete_selection(bool permanent) {
    auto sel = list_selected_pidls();
    if (sel.empty()) return;
    com_ptr<IShellFolder> psf;
    if (FAILED(bind_list(g_current, IID_PPV_ARGS(psf.put()))) || !psf) return;
    fileops_delete_items(g_main, psf.get(), sel, permanent);
}

void list_rename_selected() {
    int idx = ListView_GetNextItem(g_list, -1, LVNI_FOCUSED | LVNI_SELECTED);
    if (idx < 0) idx = list_first_selected();
    if (idx < 0) return;
    SetFocus(g_list);
    ListView_EditLabel(g_list, idx);
}

void list_show_properties() { contextmenu_properties(g_main); }

void list_select_all()  { ListView_SetItemState(g_list, -1, LVIS_SELECTED, LVIS_SELECTED); }
void list_select_none() { ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED); }
void list_invert_selection() {
    int n = ListView_GetItemCount(g_list);
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    for (int i = 0; i < n; ++i) {
        UINT st = ListView_GetItemState(g_list, i, LVIS_SELECTED);
        ListView_SetItemState(g_list, i, (st & LVIS_SELECTED) ? 0 : LVIS_SELECTED, LVIS_SELECTED);
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, FALSE);
}

// ---------------------------------------------------------------
// context menu on list

void list_context_menu(POINT screenPt) {
    if (screenPt.x == -1 && screenPt.y == -1) {
        // keyboard popup (Shift+F10 / Apps key): at the focused item, or top-left
        RECT rc{};
        int idx = ListView_GetNextItem(g_list, -1, LVNI_FOCUSED | LVNI_SELECTED);
        if (idx < 0) idx = list_first_selected();
        POINT c{ 8, 8 };
        if (idx >= 0 && ListView_GetItemRect(g_list, idx, &rc, LVIR_ICON)) c = { rc.left, rc.bottom };
        ClientToScreen(g_list, &c);
        if (idx >= 0) show_items_context_menu(g_main, c);
        else show_bg_context_menu(g_main, c);
        return;
    }
    POINT c = screenPt;
    ScreenToClient(g_list, &c);
    LVHITTESTINFO ht{}; ht.pt = c;
    int idx = ListView_HitTest(g_list, &ht);
    if (idx >= 0 && (ht.flags & LVHT_ONITEM)) {
        if (!(ListView_GetItemState(g_list, idx, LVIS_SELECTED) & LVIS_SELECTED)) {
            ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED);
            ListView_SetItemState(g_list, idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        show_items_context_menu(g_main, screenPt);
    } else {
        list_select_none();
        show_bg_context_menu(g_main, screenPt);
    }
}

// ---------------------------------------------------------------
// notification handling

static void handle_getdispinfo(LV_DISPINFOW* di) {
    int i = di->item.iItem;
    // (asked before taking the lock: the list answers selection queries itself)
    bool checked = g_checkBoxes && (di->item.mask & LVIF_STATE) &&
                   (ListView_GetItemState(g_list, i, LVIS_SELECTED) & LVIS_SELECTED);
    std::lock_guard<std::mutex> lk(g_itemsMx);
    if (i < 0 || i >= (int)g_items.size()) return;
    Item* it = g_items[i];
    if (di->item.mask & LVIF_TEXT)
        wcsncpy_s(di->item.pszText, di->item.cchTextMax, columns_text(it, di->item.iSubItem), _TRUNCATE);
    if (di->item.mask & LVIF_IMAGE) {
        di->item.iImage = it->icon;
        if (g_view.thumbsExtra && g_view.lvView == LV_VIEW_ICON) {
            int img = -1;
            thumb_get_or_request(it, i, &img);
            // until the thumbnail arrives the row shows no image (the normal
            // imagelist is the thumbnail list, so shell icon indices don't apply)
            di->item.iImage = img >= 0 ? img : I_IMAGENONE;
        }
    }
    if (di->item.mask & LVIF_STATE) {
        di->item.state = INDEXTOOVERLAYMASK(it->overlay);
        if ((it->attrs & SFGAO_GHOSTED) || fileops_is_cut(it->pidl)) di->item.state |= LVIS_CUT;
        di->item.stateMask = LVIS_OVERLAYMASK | LVIS_CUT;
        if (g_checkBoxes) {
            di->item.state |= INDEXTOSTATEIMAGEMASK(checked ? 2 : 1);
            di->item.stateMask |= LVIS_STATEIMAGEMASK;
        }
    }
    if ((di->item.mask & LVIF_COLUMNS) && g_view.lvView == LV_VIEW_TILE) {
        // tiles: type + size under the name. The control passes its own arrays with
        // cColumns slots: fill them (with a pointer of ours it reads its zeroed buffer,
        // and column 0 is the name)
        UINT n = 0;
        const UINT* cols = columns_tile_lines(&n);
        n = std::min(n, di->item.cColumns);
        for (UINT k = 0; k < n; ++k) {
            if (di->item.puColumns) di->item.puColumns[k] = cols[k];
            if ((di->item.mask & LVIF_COLFMT) && di->item.piColFmt) di->item.piColFmt[k] = LVCFMT_LEFT;
        }
        di->item.cColumns = di->item.puColumns ? n : 0;
    }
}

// ---------------------------------------------------------------
// custom draw: group headers (theme colors, chevron) and This PC drive tiles

static RECT group_chevron_rect(const RECT& header) {
    return RECT{ header.left + S(4), header.top, header.left + S(4) + S(20), header.bottom };
}

static LRESULT draw_group_header(NMLVCUSTOMDRAW* cd) {
    int id = (int)cd->nmcd.dwItemSpec;
    RECT r = cd->rcText;           // the header band
    if (!ListView_GetGroupRect(g_list, id, LVGGR_HEADER, &r)) r = cd->rcText;
    HDC dc = cd->nmcd.hdc;
    const Palette& p = pal();
    fill_rect(dc, r, p.content);
    bool collapsed = ListView_GetGroupState(g_list, id, LVGS_COLLAPSED) & LVGS_COLLAPSED;
    RECT chev = group_chevron_rect(r);
    draw_glyph(dc, collapsed ? L'\xE76C' : L'\xE70D', chev, p.textSecondary, 10);
    HGDIOBJ of = SelectObject(dc, content_font_bold());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, p.text);
    RECT t{ chev.right + S(4), r.top, r.right - S(8), r.bottom };
    const wchar_t* name = groups_name(id);
    DrawTextW(dc, name, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    int tw = text_width(dc, name);
    SelectObject(dc, of);
    int y = (r.top + r.bottom) / 2;
    RECT line{ t.left + tw + S(12), y, r.right - S(8), y + 1 };
    if (line.right > line.left) fill_rect(dc, line, p.divider);
    return CDRF_SKIPDEFAULT;
}

struct DriveTile {                 // copied out of g_items: drawing calls back into the list
    std::wstring name, type;
    int icon = 0, overlay = 0;
    ULONGLONG driveTotal = 0, driveFree = 0;
};

static bool drive_tile(int idx, DriveTile* out) {
    if (g_view.lvView != LV_VIEW_TILE) return false;
    std::lock_guard<std::mutex> lk(g_itemsMx);
    if (idx < 0 || idx >= (int)g_items.size() || !g_items[idx]->driveType) return false;
    const Item* it = g_items[idx];
    *out = { it->name, it->type, it->icon, it->overlay, it->driveTotal, it->driveFree };
    return true;
}

static LRESULT draw_drive_tile(NMLVCUSTOMDRAW* cd, const DriveTile* it, int idx) {
    HDC dc = cd->nmcd.hdc;
    const Palette& p = pal();
    RECT r{};
    ListView_GetItemRect(g_list, idx, &r, LVIR_BOUNDS);
    // selection / hover background from the list's own (light/dark) theme
    UINT st = ListView_GetItemState(g_list, idx, LVIS_SELECTED);
    bool sel = st & LVIS_SELECTED, hot = cd->nmcd.uItemState & CDIS_HOT;
    bool focused = GetFocus() == g_list;
    int part = 1 /*LVP_LISTITEM*/;
    int state = sel ? (hot ? 6 /*LISS_HOTSELECTED*/ : focused ? 3 /*LISS_SELECTED*/ : 5 /*LISS_SELECTEDNOTFOCUS*/)
                    : hot ? 2 /*LISS_HOT*/ : 0;
    fill_rect(dc, r, p.content);
    if (state) {
        if (HTHEME th = OpenThemeData(g_list, L"ListView")) {
            DrawThemeBackground(th, dc, part, state, &r, nullptr);
            CloseThemeData(th);
        } else {
            fill_rect(dc, r, sel ? p.pressed : p.hover);
        }
    }
    // icon (48 px class) vertically centered on the left
    HIMAGELIST il = sys_imagelist(SHIL_EXTRALARGE);
    int iw = 48, ih = 48;
    ImageList_GetIconSize(il, &iw, &ih);
    int pad = S(6);
    ImageList_Draw(il, it->icon, dc, r.left + pad, r.top + (r.bottom - r.top - ih) / 2,
                   ILD_TRANSPARENT | INDEXTOOVERLAYMASK(it->overlay));
    int x = r.left + pad + iw + S(8), right = r.right - S(8);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    int lh = tm.tmHeight;
    int barH = S(12);
    int total = lh + S(4) + barH + S(4) + lh;
    int y = r.top + (r.bottom - r.top - total) / 2;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, p.text);
    RECT t{ x, y, right, y + lh };
    DrawTextW(dc, it->name.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    y += lh + S(4);
    if (it->driveTotal) {
        RECT bar{ x, y, right, y + barH };
        fill_rect(dc, bar, p.divider);
        RECT inner{ bar.left + 1, bar.top + 1, bar.right - 1, bar.bottom - 1 };
        fill_rect(dc, inner, blend(p.content, p.divider, 0.5));
        double used = 1.0 - (double)it->driveFree / (double)it->driveTotal;
        RECT fill = inner;
        fill.right = fill.left + (LONG)((inner.right - inner.left) * used);
        // Explorer turns the bar red when less than 10% is free
        fill_rect(dc, fill, used > 0.9 ? RGB(218, 38, 38) : p.accent);
        y += barH + S(4);
        std::wstring s = fmt_size(it->driveFree) + L" free of " + fmt_size(it->driveTotal);
        SetTextColor(dc, p.textSecondary);
        RECT t2{ x, y, right, y + lh };
        DrawTextW(dc, s.c_str(), -1, &t2, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    } else if (!it->type.empty()) {
        SetTextColor(dc, p.textSecondary);
        RECT t2{ x, y, right, y + lh };
        DrawTextW(dc, it->type.c_str(), -1, &t2, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    return CDRF_SKIPDEFAULT;
}

// Details rows (see apply_image_lists): the icon with its overlay ("cut" and hidden
// items half transparent) centered in the reserved slot, and, like Explorer, the
// item's check box while it is checked or under the mouse.
static void draw_row_images(NMLVCUSTOMDRAW* cd) {
    int idx = (int)cd->nmcd.dwItemSpec;
    RECT ic{};
    if (!ListView_GetItemRect(g_list, idx, &ic, LVIR_ICON)) return;
    bool checked = g_checkBoxes && (ListView_GetItemState(g_list, idx, LVIS_SELECTED) & LVIS_SELECTED);
    bool hot = cd->nmcd.uItemState & CDIS_HOT;
    int icon = -1, overlay = 0;
    bool cut = false;
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        if (idx < 0 || idx >= (int)g_items.size()) return;
        const Item* it = g_items[idx];
        icon = it->icon;
        overlay = it->overlay;
        cut = (it->attrs & SFGAO_GHOSTED) || fileops_is_cut(it->pidl);
    }
    HDC dc = cd->nmcd.hdc;
    int saved = SaveDC(dc);
    // Details: stay inside the Name column (it may be narrow, scrolled or moved)
    if (HWND hdr = g_view.lvView == LV_VIEW_DETAILS ? ListView_GetHeader(g_list) : nullptr) {
        RECT hr{};
        if (Header_GetItemRect(hdr, 0, &hr)) {
            MapWindowPoints(hdr, g_list, (POINT*)&hr, 2);
            IntersectClipRect(dc, hr.left, ic.top, hr.right, ic.bottom);
        }
    }
    if (icon >= 0) {
        HIMAGELIST il = sys_imagelist(SHIL_SMALL);
        int cx = 16, cy = 16;
        ImageList_GetIconSize(il, &cx, &cy);
        IMAGELISTDRAWPARAMS p{};
        p.cbSize = sizeof(p);
        p.himl = il;
        p.i = icon;
        p.hdcDst = dc;
        p.x = ic.left + (ic.right - ic.left - cx) / 2;
        p.y = ic.top + (ic.bottom - ic.top - cy) / 2;
        p.rgbBk = CLR_NONE;
        p.rgbFg = CLR_NONE;
        p.fStyle = ILD_TRANSPARENT | INDEXTOOVERLAYMASK(overlay);
        if (cut) { p.fState = ILS_ALPHA; p.Frame = 128; }
        ImageList_DrawIndirect(&p);
    }
    if (g_checkBoxes && (checked || hot)) {
        // the state slot sits right before the icon
        int box = S(13);
        RECT slot{ ic.left - s_boxW, ic.top, ic.left, ic.bottom };
        RECT b{ slot.left + (s_boxW - box) / 2, (slot.top + slot.bottom - box) / 2, 0, 0 };
        b.right = b.left + box;
        b.bottom = b.top + box;
        if (s_boxTheme)
            DrawThemeBackground(s_boxTheme, dc, BP_CHECKBOX,
                                checked ? (hot ? CBS_CHECKEDHOT : CBS_CHECKEDNORMAL)
                                        : (hot ? CBS_UNCHECKEDHOT : CBS_UNCHECKEDNORMAL), &b, nullptr);
        else
            DrawFrameControl(dc, &b, DFC_BUTTON, DFCS_BUTTONCHECK | (checked ? DFCS_CHECKED : 0));
    }
    RestoreDC(dc, saved);
}

// Win11 Explorer's item highlight: a flat, slightly rounded fill without border or
// focus rectangle (dark: white over the background, 6% hover, 10% selected as measured).
// The control is told the item is plain so it draws only the text on top.
static void draw_item_background(NMLVCUSTOMDRAW* cd) {
    int idx = (int)cd->nmcd.dwItemSpec;
    UINT st = ListView_GetItemState(g_list, idx, LVIS_SELECTED | LVIS_DROPHILITED);
    bool sel = st & LVIS_SELECTED, drop = st & LVIS_DROPHILITED;
    bool hot = (cd->nmcd.uItemState & CDIS_HOT) || drop;
    cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_HOT | CDIS_FOCUS);
    cd->clrText = pal().text;
    cd->clrTextBk = CLR_NONE;
    if (!sel && !hot) return;
    RECT r{};
    UINT code = g_view.lvView == LV_VIEW_DETAILS ? LVIR_BOUNDS : LVIR_SELECTBOUNDS;
    if (!ListView_GetItemRect(g_list, idx, &r, code)) return;
    double a = sel ? (hot ? 0.14 : 0.10) : 0.06;
    COLORREF c = blend(pal().content, g_dark ? RGB(255, 255, 255) : RGB(0, 0, 0), g_dark ? a : a * 0.75);
    fill_rrect(cd->nmcd.hdc, r, S(2), c);
}

static LRESULT handle_custom_draw(NMLVCUSTOMDRAW* cd) {
    // group headers arrive as their own CDDS_PREPAINT with dwItemType = LVCDI_GROUP
    if (cd->dwItemType == LVCDI_GROUP)
        return cd->nmcd.dwDrawStage == CDDS_PREPAINT ? draw_group_header(cd) : CDRF_DODEFAULT;
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
            int idx = (int)cd->nmcd.dwItemSpec;
            // a hover also has the control custom-draw the item once with an empty
            // rectangle on the window's own DC, outside any paint (a measuring pass): our
            // background, icon and tiles go by the item's real rectangle and would land on
            // screen over the row, hiding its text until the next repaint
            if (IsRectEmpty(&cd->nmcd.rc)) return CDRF_DODEFAULT;
            DriveTile t;
            if (drive_tile(idx, &t)) return draw_drive_tile(cd, &t, idx);
            draw_item_background(cd);
            return (padded_rows() ? CDRF_NOTIFYPOSTPAINT : CDRF_DODEFAULT) | CDRF_NEWFONT;
        }
        case CDDS_ITEMPOSTPAINT:
            if (padded_rows()) draw_row_images(cd);
            return CDRF_DODEFAULT;
    }
    return CDRF_DODEFAULT;
}

// a click on a group header's chevron collapses / expands the group
static bool toggle_group_at(POINT pt) {
    LVHITTESTINFO ht{};
    ht.pt = pt;
    SendMessageW(g_list, LVM_HITTEST, (WPARAM)-1, (LPARAM)&ht);   // -1: extended (group) hit info
    if (!(ht.flags & LVHT_EX_GROUP_HEADER)) return false;
    RECT r{};
    if (!ListView_GetGroupRect(g_list, ht.iGroup, LVGGR_HEADER, &r)) GetClientRect(g_list, &r);
    RECT chev = group_chevron_rect(r);
    chev.top = LONG_MIN / 2; chev.bottom = LONG_MAX / 2;     // the header hit test did the vertical part
    if (!PtInRect(&chev, pt)) return false;
    UINT st = ListView_GetGroupState(g_list, ht.iGroup, LVGS_COLLAPSED);
    ListView_SetGroupState(g_list, ht.iGroup, LVGS_COLLAPSED, (st & LVGS_COLLAPSED) ? 0 : LVGS_COLLAPSED);
    return true;
}

static int find_item(LPNMLVFINDITEMW fi) {
    if (!(fi->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || !fi->lvfi.psz) return -1;
    // type-ahead asks for a prefix (LVFI_PARTIAL / LVFI_SUBSTRING); LVFI_STRING alone is exact
    const bool prefix = fi->lvfi.flags & (LVFI_PARTIAL | 0x0004 /*LVFI_SUBSTRING*/);
    std::lock_guard<std::mutex> lk(g_itemsMx);
    int n = (int)g_items.size();
    if (!n) return -1;
    size_t len = wcslen(fi->lvfi.psz);
    int start = fi->iStart > 0 ? fi->iStart % n : 0;
    for (int k = 0; k < n; ++k) {
        int i = (start + k) % n;
        const std::wstring& name = g_items[i]->name;
        if (prefix ? _wcsnicmp(name.c_str(), fi->lvfi.psz, len) == 0 : _wcsicmp(name.c_str(), fi->lvfi.psz) == 0)
            return i;
        if (!(fi->lvfi.flags & LVFI_WRAP) && start + k + 1 >= n) break;
    }
    return -1;
}

static void handle_col_click(NMLISTVIEW* lv) { columns_header_click(lv->iSubItem); }

int list_sort_display_column() { return columns_sort_display_index(); }

static void handle_keydown(LV_KEYDOWN* kd) {
    bool shift = GetKeyState(VK_SHIFT) & 0x8000;
    switch (kd->wVKey) {
        case VK_BACK:
            navigation_go_back();      // Win11: Backspace = Back
            break;
        case VK_DELETE:
            list_delete_selection(shift);
            break;
    }
}

// the item being renamed, captured when editing starts: rows can move under the edit
// box (a refresh, a streamed batch sorting in)
static void edit_forget() {
    s_editIndex = -1;
    if (s_editPidl) { CoTaskMemFree(s_editPidl); s_editPidl = nullptr; }
    if (s_editFolder) { CoTaskMemFree(s_editFolder); s_editFolder = nullptr; }
    s_editName.clear();
    s_editHiddenExt.clear();
    s_editExtForced = false;
}

static void edit_capture(int idx) {
    edit_forget();
    std::lock_guard<std::mutex> lk(g_itemsMx);
    if (idx < 0 || idx >= (int)g_items.size() || !g_items[idx]->pidl) return;
    const Item* it = g_items[idx];
    s_editIndex = idx;
    s_editPidl = pidl_clone(it->pidl);
    s_editFolder = pidl_clone(g_current);       // the folder the item is in, whatever happens meanwhile
    s_editName = it->name;
    s_editHiddenExt = it->hiddenExt;
    s_editExtForced = it->extForced;
    // Windows hid it ("File name extensions" off, shortcuts, .zip folders): the rename
    // takes the name literally, so the extension has to be put back too
    if (s_editHiddenExt.empty() && !s_editExtForced && !it->fileName.empty()) {
        std::wstring ext = PathFindExtensionW(it->fileName.c_str());
        if (!ext.empty() && it->fileName == it->name + ext) s_editHiddenExt = ext;
    }
}

static void rename_commit(const std::wstring& newName) {
    PITEMID_CHILD child = s_editPidl, folder = s_editFolder;
    s_editPidl = s_editFolder = nullptr;
    bool unchanged = newName == s_editName;
    // the shown name may differ from Windows' own (File name extensions): rename to the
    // file name the user means — a hidden extension stays, a shown one is taken as typed
    std::wstring target = newName + s_editHiddenExt;
    bool exact = s_editExtForced;
    edit_forget();
    com_ptr<IShellFolder> psf;
    if (child && folder && !newName.empty() && !unchanged &&
        SUCCEEDED(bind_list(folder, IID_PPV_ARGS(psf.put()))) && psf)
        fileops_rename_item(g_main, psf.get(), child, target, exact);
    if (child) CoTaskMemFree(child);
    if (folder) CoTaskMemFree(folder);
}

// select the base name in the label editor ("report" of "report.txt"), like Explorer
static void select_base_name() {
    HWND edit = ListView_GetEditControl(g_list);
    if (!edit) return;
    int len = GetWindowTextLengthW(edit);
    std::wstring text((size_t)len + 1, L'\0');
    GetWindowTextW(edit, text.data(), len + 1);
    text.resize(len);
    size_t dot = text.find_last_of(L'.');
    bool folder = false;
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        if (s_editIndex >= 0 && s_editIndex < (int)g_items.size()) folder = g_items[s_editIndex]->isFolder;
    }
    // a hidden extension is not in the text: then the whole text is the base name.
    // (posted: the list view selects the whole text after this notification returns)
    if (!folder && s_editHiddenExt.empty() && dot != std::wstring::npos && dot > 0)
        PostMessageW(edit, EM_SETSEL, 0, (LPARAM)dot);
}

LRESULT list_notify(HWND, WPARAM, LPARAM lp) {
    NMHDR* hdr = (NMHDR*)lp;
    switch (hdr->code) {
        case NM_CUSTOMDRAW:
            return handle_custom_draw((NMLVCUSTOMDRAW*)lp);
        case NM_RCLICK: {
            // real right-click: open at the cursor. Keyboard requests arrive as
            // WM_CONTEXTMENU(-1,-1) instead, handled in main.cpp.
            POINT pt; GetCursorPos(&pt);
            list_context_menu(pt);
            return TRUE;    // handled: suppress the follow-up WM_CONTEXTMENU
        }
        case LVN_GETDISPINFOW:
            handle_getdispinfo((LV_DISPINFOW*)lp);
            return 0;
        case LVN_ODFINDITEMW:
            return find_item((LPNMLVFINDITEMW)lp);
        case LVN_COLUMNCLICK:
            handle_col_click((NMLISTVIEW*)lp);
            return 0;
        case LVN_KEYDOWN:
            handle_keydown((LV_KEYDOWN*)lp);
            return 0;
        case LVN_BEGINDRAG:
        case LVN_BEGINRDRAG:
            dnd_begin_list_drag();
            return 0;
        case LVN_BEGINLABELEDITW: {
            NMLVDISPINFOW* di = (NMLVDISPINFOW*)lp;
            edit_capture(di->item.iItem);
            if (!s_editPidl) return TRUE;        // no such row: no editing
            select_base_name();
            return FALSE;           // FALSE = allow editing
        }
        case LVN_ENDLABELEDITW: {
            NMLVDISPINFOW* di = (NMLVDISPINFOW*)lp;
            if (di->item.pszText) rename_commit(di->item.pszText);
            else edit_forget();
            return FALSE;           // the listing refresh shows the new name
        }
        case LVN_ITEMCHANGED:
        case LVN_ODSTATECHANGED: {
            // Ctrl+A on 10k items sends 10k notifications: coalesce into one update
            if (!s_selChangePosted)       // (not posted: the next change tries again)
                s_selChangePosted = PostMessageW(g_main, WM_SELECTION_CHANGED, 0, 0) != FALSE;
            return 0;
        }
        case LVN_ITEMACTIVATE:      // double-click and Enter
            navigation_open_selected();
            return 0;
    }
    return 0;
}

void list_on_selection_changed() {
    s_selChangePosted = false;
    status_update();
    detailspane_update();
    if (g_checkBoxes) enum_update_header_arrows();     // the select-all box follows
}
