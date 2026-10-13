#include "statusbar.h"
#include "theme.h"
#include "acc.h"

static std::wstring s_left, s_right;
static int s_hover = -1;
static HWND s_tip = nullptr;

// right-side view toggles
static const wchar_t  kGlyph[] = { L'\xE8FD', L'\xECA5' };
static const int      kCmd[]   = { IDM_VIEW_DETAILS, IDM_VIEW_LARGE };
static const wchar_t* kTip[]   = { L"Display information about each item in the window (Ctrl+Shift+6)",
                                   L"Display items by using large thumbnails (Ctrl+Shift+2)" };

static RECT toggle_rect(const RECT& rc, int i) {
    int w = S(28), gap = S(2), right = rc.right - S(10);
    int x = right - (2 - i) * w - (1 - i) * gap;
    int h = S(22);
    int y = (rc.bottom - h) / 2;
    return { x, y, x + w, y + h };
}

static bool toggle_on(int i) {
    return i == 0 ? g_pane->view.lvView == LV_VIEW_DETAILS
                  : (g_pane->view.lvView == LV_VIEW_ICON && g_pane->view.thumbsExtra == 1);
}

// screen readers: the status text, then the two view buttons
static const wchar_t* kName[] = { L"Details", L"Large icons" };
static const wchar_t* kKeys[] = { L"Ctrl+Shift+6", L"Ctrl+Shift+2" };
static const AccSource s_acc = [] {
    AccSource a;
    a.name = L"Status";
    a.role = ROLE_SYSTEM_STATUSBAR;
    a.count = [] { return 3; };
    a.child = [](int i) {
        AccChild c;
        RECT rc{}; GetClientRect(g_status, &rc);
        if (i == 0) {
            c.name = s_right.empty() ? s_left : s_left + L", " + s_right;
            c.role = ROLE_SYSTEM_STATICTEXT;
            c.state = STATE_SYSTEM_READONLY;
            c.rc = { S(12), 0, rc.right - S(80), rc.bottom };
        } else {
            c.name = kName[i - 1];
            c.shortcut = kKeys[i - 1];
            c.role = ROLE_SYSTEM_PUSHBUTTON;
            c.action = L"Press";
            c.rc = toggle_rect(rc, i - 1);
            if (toggle_on(i - 1)) c.state |= STATE_SYSTEM_PRESSED;
        }
        return c;
    };
    a.action = [](int i) { if (i > 0) PostMessageW(g_main, WM_COMMAND, kCmd[i - 1], 0); };
    return a;
}();

static LRESULT CALLBACK status_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_GETOBJECT: return acc_get_object(hwnd, wp, lp, &s_acc);
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            BufferedPaint bp(hwnd);
            const Palette& p = pal();
            fill_rect(bp.dc, bp.rc, p.content);
            SetTextColor(bp.dc, p.text);
            RECT t{ S(12), 0, bp.rc.right - S(80), bp.rc.bottom };
            std::wstring s = s_left;
            if (!s_right.empty()) s += L"    |    " + s_right;
            DrawTextW(bp.dc, s.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            for (int i = 0; i < 2; ++i) {
                RECT r = toggle_rect(bp.rc, i);
                if (toggle_on(i)) fill_rrect(bp.dc, r, S(4), p.pressed);
                else if (s_hover == i) fill_rrect(bp.dc, r, S(4), p.hover);
                draw_glyph(bp.dc, kGlyph[i], r, p.text, 14);
            }
            return 0;
        }
        case WM_SIZE:
            if (s_tip) {
                RECT rc{}; GetClientRect(hwnd, &rc);
                for (int i = 0; i < 2; ++i) {
                    TTTOOLINFOW ti{ sizeof(ti) };
                    ti.hwnd = hwnd; ti.uId = (UINT_PTR)i; ti.rect = toggle_rect(rc, i);
                    SendMessageW(s_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT rc{}; GetClientRect(hwnd, &rc);
            int h = -1;
            for (int i = 0; i < 2; ++i) { RECT r = toggle_rect(rc, i); if (PtInRect(&r, pt)) h = i; }
            if (h != s_hover) { s_hover = h; InvalidateRect(hwnd, nullptr, FALSE); }
            TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&te);
            return 0;
        }
        case WM_MOUSELEAVE:
            s_hover = -1; InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
            if (s_hover >= 0) SendMessageW(g_main, WM_COMMAND, kCmd[s_hover], 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND statusbar_create(HWND parent) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = g_hinst;
    wc.lpfnWndProc = status_proc;
    wc.lpszClassName = L"ExplorerPlusStatus";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    g_status = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
                               0, 0, 100, 26, parent, (HMENU)IDC_STATUS, g_hinst, nullptr);
    s_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            0, 0, 0, 0, g_status, nullptr, g_hinst, nullptr);
    for (int i = 0; s_tip && i < 2; ++i) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = g_status;
        ti.uId = (UINT_PTR)i;
        ti.lpszText = const_cast<LPWSTR>(kTip[i]);
        SendMessageW(s_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    return g_status;
}

void statusbar_set(const std::wstring& left, const std::wstring& right) {
    if (left == s_left && right == s_right) return;
    s_left = left;
    s_right = right;
    // window text mirrors the content (accessibility, automation)
    std::wstring all = right.empty() ? left : left + L" | " + right;
    SetWindowTextW(g_status, all.c_str());
    InvalidateRect(g_status, nullptr, FALSE);
    acc_notify(g_status, EVENT_OBJECT_NAMECHANGE, 0);
}

void statusbar_refresh() {
    if (g_status) InvalidateRect(g_status, nullptr, FALSE);
}
