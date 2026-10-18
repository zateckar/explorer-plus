#include "screenshot.h"
#include "appstate.h"
#include "imageedit.h"
#include "settings.h"
#include "theme.h"
#include <windowsx.h>
#include <algorithm>
#include <vector>

namespace {

const UINT_PTR TIMER_RETRY = 1, TIMER_DELAY = 2;
const int kHotkey = 1;
HWND s_msg = nullptr;                   // owns the hotkey and the timers
bool s_hotkey = false;

// the region picker: the screen as it was, dimmed, the region chosen bright
struct Picker {
    HWND wnd = nullptr;
    int vx = 0, vy = 0, vw = 0, vh = 0;         // the virtual screen
    HDC bright = nullptr, dim = nullptr;
    HBITMAP brightBmp = nullptr, dimBmp = nullptr;
    HGDIOBJ brightOld = nullptr, dimOld = nullptr;
    const uint32_t* bits = nullptr;             // the screen's pixels (top-down, vw x vh)
    std::vector<RECT> monitors;                 // client coordinates
    bool dragging = false;
    POINT anchor{};
    RECT sel{};                                 // client coordinates; empty: none yet
    HFONT font = nullptr;
    int dpi = 96;
};
Picker* s_pick = nullptr;

int px(int v) { return MulDiv(v, s_pick ? s_pick->dpi : 96, 96); }

const wchar_t kHint[] = L"Drag to capture a region  \x00B7  Click to capture the whole screen  \x00B7  Esc to cancel";

RECT hint_rect(HDC dc, const RECT& mon) {
    int tw = text_width(dc, kHint);
    int w = tw + px(32), h = px(40);
    int x = (mon.left + mon.right - w) / 2, y = mon.top + px(24);
    return { x, y, x + w, y + h };
}

std::wstring size_text(const RECT& r) {
    return std::to_wstring(r.right - r.left) + L" \x00D7 " + std::to_wstring(r.bottom - r.top);
}

// the size shown under the region (inside it near the screen's bottom)
RECT label_rect(HDC dc, const RECT& sel) {
    std::wstring t = size_text(sel);
    int w = text_width(dc, t.c_str()) + px(16), h = px(26);
    int x = sel.left, y = sel.bottom + px(6);
    if (y + h > s_pick->vh) y = sel.bottom - h - px(6);
    x = std::clamp(x, 0, std::max(0, s_pick->vw - w));
    y = std::max(0, y);
    return { x, y, x + w, y + h };
}

// what changes with the region: its frame and its label
RECT decor_rect(const RECT& sel) {
    if (IsRectEmpty(&sel)) return {};
    HDC dc = GetDC(s_pick->wnd);
    HGDIOBJ of = SelectObject(dc, s_pick->font);
    RECT l = label_rect(dc, sel);
    SelectObject(dc, of);
    ReleaseDC(s_pick->wnd, dc);
    RECT r = sel, u;
    InflateRect(&r, px(3), px(3));
    UnionRect(&u, &r, &l);
    return u;
}

void paint(Picker& p) {
    PAINTSTRUCT ps;
    HDC win = BeginPaint(p.wnd, &ps);
    RECT r = ps.rcPaint;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w > 0 && h > 0) {
        HDC dc = CreateCompatibleDC(win);
        HBITMAP bmp = CreateCompatibleBitmap(win, w, h);
        HGDIOBJ ob = SelectObject(dc, bmp);
        SetViewportOrgEx(dc, -r.left, -r.top, nullptr);         // (drawn in client coordinates)
        BitBlt(dc, r.left, r.top, w, h, p.dim, r.left, r.top, SRCCOPY);
        const Palette& c = pal();
        HGDIOBJ of = SelectObject(dc, p.font);
        SetBkMode(dc, TRANSPARENT);
        if (!IsRectEmpty(&p.sel)) {
            RECT s = p.sel;
            BitBlt(dc, s.left, s.top, s.right - s.left, s.bottom - s.top, p.bright, s.left, s.top, SRCCOPY);
            RECT f = s;
            InflateRect(&f, 1, 1);
            HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
            FrameRect(dc, &f, white);
            InflateRect(&f, 1, 1);
            HBRUSH accent = CreateSolidBrush(c.accent);
            FrameRect(dc, &f, accent);
            DeleteObject(accent);
            RECT l = label_rect(dc, s);
            fill_rrect(dc, l, px(4), RGB(32, 32, 32));
            SetTextColor(dc, RGB(255, 255, 255));
            std::wstring t = size_text(s);
            DrawTextW(dc, t.c_str(), -1, &l, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (!p.dragging)                        // (the hint: until the drag starts)
            for (const RECT& mon : p.monitors) {
                RECT hr = hint_rect(dc, mon);
                fill_rrect(dc, hr, px(8), RGB(32, 32, 32), RGB(80, 80, 80));
                SetTextColor(dc, RGB(255, 255, 255));
                DrawTextW(dc, kHint, -1, &hr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        SelectObject(dc, of);
        BitBlt(win, r.left, r.top, w, h, dc, r.left, r.top, SRCCOPY);
        SelectObject(dc, ob);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    EndPaint(p.wnd, &ps);
}

void close_picker() {
    Picker* p = s_pick;
    if (!p) return;
    s_pick = nullptr;
    if (p->wnd) {
        HWND w = p->wnd;
        p->wnd = nullptr;
        DestroyWindow(w);
    }
    if (p->bright) { SelectObject(p->bright, p->brightOld); DeleteDC(p->bright); }
    if (p->dim) { SelectObject(p->dim, p->dimOld); DeleteDC(p->dim); }
    if (p->brightBmp) DeleteObject(p->brightBmp);
    if (p->dimBmp) DeleteObject(p->dimBmp);
    if (p->font) DeleteObject(p->font);
    delete p;
}

// `r` (client coordinates) to the picture editor
void finish(RECT r) {
    Picker& p = *s_pick;
    RECT all{ 0, 0, p.vw, p.vh };
    IntersectRect(&r, &r, &all);
    int w = r.right - r.left, h = r.bottom - r.top;
    std::vector<uint32_t> px;
    if (w > 0 && h > 0) {
        px.resize((size_t)w * h);
        for (int y = 0; y < h; ++y) {
            const uint32_t* src = p.bits + (size_t)(r.top + y) * p.vw + r.left;
            uint32_t* dst = px.data() + (size_t)y * w;
            for (int x = 0; x < w; ++x) dst[x] = src[x] | 0xFF000000u;     // (opaque)
        }
    }
    close_picker();
    if (px.empty()) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"Screenshot %04u-%02u-%02u %02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    imageedit_open_pixels(std::move(px), w, h, name);
}

RECT monitor_at(POINT pt) {
    for (const RECT& m : s_pick->monitors)
        if (PtInRect(&m, pt)) return m;
    return s_pick->monitors.empty() ? RECT{ 0, 0, s_pick->vw, s_pick->vh } : s_pick->monitors[0];
}

void set_sel(Picker& p, const RECT& r) {
    RECT old = decor_rect(p.sel);
    p.sel = r;
    RECT now = decor_rect(p.sel);
    InvalidateRect(p.wnd, &old, FALSE);
    InvalidateRect(p.wnd, &now, FALSE);
}

LRESULT CALLBACK picker_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Picker* pp = s_pick;
    if (!pp || pp->wnd != hwnd) return DefWindowProcW(hwnd, msg, wp, lp);
    Picker& p = *pp;
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(p); return 0;
        case WM_DPICHANGED: return 0;          // (it covers every screen as it is)
        case WM_LBUTTONDOWN:
            p.dragging = true;
            p.anchor = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            SetCapture(hwnd);
            for (const RECT& mon : p.monitors) {        // (the hint goes)
                HDC dc = GetDC(hwnd);
                HGDIOBJ of = SelectObject(dc, p.font);
                RECT hr = hint_rect(dc, mon);
                SelectObject(dc, of);
                ReleaseDC(hwnd, dc);
                InvalidateRect(hwnd, &hr, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (p.dragging) {
                POINT b{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                RECT r{ std::min(p.anchor.x, b.x), std::min(p.anchor.y, b.y), std::max(p.anchor.x, b.x) + 1,
                        std::max(p.anchor.y, b.y) + 1 };
                if (r.right - r.left < 4 && r.bottom - r.top < 4) r = {};     // (not a drag yet)
                set_sel(p, r);
            }
            return 0;
        case WM_LBUTTONUP:
            if (p.dragging) {
                p.dragging = false;
                ReleaseCapture();
                // a click: the whole screen under it
                RECT r = IsRectEmpty(&p.sel) ? monitor_at({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }) : p.sel;
                finish(r);
            }
            return 0;
        case WM_RBUTTONUP: close_picker(); return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { close_picker(); return 0; }
            if (wp == VK_RETURN) {
                POINT c{};
                GetCursorPos(&c);
                ScreenToClient(hwnd, &c);
                finish(monitor_at(c));
                return 0;
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }   // (another window came up)
            break;
        case WM_CLOSE: close_picker(); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void start_now() {
    if (s_pick) return;
    auto* p = new Picker;
    s_pick = p;
    p->vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    p->vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    p->vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    p->vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    // the screen now, and a dimmed copy to draw the picker on
    HDC screen = GetDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), p->vw, -p->vh, 1, 32, BI_RGB };
    void* bits = nullptr;
    void* dimBits = nullptr;
    p->brightBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    p->dimBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &dimBits, nullptr, 0);
    bool ok = p->brightBmp && p->dimBmp;
    if (ok) {
        p->bright = CreateCompatibleDC(screen);
        p->dim = CreateCompatibleDC(screen);
        p->brightOld = SelectObject(p->bright, p->brightBmp);
        p->dimOld = SelectObject(p->dim, p->dimBmp);
        ok = BitBlt(p->bright, 0, 0, p->vw, p->vh, screen, p->vx, p->vy, SRCCOPY | CAPTUREBLT) != 0;
    }
    ReleaseDC(nullptr, screen);
    if (!ok) { close_picker(); MessageBeep(MB_ICONASTERISK); return; }
    GdiFlush();
    p->bits = (const uint32_t*)bits;
    size_t n = (size_t)p->vw * p->vh;
    const uint32_t* s = p->bits;
    auto* d = (uint32_t*)dimBits;
    for (size_t i = 0; i < n; ++i) {            // (to 55 %)
        uint32_t v = s[i];
        d[i] = ((((v & 0xFF00FF) * 141) >> 8) & 0xFF00FF) | ((((v & 0x00FF00) * 141) >> 8) & 0x00FF00);
    }
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT r, LPARAM lp) -> BOOL {
        auto* pk = (Picker*)lp;
        pk->monitors.push_back({ r->left - pk->vx, r->top - pk->vy, r->right - pk->vx, r->bottom - pk->vy });
        return TRUE;
    }, (LPARAM)p);

    static bool registered = false;
    if (!registered) {
        registered = true;
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = g_hinst;
        wc.lpfnWndProc = picker_proc;
        wc.lpszClassName = L"ExplorerPlusScreenshot";
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        RegisterClassExW(&wc);
    }
    p->wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ExplorerPlusScreenshot", L"Screenshot", WS_POPUP,
                             p->vx, p->vy, p->vw, p->vh, nullptr, nullptr, g_hinst, nullptr);
    if (!p->wnd) { close_picker(); return; }
    p->dpi = (int)GetDpiForWindow(p->wnd);
    p->font = theme_create_font(p->dpi, FW_REGULAR);
    ShowWindow(p->wnd, SW_SHOW);
    SetForegroundWindow(p->wnd);
    SetFocus(p->wnd);
}

bool try_hotkey() {
    if (!s_hotkey) s_hotkey = RegisterHotKey(s_msg, kHotkey, MOD_ALT | MOD_NOREPEAT, 'Q') != 0;
    return s_hotkey;
}

// Alt+Q not to be had, and no other Explorer Plus is there to hold it: another program has
// it. Said once (it used to go unnoticed: Alt+Q simply did nothing here).
void hotkey_taken_elsewhere(HWND self) {
    if (settings_pref(L"screenshotHotkeyTaken", 0)) return;
    for (HWND w = nullptr; (w = FindWindowExW(HWND_MESSAGE, w, L"ExplorerPlusScreenshotMsg", nullptr));)
        if (w != self) return;                  // (one of ours: it's taken over when that one ends)
    HWND owner = g_main;
    if (!owner || !IsWindowVisible(owner)) return;      // (the file manager's window says it, not the player alone)
    settings_set_pref(L"screenshotHotkeyTaken", 1);
    MessageBoxW(owner, L"Alt+Q, the shortcut for a screenshot, is taken by another program, so it doesn't work here.\n\n"
                       L"The Screenshot command in the menu still does. Close the other program, or change its shortcut, "
                       L"and Alt+Q works again (Explorer Plus keeps trying).",
                L"Explorer Plus", MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK msg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    static int retries = 0;
    switch (msg) {
        case WM_HOTKEY:
            if (wp == kHotkey) start_now();
            return 0;
        case WM_TIMER:
            if (wp == TIMER_DELAY) { KillTimer(h, TIMER_DELAY); start_now(); }
            else if (wp == TIMER_RETRY && try_hotkey()) KillTimer(h, TIMER_RETRY);
            else if (wp == TIMER_RETRY && ++retries == 2) hotkey_taken_elsewhere(h);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND msg_window() {
    if (!s_msg) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = g_hinst;
        wc.lpfnWndProc = msg_proc;
        wc.lpszClassName = L"ExplorerPlusScreenshotMsg";
        RegisterClassExW(&wc);
        s_msg = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hinst, nullptr);
    }
    return s_msg;
}

} // namespace

void screenshot_init() {
    if (!settings_pref(L"screenshotHotkey", 1) || !msg_window()) return;
    // (another Explorer Plus has it: taken over once that one ends)
    if (!try_hotkey()) SetTimer(s_msg, TIMER_RETRY, 5000, nullptr);
}

void screenshot_start(UINT delayMs) {
    if (!msg_window()) return;
    if (delayMs) SetTimer(s_msg, TIMER_DELAY, delayMs, nullptr);
    else start_now();
}

bool screenshot_owns(HWND h) { return s_pick && h && h == s_pick->wnd; }
