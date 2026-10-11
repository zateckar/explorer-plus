#include "detailspane.h"
#include "theme.h"
#include "enumerate.h"
#include "navigation.h"
#include "tabs.h"
#include "layout.h"
#include "preview.h"
#include "workers.h"
#include <wincodec.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cwctype>
#include <cstdlib>
#include <algorithm>
using std::min;
using std::max;

// The right-hand pane, in one of Win11's two modes:
//   Details: thumbnail, name and properties of the selection (or of the folder)
//   Preview: the file itself through its shell preview handler; files without one
//            (images, video, ...) show a large thumbnail

static bool s_visible = true;
static PaneMode s_mode = PANE_DETAILS;
static HWND s_pane = nullptr;
static HWND s_hostWnd = nullptr;          // preview handlers draw into this child
static HBITMAP s_preview = nullptr;    // owned thumbnail
static int   s_prevW = 0, s_prevH = 0;
static bool  s_prevAlpha = false;      // s_preview has transparency (icons, PNGs)
static std::atomic<unsigned> s_gen{ 0 };   // async thumbnail generation id
static std::wstring s_message;         // preview mode: "Select a file to preview."
static const UINT_PTR kTimerPreview = 1;   // the selection settled: load its preview

// lines of "label: value"
static std::vector<std::pair<std::wstring, std::wstring>> s_lines;
static std::wstring s_title;

// ------------------------------------------------------------------
// worker: big thumbnail (+ image dimensions) for the selected item, free space of the
// folder's drive. One thread; a request replaces the one still waiting, so arrowing
// through files never queues up work (and a slow drive never blocks the UI).
struct PrevReq {
    unsigned gen;
    PIDLIST_ABSOLUTE abs = nullptr;   // owned; nullptr: no thumbnail
    int size = 220;
};

struct PrevResult {
    unsigned gen;
    HBITMAP hbm = nullptr;         // owned
    int w = 0, h = 0;
    bool alpha = false;            // hbm is 32bpp with a real (premultiplied) alpha channel
    std::wstring dims;             // "1920 × 1080" for images
};

static std::mutex s_reqMx;
static std::condition_variable s_reqCv;
static PrevReq* s_reqPending = nullptr;
static bool s_reqThread = false;

static void clear_preview() {
    if (s_preview) { DeleteObject(s_preview); s_preview = nullptr; }
    s_prevW = s_prevH = 0;
    s_prevAlpha = false;
}

static void apply_preview(void* payload) {
    PrevResult* r = static_cast<PrevResult*>(payload);
    if (r->gen == s_gen.load()) {
        clear_preview();
        s_preview = r->hbm;
        s_prevW = r->w; s_prevH = r->h;
        r->hbm = nullptr;
        s_prevAlpha = r->alpha;
        if (s_mode == PANE_DETAILS && !r->dims.empty()) s_lines.push_back({ L"Dimensions", r->dims });
        if (s_mode == PANE_PREVIEW && !s_preview) s_message = L"No preview available.";
        if (s_pane) InvalidateRect(s_pane, nullptr, TRUE);
    }
    if (r->hbm) DeleteObject(r->hbm);
    delete r;
}

// does a 32bpp bitmap carry alpha? (photos often come back with an all-zero channel)
static bool has_alpha(HBITMAP hbm) {
    DIBSECTION ds{};
    if (GetObjectW(hbm, sizeof(ds), &ds) != sizeof(ds) || ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits) return false;
    const DWORD* p = static_cast<const DWORD*>(ds.dsBm.bmBits);
    size_t n = (size_t)ds.dsBm.bmWidth * (size_t)std::abs(ds.dsBm.bmHeight);
    for (size_t i = 0; i < n; ++i) if (p[i] & 0xFF000000) return true;
    return false;
}

static void prev_work(PrevReq* r) {
    PrevResult* res = new PrevResult{ r->gen };
    com_ptr<IShellItem2> si;
    if (r->abs && SUCCEEDED(SHCreateItemFromIDList(r->abs, IID_PPV_ARGS(si.put()))) && si) {
        com_ptr<IShellItemImageFactory> f;
        if (SUCCEEDED(si->QueryInterface(IID_PPV_ARGS(f.put()))) && f) {
            SIZE sz{ r->size, r->size };
            if (FAILED(f->GetImage(sz, SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT, &res->hbm)))
                res->hbm = nullptr;
            if (res->hbm) {
                BITMAP bi{};
                GetObjectW(res->hbm, sizeof(bi), &bi);
                res->w = bi.bmWidth; res->h = bi.bmHeight;
                res->alpha = has_alpha(res->hbm);
            }
        }
        // image dimensions from the property system (no full decode)
        ULONG w = 0, h = 0;
        if (SUCCEEDED(si->GetUInt32(PKEY_Image_HorizontalSize, &w)) &&
            SUCCEEDED(si->GetUInt32(PKEY_Image_VerticalSize, &h)) && w && h) {
            WCHAR d[64]{};
            swprintf(d, 64, L"%u \x00D7 %u", w, h);
            res->dims = d;
        }
    }
    post_async(apply_preview, res);
}

// free space of a drive, for the folder's details
struct SpaceResult {
    unsigned gen;
    std::wstring space;            // "12.3 GB free of 476 GB"
};

static void apply_space(void* payload) {
    std::unique_ptr<SpaceResult> r(static_cast<SpaceResult*>(payload));
    if (r->gen != s_gen.load() || s_mode != PANE_DETAILS || r->space.empty()) return;
    s_lines.push_back({ L"Space", r->space });
    if (s_pane) InvalidateRect(s_pane, nullptr, TRUE);
}

static void space_work(unsigned gen, const std::wstring& root) {
    if (gen != s_gen.load()) return;            // the pane moved on while this waited
    auto* res = new SpaceResult{ gen, {} };
    ULARGE_INTEGER avail{}, total{}, totalfree{};
    if (GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &totalfree)) {
        WCHAR a[64]{}, b[64]{};
        StrFormatByteSizeW(totalfree.QuadPart, a, 64);
        StrFormatByteSizeW(total.QuadPart, b, 64);
        res->space = std::wstring(a) + L" free of " + b;
    }
    post_async(apply_space, res);
}

static void free_req(PrevReq* r) {
    if (!r) return;
    if (r->abs) CoTaskMemFree(r->abs);
    delete r;
}

static void prev_thread() {
    ComInit ci;
    for (;;) {
        PrevReq* r = nullptr;
        {
            std::unique_lock<std::mutex> lk(s_reqMx);
            s_reqCv.wait(lk, [] { return s_reqPending != nullptr; });
            r = s_reqPending;
            s_reqPending = nullptr;
        }
        if (r->gen == s_gen.load()) prev_work(r);    // (else the selection moved on meanwhile)
        free_req(r);
    }
}

static void submit(PrevReq* r) {
    {
        std::lock_guard<std::mutex> lk(s_reqMx);
        free_req(s_reqPending);        // superseded before it was started
        s_reqPending = r;
        if (!s_reqThread) { s_reqThread = true; std::thread(prev_thread).detach(); }
    }
    s_reqCv.notify_one();
}

static void request_thumbnail(PIDLIST_ABSOLUTE abs /*adopted*/, int size) {
    PrevReq* r = new PrevReq;
    r->gen = s_gen.load();
    r->abs = abs;
    r->size = size;
    submit(r);
}

// ------------------------------------------------------------------
// data assembly

static void add_line(PCWSTR k, const std::wstring& v) {
    if (!v.empty()) s_lines.push_back({ k, v });
}

// the selected items (UI thread: the only writer of g_items, so the pointers stay valid)
static std::vector<Item*> selected_items(int* count) {
    std::vector<Item*> sel;
    std::lock_guard<std::mutex> lk(g_itemsMx);
    if (count) *count = (int)g_items.size();
    int idx = -1;
    while ((idx = ListView_GetNextItem(g_list, idx, LVNI_SELECTED)) != -1)
        if (idx < (int)g_items.size()) sel.push_back(g_items[idx]);
    return sel;
}

static void gather_for_selection() {
    s_lines.clear(); s_title.clear();
    ++s_gen;                 // invalidate any preview still being extracted
    clear_preview();

    int count = 0;
    std::vector<Item*> sel = selected_items(&count);
    if (sel.empty()) {
        // folder info
        s_title = tabmodel_active_name();
        std::wstring path = pidl_folder_path(g_current);
        if (path.rfind(L"::", 0) != 0) add_line(L"Path", path);   // virtual folders have no path
        add_line(L"Items", std::to_wstring(count));
        // free space of the folder's drive (on the pool, not the thumbnail thread: a
        // mapped drive can be offline, and the call then waits for the server)
        if (path.size() >= 3 && iswalpha(path[0]) && path[1] == L':' && path[2] == L'\\') {
            unsigned gen = s_gen.load();
            std::wstring root = path.substr(0, 3);
            worker_submit([gen, root] { space_work(gen, root); });
        }
        return;
    }
    if (sel.size() > 1) {
        s_title = std::to_wstring(sel.size()) + L" items selected";
        ULONGLONG bytes = 0; bool anyFile = false;
        for (auto* it : sel) if (!it->isFolder) { bytes += it->bytes; anyFile = true; }
        if (anyFile) { WCHAR sb[64]{}; StrFormatByteSizeW(bytes, sb, 64); add_line(L"Total size", sb); }
        return;
    }
    Item* it = sel[0];
    s_title = it->name;
    add_line(L"Type", it->type);
    if (!it->size.empty()) add_line(L"Size", it->size);      // (a folder's once worked out)
    add_line(L"Date modified", it->date);
    // queue preview thumbnail + image dimensions (as large as the pane draws it)
    if (PIDLIST_ABSOLUTE full = ILCombine(g_current, it->pidl))
        request_thumbnail(full, S(260));
}

// Preview mode: the one selected file through its preview handler, else its thumbnail
static void load_preview() {
    ++s_gen;
    clear_preview();
    s_message.clear();
    preview_close();
    std::vector<Item*> sel = selected_items(nullptr);
    PIDLIST_ABSOLUTE full = sel.size() == 1 && !sel[0]->isFolder
                          ? ILCombine(g_current, sel[0]->pidl) : nullptr;
    if (!full) {
        s_message = L"Select a file to preview.";
    } else if (preview_open(s_hostWnd, full)) {
        CoTaskMemFree(full);
    } else {
        RECT rc{};
        GetClientRect(s_pane, &rc);
        request_thumbnail(full, std::clamp((int)max(rc.right, rc.bottom), 96, 1024));
    }
    ShowWindow(s_hostWnd, preview_active() ? SW_SHOW : SW_HIDE);
    InvalidateRect(s_pane, nullptr, TRUE);
}

// ------------------------------------------------------------------
// paint

// the thumbnail scaled down to fit the box, centered horizontally (and vertically: vcenter)
static void blit_fit(HDC dc, int x, int y, int maxW, int maxH, bool vcenter) {
    int w = s_prevW, h = s_prevH;
    if (!s_preview || w <= 0 || h <= 0 || maxW <= 0 || maxH <= 0) return;
    double sc = min(1.0, min((double)maxW / w, (double)maxH / h));
    int dw = max(1, (int)(w * sc)), dh = max(1, (int)(h * sc));
    HDC bm = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(bm, s_preview);
    int dx = x + (maxW - dw) / 2, dy = y + (vcenter ? (maxH - dh) / 2 : 0);
    if (s_prevAlpha) {              // transparent parts show the pane, not black
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(dc, dx, dy, dw, dh, bm, 0, 0, w, h, bf);
    } else {
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, dx, dy, dw, dh, bm, 0, 0, w, h, SRCCOPY);
    }
    SelectObject(bm, old);
    DeleteDC(bm);
}

static void paint_details(HDC mem, const RECT& rc) {
    const Palette& p = pal();
    int pad = S(16);
    int y = S(16);
    int cx = rc.right - 2 * pad;
    // thumbnail, centered
    if (s_preview && s_prevW > 0 && s_prevH > 0) {
        int pmax = max(1, min((int)S(260), cx));
        double sc = min(1.0, min((double)pmax / s_prevW, (double)S(200) / s_prevH));
        blit_fit(mem, 0, y, rc.right, max(1, (int)(s_prevH * sc)), false);
        y += max(1, (int)(s_prevH * sc)) + S(16);
    }
    // title
    if (!s_title.empty()) {
        SelectObject(mem, content_font_bold());
        SetTextColor(mem, p.text);
        RECT tr{ pad, y, pad + cx, y + SC(60) };
        int th = DrawTextW(mem, s_title.c_str(), -1, &tr,
                           DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
        y += th + S(16);
    }
    SelectObject(mem, content_font());
    // properties: name in secondary text, value aside
    int keyW = min(SC(110), cx / 2);
    for (auto& [k, v] : s_lines) {
        RECT kr{ pad, y, pad + keyW, y + SC(22) };
        SetTextColor(mem, p.textSecondary);
        DrawTextW(mem, k.c_str(), -1, &kr, DT_LEFT | DT_END_ELLIPSIS | DT_SINGLELINE | DT_NOPREFIX);
        RECT vr{ pad + keyW + S(8), y, pad + cx, y + SC(22) };
        SetTextColor(mem, p.text);
        DrawTextW(mem, v.c_str(), -1, &vr, DT_LEFT | DT_END_ELLIPSIS | DT_SINGLELINE | DT_NOPREFIX);
        y += SC(24);
    }
}

static void paint_preview(HDC mem, const RECT& rc) {
    int pad = S(16);
    if (s_preview) {
        blit_fit(mem, pad, pad, rc.right - 2 * pad, rc.bottom - 2 * pad, true);
        return;
    }
    if (s_message.empty()) return;
    SelectObject(mem, content_font());
    SetTextColor(mem, pal().textSecondary);
    RECT t{ pad, 0, rc.right - pad, rc.bottom };
    DrawTextW(mem, s_message.c_str(), -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static LRESULT CALLBACK pane_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            BufferedPaint bp(hwnd);
            SetBkMode(bp.dc, TRANSPARENT);
            fill_rect(bp.dc, bp.rc, pal().content);
            if (s_mode == PANE_PREVIEW) paint_preview(bp.dc, bp.rc);
            else paint_details(bp.dc, bp.rc);
            return 0;
        }
        case WM_SIZE:
            if (s_hostWnd) {
                int pad = S(4);
                MoveWindow(s_hostWnd, pad, pad, max(0, LOWORD(lp) - 2 * pad), max(0, HIWORD(lp) - 2 * pad), TRUE);
                preview_resize();
            }
            if (s_mode == PANE_PREVIEW) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (wp == kTimerPreview) {
                KillTimer(hwnd, kTimerPreview);
                if (s_visible && s_mode == PANE_PREVIEW) load_preview();
                return 0;
            }
            break;
        case WM_DESTROY:
            preview_close();
            if (s_preview) { DeleteObject(s_preview); s_preview = nullptr; }
            s_pane = s_hostWnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc; GetClientRect(hwnd, &rc);
        fill_rect((HDC)wp, rc, pal().content);
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND detailspane_create(HWND parent) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = g_hinst;
        wc.lpfnWndProc = pane_proc;
        wc.lpszClassName = L"ExplorerPlusDetailsPane";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        wc.lpfnWndProc = host_proc;
        wc.lpszClassName = L"ExplorerPlusPreviewHost";
        RegisterClassExW(&wc);
        reg = true;
    }
    s_pane = CreateWindowExW(0, L"ExplorerPlusDetailsPane", L"",
                             WS_CHILD | WS_CLIPCHILDREN | (s_visible ? WS_VISIBLE : 0),
                             0, 0, 240, 100, parent, (HMENU)IDC_DETAILSPANE, g_hinst, nullptr);
    s_hostWnd = CreateWindowExW(0, L"ExplorerPlusPreviewHost", L"", WS_CHILD | WS_CLIPCHILDREN,
                             0, 0, 100, 100, s_pane, nullptr, g_hinst, nullptr);
    fix_font(s_pane);
    return s_pane;
}

void detailspane_update() {
    if (!s_pane || !s_visible) return;    // nothing to show: skip the work
    if (s_mode == PANE_PREVIEW) {
        // wait for the selection to settle (arrowing through files would load each one)
        SetTimer(s_pane, kTimerPreview, 150, nullptr);
        return;
    }
    gather_for_selection();
    InvalidateRect(s_pane, nullptr, TRUE);
}

static void reset_content() {
    ++s_gen;
    clear_preview();
    preview_close();
    if (s_hostWnd) ShowWindow(s_hostWnd, SW_HIDE);
    s_lines.clear();
    s_title.clear();
    s_message.clear();
}

void detailspane_set_visible(bool v) {
    s_visible = v;
    if (!v) reset_content();
    if (s_pane) ShowWindow(s_pane, v ? SW_SHOW : SW_HIDE);
    if (!g_main) return;
    RECT rc{}; GetClientRect(g_main, &rc);
    layout_resize(g_main, rc.right, rc.bottom);
    if (s_mode == PANE_PREVIEW && s_pane) { KillTimer(s_pane, kTimerPreview); if (v) load_preview(); }
    else detailspane_update();
}

void detailspane_set_mode(PaneMode m) {
    if (m == s_mode) return;
    reset_content();
    s_mode = m;
    if (s_pane) InvalidateRect(s_pane, nullptr, TRUE);
}

void detailspane_toggle(PaneMode m) {
    if (s_visible && s_mode == m) { detailspane_set_visible(false); return; }
    detailspane_set_mode(m);
    detailspane_set_visible(true);
}

// before a file operation: a preview handler can hold the previewed file open, so
// delete / rename / move would fail with "the file is open in Explorer Plus". The
// preview comes back with the next selection change (the operation refreshes the list).
void detailspane_release_files() {
    if (!preview_active()) return;
    preview_close();
    if (s_hostWnd) ShowWindow(s_hostWnd, SW_HIDE);
    if (s_pane) InvalidateRect(s_pane, nullptr, TRUE);
}

void detailspane_theme_changed() {
    preview_colors_changed();
    if (s_pane) InvalidateRect(s_pane, nullptr, TRUE);
    if (s_hostWnd) InvalidateRect(s_hostWnd, nullptr, TRUE);
}

bool detailspane_visible() { return s_visible; }
PaneMode detailspane_mode() { return s_mode; }
HWND detailspane_hwnd() { return s_pane; }
