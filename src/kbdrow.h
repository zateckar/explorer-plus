#pragma once
#include "appstate.h"
#include "theme.h"
#include "acc.h"
#include <functional>

// Keyboard focus in a row of painted buttons (navigation buttons, command bar, tab
// strip): the row is a stop of the Tab / F6 cycle; Left / Right / Home / End move between
// the buttons that can be used, Enter / Space press one. While the row has the focus its
// button wears a focus ring (paint it with kbdrow_ring).
struct KbdRow {
    int focus = -1;                 // the button with the keyboard focus; -1: the row hasn't it
    int last = 0;                   // the one it had when the row lost the focus

    // true: handled (*result is the answer)
    bool handle(HWND hwnd, UINT msg, WPARAM wp, LRESULT* result, int count,
                const std::function<bool(int)>& usable, const std::function<void(int)>& press) {
        auto move_to = [&](int i) {
            if (i == focus) return;
            focus = i;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (i >= 0) acc_notify(hwnd, EVENT_OBJECT_FOCUS, i);
        };
        // the next usable button from `from` in direction `dir` (from itself when `self`)
        auto next = [&](int from, int dir, bool self) {
            for (int k = self ? 0 : 1; k <= count; ++k) {
                int i = from + dir * k;
                if (i < 0 || i >= count) break;
                if (usable(i)) return i;
            }
            return -1;
        };
        switch (msg) {
            case WM_GETDLGCODE:
                *result = DLGC_WANTARROWS;
                return true;
            case WM_SETFOCUS: {
                int start = last < 0 ? 0 : last >= count ? count - 1 : last;
                int i = next(start, 1, true);
                if (i < 0) i = next(count - 1, -1, true);
                move_to(i);
                *result = 0;
                return true;
            }
            case WM_KILLFOCUS:
                if (focus >= 0) last = focus;
                focus = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
                *result = 0;
                return true;
            case WM_KEYDOWN: {
                if (GetFocus() != hwnd) return false;
                if (focus >= count || (focus >= 0 && !usable(focus))) move_to(next(0, 1, true));
                int to = -2;
                switch (wp) {
                    case VK_LEFT:  to = next(focus, -1, false); break;
                    case VK_RIGHT: to = next(focus, 1, false); break;
                    case VK_HOME:  to = next(0, 1, true); break;
                    case VK_END:   to = next(count - 1, -1, true); break;
                    case VK_RETURN:
                    case VK_SPACE:
                        if (focus >= 0) press(focus);
                        *result = 0;
                        return true;
                    default: return false;
                }
                if (to >= 0) move_to(to);
                *result = 0;
                return true;
            }
        }
        return false;
    }
};

// the 2 px focus ring around a button (inside its rect)
inline void kbdrow_ring(HDC dc, const RECT& r, int radius) {
    const Palette& p = pal();
    fill_rrect(dc, r, radius, CLR_NONE, p.text);
    RECT in{ r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 };
    fill_rrect(dc, in, radius > 0 ? radius - 1 : 0, CLR_NONE, p.text);
}
