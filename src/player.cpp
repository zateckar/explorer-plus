// Media player window: the video (or, for sound, the album cover, the tags and a
// spectrogram; or a picture), the play queue on the right, the controls bar at the
// bottom. Playback is media.cpp's (Media Foundation); the queue's logic is playlist.cpp's;
// pictures are decoded by imageview.cpp, tracks' loudness measured by loudness.cpp.
#include "player.h"
#include "media.h"
#include "mediadata.h"
#include "mediainfo.h"
#include "imageview.h"
#include "loudness.h"
#include "spectrum.h"
#include "visual.h"
#include "playlist.h"
#include "imageedit.h"
#include "theme.h"
#include "settings.h"
#include "ini.h"
#include "acc.h"
#include "resource.h"
#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>
#include <dwmapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <array>
#include <cmath>
#include <cwctype>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace {

const UINT WM_MEDIA_NOTE = WM_APP + 40;     // lParam: MediaNote* (media.cpp)
const UINT WM_PLAYER_OPEN = WM_APP + 41;    // to the message window, lParam: OpenJob*
const UINT WM_QUEUE_INFO = WM_APP + 42;     // to the message window: tags read (s_tagDone)
const UINT WM_MEDIA_INFO = WM_APP + 43;     // lParam: MediaInfoNote* (mediainfo.cpp)
const UINT WM_IMAGE = WM_APP + 44;          // lParam: ImageNote* (imageview.cpp)
const UINT WM_LOUDNESS = WM_APP + 45;       // lParam: LoudnessNote* (loudness.cpp)
const UINT WM_VIS_FAILED = WM_APP + 46;     // the visualizations can't be drawn (visual.cpp)
const UINT WM_PLAYLIST_FAILED = WM_APP + 47;    // to the message window, lParam: std::wstring* (the file)
const UINT WM_RENAME_DONE = WM_APP + 48;    // a tab's name edited: wParam 1 taken, 0 not; lParam: the box
// WM_COPYDATA: 'EPP1' play, 'EPP2' queue, 'EPP3' view (the first path: the picture to show),
// 'EPP4' opened by its file type (play; queued after one opened a moment before: Explorer
// opens each of the files selected in a process of its own)
const ULONG_PTR kCopyPlay = 0x45505031, kCopyEnqueue = 0x45505032, kCopyView = 0x45505033, kCopyOpen = 0x45505034;
const ULONGLONG kOpenTogetherMs = 2500;     // files opened this close together: one queue
const UINT_PTR TIMER_POSITION = 1, TIMER_SKIP = 2, TIMER_HIDE_BAR = 3, TIMER_SPECTRUM = 4, TIMER_LEVEL = 5, TIMER_SUBS = 6, TIMER_ANIM = 7;
const UINT_PTR TIMER_KEEP = 8;              // the queues kept a moment after a change
const UINT kKeepDelayMs = 1000;
const UINT_PTR TIMER_DRAG_TAB = 9;          // rows dragged held over a tab: it's shown
const UINT_PTR TIMER_DRAG_SCROLL = 10;      // rows dragged near the list's top or bottom: it scrolls
const UINT kDragTabMs = 600, kDragScrollMs = 50;
const UINT_PTR kTipPlus = 2, kTipTab = 100; // the window's tooltips: 1 Clear queue, "+", the tabs (+ index)
const wchar_t kAppId[] = L"RadimZatecka.ExplorerPlus.Player";   // its own taskbar button
const UINT s_taskbarCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
const size_t kMaxFiles = 10000;             // a folder walk stops there

enum Cmd : UINT {
    CMD_PLAYPAUSE = 100, CMD_PREV, CMD_NEXT, CMD_SHUFFLE, CMD_REPEAT, CMD_MUTE, CMD_QUEUE, CMD_FULL,
    CMD_CLEAR, CMD_PLAY_SELECTED, CMD_REMOVE_SELECTED, CMD_LEVEL, CMD_VIS_NEXT, CMD_VIS_PREV,
    CMD_VIS_CYCLE, CMD_SAVE_PLAYLIST, CMD_EDIT_PICTURE, CMD_SUBS_OFF, CMD_SUBS_CHOOSE, CMD_SUBS_NEXT, CMD_AUDIO_NEXT,
    CMD_PAGE_PREV, CMD_PAGE_NEXT, CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_FIT, CMD_ZOOM_ACTUAL,
    CMD_TAB_NEW, CMD_TAB_CLOSE, CMD_TAB_RENAME, CMD_TAB_CLOSE_OTHERS, CMD_TAB_CLOSE_RIGHT, CMD_TAB_NEXT, CMD_TAB_PREV,
    CMD_SLIDE = 200,                // + seconds: the slide show's interval
    CMD_VIS = 300,                  // + VisStyle: the visualization
    CMD_SUB = 400,                  // + index: the subtitles shown (Player::subs)
    CMD_AUDIO = 500,                // + index: the audio track played
    CMD_MOVE_TO = 600,              // + tab index: the rows selected moved to its queue (the tab count: a new one)
};
const int kSlideSecs[] = { 2, 3, 5, 10, 20, 30 };
// the controls bar's parts (also its accessible children, in this order)
enum Part { P_SEEK, P_SHUFFLE, P_PREV, P_PLAY, P_NEXT, P_REPEAT, P_MUTE, P_VOLUME, P_QUEUE, P_FULL, P_COUNT };
const UINT kPartCmd[P_COUNT] = { 0, CMD_SHUFFLE, CMD_PREV, CMD_PLAYPAUSE, CMD_NEXT, CMD_REPEAT, CMD_MUTE, 0, CMD_QUEUE, CMD_FULL };

using State = MediaPlayer::State;

// a tab: a queue of its own
struct QueueTab {
    std::wstring name;
    PlayQueue q;
    int top = 0;                    // the list's first row shown, as it was left
};

struct Player {
    HWND wnd = nullptr, video = nullptr, bar = nullptr, list = nullptr, barTip = nullptr, headTip = nullptr;
    MediaPlayer* mp = nullptr;
    // the queues, a tab each: the one shown (in the list) and the one playing may differ
    std::vector<std::unique_ptr<QueueTab>> tabs;
    int shown = 0;
    QueueTab* playing = nullptr;
    PlayQueue* q = nullptr;         // the playing tab's queue (all that playback goes by)
    // the tab row, in the title bar's place: the tabs, "+", the window's buttons (0, 1, 2)
    int rowH = 0, tabW = 0, scroll = 0;     // a tab's width; the strip scrolled (more tabs than fit)
    RECT strip{}, plus{};
    int tabTips = 0;                // tab tooltips registered
    int hotTab = -1, hotClose = -1, dragTab = -1, dragX = 0;
    bool hotPlus = false, dragging = false, tracking = false;
    // the queue's rows dragged: within it, onto another tab (or "+": a new one) to go there;
    // held over a tab, that tab is shown and they go where they're let go in its list
    bool rowDrag = false;
    QueueTab* rowFrom = nullptr;    // their tab
    std::vector<unsigned> rowIds;   // the entries (QueueEntry::id)
    int dropRow = -1;               // where they'd go in the list shown (-1: not over it)
    int dropTab = -1;               // the tab they'd go to the end of (-1: none; -2: "+")
    bool activeWnd = true;
    int capHot = -1, capPressed = -1;
    int markShown = -1;             // the playing tab's mark as drawn (-1: none; else playing * 2 + tab)
    HWND rename = nullptr;          // a tab's name being edited, in this box
    QueueTab* renaming = nullptr;
    HBRUSH editBrush = nullptr;
    int volume = 80;                // 0..100
    bool muted = false;
    bool queueShown = true;
    int queueW96 = 300;             // the queue's width at 96 dpi
    bool full = false, barInFull = false;
    WINDOWPLACEMENT restore{ sizeof(WINDOWPLACEMENT) };    // the window before full screen
    POINT lastCursor{ -1, -1 };
    int dpi = 96;
    HFONT font = nullptr, fontSmall = nullptr, fontBold = nullptr, fontBig = nullptr;
    HFONT glyphs = nullptr, glyphsBig = nullptr, glyphsArt = nullptr, glyphsSmall = nullptr, glyphsMid = nullptr;
    HIMAGELIST rowHeight = nullptr; // (sets the queue's row height)
    unsigned themeSeen = 0;
    std::wstring message;           // why the current entry doesn't play
    int failsInRow = 0;
    unsigned streamRetry = 0;       // the stream (entry id) opened again after it failed
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
    // now playing, for sound: the album cover, the tags, the spectrogram
    unsigned infoFor = 0;           // the entry (QueueEntry::id) `info` is of; 0: none
    MediaInfo info;
    HBITMAP coverBmp = nullptr;     // the cover drawn at `coverSize` px (premultiplied alpha)
    int coverSize = 0;
    SpectrumFeed feed;
    std::deque<std::array<uint8_t, SpectrumFeed::kBands>> spec;      // columns shown, newest last
    HBITMAP specBmp = nullptr;      // the spectrogram as shown (newest column on the right)
    uint32_t* specBits = nullptr;
    int specW = 0, specH = 0;
    LONGLONG specPos = -1;          // the playback position at the last frame
    uint32_t specLut[256]{};
    unsigned specLutTheme = 0;
    // the taskbar button
    ITaskbarList3* taskbar = nullptr;
    int tbState = -1, tbPermille = -1;
    wchar_t tbGlyph = 0;            // the thumbnail's play / pause button shows
    HICON thumbIcons[3]{};          // previous, play / pause, next
    // pictures: the one shown (kept on screen while the next one decodes), the slide show
    ImageNote img;                  // img.id: the entry it's of
    HBITMAP imgBmp = nullptr;       // drawn at the size shown
    int imgBmpW = 0, imgBmpH = 0;
    const void* imgBmpOf = nullptr; // (the pixels it was drawn from)
    bool slideshow = false;         // "playing" a picture: on to the next after `slideSecs`
    int slideSecs = 5;
    ULONGLONG slideStart = 0;       // when the picture came on screen (0: not yet)
    ULONGLONG slideDone = 0;        // ms of it shown before a pause
    size_t animFrame = 0;           // an animation's frame on screen (img.frames)
    // zoomed into: `zoom` times the fitted size (1: fitted), the picture's point at the
    // area's centre (fractions of it); dragged about
    double zoom = 1, zoomX = 0.5, zoomY = 0.5;
    int sharpW = 0, sharpH = 0;     // the size last decoded again for it (0: none)
    bool panning = false;
    POINT panFrom{};
    double panX = 0, panY = 0;      // zoomX, zoomY as the drag began
    // loudness levelling: tracks brought to about the same loudness
    bool level = true;
    double gainDb = 0;              // the current track's
    unsigned levelWait = 0;         // the entry whose start waits for its loudness (not loud, then soft)
    std::map<std::wstring, double> loud;    // measured: path -> LUFS (not finite: couldn't)
    // the visualization (sound): the spectrogram (drawn here) or one of visual.cpp's
    int visStyle = VIS_SPECTROGRAM;
    bool visCycle = false;          // another one with each track
    Visualizer vis;
    std::wstring visCaption;        // (what the caption was last made of)
    // video: subtitles (files beside it named after it, or one chosen), drawn over the picture
    bool subsOn = true;             // the user's choice: shown when a video has some
    std::wstring subsOf;            // the video subs were looked up for
    std::vector<std::wstring> subs; // its subtitle files
    int subPick = -1;               // the one shown; -1: none
    std::vector<SubtitleCue> cues;  // its cues
    std::wstring subShown;          // the text on screen
    RECT subRect{};                 // (the picture it was drawn for, on the screen)
    int subBarTop = -1;             // (and the controls bar's top over it)
    HWND subWnd = nullptr;          // the window they show in
};
Player* s_p = nullptr;
HWND s_msg = nullptr;               // the module's message window (results of its threads)
ULONGLONG s_lastOpen = 0;           // when a file was last opened by its type (kCopyOpen)

int px(int v) { return MulDiv(v, s_p ? s_p->dpi : 96, 96); }

// the shown tab's queue: the list's
PlayQueue& vq(Player& p) { return p.tabs[(size_t)p.shown]->q; }
bool shown_plays(Player& p) { return &vq(p) == p.q; }

// a visualization of visual.cpp's is chosen (and can be drawn): not the spectrogram
bool vis_on(Player& p) { return p.visStyle != VIS_SPECTROGRAM && !p.vis.failed(); }

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
    for (HFONT* f : { &p.font, &p.fontSmall, &p.fontBold, &p.fontBig, &p.glyphs, &p.glyphsBig, &p.glyphsArt, &p.glyphsSmall, &p.glyphsMid })
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
    p.glyphsSmall = glyph_font_at(10, p.dpi);         // (the tab row's: as the Notepad's)
    p.glyphsMid = glyph_font_at(12, p.dpi);
    p.rowHeight = ImageList_Create(1, px(32), ILC_COLOR32, 1, 0);
    if (p.list) {
        SendMessageW(p.list, WM_SETFONT, (WPARAM)p.font, TRUE);
        ListView_SetImageList(p.list, p.rowHeight, LVSIL_SMALL);
    }
    if (p.rename) SendMessageW(p.rename, WM_SETFONT, (WPARAM)p.font, TRUE);
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
std::wstring url_host(const std::wstring& url);

std::wstring file_title(const std::wstring& path) {
    if (media_url(path)) {                  // (a stream: the address's last part, else its server)
        std::wstring s = path.substr(0, path.find_first_of(L"?#"));
        while (!s.empty() && s.back() == L'/') s.pop_back();
        size_t slash = s.find_last_of(L'/');
        std::wstring last = slash == std::wstring::npos ? s : s.substr(slash + 1);
        return slash == std::wstring::npos || slash < path.find(L"://") + 3 || last.empty() ? url_host(path) : last;
    }
    std::wstring s = PathFindFileNameW(path.c_str());
    size_t dot = s.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) s.resize(dot);
    return s;
}

std::wstring entry_name(const QueueEntry& e) { return e.title.empty() ? file_title(e.path) : e.title; }

// files the player takes: sound, video, pictures
bool player_file(const std::wstring& path) { return media_file_playable(path) || image_file(path); }

bool image_entry(Player& p, int i) { return i >= 0 && i < p.q->size() && image_file(p.q->at(i).path); }
bool image_current(Player& p) { return image_entry(p, p.q->current()); }

// a picture in an area aw x ah: fitted (full screen: filling it, a small picture enlarged up
// to 3x; in a window: as big as it is at most), zoomed; `s` screen pixels a pixel of its own
// size, its top-left at x0, y0, dw x dh big
struct ImageView { double fit = 0, s = 0, x0 = 0, y0 = 0, dw = 0, dh = 0; };

ImageView image_view(Player& p, int aw, int ah) {
    ImageView v;
    const ImageNote& im = p.img;
    int fw = im.fullW > 0 ? im.fullW : im.w, fh = im.fullH > 0 ? im.fullH : im.h;
    if (!im.pixels || fw <= 0 || fh <= 0 || aw <= 0 || ah <= 0) return v;
    v.fit = std::min({ p.full ? 3.0 : 1.0, (double)aw / fw, (double)ah / fh });
    v.s = v.fit * p.zoom;
    v.dw = fw * v.s;
    v.dh = fh * v.s;
    // (bigger than the area: covering it, the centre kept so; smaller: centred)
    auto place = [](double& c, double d, int a) {
        c = d <= a ? 0.5 : std::clamp(c, a / 2.0 / d, 1 - a / 2.0 / d);
        return a / 2.0 - c * d;
    };
    v.x0 = place(p.zoomX, v.dw, aw);
    v.y0 = place(p.zoomY, v.dh, ah);
    return v;
}

ImageView image_view(Player& p) {
    RECT rc{};
    GetClientRect(p.video, &rc);
    return image_view(p, rc.right, rc.bottom);
}

// sound Media Foundation decodes from a file: measured for levelling, analysed for the
// spectrogram (not a stream, nor MIDI or a CD track, which other devices play)
bool decodable(const std::wstring& path) { return !media_url(path) && !midi_file(path) && !cd_track_file(path); }

bool stream_current(Player& p) { int i = p.q->current(); return i >= 0 && media_url(p.q->at(i).path); }

// "radio.example.com" of a web address
std::wstring url_host(const std::wstring& url) {
    size_t s = url.find(L"://");
    if (s == std::wstring::npos) return url;
    s += 3;
    size_t e = url.find_first_of(L"/:?#", s);
    return url.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
}

std::wstring current_title(Player& p) {
    int i = p.q->current();
    if (i < 0) return {};
    if (!image_current(p) && !p.mp->title().empty()) return p.mp->title();
    return entry_name(p.q->at(i));
}

std::wstring current_detail(Player& p) {
    if (image_current(p)) {                 // "4032 × 3024 · 250 % · 3 of 120"
        std::wstring s;
        if (p.img.id == p.q->at(p.q->current()).id && p.img.fullW > 0) {
            s = std::to_wstring(p.img.fullW) + L" \x00D7 " + std::to_wstring(p.img.fullH) + L" \x00B7 " +
                (p.img.pages > 1 ? L"page " + std::to_wstring(p.img.page + 1) + L" of " + std::to_wstring(p.img.pages) + L" \x00B7 " : L"");
            if (p.zoom != 1) s += std::to_wstring((int)(image_view(p).s * 100 + 0.5)) + L" % \x00B7 ";
        }
        return s + std::to_wstring(p.q->current() + 1) + L" of " + std::to_wstring(p.q->size());
    }
    std::wstring a = p.mp->artist(), b = p.mp->album();
    if (!a.empty() && !b.empty()) return a + L" \x2014 " + b;
    if (a.empty() && b.empty() && stream_current(p)) return L"Stream \x00B7 " + url_host(p.q->at(p.q->current()).path);
    return a.empty() ? b : a;
}

bool seekable(Player& p) {
    State s = p.mp->state();
    return p.mp->duration() > 0 && (s == State::Playing || s == State::Paused || s == State::Stopped || s == State::Ended);
}

// playing (or about to: its loudness measured first), or (a picture) in a slide show
bool playing(Player& p) { return image_current(p) ? p.slideshow : p.levelWait || p.mp->will_play(); }

std::wstring state_text(Player& p) {
    std::wstring s;
    if (image_current(p)) {
        if (!p.message.empty()) return L"Can't show: " + p.message;
        return std::wstring(p.slideshow ? L"Slide show" : L"Picture") + L" \x00B7 " + std::to_wstring(p.q->current() + 1) +
               L" of " + std::to_wstring(p.q->size());
    }
    State st = p.mp->state();
    if (p.levelWait && st != State::Failed) st = State::Opening;    // (its loudness measured first)
    switch (st) {
        case State::Playing: s = p.mp->will_play() ? L"Playing" : L"Paused"; break;
        case State::Paused:  s = L"Paused"; break;
        case State::Opening: s = L"Opening"; break;
        case State::Failed:  return L"Can't play: " + p.message;
        default:             s = p.q->empty() ? L"Empty" : L"Stopped"; break;
    }
    if (p.mp->duration() > 0)
        s += L" \x00B7 " + fmt_duration(p.mp->position()) + L" / " + fmt_duration(p.mp->duration());
    else if (stream_current(p) && (st == State::Playing || st == State::Paused))
        s += L" \x00B7 " + fmt_duration(p.mp->position()) + L" (stream)";
    return s;
}

void update_title(Player& p) {
    std::wstring t = current_title(p);
    t = t.empty() ? L"Player" : t + L" - Player";
    WCHAR now[512]{};
    GetWindowTextW(p.wnd, now, ARRAYSIZE(now));
    if (t != now) SetWindowTextW(p.wnd, t.c_str());
}

void update_taskbar(Player& p);
bool now_playing(Player& p);
void vis_update(Player& p);
int play_mark(Player& p);
void invalidate_row(Player& p);

// the bar's text (what screen readers and the tests read), its paint, the taskbar button,
// the position timer, the spectrogram's frames and the visualization
void refresh_bar(Player& p) {
    std::wstring t = state_text(p);
    WCHAR now[512]{};
    GetWindowTextW(p.bar, now, ARRAYSIZE(now));
    if (t != now) {
        SetWindowTextW(p.bar, t.c_str());
        acc_notify(p.bar, EVENT_OBJECT_VALUECHANGE, -1);
    }
    InvalidateRect(p.bar, nullptr, FALSE);
    if (p.rowH && play_mark(p) != p.markShown) invalidate_row(p);     // (the playing tab's mark)
    update_taskbar(p);
    bool running = p.mp->state() == State::Playing || (image_current(p) && p.slideshow);
    if (running) SetTimer(p.wnd, TIMER_POSITION, 250, nullptr);
    else KillTimer(p.wnd, TIMER_POSITION);
    if (running && p.feed.active() && now_playing(p)) SetTimer(p.wnd, TIMER_SPECTRUM, 30, nullptr);
    else KillTimer(p.wnd, TIMER_SPECTRUM);
    vis_update(p);
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

// the tab row: tab i, its close button; the window's buttons at the row's right end
RECT tab_rect(const Player& p, int i) {
    int x = p.strip.left + i * p.tabW - p.scroll;
    return { x, px(6), x + p.tabW, p.rowH };
}

RECT tab_close_rect(const Player& p, int i) {
    RECT r = tab_rect(p, i);
    int s = px(24), top = (r.top + r.bottom - s) / 2;
    return { r.right - px(8) - s, top, r.right - px(8), top + s };
}

RECT cap_button(const Player& p, int i) {
    RECT rc{};
    GetClientRect(p.wnd, &rc);
    int w = px(46), right = rc.right - (2 - i) * w;
    return { right - w, 0, right, p.rowH };
}

int tab_at(const Player& p, POINT pt) {
    if (!PtInRect(&p.strip, pt)) return -1;
    for (int i = 0; i < (int)p.tabs.size(); ++i) {
        RECT r = tab_rect(p, i);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

void invalidate_row(Player& p) {
    RECT r{ 0, 0, 0x7FFF, p.rowH };
    InvalidateRect(p.wnd, &r, FALSE);
}

void place_rename(Player& p);

// the tabs share the row (each between a minimum and a maximum width) with the window's
// buttons and some room to drag it by; more than fit scroll, the shown one in view
void layout_tabs(Player& p) {
    RECT rc{};
    GetClientRect(p.wnd, &rc);
    int count = (int)p.tabs.size(), plusW = px(36);
    int left = px(8), avail = std::max(px(80), (int)rc.right - left - plusW - 3 * px(46) - px(48));
    p.tabW = count ? std::clamp(avail / count, px(90), px(220)) : 0;
    int total = count * p.tabW;
    if (total <= avail) {
        p.scroll = 0;
    } else {
        int a = p.shown * p.tabW;
        p.scroll = std::clamp(p.scroll, a + p.tabW - avail, a);
        p.scroll = std::clamp(p.scroll, 0, total - avail);
    }
    p.strip = { left, 0, left + std::min(total, avail), p.rowH };
    int py = px(8);
    p.plus = p.rowH ? RECT{ p.strip.right + px(4), py, p.strip.right + px(4) + px(32), py + px(28) } : RECT{};
    // the tooltips: one per tab, and "+"
    for (int i = 0; i < p.tabTips; ++i) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = p.wnd;
        ti.uId = kTipTab + (UINT_PTR)i;
        SendMessageW(p.headTip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    p.tabTips = p.rowH ? count : 0;
    for (int i = 0; i < p.tabTips; ++i) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = p.wnd;
        ti.uId = kTipTab + (UINT_PTR)i;
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        RECT r = tab_rect(p, i);
        IntersectRect(&ti.rect, &r, &p.strip);
        SendMessageW(p.headTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    set_tool_rect(p.headTip, p.wnd, kTipPlus, p.plus);
    place_rename(p);
}

void layout(Player& p) {
    RECT rc{};
    GetClientRect(p.wnd, &rc);
    int w = rc.right, h = rc.bottom;
    p.rowH = p.full ? 0 : px(40);              // (full screen: no tabs, nothing but the picture)
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
    int top = p.rowH;
    int bodyH = h - barH - top > 0 ? h - barH - top : 0;
    p.splitX = vw;
    p.headH = queue ? px(44) : 0;
    MoveWindow(p.video, 0, top, vw, bodyH, TRUE);
    ShowWindow(p.list, queue ? SW_SHOWNA : SW_HIDE);
    if (queue) MoveWindow(p.list, w - qw, top + p.headH, qw, bodyH - p.headH > 0 ? bodyH - p.headH : 0, TRUE);
    ShowWindow(p.bar, bar ? SW_SHOWNA : SW_HIDE);
    if (bar) MoveWindow(p.bar, 0, h - barH, w, barH, TRUE);
    int cb = px(32);
    p.clearBtn = queue ? RECT{ w - px(8) - cb, top + (p.headH - cb) / 2, w - px(8), top + (p.headH + cb) / 2 } : RECT{};
    set_tool_rect(p.headTip, p.wnd, 1, p.clearBtn);
    layout_tabs(p);
    if (queue) fit_columns(p);
    InvalidateRect(p.wnd, nullptr, FALSE);
    vis_update(p);
}

// ------------------------------------------------------------------
// playback and the queue
// ------------------------------------------------------------------
void apply_volume(Player& p) {
    double v = p.muted ? 0.0 : p.volume / 100.0;
    v *= v;                                 // (closer to how loud it sounds than a straight line)
    // levelled: the track's gain on top (a boost only as far as full volume)
    if (p.level) v *= std::pow(10.0, p.gainDb / 20);
    p.mp->set_volume((float)std::min(1.0, v));
}

// the measured loudness of `path`, or nullptr
const double* loudness_of(Player& p, const std::wstring& path) {
    auto it = p.loud.find(path);
    return it == p.loud.end() ? nullptr : &it->second;
}

// levelling: the gain of the track about to play (until it's measured: the previous
// track's, likely the same album's), and the next track measured ahead
void level_track(Player& p, int i) {
    if (!p.level || image_entry(p, i)) return;
    const std::wstring& path = p.q->at(i).path;
    if (!decodable(path)) p.gainDb = 0;                 // (a stream, MIDI, a CD: as they are)
    else if (const double* l = loudness_of(p, path)) p.gainDb = loudness_gain_db(*l);
    else loudness_request(p.wnd, WM_LOUDNESS, path, true);
    int n = p.q->next(false);
    if (n >= 0 && n != i && !image_entry(p, n) && decodable(p.q->at(n).path) && !loudness_of(p, p.q->at(n).path))
        loudness_request(p.wnd, WM_LOUDNESS, p.q->at(n).path, false);
}

// the start held back for the track's loudness: now (it's known, or took too long)
void level_start(Player& p) {
    if (!p.levelWait) return;
    p.levelWait = 0;
    KillTimer(p.wnd, TIMER_LEVEL);
    State s = p.mp->state();
    if (s == State::Opening || s == State::Stopped || s == State::Paused) p.mp->play();
    refresh_bar(p);
}

void on_loudness(Player& p, LoudnessNote* n) {
    if (p.loud.size() > 5000) p.loud.clear();
    p.loud[n->path] = n->lufs;
    int i = p.q->current();
    if (p.level && i >= 0 && !image_current(p) && p.q->at(i).path == n->path) {
        p.gainDb = loudness_gain_db(n->lufs);
        apply_volume(p);
        InvalidateRect(p.video, nullptr, FALSE);
        if (p.levelWait == p.q->at(i).id) level_start(p);
    }
    delete n;
}

void set_volume(Player& p, int v) {
    p.volume = std::clamp(v, 0, 100);
    p.muted = false;
    apply_volume(p);
    settings_set_pref(L"playerVolume", p.volume);
    settings_set_pref(L"playerMuted", 0);
    InvalidateRect(p.bar, nullptr, FALSE);
}

void tags_read(PlayQueue& q, int from, int count = INT_MAX);
void tags_forget();
void save_queue(Player& p);
void subs_open(Player& p);
void subs_tick(Player& p);
void subs_command(Player& p, UINT cmd);

void on_failed(Player& p);
void spectrum_reset(Player& p);
void set_vis(Player& p, int style);

void image_zoom_reset(Player& p) {
    p.zoom = 1;
    p.zoomX = p.zoomY = 0.5;
    p.sharpW = p.sharpH = 0;
    if (p.panning) ReleaseCapture();
    p.panning = false;
}

// zoomed past the size decoded: decoded again as big as shown (a photo: its own size, all of
// it at once; a drawing: with room to zoom on), 32 megapixels at most
void image_sharpen(Player& p, const ImageView& v) {
    const ImageNote& im = p.img;
    int i = p.q->current();
    if (p.zoom == 1 || v.fit <= 0 || i < 0 || im.id != p.q->at(i).id) return;
    auto capped = [](double w, double h, int& bw, int& bh) {
        double k = std::sqrt(std::min(1.0, (double)(1 << 25) / (w * h)));
        bw = std::max(1, (int)(w * k));
        bh = std::max(1, (int)(h * k));
    };
    int nw = 0, nh = 0, bw = 0, bh = 0;
    capped(im.drawing ? v.dw : std::min(v.dw, (double)im.fullW), im.drawing ? v.dh : std::min(v.dh, (double)im.fullH), nw, nh);
    if (nw <= im.w * 1.05 + 1 && nh <= im.h * 1.05 + 1) return;
    capped(im.drawing ? v.dw * 1.5 : im.fullW, im.drawing ? v.dh * 1.5 : im.fullH, bw, bh);
    if (bw == p.sharpW && bh == p.sharpH) return;       // (asked for already)
    p.sharpW = bw;
    p.sharpH = bh;
    image_request(p.wnd, WM_IMAGE, im.id, p.q->at(i).path, bw, bh, std::wstring(), im.page, 1e6);
}

void image_forget(Player& p) {
    KillTimer(p.wnd, TIMER_ANIM);
    image_zoom_reset(p);
    p.img = {};
    if (p.imgBmp) DeleteObject(p.imgBmp);
    p.imgBmp = nullptr;
    p.imgBmpOf = nullptr;
}

// the box a picture is decoded to: the monitor the window is on
void image_box(Player& p, int& w, int& h) {
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(p.wnd, MONITOR_DEFAULTTONEAREST), &mi);
    w = std::max(320, (int)(mi.rcMonitor.right - mi.rcMonitor.left));
    h = std::max(240, (int)(mi.rcMonitor.bottom - mi.rcMonitor.top));
}

// an animation: its frames in turn, each for its time (a still picture: none)
void anim_start(Player& p) {
    p.animFrame = 0;
    KillTimer(p.wnd, TIMER_ANIM);
    if (p.img.frames.size() > 1 && p.img.delays.size() == p.img.frames.size())
        SetTimer(p.wnd, TIMER_ANIM, (UINT)p.img.delays[0], nullptr);
}

void anim_tick(Player& p) {
    if (!image_current(p) || p.img.frames.size() < 2 || p.img.delays.size() != p.img.frames.size()) {
        KillTimer(p.wnd, TIMER_ANIM);
        return;
    }
    p.animFrame = (p.animFrame + 1) % p.img.frames.size();
    p.img.pixels = p.img.frames[p.animFrame];
    InvalidateRect(p.video, nullptr, FALSE);
    SetTimer(p.wnd, TIMER_ANIM, (UINT)p.img.delays[p.animFrame], nullptr);
}

// a TIFF's page before / after the one shown
void image_page(Player& p, int step) {
    int i = p.q->current();
    if (!image_current(p) || p.img.id != p.q->at(i).id || p.img.pages < 2) return;
    int page = p.img.page + step;
    if (page < 0 || page >= p.img.pages) { MessageBeep(MB_OK); return; }
    int w = 0, h = 0;
    image_box(p, w, h);
    image_request(p.wnd, WM_IMAGE, p.img.id, p.q->at(i).path, w, h, std::wstring(), page);
}

// a picture: shown (decoded on a thread; the one before stays meanwhile), the next one
// decoded ahead; `play`: a slide show
void show_image(Player& p, int i, bool play) {
    p.mp->close();
    const QueueEntry& e = p.q->at(i);
    p.slideshow = play;
    p.slideDone = 0;
    p.slideStart = p.img.id == e.id && p.img.pixels ? GetTickCount64() : 0;
    if (p.img.id != e.id) {
        int n = p.q->next(false);
        int w = 0, h = 0;
        image_box(p, w, h);
        image_request(p.wnd, WM_IMAGE, e.id, e.path, w, h, image_entry(p, n) && n != i ? p.q->at(n).path : std::wstring());
    }
    InvalidateRect(p.video, nullptr, FALSE);
}

void keep_soon(Player& p);

// the list's row i in view, when the list shows the queue playing
void show_row(Player& p, int i) {
    if (shown_plays(p) && i >= 0) ListView_EnsureVisible(p.list, i, FALSE);
}

// opens entry i and plays it (or shows it stopped at the start)
void play_index(Player& p, int i, bool play) {
    KillTimer(p.wnd, TIMER_SKIP);
    KillTimer(p.wnd, TIMER_LEVEL);
    p.levelWait = 0;
    p.streamRetry = 0;
    p.q->set_current(i);
    p.message.clear();
    spectrum_reset(p);
    keep_soon(p);
    if (p.q->current() < 0) {
        p.mp->close();
        p.slideshow = false;
        image_forget(p);
    } else if (image_entry(p, i)) {
        show_image(p, i, play);
        show_row(p, i);
    } else {
        p.slideshow = false;
        image_forget(p);
        const QueueEntry& e = p.q->at(i);
        // the cover and the tags (the panel keeps the album's cover meanwhile)
        if (p.infoFor != e.id) media_info_request(p.wnd, WM_MEDIA_INFO, e.id, e.path, std::max(400, px(400)));
        // not measured yet (the first track, one jumped to): its start waits for that a
        // moment, rather than its volume changing once it's playing
        bool wait = play && p.level && decodable(e.path) && !loudness_of(p, e.path);
        p.mp->open(e.path, play && !wait);
        // (after the open: Media Foundation is loaded then, the measuring needs it; the
        // volume takes effect as the playback starts)
        level_track(p, i);
        apply_volume(p);
        if (wait && p.mp->state() != State::Failed) {
            p.levelWait = e.id;
            SetTimer(p.wnd, TIMER_LEVEL, 1500, nullptr);
        }
        if (p.mp->state() == State::Failed) on_failed(p);
        show_row(p, i);
    }
    refresh_all(p);
}

// nothing playing or shown any more (the queue's current entry stays where it was)
void stop_playback(Player& p) {
    KillTimer(p.wnd, TIMER_SKIP);
    KillTimer(p.wnd, TIMER_LEVEL);
    p.levelWait = 0;
    p.streamRetry = 0;
    p.failsInRow = 0;
    p.message.clear();
    spectrum_reset(p);
    p.mp->close();
    p.slideshow = false;
    image_forget(p);
}

// the queue of tab `t` is the one playing from now on (what played before stops)
void set_playing(Player& p, QueueTab* t) {
    if (p.playing == t) return;
    if (p.playing) stop_playback(p);
    p.playing = t;
    p.q = &t->q;
    InvalidateRect(p.list, nullptr, FALSE);     // (the playing entry's mark)
    invalidate_row(p);
}

void on_failed(Player& p) {
    int i = p.q->current();
    if (i < 0) return;
    // a stream: once more before giving up on it (a server or connection hiccup)
    if (stream_current(p) && p.streamRetry != p.q->at(i).id) {
        p.streamRetry = p.q->at(i).id;
        p.mp->open(p.q->at(i).path, true);
        if (p.mp->state() != State::Failed) { refresh_all(p); return; }
    }
    p.levelWait = 0;
    KillTimer(p.wnd, TIMER_LEVEL);
    p.q->at(i).failed = true;
    p.message = media_error_text(p.mp->error());
    ++p.failsInRow;
    // (on to the next one after a moment, unless nothing can play here at all)
    if (media_available() && p.failsInRow < p.q->size() && p.q->next(false) >= 0)
        SetTimer(p.wnd, TIMER_SKIP, 1500, nullptr);
    refresh_all(p);
}

void track_ended(Player& p) {
    int n = p.q->next(true);
    if (n >= 0) play_index(p, n, true);
    else play_index(p, p.q->first(), false);         // the end of the queue: back to its start
}

// the slide show on or off (the picture's time shown so far kept)
void set_slideshow(Player& p, bool on) {
    if (on == p.slideshow) return;
    ULONGLONG now = GetTickCount64();
    if (!on && p.slideStart) p.slideDone = now - p.slideStart;
    if (on && p.slideStart) p.slideStart = now - p.slideDone;
    p.slideshow = on;
}

// zoomed to `z` times the fitted size, the area's point `at` (null: its centre) staying on
// the same spot of the picture; from its own size (or fitted, if smaller) to 16 screen
// pixels a pixel, stopping at fitted and at its own size on the way
void image_zoom_to(Player& p, double z, const POINT* at) {
    if (!image_current(p) || !p.message.empty()) return;
    RECT rc{};
    GetClientRect(p.video, &rc);
    ImageView v = image_view(p, rc.right, rc.bottom);
    if (v.fit <= 0) return;
    z = std::clamp(z, std::min(1.0, 1 / v.fit), std::max(1.0, 16 / v.fit));
    for (double stop : { 1.0, 1 / v.fit })
        if ((p.zoom - stop) * (z - stop) < 0 || std::abs(z - stop) < 0.01 * stop) z = stop;
    if (z == p.zoom) return;
    POINT m = at ? *at : POINT{ rc.right / 2, rc.bottom / 2 };
    double fx = (m.x - v.x0) / v.dw, fy = (m.y - v.y0) / v.dh;      // (the picture's point there)
    double dw = v.dw / p.zoom * z, dh = v.dh / p.zoom * z;
    p.zoomX = (rc.right / 2.0 - m.x) / dw + fx;
    p.zoomY = (rc.bottom / 2.0 - m.y) / dh + fy;
    p.zoom = z;
    if (z != 1) set_slideshow(p, false);    // (looked into: it stays)
    image_sharpen(p, image_view(p, rc.right, rc.bottom));
    InvalidateRect(p.video, nullptr, FALSE);
    refresh_bar(p);
}

void image_zoom_command(Player& p, UINT cmd) {
    ImageView v = image_view(p);
    if (v.fit <= 0) return;
    image_zoom_to(p, cmd == CMD_ZOOM_IN ? p.zoom * 1.25 : cmd == CMD_ZOOM_OUT ? p.zoom / 1.25 : cmd == CMD_ZOOM_FIT ? 1 : 1 / v.fit, nullptr);
}

// zoomed in further than the area: dragged about
bool image_pannable(Player& p) {
    if (p.zoom == 1 || !image_current(p) || !p.message.empty()) return false;
    RECT rc{};
    GetClientRect(p.video, &rc);
    ImageView v = image_view(p, rc.right, rc.bottom);
    return v.dw > rc.right + 0.5 || v.dh > rc.bottom + 0.5;
}

void image_pan(Player& p, POINT pt) {
    ImageView v = image_view(p);
    if (v.fit <= 0) return;
    p.zoomX = p.panX - (pt.x - p.panFrom.x) / v.dw;
    p.zoomY = p.panY - (pt.y - p.panFrom.y) / v.dh;
    InvalidateRect(p.video, nullptr, FALSE);
    UpdateWindow(p.video);
}

// the playing queue's current entry is on: playing, paused or opening; a picture shown
bool loaded(Player& p) {
    if (image_current(p)) return p.img.id == p.q->at(p.q->current()).id;
    State s = p.mp->state();
    return p.levelWait || s == State::Opening || s == State::Playing || s == State::Paused;
}

void play_pause(Player& p) {
    // nothing on in the queue played last: the one shown plays (where it was left)
    if (!shown_plays(p) && !vq(p).empty() && !loaded(p)) {
        set_playing(p, p.tabs[(size_t)p.shown].get());
        p.failsInRow = 0;
        play_index(p, p.q->current() >= 0 ? p.q->current() : p.q->first(), true);
        return;
    }
    if (p.q->empty()) return;
    if (image_current(p)) {
        if (!loaded(p)) play_index(p, p.q->current(), true);    // (not shown yet: as it was left)
        else set_slideshow(p, !p.slideshow);
        return;
    }
    if (p.levelWait) {                      // (about to start: it doesn't)
        p.levelWait = 0;
        KillTimer(p.wnd, TIMER_LEVEL);
        return;
    }
    switch (p.mp->state()) {
        case State::Playing: case State::Opening:
            if (p.mp->will_play()) p.mp->pause(); else p.mp->play();
            break;
        case State::Paused: case State::Stopped: case State::Ended:
            p.mp->play();
            break;
        default:
            play_index(p, p.q->current() >= 0 ? p.q->current() : p.q->first(), true);
            break;
    }
}

void previous(Player& p) {
    if (p.q->empty()) return;
    bool loaded = p.mp->state() == State::Playing || p.mp->state() == State::Paused;
    int i = p.q->previous();
    // 3 s into the track, or nothing before it: from its start
    if (loaded && (p.mp->position() > 30000000 || i < 0)) { p.mp->seek(0); return; }
    // (from a picture: a slide show goes on, a picture looked at stays looked at)
    play_index(p, i >= 0 ? i : p.q->first(), !image_current(p) || p.slideshow);
}

void next(Player& p) {
    if (p.q->empty()) return;
    int i = p.q->next(false);
    if (i >= 0) play_index(p, i, !image_current(p) || p.slideshow);
    else play_index(p, p.q->first(), false);           // past the end: its start, stopped
}

// the slide show's picture has been shown long enough
void slide_tick(Player& p) {
    int i = p.q->current();
    if (!image_current(p) || !p.slideshow || !p.slideStart || p.img.id != p.q->at(i).id) return;
    if (GetTickCount64() - p.slideStart < (ULONGLONG)p.slideSecs * 1000) return;
    int n = p.q->next(true);
    if (n >= 0) play_index(p, n, true);
    else set_slideshow(p, false);                     // the last one: it stays
}

void on_image(Player& p, ImageNote* n) {
    int i = p.q->current();
    if (i >= 0 && image_current(p) && p.q->at(i).id == n->id) {
        QueueEntry& e = p.q->at(i);
        if (n->pixels) {
            if (p.img.id != n->id || p.img.page != n->page) image_zoom_reset(p);     // (not this one decoded sharper)
            p.img = std::move(*n);
            anim_start(p);
            p.message.clear();
            e.failed = false;
            p.slideStart = GetTickCount64();
            p.slideDone = 0;
        } else {
            image_forget(p);
            p.img.id = e.id;
            e.failed = true;
            p.message = L"This picture can't be opened: its file type isn't supported, or the file is damaged.";
            if (p.slideshow && p.q->next(false) >= 0) SetTimer(p.wnd, TIMER_SKIP, 1500, nullptr);
        }
        if (shown_plays(p)) ListView_RedrawItems(p.list, i, i);
        refresh_all(p);
    }
    delete n;
}

// the shown queue's entries changed: the list, its heading; kept
void queue_changed(Player& p) {
    ListView_SetItemCountEx(p.list, vq(p).size(), LVSICF_NOSCROLL);
    InvalidateRect(p.list, nullptr, FALSE);
    fit_columns(p);
    RECT head{ p.splitX, p.rowH, 0x7FFF, p.rowH + p.headH };
    InvalidateRect(p.wnd, &head, FALSE);
    keep_soon(p);
}

// into the queue shown. `start`: the entry to begin with (a picture viewed: its folder's
// pictures from it)
void add(Player& p, const std::vector<std::wstring>& paths, bool enqueue, const std::wstring& start = {}) {
    if (paths.empty()) return;
    QueueTab* tab = p.tabs[(size_t)p.shown].get();
    if (!enqueue) {
        tags_forget();
        set_playing(p, tab);                // (what played before stops: this plays)
        p.q->replace(paths);
        if (!start.empty())
            for (int i = 0; i < p.q->size(); ++i)
                if (p.q->at(i).path == start) { p.q->set_current(i); break; }
        p.failsInRow = 0;
        queue_changed(p);
        tags_read(*p.q, 0);
        // (a picture is shown, not run as a slide show: Play starts that)
        play_index(p, p.q->current(), !image_current(p));
        return;
    }
    bool idle = !loaded(p);
    int first = tab->q.append(paths);
    queue_changed(p);
    tags_read(tab->q, first);
    if (idle) {                             // (nothing on: this plays)
        set_playing(p, tab);
        p.failsInRow = 0;
        play_index(p, first, !image_entry(p, first));
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
    PlayQueue& v = vq(p);
    bool hit = v.remove(sel) && shown_plays(p);
    ListView_SetItemState(p.list, -1, 0, LVIS_SELECTED);
    queue_changed(p);
    int focus = std::min(sel[0], v.size() - 1);
    if (focus >= 0) ListView_SetItemState(p.list, focus, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
    if (hit) {
        if (p.q->current() >= 0) play_index(p, p.q->current(), was);
        else play_index(p, -1, false);
    }
    refresh_all(p);
}

// the queue shown emptied (playing: it stops)
void clear_queue(Player& p) {
    tags_forget();
    vq(p).clear();
    if (shown_plays(p)) play_index(p, -1, false);
    queue_changed(p);
    refresh_all(p);
}

// ------------------------------------------------------------------
// the tabs: a queue each. The one shown is in the list; the one playing goes on while
// another is looked at (nothing on: the one shown is the one playing)
// ------------------------------------------------------------------
int tab_index(const Player& p, const QueueTab* t) {
    for (size_t i = 0; i < p.tabs.size(); ++i)
        if (p.tabs[i].get() == t) return (int)i;
    return -1;
}

// "Queue 2": the first number no tab has
std::wstring new_tab_name(const Player& p) {
    for (int n = 1;; ++n) {
        std::wstring s = L"Queue " + std::to_wstring(n);
        if (std::none_of(p.tabs.begin(), p.tabs.end(), [&](const auto& t) { return lstrcmpiW(t->name.c_str(), s.c_str()) == 0; }))
            return s;
    }
}

// a new tab at `at` (shuffle and repeat as the others have them)
QueueTab* add_tab(Player& p, int at, const std::wstring& name) {
    auto t = std::make_unique<QueueTab>();
    t->name = name.empty() ? new_tab_name(p) : name;
    t->q.repeat = p.q ? p.q->repeat : (Repeat)std::clamp(settings_pref(L"playerRepeat", 0), 0, 2);
    t->q.set_shuffle(p.q ? p.q->shuffle() : settings_pref(L"playerShuffle", 0) != 0);
    at = std::clamp(at, 0, (int)p.tabs.size());
    QueueTab* r = t.get();
    p.tabs.insert(p.tabs.begin() + at, std::move(t));
    if (p.tabs.size() > 1 && at <= p.shown) ++p.shown;
    return r;
}

// the playing queue's current entry on screen (not played): a picture shown; sound and
// video wait for Play
void show_current(Player& p) {
    if (image_current(p) && !loaded(p)) play_index(p, p.q->current(), false);
    else refresh_all(p);
}

void end_rename(Player& p, bool take);

// tab i shown: its queue in the list, scrolled as it was left
void show_tab(Player& p, int i, bool keepPlace = true) {
    if (i < 0 || i >= (int)p.tabs.size()) return;
    end_rename(p, true);
    if (keepPlace && p.shown < (int)p.tabs.size()) p.tabs[(size_t)p.shown]->top = ListView_GetTopIndex(p.list);
    p.shown = i;
    ListView_SetItemState(p.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    queue_changed(p);
    int n = vq(p).size(), top = std::min(p.tabs[(size_t)i]->top, n - 1);
    if (top > 0) ListView_EnsureVisible(p.list, n - 1, FALSE);     // (then up to it: it's the first row shown)
    if (n > 0) ListView_EnsureVisible(p.list, std::max(0, top), FALSE);
    // nothing on: this one plays from now on (what the video area shows is its)
    if (!loaded(p) && !shown_plays(p)) {
        set_playing(p, p.tabs[(size_t)i].get());
        show_current(p);
    }
    layout_tabs(p);
    invalidate_row(p);
    refresh_all(p);
}

void new_tab(Player& p) {
    add_tab(p, p.shown + 1, {});
    show_tab(p, p.shown + 1);
    SetFocus(IsWindowVisible(p.list) ? p.list : p.wnd);
}

// its queue goes (playing: the playback stops). The last one: the window closes (as the
// Notepad's), the queue left empty for next time
void close_tab(Player& p, int i) {
    if (i < 0 || i >= (int)p.tabs.size()) return;
    end_rename(p, false);
    if (p.tabs.size() == 1) {
        tags_forget();
        p.tabs[0]->q.clear();
        p.tabs[0]->top = 0;
        PostMessageW(p.wnd, WM_CLOSE, 0, 0);
        return;
    }
    bool wasShown = i == p.shown, wasPlaying = p.tabs[(size_t)i].get() == p.playing;
    if (wasPlaying) stop_playback(p);
    p.tabs.erase(p.tabs.begin() + i);
    p.hotTab = p.hotClose = -1;
    if (i < p.shown) --p.shown;
    p.shown = std::min(p.shown, (int)p.tabs.size() - 1);
    if (wasPlaying) {
        p.playing = nullptr;                // (gone: the one shown plays from now on)
        set_playing(p, p.tabs[(size_t)p.shown].get());
    }
    if (wasShown) show_tab(p, p.shown, false);
    else { layout_tabs(p); invalidate_row(p); keep_soon(p); }
    if (wasPlaying) show_current(p);
}

// the list's row i played: its tab is the one playing from now on
void play_row(Player& p, int i) {
    if (i < 0 || i >= vq(p).size()) return;
    set_playing(p, p.tabs[(size_t)p.shown].get());
    p.failsInRow = 0;
    play_index(p, i, true);
}

void step_tab(Player& p, int step) {
    int n = (int)p.tabs.size();
    if (n > 1) show_tab(p, (p.shown + step + n) % n);
}

// the tab shown, dragged to `to`
void move_tab(Player& p, int from, int to) {
    auto t = std::move(p.tabs[(size_t)from]);
    p.tabs.erase(p.tabs.begin() + from);
    p.tabs.insert(p.tabs.begin() + to, std::move(t));
    p.shown = to;
    layout_tabs(p);
    invalidate_row(p);
    keep_soon(p);
}

// The entries (ids) of tab `from` moved into tab `to`'s queue, before its entry `at` (-1: at
// the end); `copy`: copies of them go, they stay. The one on goes along, playing on (its new
// tab the one playing from now on). Those put in are selected, when their tab is shown.
void move_entries(Player& p, QueueTab* from, const std::vector<unsigned>& ids, QueueTab* to, int at, bool copy) {
    if (tab_index(p, from) < 0 || tab_index(p, to) < 0) return;
    std::vector<int> rows;
    for (unsigned id : ids)
        if (int i = from->q.index_of(id); i >= 0) rows.push_back(i);
    std::sort(rows.begin(), rows.end());
    if (rows.empty()) return;
    int first = 0, count = (int)rows.size();
    if (from == to && !copy) first = to->q.move(rows, at < 0 ? to->q.size() : at);
    else {
        std::vector<QueueEntry> entries;
        for (int i : rows) entries.push_back(from->q.at(i));
        auto cur = std::find(rows.begin(), rows.end(), from->q.current());
        bool carry = !copy && from == p.playing && cur != rows.end() && loaded(p);
        int curAt = (int)(cur - rows.begin());
        if (!copy) from->q.remove(rows);
        first = to->q.insert(at, std::move(entries), copy);
        if (copy) tags_read(to->q, first, count);      // (new ids: the tags still to come are the originals')
        if (carry) {
            p.playing = to;
            p.q = &to->q;
            to->q.set_current(first + curAt);
        }
    }
    QueueTab* shown = p.tabs[(size_t)p.shown].get();
    ListView_SetItemState(p.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    queue_changed(p);
    if (shown == to) {
        for (int i = first; i < first + count; ++i) ListView_SetItemState(p.list, i, LVIS_SELECTED, LVIS_SELECTED);
        ListView_SetItemState(p.list, first, LVIS_FOCUSED, LVIS_FOCUSED);
        ListView_EnsureVisible(p.list, first + count - 1, FALSE);
        ListView_EnsureVisible(p.list, first, FALSE);
    } else if (shown == from && !vq(p).empty()) {
        int focus = std::min(rows[0], vq(p).size() - 1);
        ListView_SetItemState(p.list, focus, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
    }
    invalidate_row(p);
    refresh_all(p);
}

// to the end of tab i's queue (the tab count: a new tab's, which is then shown)
void move_to_tab(Player& p, QueueTab* from, const std::vector<unsigned>& ids, int i, bool copy) {
    bool fresh = i >= (int)p.tabs.size();
    QueueTab* to = fresh ? add_tab(p, p.shown + 1, {}) : p.tabs[(size_t)i].get();
    move_entries(p, from, ids, to, -1, copy);
    if (fresh) show_tab(p, tab_index(p, to));
}

void move_selected(Player& p, int tab) {
    std::vector<unsigned> ids;
    for (int i : selected_rows(p)) ids.push_back(vq(p).at(i).id);
    if (!ids.empty()) move_to_tab(p, p.tabs[(size_t)p.shown].get(), ids, tab, false);
}

// --- a tab's name, edited in place (Enter takes it, Esc or an empty name doesn't) ---
LRESULT CALLBACK rename_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_GETDLGCODE: return DefSubclassProc(h, msg, wp, lp) | DLGC_WANTALLKEYS;
        case WM_KEYDOWN:
            // (posted: the box mustn't go while it handles its own key)
            if (wp == VK_RETURN || wp == VK_ESCAPE) { PostMessageW(GetParent(h), WM_RENAME_DONE, wp == VK_RETURN, (LPARAM)h); return 0; }
            break;
        case WM_CHAR:
            if (wp == L'\r' || wp == 27) return 0;     // (no beep)
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, rename_proc, 1);
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

RECT rename_rect(const Player& p, int i) {
    RECT r = tab_rect(p, i), cr = tab_close_rect(p, i);
    int h = px(24), cy = (r.top + r.bottom) / 2;
    return { r.left + px(10), cy - h / 2, cr.left - px(2), cy - h / 2 + h };
}

// the box over its tab (inside the frame drawn around it), as the tabs are laid out now
void place_rename(Player& p) {
    if (!p.rename) return;
    int i = tab_index(p, p.renaming);
    RECT r = i >= 0 && p.rowH ? rename_rect(p, i) : RECT{};
    InflateRect(&r, -px(4), -px(3));
    MoveWindow(p.rename, r.left, r.top, std::max(0, (int)(r.right - r.left)), std::max(0, (int)(r.bottom - r.top)), TRUE);
}

void start_rename(Player& p, int i) {
    if (i < 0 || i >= (int)p.tabs.size() || !p.rowH) return;
    end_rename(p, true);
    if (i != p.shown) show_tab(p, i);
    p.renaming = p.tabs[(size_t)i].get();
    p.rename = CreateWindowExW(0, L"EDIT", p.renaming->name.c_str(), WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, p.wnd, nullptr,
                               g_hinst, nullptr);
    if (!p.rename) { p.renaming = nullptr; return; }
    SetWindowSubclass(p.rename, rename_proc, 1, 0);
    SendMessageW(p.rename, WM_SETFONT, (WPARAM)p.font, FALSE);
    SendMessageW(p.rename, EM_LIMITTEXT, 100, 0);
    place_rename(p);
    ShowWindow(p.rename, SW_SHOW);
    SendMessageW(p.rename, EM_SETSEL, 0, -1);
    SetFocus(p.rename);
    invalidate_row(p);
}

void end_rename(Player& p, bool take) {
    if (!p.rename) return;
    HWND e = p.rename;
    p.rename = nullptr;                     // (before the focus leaves it: its EN_KILLFOCUS comes back here)
    if (take && tab_index(p, p.renaming) >= 0) {
        wchar_t b[128]{};
        GetWindowTextW(e, b, ARRAYSIZE(b));
        std::wstring name = b;
        size_t a = name.find_first_not_of(L" \t"), z = name.find_last_not_of(L" \t");
        name = a == std::wstring::npos ? std::wstring() : name.substr(a, z - a + 1);
        if (!name.empty() && name != p.renaming->name) {
            p.renaming->name = name;
            keep_soon(p);
        }
    }
    p.renaming = nullptr;
    if (GetFocus() == e) SetFocus(IsWindowVisible(p.list) ? p.list : p.wnd);
    DestroyWindow(e);
    invalidate_row(p);
}

// ------------------------------------------------------------------
// the queues kept as they were left (in a folder next to the settings file; tests: theirs)
// ------------------------------------------------------------------
std::wstring queues_file() {
    std::wstring ini = settings_file_path();
    size_t slash = ini.find_last_of(L"\\/");
    if (ini.empty() || slash == std::wstring::npos) return {};
    std::wstring dir = ini.substr(0, slash) + L"\\Player";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\queues.ini";
}

void keep_soon(Player& p) { if (p.wnd) SetTimer(p.wnd, TIMER_KEEP, kKeepDelayMs, nullptr); }

bool keep_now(Player& p) {
    if (p.wnd) KillTimer(p.wnd, TIMER_KEEP);
    std::wstring file = queues_file();
    if (file.empty() || p.tabs.empty()) return false;
    PlayerSession s;
    s.active = p.shown;
    for (auto& t : p.tabs) {
        QueueState qs;
        qs.name = t->name;
        qs.current = t->q.current();
        qs.entries.reserve((size_t)t->q.size());
        for (auto& e : t->q.entries()) qs.entries.push_back({ e.path, e.title, e.duration });
        s.queues.push_back(std::move(qs));
    }
    std::wstring text = player_session_serialize(s);
    // (UTF-16 with its BOM, as the settings file; written beside, then moved over)
    std::wstring tmp = file + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0, size = (DWORD)(text.size() * sizeof(wchar_t));
    bool ok = WriteFile(h, "\xFF\xFE", 2, &put, nullptr) && put == 2 && WriteFile(h, text.data(), size, &put, nullptr) && put == size;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// the tabs as they were left (none: an empty one); before the window is made
void restore_queues(Player& p) {
    PlayerSession s;
    std::wstring file = queues_file();
    HANDLE h = file.empty() ? INVALID_HANDLE_VALUE
                            : CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        std::string raw;
        DWORD got = 0;
        if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (256 << 20)) {
            raw.resize((size_t)size.QuadPart);
            if (ReadFile(h, raw.data(), (DWORD)raw.size(), &got, nullptr) && got == raw.size()) s = player_session_parse(ini_decode(raw));
        }
        CloseHandle(h);
    }
    for (auto& qs : s.queues) {
        QueueTab* t = add_tab(p, (int)p.tabs.size(), qs.name);
        std::vector<std::wstring> paths;
        paths.reserve(qs.entries.size());
        for (auto& e : qs.entries) paths.push_back(e.path);
        t->q.append(paths);
        for (int i = 0; i < t->q.size(); ++i) {
            t->q.at(i).title = qs.entries[(size_t)i].title;
            t->q.at(i).duration = qs.entries[(size_t)i].duration;
        }
        t->q.set_current(qs.current);
        t->top = std::max(0, qs.current - 3);    // (the list where its current entry is)
    }
    if (p.tabs.empty()) add_tab(p, 0, {});
    p.shown = std::clamp(s.active, 0, (int)p.tabs.size() - 1);
    p.playing = p.tabs[(size_t)p.shown].get();
    p.q = &p.playing->q;
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
        case CMD_SHUFFLE: {                 // (all the queues alike)
            bool on = !p.q->shuffle();
            for (auto& t : p.tabs) t->q.set_shuffle(on);
            settings_set_pref(L"playerShuffle", on);
            break;
        }
        case CMD_REPEAT: {
            Repeat r = (Repeat)(((int)p.q->repeat + 1) % 3);
            for (auto& t : p.tabs) t->q.repeat = r;
            settings_set_pref(L"playerRepeat", (int)r);
            break;
        }
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
        case CMD_SAVE_PLAYLIST: save_queue(p); break;
        case CMD_EDIT_PICTURE:
            if (image_current(p)) imageedit_open_file(p.q->at(p.q->current()).path);
            break;
        case CMD_PLAY_SELECTED: {
            int i = ListView_GetNextItem(p.list, -1, LVNI_SELECTED);
            if (i < 0) i = ListView_GetNextItem(p.list, -1, LVNI_FOCUSED);
            if (i >= 0) play_row(p, i);
            break;
        }
        case CMD_REMOVE_SELECTED: remove_selected(p); break;
        case CMD_TAB_NEW: new_tab(p); break;
        case CMD_TAB_CLOSE: close_tab(p, p.shown); break;
        case CMD_TAB_RENAME: start_rename(p, p.shown); break;
        case CMD_TAB_NEXT: case CMD_TAB_PREV: step_tab(p, cmd == CMD_TAB_NEXT ? 1 : -1); break;
        case CMD_LEVEL:
            p.level = !p.level;
            settings_set_pref(L"playerLevel", p.level);
            if (p.level && p.q->current() >= 0) level_track(p, p.q->current());
            apply_volume(p);
            InvalidateRect(p.video, nullptr, FALSE);
            break;
        case CMD_VIS_CYCLE:
            p.visCycle = !p.visCycle;
            settings_set_pref(L"playerVisualCycle", p.visCycle);
            break;
        case CMD_VIS_NEXT: case CMD_VIS_PREV: {
            // (the spectrogram and the others in turn; only it when they can't be drawn)
            int n = p.vis.failed() ? 1 : VIS_COUNT;
            set_vis(p, ((p.visStyle % n) + (cmd == CMD_VIS_NEXT ? 1 : n - 1)) % n);
            break;
        }
        case CMD_SUBS_OFF: case CMD_SUBS_CHOOSE: case CMD_SUBS_NEXT: subs_command(p, cmd); break;
        case CMD_PAGE_PREV: case CMD_PAGE_NEXT: image_page(p, cmd == CMD_PAGE_NEXT ? 1 : -1); break;
        case CMD_ZOOM_IN: case CMD_ZOOM_OUT: case CMD_ZOOM_FIT: case CMD_ZOOM_ACTUAL: image_zoom_command(p, cmd); break;
        case CMD_AUDIO_NEXT: {
            int n = (int)p.mp->audio_tracks().size();
            if (n > 1) p.mp->set_audio_track((p.mp->audio_track() + 1) % n);
            break;
        }
        default:
            if (cmd >= CMD_VIS && cmd < CMD_VIS + (UINT)VIS_COUNT) set_vis(p, (int)(cmd - CMD_VIS));
            if (cmd >= CMD_SUB && cmd < CMD_SUB + 100) subs_command(p, cmd);
            if (cmd >= CMD_AUDIO && cmd < CMD_AUDIO + 100) p.mp->set_audio_track((int)(cmd - CMD_AUDIO));
            if (cmd > CMD_SLIDE && cmd <= CMD_SLIDE + 60) {
                p.slideSecs = (int)(cmd - CMD_SLIDE);
                settings_set_pref(L"playerSlideSeconds", p.slideSecs);
            }
            break;
    }
    refresh_bar(p);
    acc_notify(p.bar, EVENT_OBJECT_STATECHANGE, -1);
}

bool media_command(Player& p, int app) {
    switch (app) {
        case APPCOMMAND_MEDIA_PLAY_PAUSE: command(p, CMD_PLAYPAUSE); return true;
        case APPCOMMAND_MEDIA_PLAY: if (!playing(p)) command(p, CMD_PLAYPAUSE); return true;
        case APPCOMMAND_MEDIA_PAUSE: if (playing(p)) command(p, CMD_PLAYPAUSE); return true;
        case APPCOMMAND_MEDIA_STOP:
            if (image_current(p)) set_slideshow(p, false);
            else p.mp->stop();
            refresh_bar(p);
            return true;
        case APPCOMMAND_MEDIA_NEXTTRACK: command(p, CMD_NEXT); return true;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK: command(p, CMD_PREV); return true;
    }
    return false;
}

void on_note(Player& p, LPARAM lp) {
    using Change = MediaPlayer::Change;
    Change c = p.mp->handle(reinterpret_cast<MediaNote*>(lp));
    int i = p.q->current();
    if (image_current(p)) return;          // (the playback closed for a picture: late news of it)
    switch (c) {
        case Change::None: return;
        case Change::Opened:
            if (i >= 0) {
                QueueEntry& e = p.q->at(i);
                if (p.mp->duration() > 0) e.duration = p.mp->duration();
                if (!p.mp->title().empty()) e.title = p.mp->title();
                e.failed = false;
                if (shown_plays(p)) ListView_RedrawItems(p.list, i, i);
                keep_soon(p);
                // sound: its spectrogram, from a decoding of its own
                if (!p.mp->has_video() && decodable(e.path)) {
                    p.feed.start(e.path, p.mp->position(), std::max(30, MulDiv(60, p.dpi, 96)));
                    p.vis.set_source(p.feed.tap());
                    if (p.visCycle && !p.vis.failed()) {    // (another visualization, by chance)
                        bool some = p.visStyle != VIS_SPECTROGRAM;
                        int s = 1 + (int)(GetTickCount64() % (VIS_COUNT - 1 - some));
                        set_vis(p, some && s >= p.visStyle ? s + 1 : s);
                    }
                }
            }
            subs_open(p);
            InvalidateRect(p.video, nullptr, FALSE);
            update_title(p);
            break;
        case Change::State:
            if (p.mp->state() == State::Playing) { p.failsInRow = 0; p.message.clear(); }
            if (p.mp->state() == State::Ended) { track_ended(p); return; }
            break;
        case Change::Failed: on_failed(p); return;
        case Change::DeviceLost:              // (headphones unplugged: on with the next device, same place)
            if (i >= 0) p.mp->open(p.q->at(i).path, playing(p), p.mp->position());
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
// the titles playlists gave their entries (a stream's name; a file's when it has no title tag)
std::mutex s_listMx;
std::map<std::wstring, std::wstring> s_listTitles;

void list_title_put(const std::wstring& path, const std::wstring& title) {
    if (title.empty()) return;
    std::lock_guard<std::mutex> lk(s_listMx);
    if (s_listTitles.size() > 20000) s_listTitles.clear();
    s_listTitles[path] = title;
}

std::wstring list_title(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(s_listMx);
    auto it = s_listTitles.find(path);
    return it == s_listTitles.end() ? std::wstring() : it->second;
}

// the file's title tag and duration (COM initialised on the calling thread); a stream: the
// playlist's name for it
void read_tags(const std::wstring& path, std::wstring& title, long long& duration) {
    if (media_url(path)) { title = list_title(path); return; }
    IPropertyStore* ps = nullptr;
    if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps))) || !ps) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(PKEY_Title, &v)) && v.vt == VT_LPWSTR && v.pwszVal) title = v.pwszVal;
    PropVariantClear(&v);
    if (SUCCEEDED(ps->GetValue(PKEY_Media_Duration, &v)) && v.vt == VT_UI8) duration = (long long)v.uhVal.QuadPart;
    PropVariantClear(&v);
    ps->Release();
}

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
        read_tags(job.path, info.title, info.duration);
        if (info.title.empty()) info.title = list_title(job.path);
        std::lock_guard<std::mutex> lk(s_tagMx);
        s_tagDone.push_back(std::move(info));
        if (s_tagDone.size() == 1 && !PostMessageW(s_msg, WM_QUEUE_INFO, 0, 0)) s_tagDone.clear();
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

void tags_read(PlayQueue& q, int from, int count) {
    std::lock_guard<std::mutex> lk(s_tagMx);
    for (int i = from; i < q.size() && i - from < count; ++i)
        if (!image_file(q.at(i).path)) s_tagJobs.push_back({ q.at(i).id, q.at(i).path });   // (pictures: no tags read)
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
    size_t at = 0;                          // (the tab the last one was found in: the next likely is too)
    for (const TagInfo& info : done) {
        for (size_t k = 0; k < p->tabs.size(); ++k) {
            PlayQueue& q = p->tabs[(at + k) % p->tabs.size()]->q;
            int i = q.index_of(info.id);
            if (i < 0) continue;
            at = (at + k) % p->tabs.size();
            QueueEntry& e = q.at(i);
            if (e.title.empty() && !info.title.empty()) e.title = info.title;
            if (e.duration < 0 && info.duration >= 0) e.duration = info.duration;
            if (&q == p->q && i == q.current()) { update_title(*p); InvalidateRect(p->video, nullptr, FALSE); InvalidateRect(p->bar, nullptr, FALSE); }
            break;
        }
    }
    InvalidateRect(p->list, nullptr, FALSE);
    keep_soon(*p);
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

// a playlist's files that are there, and its streams (one listed in it: its files, a few
// levels down); the titles it gives, kept for the queue
void expand_playlist(const std::wstring& file, int depth, std::vector<std::wstring>& out) {
    std::vector<PlaylistEntry> entries;
    if (depth > 3 || !playlist_load(file, entries)) return;
    for (auto& e : entries) {
        if (out.size() >= kMaxFiles) return;
        if (playlist_file(e.path)) { expand_playlist(e.path, depth + 1, out); continue; }
        bool there = media_url(e.path) || (player_file(e.path) && GetFileAttributesW(e.path.c_str()) != INVALID_FILE_ATTRIBUTES);
        if (!there) continue;
        list_title_put(e.path, e.title);
        out.push_back(e.path);
    }
}

std::vector<std::wstring> expand(const std::vector<MediaItem>& items) {
    std::vector<std::wstring> out;
    for (auto& it : items) {
        if (out.size() >= kMaxFiles) break;
        DWORD a = GetFileAttributesW(it.path.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        if (a & FILE_ATTRIBUTE_DIRECTORY) walk(it.path, 0, out);     // (a folder: its sound and video only)
        else if (playlist_file(it.path)) expand_playlist(it.path, 0, out);
        else if (player_file(it.path)) out.push_back(it.path);
    }
    return out;
}

// the pictures of `file`'s folder, sorted by name as Explorer does (`file` among them)
std::vector<std::wstring> folder_pictures(const std::wstring& file) {
    std::wstring base = file.substr(0, file.find_last_of(L"\\/") + 1);
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (image_file(fd.cFileName)) names.push_back(fd.cFileName);
        } while (names.size() < kMaxFiles && FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
    std::vector<std::wstring> out;
    bool found = false;
    for (auto& n : names) {
        out.push_back(base + n);
        if (lstrcmpiW(out.back().c_str(), file.c_str()) == 0) { out.back() = file; found = true; }
    }
    if (!found) out.insert(out.begin(), file);          // (a hidden one, say)
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
struct OpenJob { std::vector<std::wstring> paths; bool enqueue = false; std::wstring start; };

bool create_player(bool activate);

// `opened`: a file opened by its type (kCopyOpen)
bool hand_over(const std::vector<std::wstring>& paths, bool enqueue, const std::wstring& start, bool opened = false) {
    HWND other = FindWindowW(player_class().c_str(), nullptr);
    if (!other) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    if (pid == GetCurrentProcessId()) return false;
    bool view = !enqueue && !start.empty();
    std::vector<std::wstring> all;
    if (view) all.push_back(start);
    // (a playlist's title for an entry after a tab: no path or web address has one)
    for (auto& path : paths) {
        std::wstring t = list_title(path);
        for (auto& c : t) if (c == L'\t' || c == L'\0') c = L' ';
        all.push_back(t.empty() ? path : path + L'\t' + t);
    }
    std::wstring data = pack_paths(all);
    COPYDATASTRUCT cd{ view ? kCopyView : enqueue ? kCopyEnqueue : opened ? kCopyOpen : kCopyPlay,
                       (DWORD)(data.size() * sizeof(wchar_t)), (void*)data.data() };
    DWORD_PTR ok = 0;
    if (!SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &ok) || !ok)
        return false;
    if (!enqueue) {                    // (this process is the foreground one: it may hand that on)
        if (IsIconic(other)) ShowWindowAsync(other, SW_RESTORE);
        SetForegroundWindow(other);
    }
    return true;
}

void deliver(const std::vector<std::wstring>& paths, bool enqueue, const std::wstring& start = {}) {
    if (paths.empty()) { MessageBeep(MB_ICONASTERISK); return; }
    if (!s_p) {
        if (hand_over(paths, enqueue, start)) return;
        if (!create_player(!enqueue)) return;
    } else if (!enqueue) {
        if (IsIconic(s_p->wnd)) ShowWindow(s_p->wnd, SW_RESTORE);
        SetForegroundWindow(s_p->wnd);
    }
    add(*s_p, paths, enqueue, start);
}

LRESULT CALLBACK msg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PLAYER_OPEN) {
        auto* job = reinterpret_cast<OpenJob*>(lp);
        deliver(job->paths, job->enqueue, job->start);
        delete job;
        return 0;
    }
    if (msg == WM_QUEUE_INFO) { tags_apply(); return 0; }
    if (msg == WM_PLAYLIST_FAILED) {
        auto* file = reinterpret_cast<std::wstring*>(lp);
        HWND owner = g_main && IsWindow(g_main) ? g_main : player_window();
        MessageBoxW(owner, (L"The playlist couldn't be saved:\n" + *file).c_str(), L"Explorer Plus", MB_OK | MB_ICONWARNING);
        delete file;
        return 0;
    }
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
            if (player_file(it.path)) paths.push_back(it.path);
        deliver(paths, enqueue);
        return;
    }
    std::thread([items, enqueue, target] {
        auto* job = new OpenJob{ expand(items), enqueue };
        if (!PostMessageW(target, WM_PLAYER_OPEN, 0, (LPARAM)job)) delete job;
    }).detach();
}

// one picture viewed: its folder's pictures, from it (listed on a thread: a big folder)
void open_pictures(const std::wstring& file) {
    HWND target = msg_window();
    std::thread([file, target] {
        auto* job = new OpenJob{ folder_pictures(file), false, file };
        if (!PostMessageW(target, WM_PLAYER_OPEN, 0, (LPARAM)job)) delete job;
    }).detach();
}

// ------------------------------------------------------------------
// playlists saved: the queue, or files and folders chosen in the file list
// ------------------------------------------------------------------
std::wstring parent_dir(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : path.substr(0, s);
}

// the Save dialog for a playlist (opening in `dir`, suggesting `name`): the file chosen,
// empty when cancelled
std::wstring ask_playlist_file(HWND owner, const std::wstring& dir, const std::wstring& name) {
    IFileSaveDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    const COMDLG_FILTERSPEC types[] = { { L"M3U8 playlist (UTF-8)", L"*.m3u8" }, { L"M3U playlist", L"*.m3u" } };
    d->SetFileTypes(ARRAYSIZE(types), types);
    d->SetFileTypeIndex(1);
    d->SetDefaultExtension(L"m3u8");
    d->SetTitle(L"Save playlist");
    d->SetFileName(name.c_str());
    FILEOPENDIALOGOPTIONS opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    IShellItem* folder = nullptr;
    if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        d->SetFolder(folder);
        folder->Release();
    }
    std::wstring out;
    IShellItem* item = nullptr;
    if (SUCCEEDED(d->Show(owner)) && SUCCEEDED(d->GetResult(&item))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
        item->Release();
    }
    d->Release();
    return out;
}

// where a playlist of `paths` is suggested: their folder, named after it, when they share
// one; else beside the first, "Playlist"
void playlist_place(const std::vector<std::wstring>& paths, std::wstring& dir, std::wstring& name) {
    dir = paths.empty() ? std::wstring() : parent_dir(paths[0]);
    bool shared = !dir.empty() && std::all_of(paths.begin(), paths.end(), [&](const std::wstring& s) {
        return lstrcmpiW(parent_dir(s).c_str(), dir.c_str()) == 0;
    });
    name = L"Playlist";
    if (shared && !PathIsRootW((dir + L"\\").c_str())) name = PathFindFileNameW(dir.c_str());
}

void save_queue(Player& p) {
    std::vector<PlaylistEntry> entries;
    std::vector<std::wstring> paths;
    for (auto& e : vq(p).entries())
        if (!image_file(e.path)) {          // (sound and video: a picture isn't a playlist's)
            entries.push_back({ e.path, e.title, e.duration });
            paths.push_back(e.path);
        }
    if (entries.empty()) { MessageBeep(MB_ICONASTERISK); return; }
    std::wstring dir, name;
    playlist_place(paths, dir, name);
    std::wstring file = ask_playlist_file(p.wnd, dir, name);
    if (!file.empty() && !playlist_save(file, entries))
        MessageBoxW(p.wnd, (L"The playlist couldn't be saved:\n" + file).c_str(), L"Save playlist", MB_OK | MB_ICONWARNING);
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
        case P_PREV: case P_PLAY: case P_NEXT: return !p.q->empty();
        default: return true;
    }
}

wchar_t part_glyph(Player& p, int part) {
    switch (part) {
        case P_SHUFFLE: return L'\xE8B1';
        case P_PREV:    return L'\xE892';
        case P_PLAY:    return playing(p) ? L'\xE769' : L'\xE768';
        case P_NEXT:    return L'\xE893';
        case P_REPEAT:  return p.q->repeat == Repeat::One ? L'\xE8ED' : L'\xE8EE';
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
        case P_SHUFFLE: return p.q->shuffle();
        case P_REPEAT:  return p.q->repeat != Repeat::Off;
        case P_QUEUE:   return p.queueShown && !p.full;
    }
    return false;
}

std::wstring part_name(Player& p, int part, std::wstring* key = nullptr) {
    std::wstring k, n;
    switch (part) {
        case P_SEEK:    n = L"Position"; k = L"Left, Right"; break;
        case P_SHUFFLE: n = p.q->shuffle() ? L"Shuffle: on" : L"Shuffle: off"; k = L"Ctrl+H"; break;
        case P_PREV:    n = L"Previous"; k = image_current(p) ? L"Left" : L"Ctrl+Left"; break;
        case P_PLAY:
            n = image_current(p) ? (p.slideshow ? L"Stop slide show" : L"Slide show") : playing(p) ? L"Pause" : L"Play";
            k = L"Space";
            break;
        case P_NEXT:    n = L"Next"; k = image_current(p) ? L"Right" : L"Ctrl+Right"; break;
        case P_REPEAT:
            n = p.q->repeat == Repeat::One ? L"Repeat: one" : p.q->repeat == Repeat::All ? L"Repeat: all" : L"Repeat: off";
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
    if (image_current(p)) {
        // a picture: which one of how many; the slide show's time to the next one
        text_at(dc, std::to_wstring(p.q->current() + 1) + L" / " + std::to_wstring(p.q->size()), tl, c.textSecondary,
                p.fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        text_at(dc, std::to_wstring(p.slideSecs) + L" s", tr, p.slideshow ? c.textSecondary : c.textDisabled,
                p.fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        fill_rrect(dc, p.seekRail, px(2), blend(c.layer, c.text, 0.2));
        if (p.slideshow && p.slideStart) {
            double f = std::min(1.0, (double)(GetTickCount64() - p.slideStart) / (p.slideSecs * 1000.0));
            RECT done = p.seekRail;
            done.right = done.left + (int)((done.right - done.left) * f);
            if (done.right > done.left) fill_rrect(dc, done, px(2), c.accent);
        }
    } else if (dur <= 0 && stream_current(p) && (p.mp->state() == State::Playing || p.mp->state() == State::Paused)) {
        // a stream: how long it has played; no end, no seeking
        text_at(dc, fmt_duration(pos), tl, c.textSecondary, p.fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        text_at(dc, L"Live", tr, c.textSecondary, p.fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        fill_rrect(dc, p.seekRail, px(2), blend(c.layer, c.text, 0.2));
    } else {
        text_at(dc, canSeek ? fmt_duration(pos) : L"0:00", tl, timeColor, p.fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        text_at(dc, dur > 0 ? fmt_duration(dur) : L"0:00", tr, timeColor, p.fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        slider(dc, p, p.seekRail, dur > 0 ? (double)pos / (double)dur : 0.0, canSeek, p.hot == P_SEEK || p.drag == P_SEEK);
    }
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

// ------------------------------------------------------------------
// now playing, for sound: the album cover, the title and tags, the spectrogram
// ------------------------------------------------------------------
// the video area shows the panel: an entry is current and it isn't (or isn't yet) a video
bool now_playing(Player& p) {
    int i = p.q->current();
    if (i < 0 || p.mp->has_video() || image_current(p)) return false;
    // (a video file still opening: no panel flashing up before its picture)
    return !(p.mp->state() == State::Opening && media_file_video(p.q->at(i).path));
}

std::wstring folder_of(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/") + 1); }

// the tags read for the current entry; null while they're on their way
const MediaInfo* current_info(Player& p) {
    int i = p.q->current();
    return i >= 0 && p.infoFor == p.q->at(i).id ? &p.info : nullptr;
}

// the cover to show: the current entry's, or meanwhile the previous one's when it's a
// picture of the same folder (the next track of an album)
bool cover_shown(Player& p) {
    int i = p.q->current();
    if (i < 0 || !p.info.cover) return false;
    if (current_info(p)) return true;
    const std::wstring& from = p.info.coverFrom;
    return !from.empty() && folder_of(from) == folder_of(p.q->at(i).path) && is_image_name(from);
}

// what the panel lists under the title: the tags, then the stream's format
std::vector<std::pair<std::wstring, std::wstring>> now_details(Player& p) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    const MediaInfo* info = current_info(p);
    if (info) out = info->tags;
    const AudioInfo& a = p.mp->audio();
    int i = p.q->current();
    std::wstring fmt = audio_format_name(a.format);
    if (fmt.empty() && i >= 0 && !media_url(p.q->at(i).path)) {     // (unknown: the file type)
        const wchar_t* ext = PathFindExtensionW(p.q->at(i).path.c_str());
        if (*ext) { fmt = ext + 1; for (auto& ch : fmt) ch = (wchar_t)towupper(ch); }
    }
    unsigned bitrate = info && info->bitrate ? info->bitrate : a.bitrate;
    if (!bitrate && info && info->size && p.mp->duration() > 0 && a.format != 0x4D494449)   // (the average: size / duration; MIDI's notes aren't sound)
        bitrate = (unsigned)(info->size * 8 * 10000000.0 / (double)p.mp->duration());
    std::wstring b = fmt_bitrate(bitrate);
    if (!fmt.empty()) out.push_back({ L"Format", b.empty() ? fmt : fmt + L", " + b });
    std::wstring audio = fmt_sample_rate(a.rate);
    std::wstring ch = fmt_channels(a.channels);
    if (!ch.empty()) audio += (audio.empty() ? L"" : L", ") + ch;
    bool lossless = a.format == 1 || a.format == 3 || a.format == 0xF1AC || a.format == 0x6C61 ||
                    a.format == 0x63616C61 || a.format == 0x0163 || a.format == 0x43444441;
    if (lossless && a.bits) audio += (audio.empty() ? L"" : L", ") + std::to_wstring(a.bits) + L"-bit";
    if (!audio.empty()) out.push_back({ L"Audio", audio });
    // its loudness; levelled: by how much
    if (const double* l = i >= 0 ? loudness_of(p, p.q->at(i).path) : nullptr; l && std::isfinite(*l)) {
        wchar_t b[96];
        double g = loudness_gain_db(*l);
        if (p.level) swprintf_s(b, L"%.1f LUFS, levelled %s%.1f dB", *l, g >= 0 ? L"+" : L"\x2212", std::fabs(g));
        else swprintf_s(b, L"%.1f LUFS", *l);
        std::wstring s = b;
        size_t m = s.find(L'-');
        if (m != std::wstring::npos) s[m] = L'\x2212';
        out.push_back({ L"Loudness", s });
    }
    if (info && info->size) out.push_back({ L"Size", fmt_file_size(info->size) });
    return out;
}

struct NowRects { RECT cover{}, text{}, spec{}; bool stacked = false; };

NowRects now_rects(Player& p, const RECT& rc) {
    NowRects r;
    int w = rc.right - rc.left, h = rc.bottom - rc.top, pad = px(24);
    // (a visualization: a little more room than the spectrogram)
    int i = p.q->current();
    bool analysed = i >= 0 && decodable(p.q->at(i).path);     // (a stream, MIDI, a CD track: nothing to show there)
    int specH = h < px(300) || !analysed ? 0 : vis_on(p) ? std::clamp(h * 36 / 100, px(100), px(300)) : std::clamp(h * 30 / 100, px(80), px(220));
    if (specH) r.spec = { rc.left + pad, rc.bottom - pad - specH, rc.right - pad, rc.bottom - pad };
    int top = rc.top + pad, bottom = (specH ? r.spec.top - px(8) : rc.bottom - pad);
    int availH = bottom - top, availW = w - 2 * pad;
    r.stacked = availW < px(460);
    if (!r.stacked) {                       // the cover on the left, the text beside it
        int s = std::min({ availH, availW * 45 / 100, px(320) });
        if (s < px(64)) s = 0;
        int y = top + (availH - s) / 2;
        r.cover = { rc.left + pad, y, rc.left + pad + s, y + s };
        r.text = { s ? r.cover.right + px(28) : rc.left + pad, top, rc.right - pad, bottom };
    } else {                                // narrow: the cover above the text
        int s = std::min({ availH - px(170), availW, px(220) });     // (room for the title and a few details)
        if (s < px(64)) s = 0;
        int x = rc.left + (w - s) / 2;
        r.cover = { x, top, x + s, top + s };
        r.text = { rc.left + pad, top + s + (s ? px(16) : 0), rc.right - pad, bottom };
    }
    return r;
}

// the cover scaled into a `size` square (aspect kept, centred, rounded corners), with alpha
HBITMAP render_cover(const MediaInfo& info, int size, int radius) {
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return nullptr;
    Gdiplus::Bitmap src(info.coverW, info.coverH, info.coverW * 4, PixelFormat32bppPARGB,
                        (BYTE*)const_cast<uint32_t*>(info.cover->data()));
    double k = std::min((double)size / info.coverW, (double)size / info.coverH);
    int fw = std::max(1, (int)(info.coverW * k + 0.5)), fh = std::max(1, (int)(info.coverH * k + 0.5));
    Gdiplus::Bitmap scaled(fw, fh, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics g(&scaled);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        Gdiplus::ImageAttributes clampEdges;        // (no blend with the far edge at the borders)
        clampEdges.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
        g.DrawImage(&src, Gdiplus::Rect(0, 0, fw, fh), 0, 0, info.coverW, info.coverH, Gdiplus::UnitPixel, &clampEdges);
    }
    Gdiplus::Bitmap out(size, size, size * 4, PixelFormat32bppPARGB, (BYTE*)bits);
    {
        Gdiplus::Graphics g(&out);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        int x = (size - fw) / 2, y = (size - fh) / 2;
        Gdiplus::TextureBrush brush(&scaled);
        brush.TranslateTransform((float)x, (float)y);
        Gdiplus::GraphicsPath path;
        float d = (float)radius * 2;
        path.AddArc((float)x, (float)y, d, d, 180, 90);
        path.AddArc((float)(x + fw) - d, (float)y, d, d, 270, 90);
        path.AddArc((float)(x + fw) - d, (float)(y + fh) - d, d, d, 0, 90);
        path.AddArc((float)x, (float)(y + fh) - d, d, d, 90, 90);
        path.CloseFigure();
        g.FillPath(&brush, &path);
    }
    return bmp;
}

void draw_cover(Player& p, HDC dc, const RECT& r) {
    const Palette& c = pal();
    int size = r.right - r.left;
    if (size <= 0) return;
    if (cover_shown(p)) {
        if (!p.coverBmp || p.coverSize != size) {
            if (p.coverBmp) DeleteObject(p.coverBmp);
            p.coverBmp = render_cover(p.info, size, px(8));
            p.coverSize = size;
        }
        if (p.coverBmp) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, p.coverBmp);
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            AlphaBlend(dc, r.left, r.top, size, size, mem, 0, 0, size, size, bf);
            SelectObject(mem, old);
            DeleteDC(mem);
            return;
        }
    }
    // no cover: a tile with a note
    fill_rrect(dc, r, px(8), blend(c.content, c.text, g_dark ? 0.08 : 0.05));
    RECT g = r;
    HFONT f = size >= px(160) ? p.glyphsArt : p.glyphsBig;
    glyph_at(dc, L'\xE8D6', g, c.textDisabled, f);
}

// --- the spectrogram: one column per pixel, scrolling left as playback goes on ---
void spectrum_lut(Player& p) {
    if (p.specLutTheme == theme_generation()) return;
    p.specLutTheme = theme_generation();
    const Palette& c = pal();
    // silence is the window's background (the spectrogram blends in); loud is the accent,
    // then light (dark theme) or deep (light)
    COLORREF quiet = c.content;
    COLORREF stops[4] = { quiet, blend(quiet, c.accent, 0.45), c.accent,
                          g_dark ? blend(c.accent, RGB(255, 255, 255), 0.8) : blend(c.accent, RGB(0, 0, 0), 0.6) };
    const double at[4] = { 0.0, 0.35, 0.7, 1.0 };
    for (int i = 0; i < 256; ++i) {
        double t = i / 255.0;
        int s = t < at[1] ? 0 : t < at[2] ? 1 : 2;
        COLORREF col = blend(stops[s], stops[s + 1], (t - at[s]) / (at[s + 1] - at[s]));
        p.specLut[i] = 0xFF000000u | (GetRValue(col) << 16) | (GetGValue(col) << 8) | GetBValue(col);
    }
}

void spectrum_column(Player& p, int x, const uint8_t* bands) {
    const int B = SpectrumFeed::kBands;
    for (int y = 0; y < p.specH; ++y) {
        double f = p.specH > 1 ? (double)(p.specH - 1 - y) * (B - 1) / (p.specH - 1) : 0;    // (low at the bottom)
        int b = (int)f;
        double t = f - b;
        double v = b + 1 < B ? bands[b] * (1 - t) + bands[b + 1] * t : bands[b];
        p.specBits[(size_t)y * p.specW + x] = p.specLut[(int)v];
    }
}

// the bitmap at the area's size, drawn again from the columns kept
void spectrum_fit(Player& p, int w, int h) {
    spectrum_lut(p);
    if (p.specBmp && p.specW == w && p.specH == h) return;
    if (p.specBmp) DeleteObject(p.specBmp);
    p.specBmp = nullptr;
    p.specBits = nullptr;
    p.specW = p.specH = 0;
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    p.specBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!p.specBmp) return;
    p.specBits = (uint32_t*)bits;
    p.specW = w;
    p.specH = h;
    std::fill(p.specBits, p.specBits + (size_t)w * h, p.specLut[0]);
    int n = (int)std::min<size_t>(p.spec.size(), (size_t)w);
    for (int k = 0; k < n; ++k) spectrum_column(p, w - n + k, p.spec[p.spec.size() - n + k].data());
}

void spectrum_redraw(Player& p) {                  // (a new theme: new colours)
    int w = p.specW, h = p.specH;
    if (p.specBmp) DeleteObject(p.specBmp);
    p.specBmp = nullptr;
    p.specW = p.specH = 0;
    spectrum_fit(p, w, h);
}

// a new track (or none): an empty spectrogram, no analysis
void spectrum_reset(Player& p) {
    p.feed.stop();
    p.vis.set_source({});
    p.spec.clear();
    p.specPos = -1;
    KillTimer(p.wnd, TIMER_SPECTRUM);
    if (p.specBits) std::fill(p.specBits, p.specBits + (size_t)p.specW * p.specH, p.specLut[0]);
}

// the spectrogram's picture within its area: right of the frequency scale
RECT spectrum_image(const RECT& area) {
    RECT r = area;
    r.left = std::min(r.right, r.left + px(32));
    return r;
}

// the spectrogram's area, its frequency scale included
RECT spectrum_area(Player& p) {
    RECT rc{};
    GetClientRect(p.video, &rc);
    return now_rects(p, rc).spec;
}

RECT spectrum_rect(Player& p) {
    RECT a = spectrum_area(p);
    return IsRectEmpty(&a) ? a : spectrum_image(a);
}

// a frame: the columns playback has reached, scrolled in from the right
void spectrum_tick(Player& p) {
    if (!p.feed.active() || !now_playing(p)) return;
    LONGLONG pos = p.mp->position();
    // a seek (or a jump of the clock): the analysis starts again there
    if (p.specPos >= 0 && (pos < p.specPos - 2000000 || pos > p.specPos + 15000000)) {
        int i = p.q->current();
        if (i >= 0) p.feed.start(p.q->at(i).path, pos, std::max(30, MulDiv(60, p.dpi, 96)));
        p.vis.set_source(p.feed.tap());
        p.spec.clear();
        if (p.specBits) std::fill(p.specBits, p.specBits + (size_t)p.specW * p.specH, p.specLut[0]);
    }
    p.specPos = pos;
    p.vis.set_clock(pos, true);
    std::vector<uint8_t> cols;
    int n = p.feed.take(pos, cols);
    if (!n) return;
    const int B = SpectrumFeed::kBands;
    for (int k = 0; k < n; ++k) {
        std::array<uint8_t, SpectrumFeed::kBands> a;
        std::copy(cols.begin() + (size_t)k * B, cols.begin() + (size_t)(k + 1) * B, a.begin());
        p.spec.push_back(a);
    }
    while (p.spec.size() > 4096) p.spec.pop_front();
    if (vis_on(p)) return;                  // (kept for the spectrogram, not drawn)
    RECT r = spectrum_rect(p);
    if (IsRectEmpty(&r)) return;
    spectrum_fit(p, r.right - r.left, r.bottom - r.top);
    if (!p.specBits) return;
    int w = p.specW, shift = std::min(n, w);
    for (int y = 0; y < p.specH; ++y) {             // (scrolled left by the new columns)
        uint32_t* row = p.specBits + (size_t)y * w;
        memmove(row, row + shift, (size_t)(w - shift) * 4);
    }
    for (int k = 0; k < shift; ++k) spectrum_column(p, w - shift + k, cols.data() + (size_t)(n - shift + k) * B);
    GdiFlush();
    RECT area = spectrum_area(p);           // (the scale too: whatever painted over it meanwhile)
    InvalidateRect(p.video, &area, FALSE);
}

void draw_spectrum(Player& p, HDC dc, const RECT& area) {
    RECT r = spectrum_image(area);
    int w = r.right - r.left, h = r.bottom - r.top;
    spectrum_fit(p, w, h);
    if (!p.specBmp) return;
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, p.specBmp);
    BitBlt(dc, r.left, r.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    // the frequency scale: 100 Hz, 1 kHz, 10 kHz
    const Palette& c = pal();
    const int B = SpectrumFeed::kBands;
    double f0 = p.feed.band_freq(0), f1 = p.feed.band_freq(B - 1);
    if (f0 <= 0 || f1 <= f0) {             // (not started yet: the bands it will have, near enough)
        f0 = SpectrumFeed::kMinHz * std::pow(SpectrumFeed::kMaxHz / SpectrumFeed::kMinHz, 0.5 / B);
        f1 = SpectrumFeed::kMinHz * std::pow(SpectrumFeed::kMaxHz / SpectrumFeed::kMinHz, (B - 0.5) / B);
    }
    for (int hz : { 100, 1000, 10000 }) {
        if (hz <= f0 || hz >= f1) continue;
        double band = std::log(hz / f0) / std::log(f1 / f0) * (B - 1);
        int y = r.bottom - 1 - (int)(band / (B - 1) * (h - 1));
        RECT tick{ r.left - px(5), y, r.left - px(1), y + 1 };
        fill_rect(dc, tick, c.textSecondary);
        RECT lr{ area.left, y - px(9), r.left - px(8), y + px(9) };
        text_at(dc, hz >= 1000 ? std::to_wstring(hz / 1000) + L"k" : std::to_wstring(hz), lr, c.textSecondary,
                p.fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

// --- the visualizations (visual.cpp's window, over the video area) ---
// full screen, sound playing: the visualization is all there is to see
bool vis_fills(Player& p) { return vis_on(p) && p.full && now_playing(p) && p.message.empty(); }

// where it goes in the video area: all of it (full screen), else the spectrogram's place
RECT vis_rect(Player& p) {
    RECT rc{};
    GetClientRect(p.video, &rc);
    if (!vis_on(p) || !now_playing(p)) return {};
    return vis_fills(p) ? rc : now_rects(p, rc).spec;
}

// shown or hidden, placed, its colours, the playback clock, the full-screen caption
void vis_update(Player& p) {
    RECT r = vis_rect(p);
    if (IsRectEmpty(&r)) { p.vis.place(nullptr); return; }
    if (!p.vis.hwnd() && !p.vis.create(p.video, p.wnd, WM_VIS_FAILED)) return;
    const Palette& c = pal();
    VisLook look;
    look.style = p.visStyle;
    look.full = vis_fills(p);
    look.bg = c.content;
    look.text = c.text;
    look.dark = g_dark != 0;
    look.dpi = p.dpi;
    p.vis.set_look(look);
    p.vis.set_clock(p.mp->position(), p.mp->state() == State::Playing && p.mp->will_play());
    // (the caption made again when what it shows changes: the next track, its tags read)
    std::wstring title = current_title(p), detail = current_detail(p);
    const void* cover = cover_shown(p) ? p.info.cover.get() : nullptr;
    std::wstring key = title + L'\n' + detail + L'\n' + std::to_wstring((uintptr_t)cover);
    if (key != p.visCaption) {
        p.visCaption = key;
        VisCaption vc;
        vc.title = title;
        vc.detail = detail;
        if (cover) { vc.cover = p.info.cover; vc.coverW = p.info.coverW; vc.coverH = p.info.coverH; }
        p.vis.set_caption(vc);
    }
    p.vis.show_caption(p.barInFull);
    p.vis.place(&r);
}

void set_vis(Player& p, int style) {
    style = std::clamp(style, 0, VIS_COUNT - 1);
    if (style == p.visStyle) { if (vis_on(p)) p.vis.announce(); return; }
    p.visStyle = style;
    settings_set_pref(L"playerVisual", style);
    if (!vis_on(p)) spectrum_redraw(p);         // (its columns kept meanwhile, not drawn)
    vis_update(p);
    if (vis_on(p)) p.vis.announce();
    InvalidateRect(p.video, nullptr, FALSE);
}

// the video area with nothing playing yet: how to get something to play
void paint_idle(Player& p, HDC dc, const RECT& rc) {
    const Palette& c = pal();
    std::wstring title, detail;
    wchar_t g = L'\xE8D6';                     // (music note)
    if (p.q->empty()) {
        title = L"Nothing to play";
        detail = L"Choose Play, View or Queue in Explorer Plus in a file's menu, or drop files here.";
    } else if (p.q->current() < 0) {
        title = L"Play queue";
        detail = std::to_wstring(p.q->size()) + (p.q->size() == 1 ? L" item" : L" items");
    } else {
        title = current_title(p);
        detail = current_detail(p);
        g = L'\xE714';                         // (video)
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

// zoomed into: the part in view, drawn from the pixels decoded (much enlarged: as squares)
void paint_zoomed(Player& p, HDC dc, const RECT& rc, const ImageView& v) {
    const ImageNote& im = p.img;
    int aw = rc.right - rc.left, ah = rc.bottom - rc.top;
    double l = std::max(0.0, v.x0), t = std::max(0.0, v.y0);
    double r = std::min((double)aw, v.x0 + v.dw), b = std::min((double)ah, v.y0 + v.dh);
    if (r <= l || b <= t) return;
    double kx = im.w / v.dw, ky = im.h / v.dh;          // (pixels decoded a screen pixel)
    Gdiplus::Bitmap src(im.w, im.h, im.w * 4, PixelFormat32bppPARGB, (BYTE*)const_cast<uint32_t*>(im.pixels->data()));
    Gdiplus::Graphics g(dc);
    bool squares = kx <= 0.5;
    g.SetInterpolationMode(squares ? Gdiplus::InterpolationModeNearestNeighbor : Gdiplus::InterpolationModeHighQualityBilinear);
    g.SetPixelOffsetMode(squares ? Gdiplus::PixelOffsetModeHalf : Gdiplus::PixelOffsetModeHighQuality);
    Gdiplus::ImageAttributes clampEdges;
    clampEdges.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    g.DrawImage(&src, Gdiplus::RectF((float)(rc.left + l), (float)(rc.top + t), (float)(r - l), (float)(b - t)),
                (float)((l - v.x0) * kx), (float)((t - v.y0) * ky), (float)((r - l) * kx), (float)((b - t) * ky),
                Gdiplus::UnitPixel, &clampEdges);
}

// a picture: fitted to the area, centred (or zoomed into); black around it in full screen
void paint_image(Player& p, HDC dc, const RECT& rc) {
    const Palette& c = pal();
    fill_rect(dc, rc, p.full ? RGB(0, 0, 0) : c.content);
    int aw = rc.right - rc.left, ah = rc.bottom - rc.top;
    if (!p.message.empty()) {               // (can't be opened)
        RECT ga{ rc.left, rc.top + ah / 2 - px(80), rc.right, rc.top + ah / 2 };
        glyph_at(dc, L'\xE91B', ga, c.textDisabled, p.glyphsArt);
        RECT tr{ rc.left + px(24), ga.bottom + px(12), rc.right - px(24), ga.bottom + px(44) };
        text_at(dc, current_title(p), tr, p.full ? RGB(255, 255, 255) : c.text, p.fontBig,
                DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
        RECT mr{ tr.left, tr.bottom + px(8), tr.right, tr.bottom + px(56) };
        text_at(dc, p.message, mr, p.full ? RGB(200, 200, 200) : c.textSecondary, p.font, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
        return;
    }
    const ImageNote& im = p.img;             // (the one before while the next decodes)
    if (!im.pixels || aw <= 0 || ah <= 0) return;
    ImageView v = image_view(p, aw, ah);
    if (p.zoom != 1) { paint_zoomed(p, dc, rc, v); return; }
    int dw = std::max(1, (int)(v.dw + 0.5)), dh = std::max(1, (int)(v.dh + 0.5));
    if (!p.imgBmp || p.imgBmpW != dw || p.imgBmpH != dh || p.imgBmpOf != im.pixels.get()) {
        if (p.imgBmp) DeleteObject(p.imgBmp);
        p.imgBmp = nullptr;
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), dw, -dh, 1, 32, BI_RGB };
        void* bits = nullptr;
        p.imgBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!p.imgBmp) return;
        Gdiplus::Bitmap src(im.w, im.h, im.w * 4, PixelFormat32bppPARGB, (BYTE*)const_cast<uint32_t*>(im.pixels->data()));
        Gdiplus::Bitmap out(dw, dh, dw * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Gdiplus::Graphics g(&out);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        g.SetInterpolationMode(dw == im.w && dh == im.h ? Gdiplus::InterpolationModeNearestNeighbor
                                                        : Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        Gdiplus::ImageAttributes clampEdges;
        clampEdges.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
        g.DrawImage(&src, Gdiplus::Rect(0, 0, dw, dh), 0, 0, im.w, im.h, Gdiplus::UnitPixel, &clampEdges);
        p.imgBmpW = dw;
        p.imgBmpH = dh;
        p.imgBmpOf = im.pixels.get();
    }
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, p.imgBmp);
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };      // (transparent PNGs: on the background)
    AlphaBlend(dc, rc.left + (aw - dw) / 2, rc.top + (ah - dh) / 2, dw, dh, mem, 0, 0, dw, dh, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
}

// the video area without a video
void paint_art(Player& p, HDC dc, const RECT& rc) {
    const Palette& c = pal();
    if (image_current(p)) { paint_image(p, dc, rc); return; }
    if (vis_fills(p)) { fill_rect(dc, rc, RGB(0, 0, 0)); return; }     // (under the visualization)
    fill_rect(dc, rc, c.content);
    if (!now_playing(p)) { paint_idle(p, dc, rc); return; }
    NowRects r = now_rects(p, rc);
    if (!IsRectEmpty(&r.cover)) draw_cover(p, dc, r.cover);
    // the text: title, artist, album; the message (can't play) or the details
    std::wstring title = current_title(p), artist = p.mp->artist(), album = p.mp->album();
    auto details = p.message.empty() ? now_details(p) : decltype(now_details(p)){};
    int lineBig = px(40), line = px(26), row = px(22), gap = px(14);
    int textH = r.text.bottom - r.text.top;
    int fixed = lineBig + (artist.empty() ? 0 : line) + (album.empty() ? 0 : line);
    int extra = !p.message.empty() ? gap + px(48) : details.empty() ? 0 : gap;
    int rowsFit = std::max(0, (textH - fixed - extra) / row);
    int rows = std::min((int)details.size(), rowsFit);
    int total = fixed + extra + rows * row;
    int y = r.text.top + (r.stacked ? 0 : std::max(0, (textH - total) / 2));
    UINT align = r.stacked ? DT_CENTER : DT_LEFT;
    auto line_at = [&](const std::wstring& s, int hgt, COLORREF col, HFONT f) {
        RECT lr{ r.text.left, y, r.text.right, y + hgt };
        text_at(dc, s, lr, col, f, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += hgt;
    };
    line_at(title, lineBig, c.text, p.fontBig);
    if (!artist.empty()) line_at(artist, line, c.text, p.font);
    if (!album.empty()) line_at(album, line, c.textSecondary, p.font);
    if (!p.message.empty()) {
        y += gap;
        RECT mr{ r.text.left, y, r.text.right, y + px(48) };
        text_at(dc, p.message, mr, c.text, p.font, align | DT_WORDBREAK | DT_END_ELLIPSIS);
    } else if (rows) {
        y += gap;
        // labels in a column of their own (their widest), values beside them
        HGDIOBJ of = SelectObject(dc, p.fontSmall);
        int labelW = 0;
        for (int k = 0; k < rows; ++k) labelW = std::max(labelW, text_width(dc, details[k].first.c_str()));
        SelectObject(dc, of);
        labelW += px(16);
        int left = r.text.left;
        if (r.stacked) {                                // (centred as a block)
            int valueW = 0;
            of = SelectObject(dc, p.fontSmall);
            for (int k = 0; k < rows; ++k) valueW = std::max(valueW, text_width(dc, details[k].second.c_str()));
            SelectObject(dc, of);
            left = std::max(r.text.left, (r.text.left + r.text.right - labelW - valueW) / 2);
        }
        for (int k = 0; k < rows; ++k) {
            RECT lr{ left, y, left + labelW, y + row };
            RECT vr{ left + labelW, y, r.text.right, y + row };
            text_at(dc, details[k].first, lr, c.textSecondary, p.fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            text_at(dc, details[k].second, vr, c.text, p.fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += row;
        }
    }
    if (!IsRectEmpty(&r.spec) && !vis_on(p)) draw_spectrum(p, dc, r.spec);
}

// ------------------------------------------------------------------
// the taskbar button: its own (the player's icon), the track's progress, and Previous /
// Play / Next in the thumbnail
// ------------------------------------------------------------------
void set_app_id(HWND hwnd, bool on) {
    IPropertyStore* ps = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&ps))) || !ps) return;
    PROPVARIANT v;
    // (PreventPinning before the ID, as documented; both cleared before the window goes)
    if (on) InitPropVariantFromBoolean(TRUE, &v); else PropVariantInit(&v);
    ps->SetValue(PKEY_AppUserModel_PreventPinning, v);
    PropVariantClear(&v);
    if (on) InitPropVariantFromString(kAppId, &v); else PropVariantInit(&v);
    ps->SetValue(PKEY_AppUserModel_ID, v);
    PropVariantClear(&v);
    ps->Commit();
    ps->Release();
}

// a Segoe Fluent glyph as a small icon (the thumbnail's buttons), for the taskbar's theme
HICON glyph_icon(wchar_t glyph) {
    int s = GetSystemMetrics(SM_CXSMICON);
    DWORD light = 0, cb = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &cb);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), s, -s, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) return nullptr;
    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(mem, color);
    RECT r{ 0, 0, s, s };
    FillRect(mem, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    LOGFONTW lf{};
    lf.lfHeight = -s * 3 / 4;
    lf.lfQuality = ANTIALIASED_QUALITY;         // (grey levels: the coverage becomes the alpha)
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, theme_glyph_face());
    HFONT f = CreateFontIndirectW(&lf);
    HGDIOBJ of = SelectObject(mem, f);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    wchar_t t[2] = { glyph, 0 };
    DrawTextW(mem, t, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(mem, of);
    DeleteObject(f);
    SelectObject(mem, old);
    DeleteDC(mem);
    GdiFlush();
    uint8_t ink = light ? 0 : 255;
    for (int i = 0; i < s * s; ++i) {
        uint32_t& px32 = ((uint32_t*)bits)[i];
        uint32_t a = px32 & 0xFF;                   // (white on black: any channel is the coverage)
        uint32_t v = ink * a / 255;
        px32 = (a << 24) | (v << 16) | (v << 8) | v;
    }
    HBITMAP mask = CreateBitmap(s, s, 1, 1, nullptr);
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void free_thumb_icons(Player& p) {
    for (HICON& i : p.thumbIcons) if (i) { DestroyIcon(i); i = nullptr; }
}

void thumb_buttons(Player& p, bool add) {
    if (!p.taskbar) return;
    wchar_t playGlyph = playing(p) ? L'\xE769' : L'\xE768';
    if (!add && playGlyph == p.tbGlyph) return;
    free_thumb_icons(p);
    p.thumbIcons[0] = glyph_icon(L'\xE892');
    p.thumbIcons[1] = glyph_icon(playGlyph);
    p.thumbIcons[2] = glyph_icon(L'\xE893');
    p.tbGlyph = playGlyph;
    THUMBBUTTON b[3]{};
    const UINT ids[3] = { CMD_PREV, CMD_PLAYPAUSE, CMD_NEXT };
    const wchar_t* tips[3] = { L"Previous", playing(p) ? L"Pause" : L"Play", L"Next" };
    for (int i = 0; i < 3; ++i) {
        b[i].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        b[i].iId = ids[i];
        b[i].hIcon = p.thumbIcons[i];
        wcscpy_s(b[i].szTip, tips[i]);
        b[i].dwFlags = p.q->empty() ? THBF_DISABLED : THBF_ENABLED;
    }
    if (add) p.taskbar->ThumbBarAddButtons(p.wnd, 3, b);
    else p.taskbar->ThumbBarUpdateButtons(p.wnd, 3, b);
}

// the taskbar made the button (again, after Explorer restarted)
void taskbar_created(Player& p) {
    if (!p.taskbar && SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&p.taskbar))) &&
        FAILED(p.taskbar->HrInit())) {
        p.taskbar->Release();
        p.taskbar = nullptr;
    }
    p.tbState = p.tbPermille = -1;
    thumb_buttons(p, true);
    update_taskbar(p);
}

void update_taskbar(Player& p) {
    if (!p.taskbar) return;
    State s = p.mp->state();
    int st = s == State::Failed ? TBPF_ERROR
           : (s == State::Playing || s == State::Paused) && p.mp->duration() > 0 ? (playing(p) ? TBPF_NORMAL : TBPF_PAUSED)
           : TBPF_NOPROGRESS;
    if (st != p.tbState) {
        p.taskbar->SetProgressState(p.wnd, (TBPFLAG)st);
        p.tbState = st;
        p.tbPermille = -1;
    }
    if (st == TBPF_NORMAL || st == TBPF_PAUSED) {
        int v = (int)std::clamp(p.mp->position() * 1000 / std::max<LONGLONG>(1, p.mp->duration()), 0LL, 1000LL);
        if (v != p.tbPermille) {
            p.taskbar->SetProgressValue(p.wnd, (ULONGLONG)v, 1000);
            p.tbPermille = v;
        }
    } else if (st == TBPF_ERROR && p.tbPermille != 1000) {
        p.taskbar->SetProgressValue(p.wnd, 1000, 1000);
        p.tbPermille = 1000;
    }
    thumb_buttons(p, false);
}

// the window's buttons, at the tab row's right end: minimize, maximize, close
void paint_cap_buttons(Player& p, HDC dc, COLORREF bg) {
    const Palette& c = pal();
    static const wchar_t kGlyph[3] = { L'\xE921', L'\xE922', L'\xE8BB' };
    for (int i = 0; i < 3; ++i) {
        RECT r = cap_button(p, i);
        COLORREF fg = p.activeWnd ? c.text : c.textDisabled;
        bool on = p.capHot == i || p.capPressed == i;
        if (i == 2 && on) {
            fill_rect(dc, r, p.capPressed == 2 ? blend(c.closeHover, RGB(0, 0, 0), 0.1) : c.closeHover);
            fg = RGB(255, 255, 255);
        } else if (on) {
            fill_rect(dc, r, blend(bg, g_dark ? RGB(255, 255, 255) : RGB(0, 0, 0), p.capPressed == i ? 0.04 : 0.07));
        }
        glyph_at(dc, i == 1 && IsZoomed(p.wnd) ? L'\xE923' : kGlyph[i], r, fg, p.glyphsSmall);
    }
}

// the playing tab's mark: a speaker (accent: playing; grey: paused), none when nothing is on
int play_mark(Player& p) { return !loaded(p) ? 0 : playing(p) ? 2 : 1; }

// the tab row, in the title bar's place: the tabs (as Notepad's), "+", the window's buttons
void paint_tabs(Player& p, HDC dc, const RECT& rc) {
    const Palette& c = pal();
    COLORREF capBg = p.activeWnd ? c.caption : c.captionInactive;
    fill_rect(dc, RECT{ 0, 0, rc.right, p.rowH }, capBg);
    paint_cap_buttons(p, dc, capBg);
    p.markShown = play_mark(p);
    int saved = SaveDC(dc);                     // (scrolled: cut at the strip's ends)
    if (p.scroll || (int)p.tabs.size() * p.tabW > p.strip.right - p.strip.left)
        IntersectClipRect(dc, p.strip.left, 0, p.strip.right, p.rowH);
    for (int i = 0; i < (int)p.tabs.size(); ++i) {
        const QueueTab& t = *p.tabs[(size_t)i];
        RECT r = tab_rect(p, i);
        if (r.right < p.strip.left || r.left > p.strip.right) continue;
        bool act = i == p.shown, hot = i == p.hotTab;
        if (act) {
            fill_tab_shape(dc, r, px(8), c.layer);
        } else if (hot) {
            RECT h{ r.left + px(2), r.top + px(2), r.right - px(2), r.bottom - px(4) };
            fill_rrect(dc, h, px(6), c.tabHover);
        } else if (i + 1 != p.shown && i + 1 != p.hotTab && i + 1 < (int)p.tabs.size()) {
            fill_rect(dc, RECT{ r.right - 1, r.top + px(10), r.right, r.bottom - px(10) }, c.divider);
        }
        RECT cr = tab_close_rect(p, i);
        int bottom = r.bottom - (act ? 0 : px(2)), left = r.left + px(12);
        if (&t == p.playing && p.markShown) {
            RECT g{ left - px(2), r.top, left + px(16), bottom };
            glyph_at(dc, L'\xE767', g, p.markShown == 2 ? c.accent : c.textSecondary, p.glyphsSmall);
            left += px(20);
        }
        if (&t == p.renaming) {                 // (the box over it: framed)
            fill_rrect(dc, rename_rect(p, i), px(4), c.content, c.accent);
            continue;
        }
        RECT tr{ left, r.top, cr.left - px(4), bottom };
        text_at(dc, t.name, tr, act ? c.text : c.textSecondary, p.font, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (act || hot) {
            if (p.hotClose == i) fill_rrect(dc, cr, px(4), act ? c.hover : c.layer);
            glyph_at(dc, L'\xE711', cr, c.text, p.glyphsSmall);
        }
    }
    RestoreDC(dc, saved);
    if (p.hotPlus) fill_rrect(dc, p.plus, px(4), c.tabHover);
    glyph_at(dc, L'\xE710', p.plus, c.text, p.glyphsMid);
}

void paint_main(Player& p) {
    BufferedPaint bp(p.wnd);
    const Palette& c = pal();
    fill_rect(bp.dc, bp.rc, c.layer);           // (the gap between the video and the queue)
    if (p.rowH) paint_tabs(p, bp.dc, bp.rc);
    if (!p.queueShown || p.full) return;
    const PlayQueue& q = vq(p);
    RECT head{ p.splitX + px(4), p.rowH, bp.rc.right, p.rowH + p.headH };
    fill_rect(bp.dc, head, c.content);
    RECT tr{ head.left + px(12), head.top, p.clearBtn.left - px(8), head.bottom };
    std::wstring count = std::to_wstring(q.size()) + (q.size() == 1 ? L" item" : L" items");
    HGDIOBJ of = SelectObject(bp.dc, p.fontBold);
    int tw = text_width(bp.dc, L"Play queue");
    SelectObject(bp.dc, of);
    text_at(bp.dc, L"Play queue", tr, c.text, p.fontBold, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT cr = tr;
    cr.left += tw + px(8);
    if (cr.right > cr.left) text_at(bp.dc, count, cr, c.textSecondary, p.fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (p.clearHot && !q.empty()) fill_rrect(bp.dc, p.clearBtn, px(4), c.hover);
    glyph_at(bp.dc, L'\xE74D', p.clearBtn, q.empty() ? c.textDisabled : c.text, p.glyphs);
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

// ------------------------------------------------------------------
// subtitles: SubRip / WebVTT / SubStation files named after the video beside it (or one
// chosen), drawn over its picture by the video renderer
// ------------------------------------------------------------------
std::wstring name_of(const std::wstring& path) { return path.substr(path.find_last_of(L"\\/") + 1); }

void subs_find(Player& p, const std::wstring& video) {
    p.subsOf = video;
    p.subs.clear();
    size_t slash = video.find_last_of(L"\\/");
    if (slash == std::wstring::npos || media_url(video)) return;
    std::wstring dir = video.substr(0, slash + 1), name = video.substr(slash + 1);
    size_t dot = name.rfind(L'.');
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + name.substr(0, dot) + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::pair<std::wstring, std::wstring>> found;      // label, path
    do {
        std::wstring label;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && subtitle_for(name, fd.cFileName, &label))
            found.push_back({ label, dir + fd.cFileName });
    } while (found.size() < 99 && FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(found.begin(), found.end());                          // (the one with no label first)
    for (auto& f : found) p.subs.push_back(f.second);
}

// the file in the user's language, else the one named just as the video, else the first
int subs_preferred(Player& p) {
    std::vector<std::wstring> mine;
    WCHAR loc[LOCALE_NAME_MAX_LENGTH]{}, s[128];
    if (GetUserDefaultLocaleName(loc, ARRAYSIZE(loc))) {
        for (LCTYPE t : { LOCALE_SISO639LANGNAME, LOCALE_SISO639LANGNAME2, LOCALE_SENGLISHLANGUAGENAME,
                          LOCALE_SNATIVELANGUAGENAME, LOCALE_SLOCALIZEDLANGUAGENAME })
            if (GetLocaleInfoEx(loc, t, s, ARRAYSIZE(s)) > 1) mine.push_back(s);
    }
    int plain = -1;
    for (int i = 0; i < (int)p.subs.size(); ++i) {
        std::wstring label;
        subtitle_for(name_of(p.subsOf), name_of(p.subs[i]), &label);
        if (label.empty() && plain < 0) plain = i;
        for (size_t a = 0; a <= label.size();) {                   // its dot-separated parts
            size_t b = label.find(L'.', a);
            if (b == std::wstring::npos) b = label.size();
            std::wstring part = label.substr(a, b - a);
            for (auto& m : mine)
                if (!part.empty() && _wcsicmp(part.c_str(), m.c_str()) == 0) return i;
            a = b + 1;
        }
    }
    return plain >= 0 ? plain : p.subs.empty() ? -1 : 0;
}

// shows p.subs[i]; false: nothing in it to show
bool subs_load(Player& p, int i) {
    p.cues.clear();
    p.subPick = -1;
    if (i < 0 || i >= (int)p.subs.size()) return false;
    HANDLE h = CreateFileW(p.subs[i].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::string raw;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (32 << 20);
    if (ok) {
        raw.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = raw.empty() || (ReadFile(h, raw.data(), (DWORD)raw.size(), &got, nullptr) && got == raw.size());
    }
    CloseHandle(h);
    if (ok) p.cues = subtitles_parse(ini_decode(raw));
    if (p.cues.empty()) return false;
    p.subPick = i;
    p.subShown = L"\x1";                        // (drawn anew)
    return true;
}

LRESULT CALLBACK subs_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;          // (the mouse goes to the video under it)
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// the subtitles' window: a see-through popup of the player's over its video (the video
// renderer's own overlay mixes them into the frames, and hangs some GPUs doing it)
HWND subs_window(Player& p) {
    if (p.subWnd) return p.subWnd;
    static ATOM cls = [] {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = subs_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ExplorerPlusSubtitles";
        return RegisterClassExW(&wc);
    }();
    if (!cls) return nullptr;
    p.subWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                               L"ExplorerPlusSubtitles", L"", WS_POPUP, 0, 0, 0, 0, p.wnd, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    return p.subWnd;
}

void subs_hide(Player& p) {
    if (p.subWnd && IsWindowVisible(p.subWnd)) ShowWindow(p.subWnd, SW_HIDE);
}

// the text over the picture (`pic`, screen coordinates): white with a black outline,
// centred near its bottom (above the controls bar where that's over the video), its size
// after the picture's height
void subs_draw(Player& p, const std::wstring& text, const RECT& pic, int barTop) {
    int pw = pic.right - pic.left, ph = pic.bottom - pic.top;
    if (text.empty() || pw < 32 || ph < 32) { subs_hide(p); return; }
    using namespace Gdiplus;
    float em = std::max((float)px(13), ph * 0.055f);
    FontFamily family(L"Segoe UI");
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    GraphicsPath path;
    RectF layout(0, 0, pw * 0.9f, (REAL)ph);
    path.AddString(text.c_str(), -1, &family, FontStyleBold, em, layout, &sf);
    Pen pen(Color(255, 0, 0, 0), std::max(2.0f, em * 0.16f));
    pen.SetLineJoin(LineJoinRound);
    RectF b;
    path.GetBounds(&b, nullptr, &pen);
    if (b.Width <= 0 || b.Height <= 0) { subs_hide(p); return; }
    int bw = std::min(pw, (int)std::ceil(b.Width) + 4), bh = std::min(ph, (int)std::ceil(b.Height) + 4);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), bw, -bh, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(mem); return; }
    {
        Bitmap canvas(bw, bh, bw * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&canvas);
        g.Clear(Color(0, 0, 0, 0));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.TranslateTransform(2 - b.X, 2 - b.Y);
        g.DrawPath(&pen, &path);
        SolidBrush white(Color(255, 255, 255, 255));
        g.FillPath(&white, &path);
    }
    int bottom = pic.top + (int)(ph * 0.955f);
    if (barTop >= 0 && barTop - px(8) < bottom) bottom = barTop - px(8);
    POINT at{ pic.left + (pw - bw) / 2, std::max<int>(pic.top, bottom - bh) };
    SIZE size{ bw, bh };
    POINT zero{};
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HGDIOBJ old = SelectObject(mem, bmp);
    if (HWND w = subs_window(p)) {
        UpdateLayeredWindow(w, nullptr, &at, &size, mem, &zero, 0, &blend, ULW_ALPHA);
        if (!IsWindowVisible(w)) ShowWindow(w, SW_SHOWNOACTIVATE);
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// the text of the moment on screen (the timer's: while a video with subtitles is open;
// also as the window moves)
void subs_tick(Player& p) {
    int i = p.q->current();
    if (p.cues.empty() || !p.mp->has_video() || !p.message.empty() || i < 0 || p.q->at(i).path != p.subsOf) {
        subs_hide(p);
        p.subShown.clear();
        KillTimer(p.wnd, TIMER_SUBS);
        return;
    }
    SetTimer(p.wnd, TIMER_SUBS, 100, nullptr);
    RECT pic{};
    // (opened: the renderer comes a moment later)
    if (IsIconic(p.wnd) || !IsWindowVisible(p.video) || !p.mp->video_rect(&pic)) {
        subs_hide(p);
        p.subShown = L"\x1";
        return;
    }
    MapWindowPoints(p.video, nullptr, (POINT*)&pic, 2);
    int barTop = -1;                            // the bar, where it's over the video (full screen)
    RECT br{};
    if (IsWindowVisible(p.bar) && GetWindowRect(p.bar, &br) && br.top < pic.bottom && br.bottom > pic.top) barTop = br.top;
    std::wstring t = subtitle_at(p.cues, p.mp->position());
    if (t == p.subShown && EqualRect(&pic, &p.subRect) && barTop == p.subBarTop) return;
    p.subShown = t;
    p.subRect = pic;
    p.subBarTop = barTop;
    subs_draw(p, t, pic, barTop);
}
// a file opened (Opened): a video's subtitles looked up (once per video) and shown
void subs_open(Player& p) {
    int i = p.q->current();
    if (i >= 0 && p.mp->has_video()) {
        const std::wstring& path = p.q->at(i).path;
        if (p.subsOf != path) {
            subs_find(p, path);
            p.cues.clear();
            p.subPick = -1;
            if (p.subsOn) subs_load(p, subs_preferred(p));
        }
    } else {
        p.subsOf.clear();
        p.subs.clear();
        p.cues.clear();
        p.subPick = -1;
    }
    p.subShown = L"\x1";                        // (a new renderer: drawn anew)
    subs_tick(p);
}

std::wstring ask_subtitle_file(HWND owner, const std::wstring& dir) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    const COMDLG_FILTERSPEC types[] = { { L"Subtitles (*.srt, *.vtt, *.ass, *.ssa)", L"*.srt;*.vtt;*.ass;*.ssa" },
                                        { L"All files", L"*.*" } };
    d->SetFileTypes(ARRAYSIZE(types), types);
    d->SetTitle(L"Choose subtitles");
    FILEOPENDIALOGOPTIONS opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    IShellItem* folder = nullptr;
    if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        d->SetFolder(folder);
        folder->Release();
    }
    std::wstring out;
    IShellItem* item = nullptr;
    if (SUCCEEDED(d->Show(owner)) && SUCCEEDED(d->GetResult(&item))) {
        PWSTR s = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &s)) && s) { out = s; CoTaskMemFree(s); }
        item->Release();
    }
    d->Release();
    return out;
}

void subs_command(Player& p, UINT cmd) {
    if (!p.mp->has_video()) return;
    int pick = -2;                              // -2: as it is
    if (cmd == CMD_SUBS_OFF) pick = -1;
    else if (cmd == CMD_SUBS_NEXT) pick = p.subs.empty() ? -2 : p.subPick + 1 < (int)p.subs.size() ? p.subPick + 1 : -1;
    else if (cmd >= CMD_SUB && cmd < CMD_SUB + 100) pick = (int)(cmd - CMD_SUB);
    else if (cmd == CMD_SUBS_CHOOSE) {
        std::wstring file = ask_subtitle_file(p.wnd, p.subsOf.substr(0, p.subsOf.find_last_of(L"\\/") + 1));
        if (file.empty()) return;
        auto it = std::find_if(p.subs.begin(), p.subs.end(), [&](const std::wstring& s) { return _wcsicmp(s.c_str(), file.c_str()) == 0; });
        if (it == p.subs.end()) it = p.subs.insert(p.subs.end(), file);
        pick = (int)(it - p.subs.begin());
    }
    if (pick == -2) return;
    if (pick == -1) {
        p.cues.clear();
        p.subPick = -1;
        p.subsOn = false;
    } else if (subs_load(p, pick)) {
        p.subsOn = true;
    } else {
        MessageBoxW(p.wnd, (L"No subtitles could be read from this file:\n" + p.subs[pick]).c_str(), L"Subtitles",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    settings_set_pref(L"playerSubtitles", p.subsOn);
    subs_tick(p);
}

std::wstring menu_text(std::wstring s) {
    for (size_t i = 0; (i = s.find(L'&', i)) != std::wstring::npos; i += 2) s.insert(i, 1, L'&');
    return s;
}

// the video area's menu: the visualization, levelling, the slide show's interval, full screen
void view_menu(Player& p, POINT pt) {
    if (pt.x == -1 && pt.y == -1) {             // (keyboard: the area's middle)
        RECT r{};
        GetClientRect(p.video, &r);
        pt = { r.right / 2, r.bottom / 2 };
        ClientToScreen(p.video, &pt);
    }
    HMENU m = CreatePopupMenu();
    HMENU vis = CreatePopupMenu();
    for (int s = 0; s < VIS_COUNT; ++s)
        AppendMenuW(vis, MF_STRING | (s && p.vis.failed() ? MF_GRAYED : 0), CMD_VIS + s, vis_name(s));
    CheckMenuRadioItem(vis, CMD_VIS, CMD_VIS + (UINT)VIS_COUNT - 1, CMD_VIS + p.visStyle, MF_BYCOMMAND);
    AppendMenuW(vis, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(vis, MF_STRING, CMD_VIS_NEXT, L"Next visualization\tV");
    AppendMenuW(vis, MF_STRING | (p.visCycle ? MF_CHECKED : 0) | (p.vis.failed() ? MF_GRAYED : 0), CMD_VIS_CYCLE,
                L"Another with each track");
    HMENU slides = CreatePopupMenu();
    for (int s : kSlideSecs)
        AppendMenuW(slides, MF_STRING | (s == p.slideSecs ? MF_CHECKED : 0), CMD_SLIDE + s, (std::to_wstring(s) + L" seconds").c_str());
    if (p.mp->has_video() && !image_current(p)) {
        HMENU subs = CreatePopupMenu();
        AppendMenuW(subs, MF_STRING | (p.subPick < 0 ? MF_CHECKED : 0), CMD_SUBS_OFF, L"Off");
        for (int i = 0; i < (int)p.subs.size(); ++i)
            AppendMenuW(subs, MF_STRING | (i == p.subPick ? MF_CHECKED : 0), CMD_SUB + i, menu_text(name_of(p.subs[i])).c_str());
        AppendMenuW(subs, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(subs, MF_STRING, CMD_SUBS_NEXT, L"Next subtitles\tS");
        AppendMenuW(subs, MF_STRING, CMD_SUBS_CHOOSE, L"Choose a file...");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)subs, L"Subtitles");
    }
    if (p.mp->audio_tracks().size() > 1) {
        HMENU tracks = CreatePopupMenu();
        const auto& names = p.mp->audio_tracks();
        for (int i = 0; i < (int)names.size() && i < 100; ++i)
            AppendMenuW(tracks, MF_STRING | (i == p.mp->audio_track() ? MF_CHECKED : 0), CMD_AUDIO + i, menu_text(names[i]).c_str());
        AppendMenuW(tracks, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(tracks, MF_STRING, CMD_AUDIO_NEXT, L"Next audio track\tA");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)tracks, L"Audio track");
    }
    if (p.mp->has_video() || p.mp->audio_tracks().size() > 1) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)vis, L"Visualization");
    AppendMenuW(m, MF_STRING | (p.level ? MF_CHECKED : 0), CMD_LEVEL, L"Even out loudness between tracks");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)slides, L"Slide show interval");
    if (image_current(p)) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (p.message.empty() ? 0 : MF_GRAYED), CMD_EDIT_PICTURE, L"Edit picture\tCtrl+E");
        HMENU zoom = CreatePopupMenu();
        UINT can = p.message.empty() && p.img.pixels ? 0 : MF_GRAYED;
        AppendMenuW(zoom, MF_STRING | can, CMD_ZOOM_IN, L"Zoom in\t+");
        AppendMenuW(zoom, MF_STRING | can, CMD_ZOOM_OUT, L"Zoom out\t-");
        AppendMenuW(zoom, MF_STRING | can | (p.zoom == 1 ? MF_CHECKED : 0), CMD_ZOOM_FIT, L"Fit\t0");
        AppendMenuW(zoom, MF_STRING | can, CMD_ZOOM_ACTUAL, L"Actual size\t1");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)zoom, L"Zoom");
        if (p.img.pages > 1) {
            AppendMenuW(m, MF_STRING | (p.img.page > 0 ? 0 : MF_GRAYED), CMD_PAGE_PREV, L"Previous page\tPage Up");
            AppendMenuW(m, MF_STRING | (p.img.page + 1 < p.img.pages ? 0 : MF_GRAYED), CMD_PAGE_NEXT, L"Next page\tPage Down");
        }
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_FULL, p.full ? L"Exit full screen\tEsc" : L"Full screen\tF");
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, p.wnd, nullptr);
    DestroyMenu(m);                             // (and its submenus)
    if (id) command(p, id);
}

// the wheel over a picture: the previous / next one; with Ctrl, zooming at the pointer
bool image_wheel(Player& p, WPARAM wp) {
    if (!image_current(p)) return false;
    POINT c{};
    GetCursorPos(&c);
    RECT r{};
    GetWindowRect(p.video, &r);
    if (!PtInRect(&r, c)) return false;
    if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
        ScreenToClient(p.video, &c);
        image_zoom_to(p, p.zoom * std::pow(1.25, GET_WHEEL_DELTA_WPARAM(wp) / (double)WHEEL_DELTA), &c);
        return true;
    }
    command(p, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? CMD_PREV : CMD_NEXT);
    return true;
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
            subs_tick(p);
            if (image_current(p)) image_sharpen(p, image_view(p));     // (zoomed: as big again now)
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(p.wnd);
            if (image_pannable(p)) {               // (zoomed in: dragged about)
                p.panning = true;
                p.panFrom = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                p.panX = p.zoomX;
                p.panY = p.zoomY;
                SetCapture(hwnd);
            }
            return 0;
        case WM_LBUTTONUP:
            if (p.panning) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED: p.panning = false; return 0;
        case WM_LBUTTONDBLCLK: command(p, CMD_FULL); return 0;
        case WM_MOUSEMOVE:
            if (p.panning) { image_pan(p, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }); return 0; }
            full_show_bar(p);
            return 0;
        case WM_CONTEXTMENU: view_menu(p, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }); return 0;
        case WM_MOUSEWHEEL:
            if (image_wheel(p, wp)) return 0;
            break;
        case WM_SETCURSOR:
            if (p.full && !p.barInFull) { SetCursor(nullptr); return TRUE; }
            if (LOWORD(lp) == HTCLIENT && image_pannable(p)) { SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); return TRUE; }
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
    // (the other queues; also by dragging the rows onto a tab)
    HMENU to = CreatePopupMenu();
    int n = (int)p.tabs.size();
    for (int i = 0; i < n; ++i)
        if (i != p.shown) AppendMenuW(to, MF_STRING, CMD_MOVE_TO + i, menu_text(p.tabs[(size_t)i]->name).c_str());
    if (n > 1) AppendMenuW(to, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(to, MF_STRING, CMD_MOVE_TO + n, L"New queue");
    AppendMenuW(m, MF_POPUP | (sel ? 0 : MF_GRAYED), (UINT_PTR)to, L"Move to queue");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    bool sound = std::any_of(vq(p).entries().begin(), vq(p).entries().end(), [](const QueueEntry& e) { return !image_file(e.path); });
    AppendMenuW(m, MF_STRING | (sound ? 0 : MF_GRAYED), CMD_SAVE_PLAYLIST, L"Save queue as playlist...\tCtrl+S");
    AppendMenuW(m, MF_STRING | (vq(p).empty() ? MF_GRAYED : 0), CMD_CLEAR, L"Clear queue");
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, p.wnd, nullptr);
    DestroyMenu(m);
    if (id >= CMD_MOVE_TO && id <= CMD_MOVE_TO + (UINT)n) move_selected(p, (int)(id - CMD_MOVE_TO));
    else if (id) command(p, id);
}

// a tab's menu (what acts on the queue shown: this tab's, shown first)
void tab_menu(Player& p, int i, POINT pt) {
    QueueTab* t = p.tabs[(size_t)i].get();
    int n = (int)p.tabs.size();
    bool sound = std::any_of(t->q.entries().begin(), t->q.entries().end(), [](const QueueEntry& e) { return !image_file(e.path); });
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_TAB_NEW, L"New queue\tCtrl+N");
    AppendMenuW(m, MF_STRING, CMD_TAB_RENAME, L"Rename\tF2");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (sound ? 0 : MF_GRAYED), CMD_SAVE_PLAYLIST, L"Save queue as playlist...\tCtrl+S");
    AppendMenuW(m, MF_STRING | (t->q.empty() ? MF_GRAYED : 0), CMD_CLEAR, L"Clear queue");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_TAB_CLOSE, L"Close queue\tCtrl+W");
    AppendMenuW(m, MF_STRING | (n > 1 ? 0 : MF_GRAYED), CMD_TAB_CLOSE_OTHERS, L"Close other queues");
    AppendMenuW(m, MF_STRING | (i + 1 < n ? 0 : MF_GRAYED), CMD_TAB_CLOSE_RIGHT, L"Close queues to the right");
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, p.wnd, nullptr);
    DestroyMenu(m);
    i = tab_index(p, t);                        // (where it is now)
    if (!id || i < 0) return;
    switch (id) {
        case CMD_TAB_CLOSE: close_tab(p, i); break;
        case CMD_TAB_CLOSE_OTHERS:
            for (int k = (int)p.tabs.size() - 1; k >= 0; --k)
                if (p.tabs[(size_t)k].get() != t) close_tab(p, k);
            break;
        case CMD_TAB_CLOSE_RIGHT:
            for (int k = (int)p.tabs.size() - 1; k > tab_index(p, t); --k) close_tab(p, k);
            break;
        case CMD_TAB_RENAME: start_rename(p, i); break;
        default:
            show_tab(p, i);
            command(p, id);
            break;
    }
}

void row_drag_start(Player& p);

LRESULT list_notify(Player& p, NMHDR* hdr) {
    switch (hdr->code) {
        case LVN_GETDISPINFOW: {
            auto* di = (NMLVDISPINFOW*)hdr;
            int i = di->item.iItem;
            if (i < 0 || i >= vq(p).size() || !(di->item.mask & LVIF_TEXT) || !di->item.pszText) break;
            const QueueEntry& e = vq(p).at(i);
            lstrcpynW(di->item.pszText, entry_name(e).c_str(), di->item.cchTextMax);
            break;
        }
        case LVN_ODFINDITEMW: {
            // type-ahead: the next entry whose name starts with what was typed (0 would be "row 0")
            auto* fi = (NMLVFINDITEMW*)hdr;
            if (!(fi->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || !fi->lvfi.psz || vq(p).empty()) return -1;
            size_t n = wcslen(fi->lvfi.psz);
            int start = fi->iStart >= 0 && fi->iStart < vq(p).size() ? fi->iStart : 0;
            for (int k = 0; k < vq(p).size(); ++k) {
                int i = (start + k) % vq(p).size();
                std::wstring name = entry_name(vq(p).at(i));
                if (name.size() >= n && _wcsnicmp(name.c_str(), fi->lvfi.psz, n) == 0) return i;
            }
            return -1;
        }
        case NM_DBLCLK: {
            auto* ia = (NMITEMACTIVATE*)hdr;
            play_row(p, ia->iItem);
            break;
        }
        case LVN_BEGINDRAG:
            row_drag_start(p);
            break;
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
                // (rows dragged: the gap they'd go into)
                if (p.rowDrag && p.dropRow >= 0) {
                    int n = vq(p).size(), y = 0;
                    RECT r{};
                    if (n > 0 && ListView_GetItemRect(p.list, std::min(p.dropRow, n - 1), &r, LVIR_BOUNDS))
                        y = p.dropRow < n ? r.top : r.bottom;
                    int t = std::clamp(y - px(1), 0, std::max(0, (int)client.bottom - px(2)));
                    RECT line{ px(4), t, x - px(4), t + px(2) };
                    fill_rect(cd->nmcd.hdc, line, pal().accent);
                }
                return CDRF_DODEFAULT;
            }
            if (cd->nmcd.dwDrawStage != CDDS_ITEMPREPAINT) break;
            int i = (int)cd->nmcd.dwItemSpec;
            if (i < 0 || i >= vq(p).size()) return CDRF_SKIPDEFAULT;
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
            const QueueEntry& e = vq(p).at(i);
            bool cur = i == vq(p).current();
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
    if (p.editBrush) DeleteObject(p.editBrush);
    p.editBrush = CreateSolidBrush(pal().content);  // (a tab's name edited: the box's)
    if (p.specBmp) spectrum_redraw(p);
    vis_update(p);
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
    // (the player's own icon: its taskbar button is apart from the file windows')
    LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_PLAYER), LIM_LARGE, &big);
    LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_PLAYER), LIM_SMALL, &small);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = g_hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = player_proc;
    wc.lpszClassName = player_class().c_str();
    wc.style = CS_DBLCLKS;                  // (a tab double-clicked: its name edited)
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
    p.video = CreateWindowExW(0, L"ExplorerPlusVideo", L"Video", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
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
    add_tool(p.headTip, hwnd, kTipPlus, LPSTR_TEXTCALLBACKW);
    SendMessageW(p.headTip, TTM_SETMAXTIPWIDTH, 0, px(600));
    p.mp = new MediaPlayer(hwnd, WM_MEDIA_NOTE, p.video);
    apply_volume(p);
    apply_theme(p);
    DragAcceptFiles(hwnd, TRUE);
    p.lastFocus = hwnd;
    layout(p);
    refresh_all(p);
}

void on_destroy(Player& p) {
    end_rename(p, true);
    keep_now(p);                            // (the queues as they are now, for next time)
    p.vis.destroy();
    KillTimer(p.wnd, TIMER_KEEP);
    KillTimer(p.wnd, TIMER_POSITION);
    KillTimer(p.wnd, TIMER_SKIP);
    KillTimer(p.wnd, TIMER_HIDE_BAR);
    KillTimer(p.wnd, TIMER_SUBS);
    if (p.subWnd) { DestroyWindow(p.subWnd); p.subWnd = nullptr; }
    save_place(p);
    tags_forget();
    p.mp->close();
    // notes still on their way to the window hold references: let them go
    MSG m;
    while (PeekMessageW(&m, p.wnd, WM_MEDIA_NOTE, WM_MEDIA_NOTE, PM_REMOVE)) p.mp->handle(reinterpret_cast<MediaNote*>(m.lParam));
    delete p.mp;
    p.mp = nullptr;
    p.feed.stop();
    KillTimer(p.wnd, TIMER_SPECTRUM);
    while (PeekMessageW(&m, p.wnd, WM_MEDIA_INFO, WM_MEDIA_INFO, PM_REMOVE)) delete reinterpret_cast<MediaInfoNote*>(m.lParam);
    while (PeekMessageW(&m, p.wnd, WM_IMAGE, WM_IMAGE, PM_REMOVE)) delete reinterpret_cast<ImageNote*>(m.lParam);
    while (PeekMessageW(&m, p.wnd, WM_LOUDNESS, WM_LOUDNESS, PM_REMOVE)) delete reinterpret_cast<LoudnessNote*>(m.lParam);
    image_forget(p);
    if (p.coverBmp) DeleteObject(p.coverBmp);
    if (p.specBmp) DeleteObject(p.specBmp);
    p.coverBmp = p.specBmp = nullptr;
    p.specBits = nullptr;
    if (p.taskbar) { p.taskbar->Release(); p.taskbar = nullptr; }
    free_thumb_icons(p);
    set_app_id(p.wnd, false);
    delete_fonts(p);
    if (p.editBrush) { DeleteObject(p.editBrush); p.editBrush = nullptr; }
}

void on_media_info(Player& p, MediaInfoNote* n) {
    int i = p.q->current();
    if (i >= 0 && p.q->at(i).id == n->id) {
        p.info = std::move(n->info);
        p.infoFor = n->id;
        if (p.coverBmp) DeleteObject(p.coverBmp);
        p.coverBmp = nullptr;
        InvalidateRect(p.video, nullptr, FALSE);
    }
    delete n;
}

// --- the tab row and the mouse ---
void set_hot(Player& p, int tab, int close, bool plus) {
    if (tab == p.hotTab && close == p.hotClose && plus == p.hotPlus) return;
    p.hotTab = tab;
    p.hotClose = close;
    p.hotPlus = plus;
    invalidate_row(p);
}

// --- the queue's rows dragged (the window has the mouse meanwhile) ---
void row_drag_start(Player& p) {
    p.rowIds.clear();
    for (int i : selected_rows(p)) p.rowIds.push_back(vq(p).at(i).id);
    if (p.rowIds.empty()) return;
    p.rowFrom = p.tabs[(size_t)p.shown].get();
    p.rowDrag = true;
    p.dropRow = p.dropTab = -1;
    SetCapture(p.wnd);
}

// where rows let go at `pt` (the window's) would go in the list: before row k (the row count:
// after the last); -1: not over the list
int drop_row_at(Player& p, POINT pt) {
    if (!IsWindowVisible(p.list)) return -1;
    MapWindowPoints(p.wnd, p.list, &pt, 1);
    RECT rc{}, r0{};
    GetClientRect(p.list, &rc);
    if (!PtInRect(&rc, pt)) return -1;
    int n = vq(p).size();
    if (n == 0 || !ListView_GetItemRect(p.list, 0, &r0, LVIR_BOUNDS) || r0.bottom <= r0.top) return 0;
    int h = r0.bottom - r0.top;
    return std::clamp((int)(pt.y - r0.top + h / 2) / h, 0, n);    // (the gap between rows nearest)
}

// they can go where they are: into the list, onto "+" or another queue's tab
bool row_drop_ok(Player& p) {
    return p.dropRow >= 0 || p.dropTab == -2 || (p.dropTab >= 0 && p.tabs[(size_t)p.dropTab].get() != p.rowFrom);
}

void row_drag_move(Player& p, POINT pt) {
    int row = drop_row_at(p, pt), tab = -1;
    if (row < 0 && pt.y < p.rowH && !p.full) {
        tab = tab_at(p, pt);
        if (tab < 0 && PtInRect(&p.plus, pt)) tab = -2;
    }
    if (tab != p.dropTab) {                 // (held over another tab: it's shown)
        KillTimer(p.wnd, TIMER_DRAG_TAB);
        if (tab >= 0 && tab != p.shown) SetTimer(p.wnd, TIMER_DRAG_TAB, kDragTabMs, nullptr);
    }
    if (row != p.dropRow) InvalidateRect(p.list, nullptr, FALSE);
    p.dropRow = row;
    p.dropTab = tab;
    set_hot(p, std::max(tab, -1), -1, tab == -2);
    // (near the list's top or bottom: it scrolls)
    bool edge = false;
    if (row >= 0) {
        POINT lp = pt;
        MapWindowPoints(p.wnd, p.list, &lp, 1);
        RECT rc{};
        GetClientRect(p.list, &rc);
        edge = lp.y < px(24) || lp.y >= rc.bottom - px(24);
    }
    if (edge) SetTimer(p.wnd, TIMER_DRAG_SCROLL, kDragScrollMs, nullptr);
    else KillTimer(p.wnd, TIMER_DRAG_SCROLL);
    SetCursor(LoadCursorW(nullptr, row_drop_ok(p) ? IDC_ARROW : IDC_NO));
}

void row_drag_timer(Player& p, UINT_PTR id) {
    if (id == TIMER_DRAG_TAB || !p.rowDrag) KillTimer(p.wnd, id);
    if (!p.rowDrag) return;
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(p.wnd, &pt);
    if (id == TIMER_DRAG_TAB && p.dropTab >= 0 && p.dropTab != p.shown) show_tab(p, p.dropTab);
    if (id == TIMER_DRAG_SCROLL) {
        POINT lp = pt;
        MapWindowPoints(p.wnd, p.list, &lp, 1);
        RECT r0{};
        int h = ListView_GetItemRect(p.list, 0, &r0, LVIR_BOUNDS) ? (int)(r0.bottom - r0.top) : px(32);
        ListView_Scroll(p.list, 0, lp.y < px(24) ? -h : h);
        InvalidateRect(p.list, nullptr, FALSE);     // (the drop line drawn again where it is now)
    }
    row_drag_move(p, pt);                   // (where they'd go now: the list moved under it)
}

// let go: `drop`, they go there (Ctrl held: copies of them); else they stay
void row_drag_end(Player& p, bool drop) {
    if (!p.rowDrag) return;
    p.rowDrag = false;
    KillTimer(p.wnd, TIMER_DRAG_TAB);
    KillTimer(p.wnd, TIMER_DRAG_SCROLL);
    int row = p.dropRow, tab = p.dropTab;
    bool ok = drop && row_drop_ok(p);
    p.dropRow = p.dropTab = -1;
    if (GetCapture() == p.wnd) ReleaseCapture();
    set_hot(p, -1, -1, false);
    InvalidateRect(p.list, nullptr, FALSE);
    if (!ok) return;
    bool copy = GetKeyState(VK_CONTROL) < 0;
    if (row >= 0) move_entries(p, p.rowFrom, p.rowIds, p.tabs[(size_t)p.shown].get(), row, copy);
    else move_to_tab(p, p.rowFrom, p.rowIds, tab == -2 ? (int)p.tabs.size() : tab, copy);
}

// a tab dragged along the row (true), else what the mouse is over lit
bool tabs_mouse_move(Player& p, POINT pt) {
    if (p.dragTab >= 0) {
        if (!p.dragging && abs(pt.x - p.dragX) > GetSystemMetrics(SM_CXDRAG)) p.dragging = true;
        if (p.dragging && p.tabW > 0) {
            int to = std::clamp((int)(pt.x - p.strip.left + p.scroll) / p.tabW, 0, (int)p.tabs.size() - 1);
            if (to != p.dragTab) {
                move_tab(p, p.dragTab, to);
                p.dragTab = to;
                set_hot(p, to, -1, false);
            }
        }
        return true;
    }
    int tab = tab_at(p, pt), close = -1;
    if (tab >= 0) {
        RECT cr = tab_close_rect(p, tab);
        if (PtInRect(&cr, pt)) close = tab;
    }
    set_hot(p, tab, close, PtInRect(&p.plus, pt) != FALSE);
    return false;
}

// a press on the tab row (true: it was there)
bool tabs_mouse_down(Player& p, POINT pt, bool dbl) {
    if (pt.y >= p.rowH) return false;
    int tab = tab_at(p, pt);
    if (tab >= 0) {
        RECT cr = tab_close_rect(p, tab);
        if (PtInRect(&cr, pt) && (tab == p.shown || tab == p.hotTab)) { close_tab(p, tab); return true; }
        if (dbl) { start_rename(p, tab); return true; }
        if (tab != p.shown) show_tab(p, tab);
        p.dragTab = tab;
        p.dragX = pt.x;
        p.dragging = false;
        SetCapture(p.wnd);
        return true;
    }
    if (PtInRect(&p.plus, pt)) command(p, CMD_TAB_NEW);
    return true;
}

// The frame, as the Notepad's and the main window's (chrome.cpp): no title bar, the tab
// row in its place. Its empty part moves the window (and snaps it, maximizes on a
// double-click), its buttons are the system's to Windows (snap layouts over maximize).
// Full screen: no frame, no row.
int frame_y(const Player& p) {
    return GetSystemMetricsForDpi(SM_CYFRAME, p.dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, p.dpi);
}

void invalidate_cap_buttons(Player& p) {
    RECT r = cap_button(p, 0);
    r.right = cap_button(p, 2).right;
    InvalidateRect(p.wnd, &r, FALSE);
}

int cap_button_of(WPARAM ht) { return ht == HTMINBUTTON ? 0 : ht == HTMAXBUTTON ? 1 : ht == HTCLOSE ? 2 : -1; }

LRESULT hit_test(Player& p, LPARAM lp) {
    LRESULT def = DefWindowProcW(p.wnd, WM_NCHITTEST, 0, lp);  // (the side and bottom borders)
    if (def != HTCLIENT || p.full) return def;
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ScreenToClient(p.wnd, &pt);
    if (pt.y >= p.rowH) return HTCLIENT;
    for (int i = 0; i < 3; ++i) {
        RECT r = cap_button(p, i);
        if (PtInRect(&r, pt)) return i == 0 ? HTMINBUTTON : i == 1 ? HTMAXBUTTON : HTCLOSE;
    }
    int fy = frame_y(p);
    if (!IsZoomed(p.wnd) && pt.y < fy) {        // (the top border is in the client area)
        RECT rc{};
        GetClientRect(p.wnd, &rc);
        return pt.x < fy * 2 ? HTTOPLEFT : pt.x >= rc.right - fy * 2 ? HTTOPRIGHT : HTTOP;
    }
    if (p.dragTab >= 0 || tab_at(p, pt) >= 0 || PtInRect(&p.plus, pt)) return HTCLIENT;
    return HTCAPTION;
}

bool frame_message(Player& p, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    switch (msg) {
        case WM_NCCALCSIZE: {
            if (!wp || p.full) return false;
            auto* np = (NCCALCSIZE_PARAMS*)lp;
            LONG top = np->rgrc[0].top;
            DefWindowProcW(p.wnd, msg, wp, lp);
            np->rgrc[0].top = top + (IsZoomed(p.wnd) ? frame_y(p) : 0);  // (maximized: the frame is off the screen)
            *result = 0;
            return true;
        }
        case WM_NCHITTEST:
            *result = hit_test(p, lp);
            return true;
        case WM_NCACTIVATE:
            p.activeWnd = wp != FALSE;
            invalidate_row(p);
            *result = DefWindowProcW(p.wnd, msg, wp, -1);   // (-1: no classic frame painted)
            return true;
        case WM_NCMOUSEMOVE: {
            int b = cap_button_of(wp);
            if (b != p.capHot) { p.capHot = b; invalidate_cap_buttons(p); }
            TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE | TME_NONCLIENT, p.wnd, 0 };
            TrackMouseEvent(&te);
            if (p.hotTab >= 0 || p.hotPlus) set_hot(p, -1, -1, false);     // (off the tabs, onto the row's empty part)
            if (b < 0) return false;
            *result = 0;
            return true;
        }
        case WM_NCMOUSELEAVE:
        case WM_MOUSEMOVE:                          // (into the client area: a press there is let go)
            if (p.capHot >= 0 || p.capPressed >= 0) { p.capHot = p.capPressed = -1; invalidate_cap_buttons(p); }
            return false;
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK: {
            int b = cap_button_of(wp);
            if (b < 0) return false;
            p.capPressed = b;
            invalidate_cap_buttons(p);
            *result = 0;
            return true;
        }
        case WM_NCLBUTTONUP: {
            int b = cap_button_of(wp);
            bool fire = b >= 0 && b == p.capPressed;
            if (p.capPressed >= 0) { p.capPressed = -1; invalidate_cap_buttons(p); }
            if (b < 0) return false;
            if (fire) {
                p.capHot = -1;
                PostMessageW(p.wnd, WM_SYSCOMMAND, b == 0 ? SC_MINIMIZE : b == 2 ? SC_CLOSE : IsZoomed(p.wnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
            }
            *result = 0;
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK player_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Player* pp = s_p;
    if (msg == WM_CREATE && pp) { on_create(*pp, hwnd); return 0; }
    if (!pp || pp->wnd != hwnd || !pp->mp) return DefWindowProcW(hwnd, msg, wp, lp);
    Player& p = *pp;
    if (msg == s_taskbarCreated && s_taskbarCreated) { taskbar_created(p); return 0; }
    if (LRESULT r = 0; frame_message(p, msg, wp, lp, &r)) return r;
    switch (msg) {
        case WM_MEDIA_NOTE: on_note(p, lp); return 0;
        case WM_MEDIA_INFO: on_media_info(p, reinterpret_cast<MediaInfoNote*>(lp)); return 0;
        case WM_IMAGE: on_image(p, reinterpret_cast<ImageNote*>(lp)); return 0;
        case WM_LOUDNESS: on_loudness(p, reinterpret_cast<LoudnessNote*>(lp)); return 0;
        case WM_VIS_FAILED:                         // (the spectrogram instead: drawn again, not kept up meanwhile)
            spectrum_redraw(p);
            refresh_all(p);
            return 0;
        case WM_COMMAND:
            // (a tab's name edited: the box left takes it)
            if (lp && (HWND)lp == p.rename && HIWORD(wp) == EN_KILLFOCUS) { PostMessageW(hwnd, WM_RENAME_DONE, 1, lp); return 0; }
            // (menus and accelerators; the taskbar thumbnail's buttons)
            if ((HIWORD(wp) == 0 || HIWORD(wp) == THBN_CLICKED) && LOWORD(wp) >= CMD_PLAYPAUSE) command(p, LOWORD(wp));
            return 0;
        case WM_APPCOMMAND:
            if (media_command(p, GET_APPCOMMAND_LPARAM(lp))) return TRUE;
            break;
        case WM_RENAME_DONE:
            if (p.rename && (HWND)lp == p.rename) end_rename(p, wp != 0);
            return 0;
        case WM_CTLCOLOREDIT:
            if ((HWND)lp != p.rename) break;
            SetTextColor((HDC)wp, pal().text);
            SetBkColor((HDC)wp, pal().content);
            return (LRESULT)p.editBrush;
        case WM_TIMER:
            if (wp == TIMER_POSITION) { slide_tick(p); refresh_bar(p); }
            else if (wp == TIMER_SPECTRUM) spectrum_tick(p);
            else if (wp == TIMER_LEVEL) level_start(p);
            else if (wp == TIMER_SUBS) subs_tick(p);
            else if (wp == TIMER_ANIM) anim_tick(p);
            else if (wp == TIMER_SKIP) { KillTimer(hwnd, TIMER_SKIP); next(p); }
            else if (wp == TIMER_KEEP) keep_now(p);
            else if (wp == TIMER_DRAG_TAB || wp == TIMER_DRAG_SCROLL) row_drag_timer(p, wp);
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
            if (hdr->hwndFrom == p.headTip && hdr->code == TTN_GETDISPINFOW) {
                // "+", a tab: its name, how many entries, whether it plays
                static std::wstring text;
                size_t i = hdr->idFrom - kTipTab;
                if (hdr->idFrom == kTipPlus) text = L"New queue (Ctrl+N)";
                else if (hdr->idFrom >= kTipTab && i < p.tabs.size()) {
                    const QueueTab& t = *p.tabs[i];
                    text = t.name + L" \x00B7 " + std::to_wstring(t.q.size()) + (t.q.size() == 1 ? L" item" : L" items");
                    if (&t == p.playing && play_mark(p)) text += play_mark(p) == 2 ? L" \x00B7 playing" : L" \x00B7 paused";
                } else text.clear();
                ((NMTTDISPINFOW*)lp)->lpszText = text.data();
                return 0;
            }
            break;
        }
        case WM_CONTEXTMENU:
            if ((HWND)wp == hwnd && lp != -1) {        // (the tab row: a tab's menu)
                POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                ScreenToClient(hwnd, &pt);
                if (pt.y < p.rowH) {
                    int tab = tab_at(p, pt);
                    if (tab >= 0) tab_menu(p, tab, POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
                    return 0;
                }
            }
            if ((HWND)wp == p.list) { queue_menu(p, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }); return 0; }
            if ((HWND)wp == hwnd || (HWND)wp == p.video) { view_menu(p, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }); return 0; }
            break;
        case WM_MOUSEWHEEL:
            if (image_wheel(p, wp)) return 0;
            break;
        case WM_COPYDATA: {
            // another Explorer Plus process hands over its files
            auto* cd = (COPYDATASTRUCT*)lp;
            if (!cd || (cd->dwData != kCopyPlay && cd->dwData != kCopyEnqueue && cd->dwData != kCopyView &&
                        cd->dwData != kCopyOpen) ||
                cd->cbData % sizeof(wchar_t) || cd->cbData > (64u << 20))
                return FALSE;
            auto* job = new OpenJob;
            job->enqueue = cd->dwData == kCopyEnqueue;
            if (cd->dwData == kCopyOpen) {          // (several files opened at once: queued after the first)
                ULONGLONG now = GetTickCount64();
                job->enqueue = s_lastOpen && now - s_lastOpen < kOpenTogetherMs;
                s_lastOpen = now;
            }
            bool view = cd->dwData == kCopyView;    // (the first path: the picture to begin with)
            // (one path at a time, up to the cap: a huge payload isn't all materialised)
            const wchar_t* c = (const wchar_t*)cd->lpData;
            const wchar_t* end = c + cd->cbData / sizeof(wchar_t);
            while (c < end && job->paths.size() < kMaxFiles) {
                const wchar_t* z = std::find(c, end, L'\0');
                if (z - c > 0 && z - c < 32768) {
                    std::wstring s(c, z);
                    size_t tab = s.find(L'\t');
                    if (tab != std::wstring::npos) {
                        list_title_put(s.substr(0, tab), s.substr(tab + 1));
                        s.resize(tab);
                    }
                    if ((media_url(s) || !PathIsRelativeW(s.c_str())) && player_file(s)) {
                        if (view && job->start.empty()) job->start = std::move(s);
                        else job->paths.push_back(std::move(s));
                    }
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
        case WM_SIZE: layout(p); subs_tick(p); return 0;
        case WM_MOVE: subs_tick(p); return 0;      // (the subtitles' window goes along)
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
                if (c.y >= p.rowH && c.x >= p.splitX && c.x < p.splitX + px(4)) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
            }
            break;
        case WM_MOUSEMOVE: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (p.rowDrag) { row_drag_move(p, pt); return 0; }
            if (p.splitting) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                p.queueW96 = std::clamp(MulDiv(rc.right - pt.x, 96, p.dpi), 160, 900);
                layout(p);
                return 0;
            }
            if (tabs_mouse_move(p, pt)) return 0;
            bool hot = PtInRect(&p.clearBtn, pt) != FALSE;
            if (hot != p.clearHot) { p.clearHot = hot; InvalidateRect(hwnd, &p.clearBtn, FALSE); }
            TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&te);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (p.clearHot) { p.clearHot = false; InvalidateRect(hwnd, &p.clearBtn, FALSE); }
            if (p.dragTab < 0) set_hot(p, -1, -1, false);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (tabs_mouse_down(p, pt, msg == WM_LBUTTONDBLCLK)) return 0;
            if (PtInRect(&p.clearBtn, pt) && !vq(p).empty()) { command(p, CMD_CLEAR); return 0; }
            if (p.queueShown && !p.full && pt.x >= p.splitX && pt.x < p.splitX + px(4)) {
                p.splitting = true;
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (p.rowDrag) { row_drag_end(p, true); return 0; }
            if (p.splitting) {
                p.splitting = false;
                ReleaseCapture();
                settings_set_pref(L"playerQueueWidth", p.queueW96);
            }
            if (p.dragTab >= 0) {
                p.dragTab = -1;
                p.dragging = false;
                ReleaseCapture();
            }
            return 0;
        case WM_MBUTTONUP: {                        // (a tab: closed)
            int tab = tab_at(p, POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (tab >= 0) close_tab(p, tab);
            return 0;
        }
        case WM_CAPTURECHANGED:
            row_drag_end(p, false);
            p.splitting = false;
            p.dragTab = -1;
            p.dragging = false;
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
            // (the thumbnail's glyphs follow the taskbar's light or dark)
            if (msg == WM_SETTINGCHANGE && p.taskbar) { p.tbGlyph = 0; thumb_buttons(p, false); }
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
        case WM_QUERYENDSESSION:
            keep_now(p);
            return TRUE;
        case WM_ENDSESSION:
            if (wp) { save_place(p); keep_now(p); settings_save_all(); }
            return 0;
        case WM_CLOSE:
            // (sent while the playback closes, which answers sent messages: once that's done)
            if (p.mp->closing()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            else DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            on_destroy(p);
            s_p = nullptr;
            delete pp;
            for (int attempt = 0; attempt < 3 && !settings_save_all(); ++attempt) Sleep(200);
            app_window_closed();               // (the main window gone before: the process ends)
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
    restore_queues(p);                      // (shuffle and repeat as the settings have them)
    p.queueShown = settings_pref(L"playerQueue", 1) != 0;
    p.queueW96 = std::clamp(settings_pref(L"playerQueueWidth", 300), 160, 900);
    p.level = settings_pref(L"playerLevel", 1) != 0;
    p.slideSecs = std::clamp(settings_pref(L"playerSlideSeconds", 5), 1, 60);
    p.visStyle = std::clamp(settings_pref(L"playerVisual", VIS_SPECTROGRAM), 0, VIS_COUNT - 1);
    p.visCycle = settings_pref(L"playerVisualCycle", 0) != 0;
    p.subsOn = settings_pref(L"playerSubtitles", 1) != 0;
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
    // (the tab row in the title bar's place)
    SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    set_app_id(w, true);                    // (before it's shown: the button is made then)
    show_tab(p, p.shown, false);            // (its list where it was left)
    show_current(p);
    ShowWindow(w, maximized ? SW_SHOWMAXIMIZED : activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    if (activate) SetForegroundWindow(w);
    return true;
}

} // namespace

// ------------------------------------------------------------------
// public
// ------------------------------------------------------------------
PlayerOffer player_offer(const std::vector<MediaItem>& items) {
    int folders = 0;
    bool pictures = false;
    for (auto& it : items) {
        if (!it.folder) {
            if (media_file_playable(it.path) || playlist_file(it.path)) return PlayerOffer::Play;
            if (image_file(it.path)) pictures = true;
            continue;
        }
        if (++folders > 4) continue;           // (a few folders looked into, no more)
        if (folder_has_media(it.path)) return PlayerOffer::Play;
    }
    return pictures ? PlayerOffer::View : PlayerOffer::None;
}

void player_open(const std::vector<MediaItem>& items, bool enqueue) {
    if (!enqueue && items.size() == 1 && !items[0].folder && image_file(items[0].path)) {
        open_pictures(items[0].path);
        return;
    }
    // (folders and playlists: what's in them, read on a thread)
    bool folders = std::any_of(items.begin(), items.end(), [](const MediaItem& it) { return it.folder || playlist_file(it.path); });
    open_items(items, enqueue, folders);
}

void player_save_playlist(HWND owner, const std::vector<MediaItem>& items) {
    if (items.empty()) return;
    // (in the folder shown; named after the one folder chosen, else after the folder shown)
    std::wstring dir = parent_dir(items[0].path), name;
    if (items.size() == 1 && items[0].folder) name = PathFindFileNameW(items[0].path.c_str());
    else if (!PathIsRootW((dir + L"\\").c_str())) name = PathFindFileNameW(dir.c_str());
    if (name.empty()) name = L"Playlist";
    std::wstring file = ask_playlist_file(owner, dir, name);
    if (file.empty()) return;
    HWND target = msg_window();
    // (the files listed, and their titles read, on a thread: a big folder takes a while)
    std::thread([items, file, target] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<PlaylistEntry> entries;
        for (auto& path : expand(items)) {
            if (image_file(path) || lstrcmpiW(path.c_str(), file.c_str()) == 0) continue;
            PlaylistEntry e{ path };
            if (entries.size() < 2000) read_tags(path, e.title, e.duration);     // (titles where it doesn't take long)
            entries.push_back(std::move(e));
        }
        if (SUCCEEDED(co)) CoUninitialize();
        if (!entries.empty() && playlist_save(file, entries)) return;
        auto* what = new std::wstring(file);
        if (!PostMessageW(target, WM_PLAYLIST_FAILED, 0, (LPARAM)what)) delete what;
    }).detach();
}

bool player_open_file(const std::wstring& path) {
    // Explorer opens each of several files selected in a process of its own, all at once:
    // one at a time here, so the first one's player is there for the others to hand over to
    std::wstring lockName = L"Local\\" + player_class() + L".open";
    HANDLE mx = CreateMutexW(nullptr, FALSE, lockName.c_str());
    if (mx) WaitForSingleObject(mx, 10000);
    std::vector<std::wstring> paths;
    std::wstring start;
    if (image_file(path)) {
        paths = folder_pictures(path);         // (its folder's pictures, from it)
        start = path;
    } else {
        paths = expand({ { path, false } });
    }
    bool here = false;
    if (paths.empty()) {
        std::wstring why = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES
                         ? L"Explorer Plus can't find\n" : playlist_file(path) ? L"There is nothing to play in\n"
                         : L"Explorer Plus can't play\n";
        MessageBoxW(nullptr, (why + path).c_str(), L"Explorer Plus", MB_OK | MB_ICONINFORMATION);
    } else if (!hand_over(paths, false, start, true) && create_player(true)) {
        s_lastOpen = GetTickCount64();
        add(*s_p, paths, false, start);
        here = true;
    }
    if (mx) {
        ReleaseMutex(mx);
        CloseHandle(mx);
    }
    return here;
}

void player_show() {
    HWND w = s_p ? s_p->wnd : FindWindowW(player_class().c_str(), nullptr);
    if (!w) {
        create_player(true);
        return;
    }
    if (IsIconic(w)) ShowWindowAsync(w, SW_RESTORE);
    SetForegroundWindow(w);                 // (this process is the foreground one: it may hand that on)
}

HWND player_window() { return s_p ? s_p->wnd : nullptr; }

bool player_owns(HWND h) {
    return s_p && h && (h == s_p->wnd || GetAncestor(h, GA_ROOT) == s_p->wnd);
}

bool player_translate(MSG* m) {
    if (!s_p || !s_p->mp || (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN)) return false;
    Player& p = *s_p;
    if (p.rowDrag) {                            // (rows dragged: Esc leaves them where they were; no other keys meanwhile)
        if (m->wParam == VK_ESCAPE) row_drag_end(p, false);
        return true;
    }
    if (p.rename && GetFocus() == p.rename) return false;     // (a tab's name edited: the box's keys)
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
            if (image_current(p)) { command(p, vk == VK_LEFT ? CMD_PREV : CMD_NEXT); return true; }     // (a picture: the one before / after)
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
        case 'V':
            if (ctrl || inList) return false;       // (the queue: type-ahead)
            if (!held) command(p, shift ? CMD_VIS_PREV : CMD_VIS_NEXT);
            return true;
        case 'H': case 'T':
            if (!ctrl) return false;
            if (!held) command(p, vk == 'H' ? CMD_SHUFFLE : CMD_REPEAT);
            return true;
        case 'A':
            if (ctrl || inList) return false;
            if (!held) command(p, CMD_AUDIO_NEXT);
            return true;
        case 'S': case 'E':
            if (vk == 'S' && !ctrl && !inList) { if (!held) command(p, CMD_SUBS_NEXT); return true; }
            if (!ctrl || shift) return false;
            if (!held) command(p, vk == 'S' ? CMD_SAVE_PLAYLIST : CMD_EDIT_PICTURE);
            return true;
        case VK_PRIOR: case VK_NEXT:                // the tab before / after; a TIFF's pages
            if (ctrl && !shift) { command(p, vk == VK_NEXT ? CMD_TAB_NEXT : CMD_TAB_PREV); return true; }
            if (inList || ctrl || !image_current(p) || p.img.pages < 2) return false;
            command(p, vk == VK_NEXT ? CMD_PAGE_NEXT : CMD_PAGE_PREV);
            return true;
        case VK_ADD: case VK_OEM_PLUS: case VK_SUBTRACT: case VK_OEM_MINUS:     // a picture's zoom
        case '0': case VK_NUMPAD0: case '1': case VK_NUMPAD1:
            if (inList || !image_current(p)) return false;
            command(p, vk == VK_ADD || vk == VK_OEM_PLUS ? CMD_ZOOM_IN : vk == VK_SUBTRACT || vk == VK_OEM_MINUS ? CMD_ZOOM_OUT
                       : vk == '0' || vk == VK_NUMPAD0 ? CMD_ZOOM_FIT : CMD_ZOOM_ACTUAL);
            return true;
        case 'N': case 'W':                         // a new queue, the one shown closed
            if (!ctrl || shift) return false;
            if (!held) command(p, vk == 'N' ? CMD_TAB_NEW : CMD_TAB_CLOSE);
            return true;
        case VK_F2:
            if (ctrl || shift) return false;
            command(p, CMD_TAB_RENAME);
            return true;
        case VK_F11:
            if (!held) command(p, CMD_FULL);
            return true;
        case VK_ESCAPE:
            if (!p.full) return false;
            if (!held) command(p, CMD_FULL);
            return true;
        case VK_TAB: case VK_F6:
            if (ctrl && vk == VK_TAB) { command(p, shift ? CMD_TAB_PREV : CMD_TAB_NEXT); return true; }
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
