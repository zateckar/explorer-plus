#include "flyout.h"
#include "theme.h"
#include "acc.h"
#include <dwmapi.h>
#include <algorithm>
using std::max;
using std::min;

// One flyout at a time. It never takes activation: focus stays where it was, the
// flyout runs its own message loop, reads the keyboard from the thread's queue and
// holds the mouse capture so a press anywhere else dismisses it.

namespace {

struct Fly {
    HWND hwnd = nullptr, owner = nullptr;
    std::vector<FlyButton> row;
    std::vector<FlyEntry> entries;
    std::vector<wchar_t> keys;            // access key per entry (0: none)
    std::vector<RECT> btnRects, entryRects;
    int hotBtn = -1, hotEntry = -1;
    UINT result = 0;
    bool done = false, inSub = false;
    int subPending = -1;                  // submenu entry waiting for the hover delay
    int subOpen = -1;                     // entry whose submenu is open
    bool switchTo = false;                // the submenu was closed for another hovered entry
    UINT replayMsg = 0;                   // the press elsewhere that dismissed it (WM_xBUTTONDOWN)...
    POINT replayAt{};                     // ... and where (screen): handed on once closed
};

Fly* s_fly = nullptr;
HHOOK s_subHook = nullptr;
const UINT_PTR kTimerSub = 1;             // hovered submenu entry: open it
const UINT_PTR kTimerSwitch = 2;          // another entry hovered while a submenu is open: close it
const UINT kHoverMs = 350;

COLORREF menu_bg()     { return g_dark ? RGB(44, 44, 44) : RGB(249, 249, 249); }
COLORREF menu_hover()  { return g_dark ? RGB(61, 61, 61) : RGB(234, 234, 234); }
COLORREF menu_border() { return g_dark ? RGB(70, 70, 70) : RGB(222, 222, 222); }

// "Cu&t" -> "Cut" (key 't'); "&&" stays a literal '&'
std::wstring strip_amp(const std::wstring& s, wchar_t* key) {
    std::wstring out;
    if (key) *key = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'&' && i + 1 < s.size()) {
            ++i;
            if (s[i] != L'&' && key && !*key) *key = (wchar_t)towlower(s[i]);
        }
        out += s[i];
    }
    // shell menus put the accelerator after a tab: drop it ("Open\tEnter")
    size_t tab = out.find(L'\t');
    if (tab != std::wstring::npos) out.resize(tab);
    return out;
}

bool selectable(const FlyEntry& e) { return e.kind != FlyEntry::Separator && !e.disabled; }

int entry_h(const FlyEntry& e) { return e.kind == FlyEntry::Separator ? S(9) : S(32); }

void layout(Fly& f, int* outW, int* outH) {
    HDC dc = GetDC(f.hwnd);
    HGDIOBJ old = SelectObject(dc, g_font);
    // snug like Win11's menus: as wide as the widest row (its text, shortcut and submenu
    // arrow together); a very long entry is cut short with an ellipsis
    int w = 0;
    for (auto& e : f.entries) {
        if (e.kind == FlyEntry::Separator) continue;
        SelectObject(dc, e.bold ? ui_font_bold() : g_font);
        int rowW = S(44) + text_width(dc, e.text.c_str()) + S(12);
        SelectObject(dc, g_font);
        if (!e.accel.empty()) rowW += S(16) + text_width(dc, e.accel.c_str());
        if (e.kind == FlyEntry::Submenu) rowW += S(24);
        w = max(w, rowW);
    }
    SelectObject(dc, old);
    ReleaseDC(f.hwnd, dc);
    int pad = S(4);
    int rowW = pad * 2 + S(4) + (int)f.row.size() * S(44);
    w = std::clamp(max(w, rowW), S(200), S(360));
    int y = pad;
    f.btnRects.clear();
    if (!f.row.empty()) {
        int x = pad + S(4);
        for (size_t i = 0; i < f.row.size(); ++i) {
            f.btnRects.push_back({ x, y + S(2), x + S(40), y + S(38) });
            x += S(44);
        }
        y += S(40) + S(9);                    // + the line under the row
    }
    f.entryRects.clear();
    for (auto& e : f.entries) {
        f.entryRects.push_back({ 0, y, w, y + entry_h(e) });
        y += entry_h(e);
    }
    *outW = w;
    *outH = y + pad;
}

void draw_bitmap(HDC dc, HBITMAP hb, const RECT& box, COLORREF fg) {
    BITMAP bm{};
    if (!GetObjectW(hb, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return;
    int sz = S(16);
    int x = box.left + (box.right - box.left - sz) / 2, y = box.top + (box.bottom - box.top - sz) / 2;
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, hb);
    if (bm.bmBitsPixel == 1) {
        // a check-mark style logo (7-Zip): its black pixels in the text color, like a glyph
        int w = (int)bm.bmWidth;
        int s = w * std::max(1, sz / w);                         // whole multiples stay crisp
        int ox = box.left + (box.right - box.left - s) / 2, oy = box.top + (box.bottom - box.top - s) / 2;
        HBRUSH br = CreateSolidBrush(fg);
        HGDIOBJ ob = SelectObject(dc, br);
        COLORREF ot = SetTextColor(dc, RGB(0, 0, 0)), obk = SetBkColor(dc, RGB(255, 255, 255));
        StretchBlt(dc, ox, oy, s, s * bm.bmHeight / bm.bmWidth, mem, 0, 0, bm.bmWidth, bm.bmHeight,
                   0x00B8074A);                                   // PSDPxax: brush where the source is black
        SetTextColor(dc, ot);
        SetBkColor(dc, obk);
        SelectObject(dc, ob);
        DeleteObject(br);
    } else if (bm.bmBitsPixel == 32) {
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(dc, x, y, sz, sz, mem, 0, 0, bm.bmWidth, bm.bmHeight, bf);
    } else {
        SetStretchBltMode(dc, HALFTONE);
        StretchBlt(dc, x, y, sz, sz, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
}

void paint(Fly& f, HDC dc, const RECT& rc) {
    const Palette& p = pal();
    fill_rect(dc, rc, menu_bg());
    SetBkMode(dc, TRANSPARENT);
    for (size_t i = 0; i < f.row.size(); ++i) {
        const RECT& r = f.btnRects[i];
        if ((int)i == f.hotBtn && !f.row[i].disabled) fill_rrect(dc, r, S(4), menu_hover());
        draw_glyph(dc, f.row[i].glyph, r, f.row[i].disabled ? p.textDisabled : p.text, 16);
    }
    if (!f.row.empty()) {
        int ly = f.btnRects[0].bottom + S(6);
        RECT line{ 0, ly, rc.right, ly + 1 };
        fill_rect(dc, line, menu_border());
    }
    for (size_t i = 0; i < f.entries.size(); ++i) {
        const FlyEntry& e = f.entries[i];
        RECT r = f.entryRects[i];
        if (e.kind == FlyEntry::Separator) {
            int my = (r.top + r.bottom) / 2;
            RECT line{ 0, my, rc.right, my + 1 };
            fill_rect(dc, line, menu_border());
            continue;
        }
        if ((int)i == f.hotEntry && !e.disabled) {
            RECT h{ r.left + S(4), r.top + S(2), r.right - S(4), r.bottom - S(2) };
            fill_rrect(dc, h, S(4), menu_hover());
        }
        COLORREF fg = e.disabled ? p.textDisabled : p.text;
        RECT icon{ S(12), r.top, S(12) + S(20), r.bottom };
        if (e.checked) draw_glyph(dc, L'\xE73E', icon, fg, 16);
        else if (e.glyph) draw_glyph(dc, e.glyph, icon, fg, 16);
        else if (e.icon) {
            int sz = S(16);
            DrawIconEx(dc, icon.left + (icon.right - icon.left - sz) / 2, icon.top + (icon.bottom - icon.top - sz) / 2,
                       e.icon, sz, sz, 0, nullptr, DI_NORMAL);
        }
        else if (e.bitmap) draw_bitmap(dc, e.bitmap, icon, fg);
        SelectObject(dc, e.bold ? ui_font_bold() : g_font);
        SetTextColor(dc, fg);
        RECT t{ S(44), r.top, r.right - S(12), r.bottom };
        DrawTextW(dc, e.text.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, g_font);
        int right = r.right - S(12);
        if (e.kind == FlyEntry::Submenu) {
            RECT chev{ right - S(16), r.top, right, r.bottom };
            draw_glyph(dc, L'\xE76C', chev, p.textSecondary, 10);
            right -= S(24);
        }
        if (!e.accel.empty()) {
            SetTextColor(dc, e.disabled ? p.textDisabled : p.textSecondary);
            RECT a{ S(44), r.top, right, r.bottom };
            DrawTextW(dc, e.accel.c_str(), -1, &a, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
}

void set_hot(Fly& f, int btn, int entry) {
    if (btn == f.hotBtn && entry == f.hotEntry) return;
    f.hotBtn = btn;
    f.hotEntry = entry;
    InvalidateRect(f.hwnd, nullptr, FALSE);
    // screen readers follow the highlight like a menu's (the flyout never has focus)
    if (btn >= 0) acc_notify(f.hwnd, EVENT_OBJECT_FOCUS, btn);
    else if (entry >= 0) acc_notify(f.hwnd, EVENT_OBJECT_FOCUS, (int)f.row.size() + entry);
}

void finish(Fly& f, UINT id) {
    f.result = id;
    f.done = true;
}

// dismissed by a press outside: the press still counts where it landed (one click picks
// another file, not one to close the menu and one more to select)
void dismiss_by_press(Fly& f, UINT msg, POINT screen) {
    f.replayMsg = msg;
    f.replayAt = screen;
    finish(f, 0);
}

// hand the dismissing press on to the window under the pointer — only the app's own
// windows (another program's never gets messages from us), and only while the button
// is still down: the real button-up then follows as usual (a click, or a drag)
void replay_press(const Fly& f) {
    if (!f.replayMsg) return;
    int vk = f.replayMsg == WM_LBUTTONDOWN ? VK_LBUTTON : f.replayMsg == WM_RBUTTONDOWN ? VK_RBUTTON : VK_MBUTTON;
    if (GetSystemMetrics(SM_SWAPBUTTON) && vk != VK_MBUTTON) vk = vk == VK_LBUTTON ? VK_RBUTTON : VK_LBUTTON;
    if (!(GetAsyncKeyState(vk) & 0x8000)) return;
    HWND target = WindowFromPoint(f.replayAt);
    DWORD pid = 0;
    if (!target || !GetWindowThreadProcessId(target, &pid) || pid != GetCurrentProcessId() ||
        !IsWindowEnabled(GetAncestor(target, GA_ROOT)))
        return;
    LRESULT ht = SendMessageW(target, WM_NCHITTEST, 0, MAKELPARAM(f.replayAt.x, f.replayAt.y));
    if (ht == HTCLIENT) {
        POINT pt = f.replayAt;
        ScreenToClient(target, &pt);
        WPARAM keys = (f.replayMsg == WM_LBUTTONDOWN ? MK_LBUTTON : f.replayMsg == WM_RBUTTONDOWN ? MK_RBUTTON : MK_MBUTTON) |
                      (GetKeyState(VK_SHIFT) < 0 ? MK_SHIFT : 0) | (GetKeyState(VK_CONTROL) < 0 ? MK_CONTROL : 0);
        PostMessageW(target, f.replayMsg, keys, MAKELPARAM(pt.x, pt.y));
    } else if (ht != HTNOWHERE && ht != HTTRANSPARENT && ht != HTERROR) {
        UINT nc = f.replayMsg - WM_LBUTTONDOWN + WM_NCLBUTTONDOWN;    // (same order: L, -, -, R, -, -, M)
        PostMessageW(target, nc, (WPARAM)ht, MAKELPARAM(f.replayAt.x, f.replayAt.y));
    }
}

void hit(Fly& f, POINT pt, int* btn, int* entry);

// The submenu's modal loop owns the mouse; this filter sees its messages, so hovering
// the flyout's other items still highlights them and, after the hover delay, closes the
// submenu (like Win11's menus). Back on the open entry or inside the submenu: stay.
LRESULT CALLBACK sub_filter(int code, WPARAM wp, LPARAM lp) {
    Fly* f = s_fly;
    if (code == MSGF_MENU && f && f->inSub) {
        const MSG* m = (const MSG*)lp;
        if (m->message == WM_MOUSEMOVE || m->message == WM_NCMOUSEMOVE) {
            POINT pt = m->pt;
            ScreenToClient(f->hwnd, &pt);
            RECT rc;
            GetClientRect(f->hwnd, &rc);
            int b = -1, e = -1;
            if (PtInRect(&rc, pt)) hit(*f, pt, &b, &e);
            if (b < 0 && (e < 0 || e == f->subOpen || !selectable(f->entries[e]))) {
                KillTimer(f->hwnd, kTimerSwitch);
                set_hot(*f, -1, f->subOpen);
            } else if (b != f->hotBtn || e != f->hotEntry) {
                set_hot(*f, b, e);
                SetTimer(f->hwnd, kTimerSwitch, kHoverMs, nullptr);
            }
        }
    }
    return CallNextHookEx(s_subHook, code, wp, lp);
}

void open_sub(Fly& f, int i) {
    KillTimer(f.hwnd, kTimerSub);
    f.subPending = -1;
    for (;;) {
        if (i < 0 || i >= (int)f.entries.size() || f.entries[i].kind != FlyEntry::Submenu || !f.entries[i].sub) return;
        set_hot(f, -1, i);
        UpdateWindow(f.hwnd);
        RECT r = f.entryRects[i];
        MapWindowPoints(f.hwnd, nullptr, (POINT*)&r, 2);
        TPMPARAMS tp{ sizeof(tp) };
        GetWindowRect(f.hwnd, &tp.rcExclude);         // open beside the flyout, never over it
        tp.rcExclude.top = r.top;
        tp.rcExclude.bottom = r.bottom;
        f.inSub = true;
        f.subOpen = i;
        f.switchTo = false;
        ReleaseCapture();
        s_subHook = SetWindowsHookExW(WH_MSGFILTER, sub_filter, nullptr, GetCurrentThreadId());
        UINT id = TrackPopupMenuEx(f.entries[i].sub, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_HORIZONTAL,
                                   r.right, r.top, f.owner, &tp);
        if (s_subHook) UnhookWindowsHookEx(s_subHook);
        s_subHook = nullptr;
        KillTimer(f.hwnd, kTimerSwitch);
        f.inSub = false;
        f.subOpen = -1;
        if (id) { finish(f, id); return; }
        if (f.switchTo) {                              // closed for another hovered entry
            SetCapture(f.hwnd);
            int e = f.hotEntry;
            if (e >= 0 && f.entries[e].kind == FlyEntry::Submenu && selectable(f.entries[e])) { i = e; continue; }
            return;
        }
        // the submenu was dismissed by a click outside both menus: close the flyout too
        // (Esc or Left only close the submenu)
        POINT cur;
        GetCursorPos(&cur);
        RECT wr;
        GetWindowRect(f.hwnd, &wr);
        bool left = GetAsyncKeyState(VK_LBUTTON) & 0x8000, right = GetAsyncKeyState(VK_RBUTTON) & 0x8000;   // (physical)
        if ((left || right) && !PtInRect(&wr, cur)) {
            bool primary = left != (GetSystemMetrics(SM_SWAPBUTTON) != 0);
            dismiss_by_press(f, primary ? WM_LBUTTONDOWN : WM_RBUTTONDOWN, cur);
            return;
        }
        SetCapture(f.hwnd);
        // back in the flyout: highlight what the pointer is on
        ScreenToClient(f.hwnd, &cur);
        int b, e;
        hit(f, cur, &b, &e);
        if (b >= 0 || e >= 0) set_hot(f, b, e);
        return;
    }
}

void choose_entry(Fly& f, int i) {
    if (i < 0 || i >= (int)f.entries.size() || !selectable(f.entries[i])) return;
    if (f.entries[i].kind == FlyEntry::Submenu) open_sub(f, i);
    else finish(f, f.entries[i].id);
}

void hit(Fly& f, POINT pt, int* btn, int* entry) {
    *btn = *entry = -1;
    for (size_t i = 0; i < f.btnRects.size(); ++i)
        if (PtInRect(&f.btnRects[i], pt)) *btn = (int)i;
    for (size_t i = 0; i < f.entryRects.size(); ++i)
        if (PtInRect(&f.entryRects[i], pt) && f.entries[i].kind != FlyEntry::Separator) *entry = (int)i;
}

int next_entry(Fly& f, int from, int dir) {
    int n = (int)f.entries.size();
    for (int k = 1; k <= n; ++k) {
        // from < 0: start at the first (dir > 0) or last entry; else wrap around
        int i = from < 0 ? (dir > 0 ? k - 1 : n - k) : ((from + dir * k) % n + n) % n;
        if (selectable(f.entries[i])) return i;
    }
    return -1;
}

void on_key(Fly& f, WPARAM vk) {
    int nb = (int)f.row.size();
    switch (vk) {
        case VK_ESCAPE: finish(f, 0); return;
        case VK_DOWN:
            if (f.hotBtn >= 0) set_hot(f, -1, next_entry(f, -1, +1));
            else set_hot(f, -1, next_entry(f, f.hotEntry, +1));
            return;
        case VK_UP: {
            int first = next_entry(f, -1, +1);
            if (f.hotEntry == first && nb) set_hot(f, 0, -1);
            else if (f.hotBtn < 0) set_hot(f, -1, next_entry(f, f.hotEntry, -1));
            return;
        }
        case VK_HOME: set_hot(f, -1, next_entry(f, -1, +1)); return;
        case VK_END:  set_hot(f, -1, next_entry(f, -1, -1)); return;
        case VK_TAB:
            if (f.hotBtn >= 0 || !nb) set_hot(f, -1, next_entry(f, -1, +1));
            else set_hot(f, 0, -1);
            return;
        case VK_LEFT:
            if (f.hotBtn > 0) set_hot(f, f.hotBtn - 1, -1);
            return;
        case VK_RIGHT:
            if (f.hotBtn >= 0 && f.hotBtn + 1 < nb) set_hot(f, f.hotBtn + 1, -1);
            else if (f.hotEntry >= 0 && f.entries[f.hotEntry].kind == FlyEntry::Submenu) open_sub(f, f.hotEntry);
            return;
        case VK_RETURN: case VK_SPACE:
            if (f.hotBtn >= 0 && !f.row[f.hotBtn].disabled) finish(f, f.row[f.hotBtn].id);
            else choose_entry(f, f.hotEntry);
            return;
    }
}

// access keys: the marked letter chooses; else the first letter moves (or chooses when unique)
void on_char(Fly& f, wchar_t ch) {
    ch = (wchar_t)towlower(ch);
    std::vector<int> marked, initial;
    for (size_t i = 0; i < f.entries.size(); ++i) {
        if (!selectable(f.entries[i])) continue;
        if (f.keys[i] == ch) marked.push_back((int)i);
        else if (!f.entries[i].text.empty() && towlower(f.entries[i].text[0]) == ch) initial.push_back((int)i);
    }
    std::vector<int>& hits = marked.empty() ? initial : marked;
    if (hits.size() == 1) { choose_entry(f, hits[0]); return; }
    if (hits.empty()) return;
    auto it = std::upper_bound(hits.begin(), hits.end(), f.hotEntry);
    set_hot(f, -1, it == hits.end() ? hits[0] : *it);
}

// screen readers: a menu of the icon row's buttons, then the entries. Their actions
// come back as kAccAction (the flyout's own loop runs them).
const UINT kAccAction = WM_APP + 51;
const AccSource s_acc = [] {
    AccSource a;
    a.name = L"Context";
    a.role = ROLE_SYSTEM_MENUPOPUP;
    a.count = [] { return s_fly ? (int)(s_fly->row.size() + s_fly->entries.size()) : 0; };
    a.child = [](int i) {
        AccChild c;
        Fly& f = *s_fly;
        int nb = (int)f.row.size();
        if (i < nb) {
            const FlyButton& b = f.row[i];
            size_t paren = b.tip.find(L" (");
            c.name = b.tip.substr(0, paren);
            if (paren != std::wstring::npos && !b.tip.empty() && b.tip.back() == L')')
                c.shortcut = b.tip.substr(paren + 2, b.tip.size() - paren - 3);
            c.role = ROLE_SYSTEM_PUSHBUTTON;
            c.rc = i < (int)f.btnRects.size() ? f.btnRects[i] : RECT{};
            if (b.disabled) c.state |= STATE_SYSTEM_UNAVAILABLE;
            if (f.hotBtn == i) c.state |= STATE_SYSTEM_FOCUSED | STATE_SYSTEM_HOTTRACKED;
            c.action = L"Press";
            return c;
        }
        int e = i - nb;
        const FlyEntry& en = f.entries[e];
        c.rc = e < (int)f.entryRects.size() ? f.entryRects[e] : RECT{};
        if (en.kind == FlyEntry::Separator) { c.role = ROLE_SYSTEM_SEPARATOR; return c; }
        c.name = en.text;
        c.shortcut = en.accel;
        c.role = ROLE_SYSTEM_MENUITEM;
        if (en.kind == FlyEntry::Submenu) c.state |= STATE_SYSTEM_HASPOPUP;
        if (en.disabled) c.state |= STATE_SYSTEM_UNAVAILABLE;
        if (en.checked) c.state |= STATE_SYSTEM_CHECKED;
        if (f.hotEntry == e) c.state |= STATE_SYSTEM_FOCUSED | STATE_SYSTEM_HOTTRACKED;
        c.action = en.kind == FlyEntry::Submenu ? L"Open" : L"Execute";
        return c;
    };
    a.action = [](int i) { if (s_fly) PostMessageW(s_fly->hwnd, kAccAction, (WPARAM)i, 0); };
    a.focus = [] {
        if (!s_fly) return -1;
        return s_fly->hotBtn >= 0 ? s_fly->hotBtn : s_fly->hotEntry >= 0 ? (int)s_fly->row.size() + s_fly->hotEntry : -1;
    };
    return a;
}();

LRESULT CALLBACK fly_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Fly* f = s_fly;
    if (!f || f->hwnd != hwnd) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_GETOBJECT: return acc_get_object(hwnd, wp, lp, &s_acc);
        case kAccAction: {
            int i = (int)wp, nb = (int)f->row.size();
            if (i < nb) { if (!f->row[i].disabled) finish(*f, f->row[i].id); }
            else choose_entry(*f, i - nb);
            return 0;
        }
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            BufferedPaint bp(hwnd);
            paint(*f, bp.dc, bp.rc);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
            int b, e;
            hit(*f, pt, &b, &e);
            RECT rc; GetClientRect(hwnd, &rc);
            if (!PtInRect(&rc, pt)) return 0;          // outside: keep the keyboard highlight
            set_hot(*f, b, e);
            if (e >= 0 && f->entries[e].kind == FlyEntry::Submenu) {
                if (f->subPending != e) { f->subPending = e; SetTimer(hwnd, kTimerSub, kHoverMs, nullptr); }
            } else if (f->subPending >= 0) {
                f->subPending = -1;
                KillTimer(hwnd, kTimerSub);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
            POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
            RECT rc; GetClientRect(hwnd, &rc);
            if (!PtInRect(&rc, pt)) {                  // a press elsewhere dismisses
                ClientToScreen(hwnd, &pt);
                dismiss_by_press(*f, msg, pt);
            }
            return 0;
        }
        case WM_LBUTTONUP: case WM_RBUTTONUP: {
            POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
            int b, e;
            hit(*f, pt, &b, &e);
            if (b >= 0 && !f->row[b].disabled) finish(*f, f->row[b].id);
            else if (e >= 0) choose_entry(*f, e);
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerSub) {
                KillTimer(hwnd, kTimerSub);
                if (f->subPending >= 0 && f->subPending == f->hotEntry) open_sub(*f, f->subPending);
                return 0;
            }
            if (wp == kTimerSwitch) {
                KillTimer(hwnd, kTimerSwitch);
                if (f->inSub && (f->hotBtn >= 0 || (f->hotEntry >= 0 && f->hotEntry != f->subOpen))) {
                    f->switchTo = true;
                    EndMenu();
                }
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (!f->inSub && !f->done && (HWND)lp != hwnd) finish(*f, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND flyout_window() { return s_fly ? s_fly->hwnd : nullptr; }

UINT flyout_track(HWND owner, POINT pt, const std::vector<FlyButton>& row, const std::vector<FlyEntry>& entries) {
    if (s_fly) return 0;                              // one at a time
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DROPSHADOW;
        wc.hInstance = g_hinst;
        wc.lpfnWndProc = fly_proc;
        wc.lpszClassName = L"ExplorerPlusFlyout";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        reg = true;
    }
    Fly f;
    f.owner = owner;
    f.row = row;
    f.entries = entries;
    std::wstring names;                              // window text: the entries (accessibility, tests)
    for (auto& e : f.entries) {
        wchar_t key = 0;
        if (e.kind != FlyEntry::Separator) e.text = strip_amp(e.text, &key);
        f.keys.push_back(key);
        names += (e.kind == FlyEntry::Separator ? std::wstring(L"-") : e.text) + L"\n";
    }
    f.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"ExplorerPlusFlyout",
                             names.c_str(), WS_POPUP, 0, 0, 10, 10, owner, nullptr, g_hinst, nullptr);
    if (!f.hwnd) return 0;
    s_fly = &f;
    int w = 0, h = 0;
    layout(f, &w, &h);
    // keep it on the monitor: open up / to the left of the point when needed
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;
    int x = pt.x + w > wa.right ? pt.x - w : pt.x;
    int y = pt.y + h > wa.bottom ? pt.y - h : pt.y;
    x = std::clamp(x, (int)wa.left, max((int)wa.left, (int)wa.right - w));
    y = std::clamp(y, (int)wa.top, max((int)wa.top, (int)wa.bottom - h));
    // Win11 rounding and border
    DWORD round = 2;                                  // DWMWCP_ROUND
    DwmSetWindowAttribute(f.hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
    COLORREF border = menu_border();
    DwmSetWindowAttribute(f.hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
    SetWindowPos(f.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    NotifyWinEvent(EVENT_SYSTEM_MENUPOPUPSTART, f.hwnd, OBJID_CLIENT, CHILDID_SELF);
    // tooltips for the icon row ("Cut (Ctrl+X)"); destroyed with the flyout
    if (!f.row.empty()) {
        HWND tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
                                   WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, f.hwnd, nullptr, g_hinst, nullptr);
        for (size_t i = 0; i < f.row.size(); ++i) {
            TTTOOLINFOW ti{ sizeof(ti) };
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = f.hwnd;
            ti.uId = i;
            ti.rect = f.btnRects[i];
            ti.lpszText = const_cast<wchar_t*>(f.row[i].tip.c_str());
            SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }
    SetCapture(f.hwnd);

    HWND fgAtStart = GetForegroundWindow();
    MSG msg;
    while (!f.done) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) { PostQuitMessage((int)msg.wParam); break; }
        if (got < 0) break;
        switch (msg.message) {
            case WM_KEYDOWN:
                // letters become WM_CHAR (access keys); the keys handled here make none,
                // so no stray Enter / Esc char reaches the focused window afterwards
                if (msg.wParam != VK_RETURN && msg.wParam != VK_ESCAPE && msg.wParam != VK_SPACE && msg.wParam != VK_TAB)
                    TranslateMessage(&msg);
                on_key(f, msg.wParam);
                continue;
            case WM_SYSKEYDOWN: case WM_SYSCHAR: finish(f, 0); continue;   // Alt / F10
            case WM_CHAR: if (msg.wParam > L' ') on_char(f, (wchar_t)msg.wParam); continue;
            case WM_KEYUP: case WM_SYSKEYUP: case WM_DEADCHAR: continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        // another application took the foreground: close like a menu does
        HWND fg = GetForegroundWindow();
        if (!f.done && !f.inSub && fg != fgAtStart && fg) {
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            if (pid != GetCurrentProcessId()) finish(f, 0);
        }
    }
    if (GetCapture() == f.hwnd) ReleaseCapture();
    NotifyWinEvent(EVENT_SYSTEM_MENUPOPUPEND, f.hwnd, OBJID_CLIENT, CHILDID_SELF);
    s_fly = nullptr;
    DestroyWindow(f.hwnd);
    replay_press(f);                                 // (handled once the caller is back in its loop)
    return f.result;
}
