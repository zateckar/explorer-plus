#include "layout.h"
#include "settings.h"
#include "navbar.h"
#include "tabs.h"
#include "detailspane.h"
#include "theme.h"
#include "chrome.h"
#include "cmdbar.h"
#include "statusbar.h"
#include <algorithm>
using std::max;
using std::min;

int g_splitterRight = 260;    // details pane width (96-dpi pixels)

int layout_caption_h() { return S(40); }
int layout_nav_h()     { return S(48); }
int layout_cmdbar_h()  { return g_cmdBarVisible ? S(48) : 0; }
int layout_status_h()  { return S(26); }
static int splitter_w() { return S(5); }

static RECT s_tree{}, s_list{}, s_pane{}, s_search{};
static bool s_details = false;

static bool details_shown(int clientW) { return detailspane_visible() && clientW > S(760); }

RECT layout_search_frame() { return s_search; }

void layout_create_splitters() {
    if (!g_main) return;
    // Win11 navigation pane: no connecting lines, full-row hover/selection
    DWORD treeStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                      TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
                      TVS_TRACKSELECT | TVS_FULLROWSELECT | TVS_NOHSCROLL | TVS_EDITLABELS;
    g_tree = CreateWindowExW(0, WC_TREEVIEWW, L"", treeStyle,
                             0, 0, 100, 100, g_main, (HMENU)IDC_TREE, g_hinst, nullptr);
    SendMessageW(g_tree, WM_SETFONT, (WPARAM)content_font(), FALSE);
}

// The splitters are the gaps between the panes (main-window client area).
DragKind layout_splitter_kind(HWND hwnd, POINT pt) {
    if (pt.y < s_list.top || pt.y >= s_list.bottom) return DragKind::None;
    if (g_treeVisible && pt.x >= s_tree.right && pt.x < s_list.left) return DragKind::Left;
    if (s_details && pt.x >= s_list.right && pt.x < s_pane.left) return DragKind::Right;
    return DragKind::None;
}

void layout_splitter_drag(HWND hwnd, POINT pt, DragKind kind) {
    RECT rc{}; GetClientRect(hwnd, &rc);
    if (kind == DragKind::Left) {
        int nx = max((int)S(120), min((int)pt.x, (int)rc.right - S(260)));
        g_splitterX = MulDiv(nx, 96, theme_dpi());
    } else if (kind == DragKind::Right) {
        int nw = max((int)S(140), min((int)(rc.right - pt.x), (int)S(480)));
        g_splitterRight = MulDiv(nw, 96, theme_dpi());
    }
    layout_resize(hwnd, rc.right, rc.bottom);
}

void layout_relayout() {
    if (!g_main) return;
    RECT rc{}; GetClientRect(g_main, &rc);
    layout_resize(g_main, rc.right, rc.bottom);
    InvalidateRect(g_main, nullptr, FALSE);
}

void layout_resize(HWND main, int w, int h) {
    if (w <= 60 || h <= 60) return;
    HDWP dw = BeginDeferWindowPos(12);
    auto place = [&](HWND hw, int x, int y, int cx, int cy) {
        if (hw) dw = DeferWindowPos(dw, hw, nullptr, x, y, max(0, cx), max(0, cy),
                                    SWP_NOZORDER | SWP_NOACTIVATE);
    };

    // --- caption row: tabs (caption buttons are painted by the main window) ---
    int cap = layout_caption_h();
    place(tabstrip_hwnd(), 0, 0, w - chrome_buttons_width(), cap);

    // --- navigation row: back/forward/up/refresh | address | search ---
    int y = cap, navH = layout_nav_h();
    int boxH = S(32), boxY = y + (navH - boxH) / 2;
    int navW = navbar_width();
    place(g_navbar, S(6), boxY, navW, boxH);
    int searchW = max((int)S(160), min((int)S(340), w / 5));
    int addrX = S(6) + navW + S(8);
    int searchX = w - S(12) - searchW;
    if (searchX - S(8) - addrX < S(160)) {               // narrow window: shrink search first
        searchW = max((int)S(80), w - S(12) - addrX - S(8) - S(160));
        searchX = w - S(12) - searchW;
    }
    place(g_address, addrX, boxY, searchX - S(8) - addrX, boxH);
    s_search = { searchX, boxY, searchX + searchW, boxY + boxH };
    {   // the search edit sits inside the painted rounded frame, leaving room for the glyph
        HDC dc = GetDC(main);
        HGDIOBJ of = SelectObject(dc, g_font);
        TEXTMETRICW tm{}; GetTextMetricsW(dc, &tm);
        SelectObject(dc, of);
        ReleaseDC(main, dc);
        int eh = tm.tmHeight + 2;
        place(g_search, searchX + S(12), boxY + (boxH - eh) / 2, searchW - S(12) - S(36), eh);
    }

    // --- command bar ---
    y += navH;
    place(cmdbar_hwnd(), 0, y, w, layout_cmdbar_h());
    if (cmdbar_hwnd()) ShowWindow(cmdbar_hwnd(), g_cmdBarVisible ? SW_SHOWNA : SW_HIDE);
    y += layout_cmdbar_h();

    // --- status bar ---
    int stH = layout_status_h();
    place(g_status, 0, h - stH, w, stH);
    g_statusH = stH;

    // --- panes ---
    int top = y, bottom = max(top + 50, h - stH);
    s_details = details_shown(w);
    int sw = splitter_w();
    // clamp the saved widths to this window (they may come from a larger monitor / DPI);
    // the preferences themselves are kept
    int minList = S(200);
    int treeW = g_treeVisible ? max(0, min((int)S(g_splitterX), w - sw - minList)) : 0;
    int paneW = s_details ? max(0, min((int)S(g_splitterRight), w - treeW - 2 * sw - minList)) : 0;
    s_tree = { 0, top, treeW, bottom };
    int listX = g_treeVisible ? treeW + sw : 0;
    int listR = s_details ? w - paneW - sw : w;
    if (listR - listX < S(60)) listR = listX + S(60);
    s_list = { listX, top, listR, bottom };
    s_pane = { w - paneW, top, w, bottom };
    if (g_treeVisible) place(g_tree, s_tree.left, top, treeW, bottom - top);
    place(g_list, s_list.left, top, s_list.right - s_list.left, bottom - top);
    HWND pane = detailspane_hwnd();
    if (pane) {
        if (s_details) place(pane, s_pane.left, top, paneW, bottom - top);
        ShowWindow(pane, s_details ? SW_SHOWNA : SW_HIDE);
    }
    EndDeferWindowPos(dw);
}

void layout_paint(HDC dc, const RECT& client) {
    const Palette& p = pal();
    // navigation row
    RECT nav{ 0, layout_caption_h(), client.right, layout_caption_h() + layout_nav_h() };
    fill_rect(dc, nav, p.layer);
    if (!g_cmdBarVisible) {          // the command bar draws this line when shown
        RECT line{ 0, nav.bottom - 1, client.right, nav.bottom };
        fill_rect(dc, line, p.divider);
    }
    // search box: rounded frame, glyph on the right, accent underline when focused
    bool focused = GetFocus() == g_search;
    RECT sb = s_search;
    fill_rrect(dc, sb, S(4), p.boxFill, p.boxBorder);
    if (focused) {
        RECT ul{ sb.left + S(1), sb.bottom - S(2), sb.right - S(1), sb.bottom };
        fill_rect(dc, ul, p.accent);
    }
    RECT gl{ sb.right - S(34), sb.top, sb.right - S(4), sb.bottom };
    draw_glyph(dc, L'\xE721', gl, p.textSecondary, 14);       // magnifier
    // content area gaps (splitters) and anything not covered by a child
    RECT content{ 0, s_list.top, client.right, s_list.bottom };
    fill_rect(dc, content, p.content);
    if (g_treeVisible) {
        int x = s_tree.right + splitter_w() / 2;
        RECT d{ x, s_list.top, x + 1, s_list.bottom };
        fill_rect(dc, d, p.divider);
    }
    if (s_details) {
        int x = s_list.right + splitter_w() / 2;
        RECT d{ x, s_list.top, x + 1, s_list.bottom };
        fill_rect(dc, d, p.divider);
    }
}
