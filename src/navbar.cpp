#include "navbar.h"
#include "navigation.h"
#include "tabs.h"
#include "theme.h"
#include "acc.h"
#include "kbdrow.h"

// Segoe Fluent Icons / MDL2 share these code points (the Notepad's and the player's as in the
// "..." menu). The last one, "...", holds the command bar's commands while that row is hidden.
static const wchar_t  kGlyph[] = { L'\xE72B', L'\xE72A', L'\xE74A', L'\xE72C', L'\xE70B', L'\xE768', L'\xE712' };
static const int      kCmd[]   = { IDM_NAV_BACK, IDM_NAV_FORWARD, IDM_NAV_UP, IDM_NAV_REFRESH, IDM_NOTEPAD, IDM_PLAYER, IDM_TOOLS_MENU };
static const wchar_t* kTip[]   = { L"Back (Alt+Left Arrow)", L"Forward (Alt+Right Arrow)",
                                   L"Up to parent folder (Alt+Up Arrow)", L"Refresh (F5)", L"Notepad", L"Player",
                                   L"New, Cut, Copy, Paste, Sort, View and more" };
constexpr int kMax = 7;
static int count() { return g_cmdBarVisible ? kMax - 1 : kMax; }

static HWND s_bar = nullptr, s_tip = nullptr;
static int s_hover = -1, s_pressed = -1;
static KbdRow s_kbd;

static int btn_w() { return S(36); }

int navbar_width() { return count() * btn_w() + (count() - 1) * S(4); }

static RECT btn_rect(int i, int h) {
    int x = i * (btn_w() + S(4));
    return { x, 0, x + btn_w(), h };
}

static bool btn_enabled(int i) {
    switch (kCmd[i]) {
        case IDM_NAV_BACK:    return tabmodel_can_back();
        case IDM_NAV_FORWARD: return tabmodel_can_forward();
        case IDM_NAV_UP:      return navigation_can_up();
        default:              return true;
    }
}

static int hit(HWND hwnd, POINT pt) {
    RECT rc{}; GetClientRect(hwnd, &rc);
    for (int i = 0; i < count(); ++i) {
        RECT r = btn_rect(i, rc.bottom);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

static void update_tips(HWND hwnd) {
    if (!s_tip) return;
    RECT rc{}; GetClientRect(hwnd, &rc);
    for (int i = 0; i < kMax; ++i) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = hwnd;
        ti.uId = (UINT_PTR)i;
        ti.rect = i < count() ? btn_rect(i, rc.bottom) : RECT{};
        SendMessageW(s_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
}

static void add_tooltips(HWND hwnd) {
    s_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            0, 0, 0, 0, hwnd, nullptr, g_hinst, nullptr);
    if (!s_tip) return;
    for (int i = 0; i < kMax; ++i) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = hwnd;
        ti.uId = (UINT_PTR)i;
        ti.lpszText = const_cast<LPWSTR>(kTip[i]);
        SendMessageW(s_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

// screen readers: the buttons by name, shortcut and whether they can be used now
static const wchar_t* kName[] = { L"Back", L"Forward", L"Up", L"Refresh", L"Notepad", L"Player", L"See more" };
static const wchar_t* kKeys[] = { L"Alt+Left Arrow", L"Alt+Right Arrow", L"Alt+Up Arrow", L"F5", L"", L"", L"" };
static const AccSource s_acc = [] {
    AccSource a;
    a.name = L"Navigation";
    a.role = ROLE_SYSTEM_TOOLBAR;
    a.count = [] { return count(); };
    a.child = [](int i) {
        AccChild c;
        c.name = kName[i];
        c.shortcut = kKeys[i];
        c.role = kCmd[i] == IDM_TOOLS_MENU ? ROLE_SYSTEM_BUTTONMENU : ROLE_SYSTEM_PUSHBUTTON;
        RECT rc{}; GetClientRect(s_bar, &rc);
        c.rc = btn_rect(i, rc.bottom);
        if (!btn_enabled(i)) c.state |= STATE_SYSTEM_UNAVAILABLE;
        if (s_hover == i) c.state |= STATE_SYSTEM_HOTTRACKED;
        if (s_pressed == i) c.state |= STATE_SYSTEM_PRESSED;
        c.state |= STATE_SYSTEM_FOCUSABLE | (s_kbd.focus == i ? STATE_SYSTEM_FOCUSED : 0);
        c.action = L"Press";
        return c;
    };
    a.action = [](int i) { PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(kCmd[i], 0), 0); };
    a.focus = [] { return s_kbd.focus; };
    return a;
}();

static LRESULT CALLBACK bar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT kr = 0;
    if (s_kbd.handle(hwnd, msg, wp, &kr, count(), btn_enabled,
                     [](int i) { SendMessageW(g_main, WM_COMMAND, MAKEWPARAM(kCmd[i], 0), 0); }))
        return kr;
    switch (msg) {
        case WM_GETOBJECT: return acc_get_object(hwnd, wp, lp, &s_acc);
        case WM_CREATE: add_tooltips(hwnd); return 0;
        case WM_SIZE: update_tips(hwnd); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            BufferedPaint bp(hwnd);
            const Palette& p = pal();
            fill_rect(bp.dc, bp.rc, p.layer);
            for (int i = 0; i < count(); ++i) {
                bool en = btn_enabled(i);
                RECT r = btn_rect(i, bp.rc.bottom);
                if (en && (s_hover == i || s_pressed == i))
                    fill_rrect(bp.dc, r, S(4), s_pressed == i ? p.pressed : p.hover);
                draw_glyph(bp.dc, kGlyph[i], r, en ? p.text : p.textDisabled, 16);
                if (s_kbd.focus == i) kbdrow_ring(bp.dc, r, S(4));
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            int h = hit(hwnd, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (h != s_hover) { s_hover = h; InvalidateRect(hwnd, nullptr, FALSE); }
            TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&te);
            return 0;
        }
        case WM_MOUSELEAVE:
            s_hover = -1;
            if (GetCapture() != hwnd) s_pressed = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            int h = hit(hwnd, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (h < 0 || !btn_enabled(h)) return 0;
            s_pressed = h;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            int h = hit(hwnd, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            int pressed = s_pressed;
            s_pressed = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (h >= 0 && h == pressed && btn_enabled(h))
                SendMessageW(g_main, WM_COMMAND, MAKEWPARAM(kCmd[h], 0), 0);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (s_pressed >= 0) { s_pressed = -1; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND navbar_create(HWND parent) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = g_hinst;
    wc.lpfnWndProc = bar_proc;
    wc.lpszClassName = L"ExplorerPlusNavBar";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc);
    s_bar = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
                            0, 0, navbar_width(), S(32), parent, (HMENU)IDC_NAVBAR, g_hinst, nullptr);
    return s_bar;
}

void navbar_refresh() {
    if (!s_bar) return;
    update_tips(s_bar);
    InvalidateRect(s_bar, nullptr, FALSE);
    acc_notify(s_bar, EVENT_OBJECT_STATECHANGE, -1);    // Back / Forward / Up may have changed
}

POINT navbar_menu_point() {
    RECT rc{}; GetClientRect(s_bar, &rc);
    RECT r = btn_rect(kMax - 1, rc.bottom);
    POINT pt{ r.left, r.bottom + S(4) };
    ClientToScreen(s_bar, &pt);
    return pt;
}
