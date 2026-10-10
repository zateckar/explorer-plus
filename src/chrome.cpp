#include "chrome.h"
#include "theme.h"
#include "layout.h"
#include <dwmapi.h>

static bool s_active = true;
static int s_hover = -1;       // 0 min, 1 max, 2 close
static int s_pressed = -1;

static int frame_y(HWND hwnd) {
    UINT dpi = GetDpiForWindow(hwnd);
    return GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

int chrome_buttons_width() { return 3 * S(46); }
bool chrome_active() { return s_active; }

static RECT button_rect(const RECT& client, int i) {
    int w = S(46);
    int right = client.right - (2 - i) * w;
    return { right - w, 0, right, layout_caption_h() };
}

static int button_at(HWND hwnd, POINT clientPt) {
    RECT rc{}; GetClientRect(hwnd, &rc);
    for (int i = 0; i < 3; ++i) {
        RECT b = button_rect(rc, i);
        if (PtInRect(&b, clientPt)) return i;
    }
    return -1;
}

static void invalidate_buttons(HWND hwnd) {
    RECT rc{}; GetClientRect(hwnd, &rc);
    RECT r{ rc.right - chrome_buttons_width(), 0, rc.right, layout_caption_h() };
    InvalidateRect(hwnd, &r, FALSE);
}

void chrome_theme_changed(HWND hwnd) {
    BOOL dark = g_dark;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
}

void chrome_init(HWND hwnd) {
    chrome_theme_changed(hwnd);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT hit_test(HWND hwnd, LPARAM lp) {
    // left / right / bottom borders are still real non-client area
    LRESULT def = DefWindowProcW(hwnd, WM_NCHITTEST, 0, lp);
    if (def != HTCLIENT) return def;
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ScreenToClient(hwnd, &pt);
    RECT rc{}; GetClientRect(hwnd, &rc);
    int b = button_at(hwnd, pt);
    if (!IsZoomed(hwnd) && b < 0) {
        int fy = frame_y(hwnd);
        if (pt.y < fy) {                 // the top resize border lives in our client area
            if (pt.x < fy * 2) return HTTOPLEFT;
            if (pt.x >= rc.right - fy * 2) return HTTOPRIGHT;
            return HTTOP;
        }
    }
    if (pt.y < layout_caption_h()) {
        if (b == 0) return HTMINBUTTON;
        if (b == 1) return HTMAXBUTTON;   // Win11 shows snap layouts for this
        if (b == 2) return HTCLOSE;
        return HTCAPTION;
    }
    return HTCLIENT;
}

static int ht_to_button(WPARAM ht) {
    switch (ht) {
        case HTMINBUTTON: return 0;
        case HTMAXBUTTON: return 1;
        case HTCLOSE:     return 2;
        default:          return -1;
    }
}

bool chrome_handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    switch (msg) {
        case WM_NCCALCSIZE: {
            if (!wp) return false;
            auto* p = (NCCALCSIZE_PARAMS*)lp;
            LONG top = p->rgrc[0].top;
            DefWindowProcW(hwnd, msg, wp, lp);     // side + bottom borders
            p->rgrc[0].top = top;                  // no title bar
            if (IsZoomed(hwnd)) p->rgrc[0].top += frame_y(hwnd);   // maximized: frame is off-screen
            *result = 0;
            return true;
        }
        case WM_NCHITTEST:
            *result = hit_test(hwnd, lp);
            return true;
        case WM_NCACTIVATE:
            s_active = wp != FALSE;
            InvalidateRect(hwnd, nullptr, FALSE);
            EnumChildWindows(hwnd, [](HWND h, LPARAM) -> BOOL {
                InvalidateRect(h, nullptr, FALSE); return TRUE; }, 0);
            *result = DefWindowProcW(hwnd, msg, wp, -1);   // -1: don't paint a classic frame
            return true;
        case WM_NCMOUSEMOVE: {
            int b = ht_to_button(wp);
            if (b != s_hover) { s_hover = b; invalidate_buttons(hwnd); }
            TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE | TME_NONCLIENT, hwnd, 0 };
            TrackMouseEvent(&te);
            if (b < 0) return false;
            *result = 0;
            return true;
        }
        case WM_NCMOUSELEAVE:
            if (s_hover >= 0 || s_pressed >= 0) { s_hover = s_pressed = -1; invalidate_buttons(hwnd); }
            return false;
        case WM_MOUSEMOVE:
            if (s_hover >= 0) { s_hover = -1; invalidate_buttons(hwnd); }
            return false;
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK: {
            int b = ht_to_button(wp);
            if (b < 0) return false;
            s_pressed = b;
            invalidate_buttons(hwnd);
            *result = 0;
            return true;
        }
        case WM_NCLBUTTONUP: {
            int b = ht_to_button(wp);
            if (b < 0) return false;
            bool fire = b == s_pressed;
            s_pressed = -1;
            invalidate_buttons(hwnd);
            if (fire) {
                WPARAM sc = b == 0 ? SC_MINIMIZE : b == 2 ? SC_CLOSE : (IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE);
                s_hover = -1;
                PostMessageW(hwnd, WM_SYSCOMMAND, sc, 0);
            }
            *result = 0;
            return true;
        }
    }
    return false;
}

void chrome_paint(HDC dc, const RECT& client) {
    const Palette& p = pal();
    COLORREF bg = s_active ? p.caption : p.captionInactive;
    RECT cap{ 0, 0, client.right, layout_caption_h() };
    fill_rect(dc, cap, bg);
    static const wchar_t kGlyph[3] = { L'\xE921', L'\xE922', L'\xE8BB' };   // min, max, close
    HWND hwnd = g_main;
    for (int i = 0; i < 3; ++i) {
        RECT r = button_rect(client, i);
        COLORREF fg = s_active ? p.text : p.textDisabled;
        if (i == 2 && (s_hover == 2 || s_pressed == 2)) {
            fill_rect(dc, r, s_pressed == 2 ? blend(p.closeHover, RGB(0, 0, 0), 0.1) : p.closeHover);
            fg = RGB(255, 255, 255);
        } else if (s_hover == i || s_pressed == i) {
            COLORREF base = g_dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
            fill_rect(dc, r, blend(bg, base, s_pressed == i ? 0.04 : 0.07));
        }
        wchar_t g = kGlyph[i];
        if (i == 1 && hwnd && IsZoomed(hwnd)) g = L'\xE923';                    // restore
        draw_glyph(dc, g, r, fg, 10);
    }
}
