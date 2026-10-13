// Media player window: the video (or what is playing, for audio), the play queue on the
// right, the controls bar at the bottom. Playback is media.cpp's (Media Foundation); the
// queue's logic is playlist.cpp's.
#include "player.h"
#include "media.h"
#include "playlist.h"
#include "theme.h"
#include "settings.h"
#include "acc.h"
#include <dwmapi.h>
#include <propkey.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>

namespace {

const UINT WM_MEDIA_NOTE = WM_APP + 40;     // lParam: MediaNote* (media.cpp)
const UINT WM_PLAYER_OPEN = WM_APP + 41;    // to the message window, lParam: OpenJob*
const UINT WM_QUEUE_INFO = WM_APP + 42;     // to the message window: tags read (s_tagDone)
const ULONG_PTR kCopyPlay = 0x45505031, kCopyEnqueue = 0x45505032;     // WM_COPYDATA: 'EPP1', 'EPP2'
const UINT_PTR TIMER_POSITION = 1, TIMER_SKIP = 2, TIMER_HIDE_BAR = 3;
const size_t kMaxFiles = 10000;             // a folder walk stops there

enum Cmd : UINT {
    CMD_PLAYPAUSE = 100, CMD_PREV, CMD_NEXT, CMD_SHUFFLE, CMD_REPEAT, CMD_MUTE, CMD_QUEUE, CMD_FULL,
    CMD_CLEAR, CMD_PLAY_SELECTED, CMD_REMOVE_SELECTED,
};
// the controls bar's parts (also its accessible children, in this order)
enum Part { P_SEEK, P_SHUFFLE, P_PREV, P_PLAY, P_NEXT, P_REPEAT, P_MUTE, P_VOLUME, P_QUEUE, P_FULL, P_COUNT };
const UINT kPartCmd[P_COUNT] = { 0, CMD_SHUFFLE, CMD_PREV, CMD_PLAYPAUSE, CMD_NEXT, CMD_REPEAT, CMD_MUTE, 0, CMD_QUEUE, CMD_FULL };

using State = MediaPlayer::State;

struct Player {
    HWND wnd = nullptr, video = nullptr, bar = nullptr, list = nullptr, barTip = nullptr, headTip = nullptr;
    MediaPlayer* mp = nullptr;
    PlayQueue q;
    int volume = 80;                // 0..100
    bool muted = false;
    bool queueShown = true;
    int queueW96 = 300;             // the queue's width at 96 dpi
    bool full = false, barInFull = false;
    WINDOWPLACEMENT restore{ sizeof(WINDOWPLACEMENT) };    // the window before full screen
    POINT lastCursor{ -1, -1 };
    int dpi = 96;
    HFONT font = nullptr, fontSmall = nullptr, fontBold = nullptr, fontBig = nullptr;
    HFONT glyphs = nullptr, glyphsBig = nullptr, glyphsArt = nullptr;
    HIMAGELIST rowHeight = nullptr; // (sets the queue's row height)
    bool mainClosed = false;        // the main window is gone: closing the player ends the process
    unsigned themeSeen = 0;
    std::wstring message;           // why the current entry doesn't play
    int failsInRow = 0;
    HWND lastFocus = nullptr;
    // the window's parts
    int splitX = 0;                 // the gap between the video and the queue
    int headH = 0;
    RECT clearBtn{};
    bool clearHot = false, splitting = false;
    // the controls bar
    RECT parts[P_COUNT]{};
    RECT seekRail{}, volRail{};
    int titleLeft = 0, titleRight = 0;
    int hot = -1, pressed = -1, drag = -1;
    LONGLONG dragPos = 0;
};
Player* s_p = nullptr;
HWND s_msg = nullptr;               // the module's message window (results of its threads)

int px(int v) { return MulDiv(v, s_p ? s_p->dpi : 96, 96); }

// The player's window class. Tests run with a settings file of their own: their player
// has a class of its own too, so a test never reaches the user's real player.
const std::wstring& player_class() {
    static const std::wstring name = [] {
        std::wstring n = L"ExplorerPlusPlayer";
        WCHAR ini[MAX_PATH * 2]{};
        if (GetEnvironmentVariableW(L"EXPLORERPLUS_SETTINGS", ini, ARRAYSIZE(ini)) && ini[0]) {
            unsigned h = 2166136261u;
            for (WCHAR* c = ini; *c; ++c) { h ^= (unsigned)towlower(*c); h *= 16777619u; }
            WCHAR b[16];
            swprintf_s(b, L".%08x", h);
            n += b;
        }
        return n;
    }();
    return name;
}

// ------------------------------------------------------------------
// fonts and drawing (at the player's own DPI: it may sit on another monitor than the
// main window)
// ------------------------------------------------------------------
void delete_fonts(Player& p) {
    for (HFONT* f : { &p.font, &p.fontSmall, &p.fontBold, &p.fontBig, &p.glyphs, &p.glyphsBig, &p.glyphsArt })
        if (*f) { DeleteObject(*f); *f = nullptr; }
    if (p.rowHeight) { ImageList_Destroy(p.rowHeight); p.rowHeight = nullptr; }
}

HFONT glyph_font_at(int px96, int dpi) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(px96, dpi, 96);
    lf.lfWeight = FW_REGULAR;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcscpy_s(lf.lfFaceName, theme_glyph_face());
    return CreateFontIndirectW(&lf);
}

void make_fonts(Player& p) {
    HIMAGELIST oldRows = p.rowHeight;           // (let go of once the list has the new one)
    p.rowHeight = nullptr;
    delete_fonts(p);
    p.font = theme_create_font(p.dpi, FW_REGULAR);
    p.fontSmall = theme_create_font(p.dpi, FW_REGULAR, 90);
    p.fontBold = theme_create_font(p.dpi, FW_SEMIBOLD);
    p.fontBig = theme_create_font(p.dpi, FW_SEMIBOLD, 170);
    p.glyphs = glyph_font_at(16, p.dpi);
    p.glyphsBig = glyph_font_at(18, p.dpi);
    p.glyphsArt = glyph_font_at(72, p.dpi);
    p.rowHeight = ImageList_Create(1, px(32), ILC_COLOR32, 1, 0);
    if (p.list) {
        SendMessageW(p.list, WM_SETFONT, (WPARAM)p.font, TRUE);
        ListView_SetImageList(p.list, p.rowHeight, LVSIL_SMALL);
    }
    if (oldRows) ImageList_Destroy(oldRows);
}

void glyph_at(HDC dc, wchar_t g, const RECT& r, COLORREF c, HFONT f) {
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    RECT rr = r;
    wchar_t s[2] = { g, 0 };
    DrawTextW(dc, s, 1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

void text_at(HDC dc, const std::wstring& t, RECT r, COLORREF c, HFONT f, UINT fmt) {
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, t.c_str(), (int)t.size(), &r, fmt | DT_NOPREFIX);
    SelectObject(dc, of);
}

COLORREF on_accent() {
    if (theme_high_contrast()) return GetSysColor(COLOR_HIGHLIGHTTEXT);
    return g_dark ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

// ------------------------------------------------------------------
// what is shown
// ------------------------------------------------------------------
std::wstring file_title(const std::wstring& path) {
    std::wstring s = PathFindFileNameW(path.c_str());
    size_t dot = s.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) s.resize(dot);
    return s;
}

std::wstring entry_name(const QueueEntry& e) { return e.title.empty() ? file_title(e.path) : e.title; }

std::wstring current_title(Player& p) {
    int i = p.q.current();
    if (i < 0) return {};
    if (!p.mp->title().empty()) return p.mp->title();
    return entry_name(p.q.at(i));
}

std::wstring current_detail(Player& p) {
    std::wstring a = p.mp->artist(), b = p.mp->album();
    if (!a.empty() && !b.empty()) return a + L" \x2014 " + b;
    return a.empty() ? b : a;
}

bool seekable(Player& p) {
    State s = p.mp->state();
    return p.mp->duration() > 0 && (s == State::Playing || s == State::Paused || s == State::Stopped || s == State::Ended);
}

bool playing(Player& p) { return p.mp->will_play(); }

std::wstring state_text(Player& p) {
    std::wstring s;
    switch (p.mp->state()) {
        case State::Playing: s = p.mp->will_play() ? L"Playing" : L"Paused"; break;
        case State::Paused:  s = L"Paused"; break;
        case State::Opening: s = L"Opening"; break;
        case State::Failed:  return L"Can't play: " + p.message;
        default:             s = p.q.empty() ? L"Empty" : L"Stopped"; break;
    }
    if (p.mp->duration() > 0)
        s += L" \x00B7 " + fmt_duration(p.mp->position()) + L" / " + fmt_duration(p.mp->duration());
    return s;
}

void update_title(Player& p) {
    std::wstring t = current_title(p);
    t = t.empty() ? L"Player" : t + L" - Player";
    WCHAR now[512]{};
    GetWindowTextW(p.wnd, now, ARRAYSIZE(now));
    if (t != now) SetWindowTextW(p.wnd, t.c_str());
}

// the bar's text (what screen readers and the tests read), its paint, the position timer
void refresh_bar(Player& p) {
    std::wstring t = state_text(p);
    WCHAR now[512]{};
    GetWindowTextW(p.bar, now, ARRAYSIZE(now));
    if (t != now) {
        SetWindowTextW(p.bar, t.c_str());
        acc_notify(p.bar, EVENT_OBJECT_VALUECHANGE, -1);
    }
    InvalidateRect(p.bar, nullptr, FALSE);
    if (p.mp->state() == State::Playing) SetTimer(p.wnd, TIMER_POSITION, 250, nullptr);
    else KillTimer(p.wnd, TIMER_POSITION);
}

void refresh_all(Player& p) {
    refresh_bar(p);
    update_title(p);
    InvalidateRect(p.video, nullptr, FALSE);
    InvalidateRect(p.list, nullptr, FALSE);
}

// ------------------------------------------------------------------
// layout
// ------------------------------------------------------------------
void fit_columns(Player& p) {
    RECT rc{};
    GetClientRect(p.list, &rc);
    ListView_SetColumnWidth(p.list, 0, rc.right > px(40) ? rc.right : px(40));
}

void set_tool_rect(HWND tip, HWND owner, UINT_PTR id, const RECT& r) {
    if (!tip) return;
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.hwnd = owner;
    ti.uId = id;
    ti.rect = r;
    SendMessageW(tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
}

void layout_bar(Player& p) {
    RECT rc{};
    GetClientRect(p.bar, &rc);
    int w = rc.right;
    int seekY = px(24), timeW = px(56);
    p.seekRail = { px(12) + timeW, seekY - px(2), w - px(12) - timeW, seekY + px(2) };
    p.parts[P_SEEK] = { p.seekRail.left - px(10), seekY - px(12), p.seekRail.right + px(10), seekY + px(12) };
    int y = px(66), b = px(40);
    int x = px(12);
    auto put = [&](Part part, int width) {
        p.parts[part] = { x, y - width / 2, x + width, y + width / 2 };
        x += width + px(4);
    };
    put(P_SHUFFLE, b); put(P_PREV, b); put(P_PLAY, px(48)); put(P_NEXT, b); put(P_REPEAT, b);
    p.titleLeft = x + px(12);
    int r = w - px(12);
    auto put_right = [&](Part part, int width) {
        p.parts[part] = { r - width, y - b / 2, r, y + b / 2 };
        r -= width + px(4);
    };
    put_right(P_FULL, b);
    put_right(P_QUEUE, b);
    r -= px(8);
    int vol = px(100);
    p.parts[P_VOLUME] = { r - vol, y - px(12), r, y + px(12) };
    p.volRail = { r - vol + px(10), y - px(2), r - px(10), y + px(2) };
    r -= vol + px(4);
    put_right(P_MUTE, b);
    p.titleRight = r - px(12);
    for (int i = 0; i < P_COUNT; ++i) set_tool_rect(p.barTip, p.bar, (UINT_PTR)i, p.parts[i]);
}

void layout(Player& p) {
    RECT rc{};
    GetClientRect(p.wnd, &rc);
    int w = rc.right, h = rc.bottom;
    bool bar = !p.full || p.barInFull;
    int barH = bar ? px(96) : 0;
    bool queue = p.queueShown && !p.full;
    int qw = 0, gap = 0;
    if (queue) {
        int most = w - px(240);
        qw = std::clamp(px(p.queueW96), px(180), most > px(180) ? most : px(180));
        gap = px(4);
    }
    int vw = w - qw - gap > 0 ? w - qw - gap : 0;
    int bodyH = h - barH > 0 ? h - barH : 0;
    p.splitX = vw;
    p.headH = queue ? px(44) : 0;
    MoveWindow(p.video, 0, 0, vw, bodyH, TRUE);
    ShowWindow(p.list, queue ? SW_SHOWNA : SW_HIDE);
    if (queue) MoveWindow(p.list, w - qw, p.headH, qw, bodyH - p.headH > 0 ? bodyH - p.headH : 0, TRUE);
    ShowWindow(p.bar, bar ? SW_SHOWNA : SW_HIDE);
    if (bar) MoveWindow(p.bar, 0, h - barH, w, barH, TRUE);
    int cb = px(32);
    p.clearBtn = queue ? RECT{ w - px(8) - cb, (p.headH - cb) / 2, w - px(8), (p.headH + cb) / 2 } : RECT{};
    set_tool_rect(p.headTip, p.wnd, 1, p.clearBtn);
    if (queue) fit_columns(p);
    InvalidateRect(p.wnd, nullptr, FALSE);
}

// ------------------------------------------------------------------
// playback and the queue
// ------------------------------------------------------------------
void apply_volume(Player& p) {
    double v = p.muted ? 0.0 : p.volume / 100.0;
    p.mp->set_volume((float)(v * v));       // (closer to how loud it sounds than a straight line)
}

void set_volume(Player& p, int v) {
    p.volume = std::clamp(v, 0, 100);
    p.muted = false;
    apply_volume(p);
    settings_set_pref(L"playerVolume", p.volume);
    settings_set_pref(L"playerMuted", 0);
    InvalidateRect(p.bar, nullptr, FALSE);
}

void tags_read(Player& p, int from);
void tags_forget();

void on_failed(Player& p);

// opens entry i and plays it (or shows it stopped at the start)
void play_index(Player& p, int i, bool play) {
    KillTimer(p.wnd, TIMER_SKIP);
    p.q.set_current(i);
    p.message.clear();
    if (p.q.current() < 0) {
        p.mp->close();
    } else {
        p.mp->open(p.q.at(i).path, play);
        if (p.mp->state() == State::Failed) on_failed(p);
        ListView_EnsureVisible(p.list, i, FALSE);
    }
    refresh_all(p);
}

void on_failed(Player& p) {
    int i = p.q.current();
    if (i < 0) return;
    p.q.at(i).failed = true;
    p.message = media_error_text(p.mp->error());
    ++p.failsInRow;
    // (on to the next one after a moment, unless nothing can play here at all)
    if (media_available() && p.failsInRow < p.q.size() && p.q.next(false) >= 0)
        SetTimer(p.wnd, TIMER_SKIP, 1500, nullptr);
    refresh_all(p);
}

void track_ended(Player& p) {
    int n = p.q.next(true);
    if (n >= 0) play_index(p, n, true);
    else play_index(p, p.q.first(), false);         // the end of the queue: back to its start
}

void play_pause(Player& p) {
    if (p.q.empty()) return;
    switch (p.mp->state()) {
        case State::Playing: case State::Opening:
            if (p.mp->will_play()) p.mp->pause(); else p.mp->play();
            break;
        case State::Paused: case State::Stopped: case State::Ended:
            p.mp->play();
            break;
        default:
            play_index(p, p.q.current() >= 0 ? p.q.current() : p.q.first(), true);
            break;
    }
}

void previous(Player& p) {
    if (p.q.empty()) return;
    bool loaded = p.mp->state() == State::Playing || p.mp->state() == State::Paused;
    int i = p.q.previous();
    // 3 s into the track, or nothing before it: from its start
    if (loaded && (p.mp->position() > 30000000 || i < 0)) { p.mp->seek(0); return; }
    play_index(p, i >= 0 ? i : p.q.first(), true);
}

void next(Player& p) {
    if (p.q.empty()) return;
    int i = p.q.next(false);
    if (i >= 0) play_index(p, i, true);
    else play_index(p, p.q.first(), false);           // past the end: its start, stopped
}

void queue_changed(Player& p) {
    ListView_SetItemCountEx(p.list, p.q.size(), LVSICF_NOSCROLL);
    InvalidateRect(p.list, nullptr, FALSE);
    fit_columns(p);
    RECT head{ p.splitX, 0, 0x7FFF, p.headH };
    InvalidateRect(p.wnd, &head, FALSE);
}

void add(Player& p, const std::vector<std::wstring>& paths, bool enqueue) {
    if (paths.empty()) return;
    State s = p.mp->state();
    bool idle = !(s == State::Playing || s == State::Paused || s == State::Opening);
    if (!enqueue) {
        tags_forget();
        p.q.replace(paths);
        p.failsInRow = 0;
        queue_changed(p);
        tags_read(p, 0);
        play_index(p, p.q.current(), true);
        return;
    }
    int first = p.q.append(paths);
    queue_changed(p);
    tags_read(p, first);
    if (idle) {
        p.failsInRow = 0;
        play_index(p, first, true);
    }
    refresh_bar(p);
}

std::vector<int> selected_rows(Player& p) {
    std::vector<int> v;
    for (int i = -1; (i = ListView_GetNextItem(p.list, i, LVNI_SELECTED)) >= 0;) v.push_back(i);
    return v;
}

void remove_selected(Player& p) {
    std::vector<int> sel = selected_rows(p);
    if (sel.empty()) return;
    bool was = playing(p);
    bool hit = p.q.remove(sel);
    ListView_SetItemState(p.list, -1, 0, LVIS_SELECTED);
    queue_changed(p);
    int focus = std::min(sel[0], p.q.size() - 1);
    if (focus >= 0) ListView_SetItemState(p.list, focus, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
    if (hit) {
        if (p.q.current() >= 0) play_index(p, p.q.current(), was);
        else { KillTimer(p.wnd, TIMER_SKIP); p.mp->close(); p.message.clear(); }
    }
    refresh_all(p);
}

void clear_queue(Player& p) {
    tags_forget();
    KillTimer(p.wnd, TIMER_SKIP);
    p.q.clear();
    p.mp->close();
    p.message.clear();
    queue_changed(p);
    refresh_all(p);
}

void set_full(Player& p, bool on) {
    if (on == p.full) return;
    LONG style = GetWindowLongW(p.wnd, GWL_STYLE);
    if (on) {
        MONITORINFO mi{ sizeof(mi) };
        if (!GetWindowPlacement(p.wnd, &p.restore) ||
            !GetMonitorInfoW(MonitorFromWindow(p.wnd, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        p.full = true;
        p.barInFull = false;
        SetWindowLongW(p.wnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(p.wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        SetFocus(p.wnd);
    } else {
        p.full = false;
        KillTimer(p.wnd, TIMER_HIDE_BAR);
        SetWindowLongW(p.wnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(p.wnd, &p.restore);
        SetWindowPos(p.wnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    layout(p);
    InvalidateRect(p.bar, nullptr, FALSE);
}

// full screen: the controls come with the mouse and go a moment after it stops
void full_show_bar(Player& p) {
    if (!p.full) return;
    POINT c{};
    GetCursorPos(&c);
    if (c.x == p.lastCursor.x && c.y == p.lastCursor.y) return;    // (a re-layout's mouse move)
    p.lastCursor = c;
    if (!p.barInFull) { p.barInFull = true; layout(p); }
    SetTimer(p.wnd, TIMER_HIDE_BAR, 2500, nullptr);
}

void command(Player& p, UINT cmd) {
    switch (cmd) {
        case CMD_PLAYPAUSE: play_pause(p); break;
        case CMD_PREV: previous(p); break;
        case CMD_NEXT: next(p); break;
        case CMD_SHUFFLE:
            p.q.set_shuffle(!p.q.shuffle());
            settings_set_pref(L"playerShuffle", p.q.shuffle());
            break;
        case CMD_REPEAT:
            p.q.repeat = (Repeat)(((int)p.q.repeat + 1) % 3);
            settings_set_pref(L"playerRepeat", (int)p.q.repeat);
            break;
        case CMD_MUTE:
            p.muted = !p.muted;
            apply_volume(p);
            settings_set_pref(L"playerMuted", p.muted);
            break;
        case CMD_QUEUE:
            p.queueShown = !p.queueShown;
            settings_set_pref(L"playerQueue", p.queueShown);
            if (!p.queueShown && GetFocus() == p.list) SetFocus(p.wnd);
            layout(p);
            break;
        case CMD_FULL: set_full(p, !p.full); break;
        case CMD_CLEAR: clear_queue(p); break;
        case CMD_PLAY_SELECTED: {
            int i = ListView_GetNextItem(p.list, -1, LVNI_SELECTED);
            if (i < 0) i = ListView_GetNextItem(p.list, -1, LVNI_FOCUSED);
            if (i >= 0) { p.failsInRow = 0; play_index(p, i, true); }
            break;
        }
        case CMD_REMOVE_SELECTED: remove_selected(p); break;
    }
    refresh_bar(p);
    acc_notify(p.bar, EVENT_OBJECT_STATECHANGE, -1);
}

bool media_command(Player& p, int app) {
    switch (app) {
        case APPCOMMAND_MEDIA_PLAY_PAUSE: command(p, CMD_PLAYPAUSE); return true;
        case APPCOMMAND_MEDIA_PLAY: if (!playing(p)) command(p, CMD_PLAYPAUSE); return true;
        case APPCOMMAND_MEDIA_PAUSE: if (playing(p)) command(p, CMD_PLAYPAUSE); return true;
        case APPCOMMAND_MEDIA_STOP: p.mp->stop(); refresh_bar(p); return true;
        case APPCOMMAND_MEDIA_NEXTTRACK: command(p, CMD_NEXT); return true;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK: command(p, CMD_PREV); return true;
    }
    return false;
}

void on_note(Player& p, LPARAM lp) {
    using Change = MediaPlayer::Change;
    Change c = p.mp->handle(reinterpret_cast<MediaNote*>(lp));
    int i = p.q.current();
    switch (c) {
        case Change::None: return;
        case Change::Opened:
            if (i >= 0) {
                QueueEntry& e = p.q.at(i);
                if (p.mp->duration() > 0) e.duration = p.mp->duration();
                if (!p.mp->title().empty()) e.title = p.mp->title();
                e.failed = false;
                ListView_RedrawItems(p.list, i, i);
            }
            InvalidateRect(p.video, nullptr, FALSE);
            update_title(p);
            break;
        case Change::State:
            if (p.mp->state() == State::Playing) { p.failsInRow = 0; p.message.clear(); }
            if (p.mp->state() == State::Ended) { track_ended(p); return; }
            break;
        case Change::Failed: on_failed(p); return;
        case Change::DeviceLost:              // (headphones unplugged: on with the next device, same place)
            if (i >= 0) p.mp->open(p.q.at(i).path, playing(p), p.mp->position());
            break;
    }
    refresh_bar(p);
}

// ------------------------------------------------------------------
// the queue entries' tags (title, duration), read on a thread of the player's own: the
// app's worker pool stops with the main window
// ------------------------------------------------------------------
struct TagJob { unsigned id; std::wstring path; };
struct TagInfo { unsigned id; std::wstring title; long long duration = -1; };
std::mutex s_tagMx;
std::deque<TagJob> s_tagJobs;
std::vector<TagInfo> s_tagDone;     // read, not yet shown: one message for them all (a folder of
bool s_tagBusy = false;             // thousands mustn't fill the queue the playback's notes use)

void tag_thread() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        TagJob job;
        {
            std::lock_guard<std::mutex> lk(s_tagMx);
            if (s_tagJobs.empty()) { s_tagBusy = false; break; }
            job = std::move(s_tagJobs.front());
            s_tagJobs.pop_front();
        }
        TagInfo info{ job.id, {}, -1 };
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(SHGetPropertyStoreFromParsingName(job.path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps))) && ps) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_Title, &v)) && v.vt == VT_LPWSTR && v.pwszVal) info.title = v.pwszVal;
            PropVariantClear(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_Media_Duration, &v)) && v.vt == VT_UI8) info.duration = (long long)v.uhVal.QuadPart;
            PropVariantClear(&v);
            ps->Release();
        }
        std::lock_guard<std::mutex> lk(s_tagMx);
        s_tagDone.push_back(std::move(info));
        if (s_tagDone.size() == 1 && !PostMessageW(s_msg, WM_QUEUE_INFO, 0, 0)) s_tagDone.clear();
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

void tags_read(Player& p, int from) {
    std::lock_guard<std::mutex> lk(s_tagMx);
    for (int i = from; i < p.q.size(); ++i) s_tagJobs.push_back({ p.q.at(i).id, p.q.at(i).path });
    if (!s_tagBusy && !s_tagJobs.empty()) {
        s_tagBusy = true;
        std::thread(tag_thread).detach();
    }
}

void tags_forget() {
    std::lock_guard<std::mutex> lk(s_tagMx);
    s_tagJobs.clear();
}

void tags_apply() {
    std::vector<TagInfo> done;
    {
        std::lock_guard<std::mutex> lk(s_tagMx);
        done.swap(s_tagDone);
    }
    Player* p = s_p;
    if (!p) return;
    for (const TagInfo& info : done) {
        int i = p->q.index_of(info.id);
        if (i < 0) continue;
        QueueEntry& e = p->q.at(i);
        if (e.title.empty() && !info.title.empty()) e.title = info.title;
        if (e.duration < 0 && info.duration >= 0) e.duration = info.duration;
        if (i == p->q.current()) { update_title(*p); InvalidateRect(p->video, nullptr, FALSE); InvalidateRect(p->bar, nullptr, FALSE); }
    }
    InvalidateRect(p->list, nullptr, FALSE);
}

// ------------------------------------------------------------------
// folders: their media files, sorted by name as Explorer does, subfolders after
// (recursive, hidden and system items left out, junctions not followed, capped)
// ------------------------------------------------------------------
void walk(const std::wstring& dir, int depth, std::vector<std::wstring>& out) {
    if (out.size() >= kMaxFiles || depth > 8) return;
    std::wstring base = dir;
    if (!base.empty() && base.back() != L'\\') base += L'\\';
    std::vector<std::wstring> files, dirs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        std::wstring name = fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (name != L"." && name != L".." && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) dirs.push_back(name);
        } else if (media_file_playable(name)) {
            files.push_back(name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    auto logical = [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; };
    std::sort(files.begin(), files.end(), logical);
    std::sort(dirs.begin(), dirs.end(), logical);
    for (auto& f : files) {
        if (out.size() >= kMaxFiles) return;
        out.push_back(base + f);
    }
    for (auto& d : dirs) walk(base + d, depth + 1, out);
}

std::vector<std::wstring> expand(const std::vector<MediaItem>& items) {
    std::vector<std::wstring> out;
    for (auto& it : items) {
        if (out.size() >= kMaxFiles) break;
        DWORD a = GetFileAttributesW(it.path.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        if (a & FILE_ATTRIBUTE_DIRECTORY) walk(it.path, 0, out);
        else if (media_file_playable(it.path)) out.push_back(it.path);
    }
    return out;
}

// a few hundred names of the folder and its first subfolders: is there media?
bool scan(const std::wstring& dir, int& budget, std::vector<std::wstring>* subs) {
    std::wstring base = dir;
    if (!base.empty() && base.back() != L'\\') base += L'\\';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (--budget < 0) break;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (subs && fd.cFileName[0] != L'.' && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                subs->push_back(base + fd.cFileName);
        } else if (media_file_playable(fd.cFileName)) {
            found = true;
        }
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

bool folder_has_media(const std::wstring& dir) {
    if (PathIsRootW(dir.c_str())) return false;          // a whole drive isn't offered
    // a share, a removable disk: not looked into (it may take a while to answer)
    if (!path_on_internal_drive(dir)) return true;
    int budget = 400;
    std::vector<std::wstring> subs;
    if (scan(dir, budget, &subs)) return true;
    for (size_t i = 0; i < subs.size() && i < 8 && budget > 0; ++i)
        if (scan(subs[i], budget, nullptr)) return true;
    return false;
}

// ------------------------------------------------------------------
// handing over: to this process's player, or another process's
// ------------------------------------------------------------------
struct OpenJob { std::vector<std::wstring> paths; bool enqueue = false; };

bool create_player(bool activate);

bool hand_over(const std::vector<std::wstring>& paths, bool enqueue) {
    HWND other = FindWindowW(player_class().c_str(), nullptr);
    if (!other) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    if (pid == GetCurrentProcessId()) return false;
    std::wstring data = pack_paths(paths);
    COPYDATASTRUCT cd{ enqueue ? kCopyEnqueue : kCopyPlay, (DWORD)(data.size() * sizeof(wchar_t)), (void*)data.data() };
    DWORD_PTR ok = 0;
    if (!SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &ok) || !ok)
        return false;
    if (!enqueue) {                    // (this process is the foreground one: it may hand that on)
        if (IsIconic(other)) ShowWindowAsync(other, SW_RESTORE);
        SetForegroundWindow(other);
    }
    return true;
}

void deliver(const std::vector<std::wstring>& paths, bool enqueue) {
    if (paths.empty()) { MessageBeep(MB_ICONASTERISK); return; }
    if (!s_p) {
        if (hand_over(paths, enqueue)) return;
        if (!create_player(!enqueue)) return;
    } else if (!enqueue) {
        if (IsIconic(s_p->wnd)) ShowWindow(s_p->wnd, SW_RESTORE);
        SetForegroundWindow(s_p->wnd);
    }
    add(*s_p, paths, enqueue);
}

LRESULT CALLBACK msg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PLAYER_OPEN) {
        auto* job = reinterpret_cast<OpenJob*>(lp);
        deliver(job->paths, job->enqueue);
        delete job;
        return 0;
    }
    if (msg == WM_QUEUE_INFO) { tags_apply(); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND msg_window() {
    if (!s_msg) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = g_hinst;
        wc.lpfnWndProc = msg_proc;
        wc.lpszClassName = L"ExplorerPlusPlayerMsg";
        RegisterClassExW(&wc);
        s_msg = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hinst, nullptr);
    }
    return s_msg;
}

// files and folders (expanded on a thread: a big folder or a slow share mustn't hold up
// the window) to `deliver`
void open_items(const std::vector<MediaItem>& items, bool enqueue, bool maybeFolders) {
    HWND target = msg_window();
    if (!maybeFolders) {
        std::vector<std::wstring> paths;
        for (auto& it : items)
            if (media_file_playable(it.path)) paths.push_back(it.path);
        deliver(paths, enqueue);
        return;
    }
    std::thread([items, enqueue, target] {
        auto* job = new OpenJob{ expand(items), enqueue };
        if (!PostMessageW(target, WM_PLAYER_OPEN, 0, (LPARAM)job)) delete job;
    }).detach();
}

// ------------------------------------------------------------------
// painting
// ------------------------------------------------------------------
void slider(HDC dc, Player& p, const RECT& rail, double frac, bool enabled, bool hot) {
    const Palette& c = pal();
    frac = std::clamp(frac, 0.0, 1.0);
    int x = rail.left + (int)((rail.right - rail.left) * frac);
    fill_rrect(dc, rail, px(2), blend(c.layer, c.text, enabled ? 0.4 : 0.2));
    if (!enabled) return;
    RECT done = rail;
    done.right = x;
    if (done.right > done.left) fill_rrect(dc, done, px(2), c.accent);
    int cy = (rail.top + rail.bottom) / 2, R = px(10);
    RECT thumb{ x - R, cy - R, x + R, cy + R };
    fill_rrect(dc, thumb, R, g_dark ? RGB(69, 69, 69) : RGB(255, 255, 255), g_dark ? RGB(80, 80, 80) : RGB(214, 214, 214));
    int r = hot ? px(7) : px(5);
    RECT dot{ x - r, cy - r, x + r, cy + r };
    fill_rrect(dc, dot, r, c.accent);
}

bool part_enabled(Player& p, int part) {
    switch (part) {
        case P_SEEK: return seekable(p);
        case P_PREV: case P_PLAY: case P_NEXT: return !p.q.empty();
        default: return true;
    }
}

wchar_t part_glyph(Player& p, int part) {
    switch (part) {
        case P_SHUFFLE: return L'\xE8B1';
        case P_PREV:    return L'\xE892';
        case P_PLAY:    return playing(p) ? L'\xE769' : L'\xE768';
        case P_NEXT:    return L'\xE893';
        case P_REPEAT:  return p.q.repeat == Repeat::One ? L'\xE8ED' : L'\xE8EE';
        case P_MUTE:
            if (p.muted || p.volume == 0) return L'\xE74F';
            return p.volume < 34 ? L'\xE993' : p.volume < 67 ? L'\xE994' : L'\xE995';
        case P_QUEUE:   return L'\xE8FD';
        case P_FULL:    return p.full ? L'\xE73F' : L'\xE740';
    }
    return 0;
}

bool part_on(Player& p, int part) {
    switch (part) {
        case P_SHUFFLE: return p.q.shuffle();
        case P_REPEAT:  return p.q.repeat != Repeat::Off;
        case P_QUEUE:   return p.queueShown && !p.full;
    }
    return false;
}

std::wstring part_name(Player& p, int part, std::wstring* key = nullptr) {
    std::wstring k, n;
    switch (part) {
        case P_SEEK:    n = L"Position"; k = L"Left, Right"; break;
        case P_SHUFFLE: n = p.q.shuffle() ? L"Shuffle: on" : L"Shuffle: off"; k = L"Ctrl+H"; break;
        case P_PREV:    n = L"Previous"; k = L"Ctrl+Left"; break;
        case P_PLAY:    n = playing(p) ? L"Pause" : L"Play"; k = L"Space"; break;
        case P_NEXT:    n = L"Next"; k = L"Ctrl+Right"; break;
        case P_REPEAT:
            n = p.q.repeat == Repeat::One ? L"Repeat: one" : p.q.repeat == Repeat::All ? L"Repeat: all" : L"Repeat: off";
            k = L"Ctrl+T";
            break;
        case P_MUTE:    n = p.muted ? L"Unmute" : L"Mute"; k = L"M"; break;
        case P_VOLUME:  n = L"Volume"; k = L"Up, Down"; break;
        case P_QUEUE:   n = p.queueShown ? L"Hide play queue" : L"Show play queue"; break;
        case P_FULL:    n = p.full ? L"Exit full screen" : L"Full screen"; k = p.full ? L"Esc" : L"F"; break;
    }
    if (key) *key = k;
    return n;
}

void paint_bar(Player& p) {
    BufferedPaint bp(p.bar);
    HDC dc = bp.dc;
    const Palette& c = pal();
    fill_rect(dc, bp.rc, c.layer);
    RECT line{ 0, 0, bp.rc.right, 1 };
    fill_rect(dc, line, c.divider);
    // position
    bool canSeek = seekable(p);
    LONGLONG dur = p.mp->duration();
    LONGLONG pos = p.drag == P_SEEK ? p.dragPos : p.mp->position();
    int seekY = (p.seekRail.top + p.seekRail.bottom) / 2;
    RECT tl{ px(12), seekY - px(10), p.seekRail.left - px(12), seekY + px(10) };
    RECT tr{ p.seekRail.right + px(12), seekY - px(10), bp.rc.right - px(12), seekY + px(10) };
    COLORREF timeColor = canSeek ? c.textSecondary : c.textDisabled;
    text_at(dc, canSeek ? fmt_duration(pos) : L"0:00", tl, timeColor, p.fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text_at(dc, dur > 0 ? fmt_duration(dur) : L"0:00", tr, timeColor, p.fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    slider(dc, p, p.seekRail, dur > 0 ? (double)pos / (double)dur : 0.0, canSeek, p.hot == P_SEEK || p.drag == P_SEEK);
    // buttons
    for (int part : { P_SHUFFLE, P_PREV, P_PLAY, P_NEXT, P_REPEAT, P_MUTE, P_QUEUE, P_FULL }) {
        RECT r = p.parts[part];
        bool en = part_enabled(p, part);
        bool hot = en && (p.hot == part || p.pressed == part);
        if (part == P_PLAY) {
            int d = px(44), cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            RECT circle{ cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2 };
            COLORREF fill = en ? c.accent : c.textDisabled;
            if (hot) fill = blend(fill, c.layer, p.pressed == part ? 0.25 : 0.12);
            fill_rrect(dc, circle, d / 2, fill);
            glyph_at(dc, part_glyph(p, part), circle, on_accent(), p.glyphsBig);
            continue;
        }
        if (hot) fill_rrect(dc, r, px(4), p.pressed == part ? c.pressed : c.hover);
        bool on = part_on(p, part);
        COLORREF fg = !en ? c.textDisabled : on ? c.accent : c.text;
        glyph_at(dc, part_glyph(p, part), r, fg, p.glyphs);
        if (on) {                      // (not by colour alone: a short bar under the glyph)
            int cx = (r.left + r.right) / 2;
            RECT mark{ cx - px(6), r.bottom - px(6), cx + px(6), r.bottom - px(4) };
            fill_rrect(dc, mark, px(1), c.accent);
        }
    }
    slider(dc, p, p.volRail, p.muted ? 0.0 : p.volume / 100.0, true, p.hot == P_VOLUME || p.drag == P_VOLUME);
    // what is playing
    if (p.titleRight - p.titleLeft > px(40)) {
        int cy = (p.parts[P_PLAY].top + p.parts[P_PLAY].bottom) / 2;
        std::wstring t = current_title(p), d = current_detail(p);
        if (d.empty() && p.mp->state() == State::Failed) d = p.message;
        RECT r1{ p.titleLeft, cy - px(20), p.titleRight, d.empty() ? cy + px(20) : cy };
        RECT r2{ p.titleLeft, cy, p.titleRight, cy + px(20) };
        UINT fmt = DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | (d.empty() ? DT_VCENTER : DT_BOTTOM);
        text_at(dc, t, r1, c.text, p.fontBold, fmt);
        if (!d.empty()) text_at(dc, d, r2, c.textSecondary, p.fontSmall, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// the video area without a video: what is playing (or how to get something to play)
void paint_art(Player& p, HDC dc, const RECT& rc) {
    const Palette& c = pal();
    fill_rect(dc, rc, c.content);
    std::wstring title, detail;
    wchar_t g = L'\xE8D6';                     // (music note)
    if (p.q.empty()) {
        title = L"Nothing to play";
        detail = L"Choose Play or Queue in Explorer Plus in a file's menu, or drop files here.";
    } else if (p.q.current() < 0) {
        title = L"Play queue";
        detail = std::to_wstring(p.q.size()) + (p.q.size() == 1 ? L" item" : L" items");
    } else {
        title = current_title(p);
        detail = current_detail(p);
        if (p.mp->has_video()) g = L'\xE714';      // (video)
    }
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < px(80) || h < px(80)) return;
    int art = std::min(px(120), h / 3);
    int top = rc.top + (h - art - px(100)) / 2;
    if (top < rc.top + px(8)) top = rc.top + px(8);
    RECT ga{ rc.left, top, rc.right, top + art };
    glyph_at(dc, g, ga, c.textDisabled, p.glyphsArt);
    RECT tr{ rc.left + px(24), ga.bottom + px(12), rc.right - px(24), ga.bottom + px(52) };
    text_at(dc, title, tr, c.text, p.fontBig, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
    RECT dr{ tr.left, tr.bottom, tr.right, tr.bottom + px(24) };
    text_at(dc, detail, dr, c.textSecondary, p.font, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
    if (!p.message.empty()) {
        RECT mr{ tr.left, dr.bottom + px(8), tr.right, dr.bottom + px(56) };
        text_at(dc, p.message, mr, c.text, p.font, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
    }
}

void paint_main(Player& p) {
    BufferedPaint bp(p.wnd);
    const Palette& c = pal();
    fill_rect(bp.dc, bp.rc, c.layer);           // (the gap between the video and the queue)
    if (!p.queueShown || p.full) return;
    RECT head{ p.splitX + px(4), 0, bp.rc.right, p.headH };
    fill_rect(bp.dc, head, c.content);
    RECT tr{ head.left + px(12), 0, p.clearBtn.left - px(8), p.headH };
    std::wstring count = std::to_wstring(p.q.size()) + (p.q.size() == 1 ? L" item" : L" items");
    HGDIOBJ of = SelectObject(bp.dc, p.fontBold);
    int tw = text_width(bp.dc, L"Play queue");
    SelectObject(bp.dc, of);
    text_at(bp.dc, L"Play queue", tr, c.text, p.fontBold, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT cr = tr;
    cr.left += tw + px(8);
    if (cr.right > cr.left) text_at(bp.dc, count, cr, c.textSecondary, p.fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (p.clearHot && !p.q.empty()) fill_rrect(bp.dc, p.clearBtn, px(4), c.hover);
    glyph_at(bp.dc, L'\xE74D', p.clearBtn, p.q.empty() ? c.textDisabled : c.text, p.glyphs);
}

// ------------------------------------------------------------------
// screen readers: the controls bar's buttons and sliders
// ------------------------------------------------------------------
const AccSource s_acc = [] {
    AccSource a;
    a.name = L"Player controls";
    a.role = ROLE_SYSTEM_TOOLBAR;
    a.count = [] { return s_p ? (int)P_COUNT : 0; };
    a.child = [](int i) {
        AccChild ch;
        if (!s_p) return ch;
        Player& p = *s_p;
        ch.name = part_name(p, i, &ch.shortcut);
        ch.rc = p.parts[i];
        bool slider = i == P_SEEK || i == P_VOLUME;
        ch.role = slider ? ROLE_SYSTEM_SLIDER : ROLE_SYSTEM_PUSHBUTTON;
        ch.action = slider ? L"" : L"Press";
        if (i == P_SEEK) ch.value = fmt_duration(p.mp->position()) + L" of " + fmt_duration(p.mp->duration());
        if (i == P_VOLUME) ch.value = std::to_wstring(p.muted ? 0 : p.volume);
        if (!part_enabled(p, i)) ch.state |= STATE_SYSTEM_UNAVAILABLE;
        if (part_on(p, i)) ch.state |= STATE_SYSTEM_CHECKED;
        if (p.hot == i) ch.state |= STATE_SYSTEM_HOTTRACKED;
        return ch;
    };
    // (posted: whatever it does mustn't run inside the screen reader's call)
    a.action = [](int i) { if (s_p && kPartCmd[i]) PostMessageW(s_p->wnd, WM_COMMAND, kPartCmd[i], 0); };
    a.value = [] { return s_p ? state_text(*s_p) : std::wstring(); };
    return a;
}();

// ------------------------------------------------------------------
// window procedures
// ------------------------------------------------------------------
int bar_hit(Player& p, POINT pt) {
    for (int i = 0; i < P_COUNT; ++i)
        if (PtInRect(&p.parts[i], pt)) return i;
    return -1;
}

LONGLONG seek_at(Player& p, int x) {
    int w = p.seekRail.right - p.seekRail.left;
    double f = w > 0 ? std::clamp((double)(x - p.seekRail.left) / w, 0.0, 1.0) : 0.0;
    return (LONGLONG)(f * (double)p.mp->duration());
}

int volume_at(Player& p, int x) {
    int w = p.volRail.right - p.volRail.left;
    return w > 0 ? (int)std::lround(std::clamp((double)(x - p.volRail.left) / w, 0.0, 1.0) * 100) : 0;
}

LRESULT CALLBACK bar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Player* pp = s_p;
    if (!pp || !pp->mp) return DefWindowProcW(hwnd, msg, wp, lp);
    Player& p = *pp;
    switch (msg) {
        case WM_GETOBJECT: return acc_get_object(hwnd, wp, lp, &s_acc);
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint_bar(p); return 0;
        case WM_SIZE: layout_bar(p); InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_NOTIFY: {
            auto* hdr = (NMHDR*)lp;
            if (hdr->code == TTN_GETDISPINFOW && hdr->idFrom < P_COUNT) {
                static std::wstring tip;
                std::wstring key;
                tip = part_name(p, (int)hdr->idFrom, &key);
                if ((int)hdr->idFrom == P_VOLUME) tip += L": " + std::to_wstring(p.muted ? 0 : p.volume);
                if (!key.empty()) tip += L" (" + key + L")";
                ((NMTTDISPINFOW*)lp)->lpszText = const_cast<wchar_t*>(tip.c_str());
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (p.full) SetTimer(p.wnd, TIMER_HIDE_BAR, 2500, nullptr);
            if (p.drag == P_SEEK) { p.dragPos = seek_at(p, pt.x); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (p.drag == P_VOLUME) { set_volume(p, volume_at(p, pt.x)); return 0; }
            int h = bar_hit(p, pt);
            if (h != p.hot) { p.hot = h; InvalidateRect(hwnd, nullptr, FALSE); }
            TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&te);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (p.hot >= 0) { p.hot = -1; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int h = bar_hit(p, pt);
            if (h < 0 || !part_enabled(p, h)) return 0;
            SetCapture(hwnd);
            if (h == P_SEEK) { p.drag = P_SEEK; p.dragPos = seek_at(p, pt.x); }
            else if (h == P_VOLUME) { p.drag = P_VOLUME; set_volume(p, volume_at(p, pt.x)); }
            else p.pressed = h;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int drag = p.drag, pressed = p.pressed;
            p.drag = p.pressed = -1;                 // (before ReleaseCapture: WM_CAPTURECHANGED)
            if (GetCapture() == hwnd) ReleaseCapture();
            if (drag == P_SEEK) p.mp->seek(seek_at(p, pt.x));
            if (pressed >= 0 && bar_hit(p, pt) == pressed) command(p, kPartCmd[pressed]);
            refresh_bar(p);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (p.drag >= 0 || p.pressed >= 0) { p.drag = p.pressed = -1; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        case WM_MOUSEWHEEL:
            set_volume(p, p.volume + 5 * GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK video_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Player* pp = s_p;
    if (!pp || !pp->mp) return DefWindowProcW(hwnd, msg, wp, lp);
    Player& p = *pp;
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            bool video = p.mp->has_video() && p.message.empty();
            if (video) {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                if (!p.mp->video_paint()) FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
                EndPaint(hwnd, &ps);
            } else {
                BufferedPaint bp(hwnd);
                paint_art(p, bp.dc, bp.rc);
            }
            return 0;
        }
        case WM_SIZE:
            p.mp->video_resized();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: SetFocus(p.wnd); return 0;
        case WM_LBUTTONDBLCLK: command(p, CMD_FULL); return 0;
        case WM_MOUSEMOVE: full_show_bar(p); return 0;
        case WM_SETCURSOR:
            if (p.full && !p.barInFull) { SetCursor(nullptr); return TRUE; }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void queue_menu(Player& p, POINT pt) {
    if (pt.x == -1 && pt.y == -1) {             // (keyboard: at the focused row)
        RECT r{};
        int i = ListView_GetNextItem(p.list, -1, LVNI_FOCUSED);
        if (i >= 0 && ListView_GetItemRect(p.list, i, &r, LVIR_LABEL)) pt = { r.left + px(16), r.bottom };
        else pt = { px(16), px(16) };
        ClientToScreen(p.list, &pt);
    }
    bool sel = ListView_GetSelectedCount(p.list) > 0;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), CMD_PLAY_SELECTED, L"Play\tEnter");
    AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), CMD_REMOVE_SELECTED, L"Remove from queue\tDelete");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (p.q.empty() ? MF_GRAYED : 0), CMD_CLEAR, L"Clear queue");
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, p.wnd, nullptr);
    DestroyMenu(m);
    if (id) command(p, id);
}

LRESULT list_notify(Player& p, NMHDR* hdr) {
    switch (hdr->code) {
        case LVN_GETDISPINFOW: {
            auto* di = (NMLVDISPINFOW*)hdr;
            int i = di->item.iItem;
            if (i < 0 || i >= p.q.size() || !(di->item.mask & LVIF_TEXT) || !di->item.pszText) break;
            const QueueEntry& e = p.q.at(i);
            lstrcpynW(di->item.pszText, entry_name(e).c_str(), di->item.cchTextMax);
            break;
        }
        case LVN_ODFINDITEMW: {
            // type-ahead: the next entry whose name starts with what was typed (0 would be "row 0")
            auto* fi = (NMLVFINDITEMW*)hdr;
            if (!(fi->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || !fi->lvfi.psz || p.q.empty()) return -1;
            size_t n = wcslen(fi->lvfi.psz);
            int start = fi->iStart >= 0 && fi->iStart < p.q.size() ? fi->iStart : 0;
            for (int k = 0; k < p.q.size(); ++k) {
                int i = (start + k) % p.q.size();
                std::wstring name = entry_name(p.q.at(i));
                if (name.size() >= n && _wcsnicmp(name.c_str(), fi->lvfi.psz, n) == 0) return i;
            }
            return -1;
        }
        case NM_DBLCLK: {
            auto* ia = (NMITEMACTIVATE*)hdr;
            if (ia->iItem >= 0) { p.failsInRow = 0; play_index(p, ia->iItem, true); }
            break;
        }
        case NM_CUSTOMDRAW: {
            // the rows are drawn here (title, duration on the right, Win11's rounded
            // selection); one column: the theme's column lines stay out
            auto* cd = (NMLVCUSTOMDRAW*)hdr;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
            if (cd->nmcd.dwDrawStage == CDDS_POSTPAINT) {
                // (the theme's line at the column's right edge)
                RECT client{};
                GetClientRect(p.list, &client);
                int x = ListView_GetColumnWidth(p.list, 0);
                RECT strip{ x - px(3), client.top, x + 1, client.bottom };
                fill_rect(cd->nmcd.hdc, strip, pal().content);
                return CDRF_DODEFAULT;
            }
            if (cd->nmcd.dwDrawStage != CDDS_ITEMPREPAINT) break;
            int i = (int)cd->nmcd.dwItemSpec;
            if (i < 0 || i >= p.q.size()) return CDRF_SKIPDEFAULT;
            HDC dc = cd->nmcd.hdc;
            const Palette& c = pal();
            RECT rc{};
            ListView_GetItemRect(p.list, i, &rc, LVIR_BOUNDS);
            RECT client{};
            GetClientRect(p.list, &client);
            rc.right = client.right;
            fill_rect(dc, rc, c.content);
            UINT st = ListView_GetItemState(p.list, i, LVIS_SELECTED | LVIS_FOCUSED);
            bool hot = (cd->nmcd.uItemState & CDIS_HOT) != 0;
            RECT box{ rc.left + px(4), rc.top + px(2), rc.right - px(4), rc.bottom - px(2) };
            if (st & LVIS_SELECTED) fill_rrect(dc, box, px(4), hot ? c.pressed : blend(c.content, c.text, 0.10));
            else if (hot) fill_rrect(dc, box, px(4), c.hover);
            if ((st & LVIS_FOCUSED) && GetFocus() == p.list &&
                !(SendMessageW(p.list, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
                fill_rrect(dc, box, px(4), CLR_NONE, c.text);
            const QueueEntry& e = p.q.at(i);
            bool cur = i == p.q.current();
            if (cur) {                                   // (the playing one: a bar on its left, as Win11's selection)
                int h = px(16), cy = (box.top + box.bottom) / 2;
                RECT bar{ box.left, cy - h / 2, box.left + px(3), cy + h / 2 };
                fill_rrect(dc, bar, px(1), c.accent);
            }
            COLORREF fg = cur ? c.accent : e.failed ? c.textDisabled : c.text;
            HFONT f = cur ? p.fontBold : p.font;
            std::wstring d = fmt_duration(e.duration);
            RECT dr{ box.right - px(72), box.top, box.right - px(8), box.bottom };
            RECT tr{ box.left + px(12), box.top, d.empty() ? box.right - px(8) : dr.left - px(8), box.bottom };
            text_at(dc, entry_name(e), tr, fg, f, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (!d.empty())
                text_at(dc, d, dr, cur ? c.accent : c.textSecondary, f, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            return CDRF_SKIPDEFAULT;
        }
    }
    return 0;
}

void apply_theme(Player& p) {
    p.themeSeen = theme_generation();
    BOOL dark = g_dark;
    DwmSetWindowAttribute(p.wnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    theme_style_list(p.list);
    RedrawWindow(p.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}

void save_place(Player& p) {
    WINDOWPLACEMENT wpl{ sizeof(wpl) };
    if (p.full) wpl = p.restore;
    else if (!GetWindowPlacement(p.wnd, &wpl)) return;
    // (the normal position is in workspace coordinates; the restore works in screen ones)
    RECT r = wpl.rcNormalPosition;
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi))
        OffsetRect(&r, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
    settings_set_pref(L"playerX", r.left);
    settings_set_pref(L"playerY", r.top);
    settings_set_pref(L"playerW", r.right - r.left);
    settings_set_pref(L"playerH", r.bottom - r.top);
    settings_set_pref(L"playerMax", wpl.showCmd == SW_SHOWMAXIMIZED);
}

// where the window opens: as last closed (kept on a monitor), else centred on the main
// window's monitor
RECT initial_rect(bool& maximized) {
    int w = settings_pref(L"playerW", 0), h = settings_pref(L"playerH", 0);
    int x = settings_pref(L"playerX", INT_MIN), y = settings_pref(L"playerY", INT_MIN);
    maximized = settings_pref(L"playerMax", 0) != 0;
    bool saved = w >= 300 && h >= 200 && x != INT_MIN && y != INT_MIN;
    RECT r{ x, y, x + w, y + h };
    HMONITOR mon = saved ? MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST)
                 : g_main && IsWindow(g_main) ? MonitorFromWindow(g_main, MONITOR_DEFAULTTONEAREST)
                 : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    RECT work = mi.rcWork;
    if (!saved) {
        UINT dpi = GetDpiForSystem();
        w = MulDiv(880, (int)dpi, 96);
        h = MulDiv(560, (int)dpi, 96);
        x = work.left + (work.right - work.left - w) / 2;
        y = work.top + (work.bottom - work.top - h) / 2;
    }
    w = std::min(w, (int)(work.right - work.left));
    h = std::min(h, (int)(work.bottom - work.top));
    x = std::clamp(x, (int)work.left, (int)work.right - w);
    y = std::clamp(y, (int)work.top, (int)work.bottom - h);
    return { x, y, x + w, y + h };
}

LRESULT CALLBACK player_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

void register_classes() {
    static bool done = false;
    if (done) return;
    done = true;
    HICON big = nullptr, small = nullptr;
    LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &big);
    LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &small);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = g_hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = player_proc;
    wc.lpszClassName = player_class().c_str();
    wc.hIcon = big;
    wc.hIconSm = small ? small : big;
    RegisterClassExW(&wc);
    wc.hIcon = wc.hIconSm = nullptr;
    wc.lpfnWndProc = video_proc;
    wc.lpszClassName = L"ExplorerPlusVideo";
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = bar_proc;
    wc.lpszClassName = L"ExplorerPlusPlayerBar";
    wc.style = 0;
    RegisterClassExW(&wc);
}

HWND make_tip(HWND owner) {
    return CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                           0, 0, 0, 0, owner, nullptr, g_hinst, nullptr);
}

void add_tool(HWND tip, HWND owner, UINT_PTR id, const wchar_t* text) {
    if (!tip) return;
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = owner;
    ti.uId = id;
    ti.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void on_create(Player& p, HWND hwnd) {
    p.wnd = hwnd;
    p.dpi = (int)GetDpiForWindow(hwnd);
    p.video = CreateWindowExW(0, L"ExplorerPlusVideo", L"Video", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                              0, 0, 0, 0, hwnd, nullptr, g_hinst, nullptr);
    p.list = CreateWindowExW(0, WC_LISTVIEWW, L"Play queue",
                             WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS | LVS_REPORT | LVS_OWNERDATA | LVS_NOCOLUMNHEADER |
                                 LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,     // (the row-height image list is ours)
                             0, 0, 0, 0, hwnd, nullptr, g_hinst, nullptr);
    ListView_SetExtendedListViewStyle(p.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col{};
    col.mask = LVCF_WIDTH | LVCF_FMT;
    col.fmt = LVCFMT_LEFT;
    col.cx = 200;
    ListView_InsertColumn(p.list, 0, &col);
    p.bar = CreateWindowExW(0, L"ExplorerPlusPlayerBar", L"Empty", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                            0, 0, 0, 0, hwnd, nullptr, g_hinst, nullptr);
    make_fonts(p);
    p.barTip = make_tip(p.bar);
    for (int i = 1; i < P_COUNT; ++i) add_tool(p.barTip, p.bar, (UINT_PTR)i, LPSTR_TEXTCALLBACKW);
    p.headTip = make_tip(hwnd);
    add_tool(p.headTip, hwnd, 1, L"Clear queue");
    p.mp = new MediaPlayer(hwnd, WM_MEDIA_NOTE, p.video);
    apply_volume(p);
    apply_theme(p);
    DragAcceptFiles(hwnd, TRUE);
    p.lastFocus = hwnd;
    layout(p);
    refresh_all(p);
}

void on_destroy(Player& p) {
    KillTimer(p.wnd, TIMER_POSITION);
    KillTimer(p.wnd, TIMER_SKIP);
    KillTimer(p.wnd, TIMER_HIDE_BAR);
    save_place(p);
    tags_forget();
    p.mp->close();
    // notes still on their way to the window hold references: let them go
    MSG m;
    while (PeekMessageW(&m, p.wnd, WM_MEDIA_NOTE, WM_MEDIA_NOTE, PM_REMOVE)) p.mp->handle(reinterpret_cast<MediaNote*>(m.lParam));
    delete p.mp;
    p.mp = nullptr;
    delete_fonts(p);
}

LRESULT CALLBACK player_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Player* pp = s_p;
    if (msg == WM_CREATE && pp) { on_create(*pp, hwnd); return 0; }
    if (!pp || pp->wnd != hwnd || !pp->mp) return DefWindowProcW(hwnd, msg, wp, lp);
    Player& p = *pp;
    switch (msg) {
        case WM_MEDIA_NOTE: on_note(p, lp); return 0;
        case WM_COMMAND: if (HIWORD(wp) == 0 && LOWORD(wp) >= CMD_PLAYPAUSE) command(p, LOWORD(wp)); return 0;
        case WM_APPCOMMAND:
            if (media_command(p, GET_APPCOMMAND_LPARAM(lp))) return TRUE;
            break;
        case WM_TIMER:
            if (wp == TIMER_POSITION) refresh_bar(p);
            else if (wp == TIMER_SKIP) { KillTimer(hwnd, TIMER_SKIP); next(p); }
            else if (wp == TIMER_HIDE_BAR) {
                POINT c{};
                GetCursorPos(&c);
                if (p.full && p.drag < 0 && WindowFromPoint(c) != p.bar) {
                    KillTimer(hwnd, TIMER_HIDE_BAR);
                    p.barInFull = false;
                    layout(p);
                }
            }
            return 0;
        case WM_NOTIFY: {
            auto* hdr = (NMHDR*)lp;
            if (hdr->hwndFrom == p.list) return list_notify(p, hdr);
            break;
        }
        case WM_CONTEXTMENU:
            if ((HWND)wp == p.list) { queue_menu(p, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }); return 0; }
            break;
        case WM_COPYDATA: {
            // another Explorer Plus process hands over its files
            auto* cd = (COPYDATASTRUCT*)lp;
            if (!cd || (cd->dwData != kCopyPlay && cd->dwData != kCopyEnqueue) || cd->cbData % sizeof(wchar_t) ||
                cd->cbData > (64u << 20))
                return FALSE;
            auto* job = new OpenJob;
            job->enqueue = cd->dwData == kCopyEnqueue;
            // (one path at a time, up to the cap: a huge payload isn't all materialised)
            const wchar_t* c = (const wchar_t*)cd->lpData;
            const wchar_t* end = c + cd->cbData / sizeof(wchar_t);
            while (c < end && job->paths.size() < kMaxFiles) {
                const wchar_t* z = std::find(c, end, L'\0');
                if (z - c > 0 && z - c < 32768) {
                    std::wstring s(c, z);
                    if (!PathIsRelativeW(s.c_str()) && media_file_playable(s)) job->paths.push_back(std::move(s));
                }
                c = z + (z < end ? 1 : 0);
            }
            // (played after the sender's call returns: it waits meanwhile)
            if (job->paths.empty() || !PostMessageW(msg_window(), WM_PLAYER_OPEN, 0, (LPARAM)job)) { delete job; return FALSE; }
            return TRUE;
        }
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            std::vector<MediaItem> items;
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                WCHAR path[MAX_PATH * 4]{};
                if (DragQueryFileW(drop, i, path, ARRAYSIZE(path))) items.push_back({ path, true });
            }
            DragFinish(drop);
            open_items(items, true, true);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint_main(p); return 0;
        case WM_SIZE: layout(p); return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = { px(500), px(320) };
            return 0;
        }
        case WM_SETCURSOR:
            if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT && p.queueShown && !p.full) {
                POINT c{};
                GetCursorPos(&c);
                ScreenToClient(hwnd, &c);
                if (c.x >= p.splitX && c.x < p.splitX + px(4)) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
            }
            break;
        case WM_MOUSEMOVE: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (p.splitting) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                p.queueW96 = std::clamp(MulDiv(rc.right - pt.x, 96, p.dpi), 160, 900);
                layout(p);
                return 0;
            }
            bool hot = PtInRect(&p.clearBtn, pt) != FALSE;
            if (hot != p.clearHot) { p.clearHot = hot; InvalidateRect(hwnd, &p.clearBtn, FALSE); }
            TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&te);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (p.clearHot) { p.clearHot = false; InvalidateRect(hwnd, &p.clearBtn, FALSE); }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (PtInRect(&p.clearBtn, pt) && !p.q.empty()) { command(p, CMD_CLEAR); return 0; }
            if (p.queueShown && !p.full && pt.x >= p.splitX && pt.x < p.splitX + px(4)) {
                p.splitting = true;
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (p.splitting) {
                p.splitting = false;
                ReleaseCapture();
                settings_set_pref(L"playerQueueWidth", p.queueW96);
            }
            return 0;
        case WM_CAPTURECHANGED:
            p.splitting = false;
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                HWND f = GetFocus();
                p.lastFocus = f == p.list ? p.list : hwnd;
            } else if (!HIWORD(wp)) {
                SetFocus(p.lastFocus == p.list && IsWindowVisible(p.list) ? p.list : hwnd);
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (!((lp && lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) || wp == SPI_SETHIGHCONTRAST)) break;
            [[fallthrough]];
        case WM_SYSCOLORCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            // (the main window may have read the change first: the generation tells)
            theme_reload();
            if (p.themeSeen != theme_generation()) apply_theme(p);
            break;
        case WM_DPICHANGED: {
            p.dpi = HIWORD(wp);
            make_fonts(p);
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout(p);
            layout_bar(p);
            InvalidateRect(p.bar, nullptr, FALSE);
            return 0;
        }
        case WM_ENDSESSION:
            if (wp) { save_place(p); settings_save_all(); }
            return 0;
        case WM_CLOSE:
            // (sent while the playback closes, which answers sent messages: once that's done)
            if (p.mp->closing()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            else DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            on_destroy(p);
            bool quit = p.mainClosed;
            s_p = nullptr;
            delete pp;
            for (int attempt = 0; attempt < 3 && !settings_save_all(); ++attempt) Sleep(200);
            if (quit) PostQuitMessage(0);     // (the main window has gone before)
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool create_player(bool activate) {
    msg_window();
    register_classes();
    s_p = new Player;
    Player& p = *s_p;
    p.volume = std::clamp(settings_pref(L"playerVolume", 80), 0, 100);
    p.muted = settings_pref(L"playerMuted", 0) != 0;
    p.q.repeat = (Repeat)std::clamp(settings_pref(L"playerRepeat", 0), 0, 2);
    p.q.set_shuffle(settings_pref(L"playerShuffle", 0) != 0);
    p.queueShown = settings_pref(L"playerQueue", 1) != 0;
    p.queueW96 = std::clamp(settings_pref(L"playerQueueWidth", 300), 160, 900);
    bool maximized = false;
    RECT r = initial_rect(maximized);
    HWND w = CreateWindowExW(0, player_class().c_str(), L"Player", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, g_hinst, nullptr);
    if (!w || !p.mp) {
        if (w) DestroyWindow(w);
        delete s_p;
        s_p = nullptr;
        return false;
    }
    ShowWindow(w, maximized ? SW_SHOWMAXIMIZED : activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    if (activate) SetForegroundWindow(w);
    return true;
}

} // namespace

// ------------------------------------------------------------------
// public
// ------------------------------------------------------------------
bool player_offer(const std::vector<MediaItem>& items) {
    int folders = 0;
    for (auto& it : items) {
        if (!it.folder) {
            if (media_file_playable(it.path)) return true;
            continue;
        }
        if (++folders > 4) continue;           // (a few folders looked into, no more)
        if (folder_has_media(it.path)) return true;
    }
    return false;
}

void player_open(const std::vector<MediaItem>& items, bool enqueue) {
    bool folders = std::any_of(items.begin(), items.end(), [](const MediaItem& it) { return it.folder; });
    open_items(items, enqueue, folders);
}

HWND player_window() { return s_p ? s_p->wnd : nullptr; }

bool player_owns(HWND h) {
    return s_p && h && (h == s_p->wnd || GetAncestor(h, GA_ROOT) == s_p->wnd);
}

bool player_translate(MSG* m) {
    if (!s_p || !s_p->mp || (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN)) return false;
    Player& p = *s_p;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    bool inList = GetFocus() == p.list;
    UINT vk = (UINT)m->wParam;
    // a toggle held down acts once (its auto-repeat would flip it back and forth)
    bool held = (m->lParam & 0x40000000) != 0;
    if (m->message == WM_SYSKEYDOWN) {
        if (vk == VK_RETURN && alt) { if (!held) command(p, CMD_FULL); return true; }
        return false;                          // (Alt+F4, Alt+Space: the system's)
    }
    if (alt) return false;
    switch (vk) {
        case VK_SPACE:
            if (!held) command(p, CMD_PLAYPAUSE);
            return true;
        case 'K': case 'P':
            if (ctrl != (vk == 'P')) return false;     // K, Ctrl+P
            if (!held) command(p, CMD_PLAYPAUSE);
            return true;
        case VK_LEFT: case VK_RIGHT:
            if (ctrl) { command(p, vk == VK_LEFT ? CMD_PREV : CMD_NEXT); return true; }
            if (inList) return false;
            if (seekable(p)) {
                LONGLONG step = (shift ? 30 : 5) * 10000000LL;
                p.mp->seek(p.mp->position() + (vk == VK_LEFT ? -step : step));
                refresh_bar(p);
            }
            return true;
        case 'B':
            if (!ctrl) return false;
            command(p, CMD_PREV);
            return true;
        case 'F':
            if (!ctrl && held) return true;
            command(p, ctrl ? CMD_NEXT : CMD_FULL);
            return true;
        case VK_UP: case VK_DOWN:
            if (inList || ctrl) return false;
            set_volume(p, p.volume + (vk == VK_UP ? 5 : -5));
            return true;
        case 'M':
            if (ctrl) return false;
            if (!held) command(p, CMD_MUTE);
            return true;
        case 'H': case 'T':
            if (!ctrl) return false;
            if (!held) command(p, vk == 'H' ? CMD_SHUFFLE : CMD_REPEAT);
            return true;
        case VK_F11:
            if (!held) command(p, CMD_FULL);
            return true;
        case VK_ESCAPE:
            if (!p.full) return false;
            if (!held) command(p, CMD_FULL);
            return true;
        case VK_TAB: case VK_F6:
            if (ctrl) return false;
            if (IsWindowVisible(p.list)) SetFocus(inList ? p.wnd : p.list);
            return true;
        case VK_RETURN:
            if (!inList) return false;
            command(p, CMD_PLAY_SELECTED);
            return true;
        case VK_DELETE:
            if (!inList) return false;
            command(p, CMD_REMOVE_SELECTED);
            return true;
    }
    return false;
}

bool player_app_command(int cmd) {
    return s_p && s_p->mp && media_command(*s_p, cmd);
}

bool player_main_closed() {
    if (!s_p) return false;
    s_p->mainClosed = true;
    return true;
}
