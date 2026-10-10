#include "thumbs.h"
#include <thread>
#include <condition_variable>

// Thumbnails for the Medium (48px), Large (96px) and Extra large (256px) icon views,
// extracted by a small pool of worker threads. Requests are served newest-first so
// the rows being looked at win.

static HIMAGELIST s_lists[4] = {};       // index by View::thumbsExtra (1..3)
static int thumb_px(int extra) { return extra == 2 ? 256 : extra == 3 ? 48 : 96; }
static std::atomic<UINT> s_opId{ 1 };

struct ThumbReq {
    UINT opId = 0;
    ULONGLONG sig = 0;
    int px = 96;
    int extra = 1;
    PITEMID_CHILD abs = nullptr;   // owned
};
struct ThumbResult {
    UINT opId;
    ULONGLONG sig;
    int extra;
    HBITMAP hbm;                   // px*px 32bpp, owned
};

static std::mutex s_qMx;
static std::condition_variable s_qCv;
static std::deque<ThumbReq*> s_queue;
static bool s_poolStarted = false;

// Fit `src` into a px*px transparent 32bpp DIB (image lists need fixed-size images).
static HBITMAP square_bitmap(HBITMAP src, int px) {
    BITMAP bm{};
    if (!GetObjectW(src, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return nullptr;
    int w = std::min<int>(bm.bmWidth, px), h = std::min<int>(bm.bmHeight, px);
    std::vector<DWORD> pix((size_t)bm.bmWidth * bm.bmHeight);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;     // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    int got = GetDIBits(dc, src, 0, bm.bmHeight, pix.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (!got) return nullptr;
    // photos often come back with an all-zero alpha channel: treat as opaque
    bool anyAlpha = false;
    for (DWORD p : pix) if (p & 0xFF000000) { anyAlpha = true; break; }

    BITMAPINFO bo = bi;
    bo.bmiHeader.biWidth = px;
    bo.bmiHeader.biHeight = -px;
    void* bits = nullptr;
    HBITMAP out = CreateDIBSection(nullptr, &bo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!out) return nullptr;
    DWORD* dst = static_cast<DWORD*>(bits);
    memset(dst, 0, (size_t)px * px * 4);
    int ox = (px - w) / 2, oy = (px - h) / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            DWORD p = pix[(size_t)y * bm.bmWidth + x];
            dst[(size_t)(y + oy) * px + (x + ox)] = anyAlpha ? p : (p | 0xFF000000);
        }
    return out;
}

static void apply_result(void* payload);

static void pool_worker() {
    ComInit ci;
    for (;;) {
        ThumbReq* r = nullptr;
        {
            std::unique_lock<std::mutex> lk(s_qMx);
            s_qCv.wait(lk, [] { return !s_queue.empty(); });
            r = s_queue.back();            // newest first
            s_queue.pop_back();
        }
        HBITMAP hbm = nullptr;
        if (r->opId == s_opId.load()) {
            com_ptr<IShellItemImageFactory> f;
            if (SUCCEEDED(SHCreateItemFromIDList(r->abs, IID_PPV_ARGS(f.put()))) && f) {
                HBITMAP raw = nullptr;
                SIZE sz{ r->px, r->px };
                if (SUCCEEDED(f->GetImage(sz, SIIGBF_RESIZETOFIT, &raw)) && raw) {
                    hbm = square_bitmap(raw, r->px);
                    DeleteObject(raw);
                }
            }
        }
        CoTaskMemFree(r->abs);
        if (r->opId == s_opId.load())
            post_async(apply_result, new ThumbResult{ r->opId, r->sig, r->extra, hbm });
        else if (hbm)
            DeleteObject(hbm);
        delete r;
    }
}

static void enqueue(ThumbReq* r) {
    {
        std::lock_guard<std::mutex> lk(s_qMx);
        if (!s_poolStarted) {
            s_poolStarted = true;
            unsigned n = std::max(2u, std::min(4u, std::thread::hardware_concurrency()));
            for (unsigned i = 0; i < n; ++i) std::thread(pool_worker).detach();
        }
        s_queue.push_back(r);
    }
    s_qCv.notify_one();
}

static void drop_queued() {
    std::lock_guard<std::mutex> lk(s_qMx);
    for (ThumbReq* r : s_queue) { CoTaskMemFree(r->abs); delete r; }
    s_queue.clear();
}

static HIMAGELIST ensure_list(int extra) {
    if (extra < 1 || extra > 3) return nullptr;
    if (!s_lists[extra]) {
        int px = thumb_px(extra);
        s_lists[extra] = ImageList_Create(px, px, ILC_COLOR32, 32, 64);
    }
    return s_lists[extra];
}

void thumb_reset_for_new_folder() {
    ++s_opId;
    drop_queued();
    for (int e = 1; e <= 3; ++e)
        if (s_lists[e]) ImageList_SetImageCount(s_lists[e], 0);
    // items carry their own thumbReady/thumbIndex; reset them for the current listing
    std::lock_guard<std::mutex> lk(g_itemsMx);
    for (Item* it : g_items) { it->thumbReady.store(0); it->thumbIndex = -1; }
}

// Called with g_itemsMx held (from LVN_GETDISPINFO).
void thumb_get_or_request(Item* it, int, int* outImageIndex) {
    *outImageIndex = -1;
    int extra = g_view.thumbsExtra;
    if (!it || !extra) return;
    if (it->thumbReady.load() == 2) { *outImageIndex = it->thumbIndex; return; }
    int expected = 0;
    if (!it->thumbReady.compare_exchange_strong(expected, 1)) return;
    ThumbReq* r = new ThumbReq;
    r->opId = s_opId.load();
    r->sig = it->sig;
    r->px = thumb_px(extra);
    r->extra = extra;
    r->abs = static_cast<PITEMID_CHILD>(ILCombine(g_current, it->pidl));
    if (!r->abs) { delete r; it->thumbReady.store(0); return; }
    enqueue(r);
}

static void apply_result(void* payload) {
    ThumbResult* m = static_cast<ThumbResult*>(payload);
    if (m->opId != s_opId.load() || m->extra != g_view.thumbsExtra) {
        if (m->hbm) DeleteObject(m->hbm);
        delete m;
        return;
    }
    int imgIndex = -1;
    if (m->hbm) {
        imgIndex = ImageList_Add(ensure_list(m->extra), m->hbm, nullptr);
        DeleteObject(m->hbm);
    }
    // find the row by signature (the list may have been re-sorted meanwhile)
    int row = -1;
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        for (size_t i = 0; i < g_items.size(); ++i) {
            if (g_items[i]->sig != m->sig) continue;
            row = (int)i;
            if (imgIndex >= 0) { g_items[i]->thumbIndex = imgIndex; g_items[i]->thumbReady.store(2); }
            // failure: leave "queued" so we don't retry forever; the row stays imageless
            break;
        }
    }
    if (row >= 0) ListView_RedrawItems(g_list, row, row);
    delete m;
}

HIMAGELIST thumb_imagelist_for_current_view() {
    return ensure_list(g_view.thumbsExtra);
}
