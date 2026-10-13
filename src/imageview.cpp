#include "imageview.h"
#include <propvarutil.h>
#include <wincodec.h>
#include <algorithm>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>

namespace {

// ------------------------------------------------------------------
// which files: the extensions of the WIC decoders installed
// ------------------------------------------------------------------
std::once_flag s_extOnce;
std::vector<std::wstring> s_exts;           // ".jpg", ... lower case

void load_extensions() {
    // (any thread: COM as it is there, or for this call)
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    IEnumUnknown* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault, &en))) {
        IUnknown* u = nullptr;
        ULONG got = 0;
        while (en->Next(1, &u, &got) == S_OK && u) {
            IWICBitmapCodecInfo* info = nullptr;
            if (SUCCEEDED(u->QueryInterface(IID_PPV_ARGS(&info)))) {
                UINT len = 0;
                if (SUCCEEDED(info->GetFileExtensions(0, nullptr, &len)) && len > 1) {
                    std::wstring list(len, L'\0');
                    if (SUCCEEDED(info->GetFileExtensions(len, list.data(), &len))) {
                        list.resize(wcslen(list.c_str()));
                        size_t start = 0;
                        while (start < list.size()) {
                            size_t comma = list.find(L',', start);
                            std::wstring e = list.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
                            for (auto& c : e) c = (wchar_t)towlower(c);
                            if (e.size() > 1 && e[0] == L'.') s_exts.push_back(e);
                            if (comma == std::wstring::npos) break;
                            start = comma + 1;
                        }
                    }
                }
                info->Release();
            }
            u->Release();
            u = nullptr;
        }
    }
    if (en) en->Release();
    if (wic) wic->Release();
    if (SUCCEEDED(co)) CoUninitialize();
}

// ------------------------------------------------------------------
// decoding
// ------------------------------------------------------------------
// the EXIF orientation (1..8; 1: upright)
int orientation(IWICBitmapFrameDecode* frame) {
    IWICMetadataQueryReader* q = nullptr;
    if (FAILED(frame->GetMetadataQueryReader(&q)) || !q) return 1;
    int o = 1;
    for (const wchar_t* name : { L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}" }) {
        PROPVARIANT v;
        PropVariantInit(&v);
        bool found = SUCCEEDED(q->GetMetadataByName(name, &v)) && v.vt == VT_UI2;
        if (found) o = v.uiVal;
        PropVariantClear(&v);
        if (found) break;
    }
    q->Release();
    return o >= 1 && o <= 8 ? o : 1;
}

bool decode(IWICImagingFactory* wic, const std::wstring& path, int maxW, int maxH, ImageNote& out) {
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    IWICBitmap* still = nullptr;
    IWICBitmapFlipRotator* turn = nullptr;
    bool ok = false;
    UINT w = 0, h = 0;
    if (wic && SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h) {
        int o = orientation(frame);
        bool sideways = o >= 5;                     // (5..8: turned a quarter)
        UINT uw = sideways ? h : w, uh = sideways ? w : h;
        double k = std::min({ 1.0, (double)std::max(1, maxW) / uw, (double)std::max(1, maxH) / uh });
        UINT tw = std::max(1u, (UINT)(w * k + 0.5)), th = std::max(1u, (UINT)(h * k + 0.5));
        IWICBitmapSource* src = frame;
        if ((tw != w || th != h) && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant)))
            src = scaler;
        else { tw = w; th = h; }
        if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom))) {
            src = conv;
            if (o != 1) {
                static const WICBitmapTransformOptions kTurn[9] = {
                    WICBitmapTransformRotate0, WICBitmapTransformRotate0, WICBitmapTransformFlipHorizontal,
                    WICBitmapTransformRotate180, WICBitmapTransformFlipVertical,
                    (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal),
                    WICBitmapTransformRotate90,
                    (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal),
                    WICBitmapTransformRotate270,
                };
                // (turned from a decoded copy: the turn reads the picture out of order)
                if (SUCCEEDED(wic->CreateBitmapFromSource(conv, WICBitmapCacheOnLoad, &still)) &&
                    SUCCEEDED(wic->CreateBitmapFlipRotator(&turn)) && SUCCEEDED(turn->Initialize(still, kTurn[o])))
                    src = turn;
            }
            UINT ow = 0, oh = 0;
            src->GetSize(&ow, &oh);
            auto px = std::make_shared<std::vector<uint32_t>>((size_t)ow * oh);
            if (ow && oh && SUCCEEDED(src->CopyPixels(nullptr, ow * 4, ow * oh * 4, (BYTE*)px->data()))) {
                out.pixels = px;
                out.w = (int)ow;
                out.h = (int)oh;
                bool turned = src == turn;
                out.fullW = (int)(turned && sideways ? h : w);
                out.fullH = (int)(turned && sideways ? w : h);
                ok = true;
            }
        }
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{ turn, still, conv, scaler, frame, dec })
        if (u) u->Release();
    return ok;
}

// ------------------------------------------------------------------
// the thread: the newest request, then the next picture ahead
// ------------------------------------------------------------------
struct Request { HWND notify; UINT msg; unsigned id; std::wstring path; int maxW, maxH; std::wstring next; };
struct Cached { std::wstring path; int maxW, maxH; FILETIME time; ImageNote img; };
std::mutex s_mx;
std::condition_variable s_cv;
std::unique_ptr<Request> s_req;
bool s_running = false;
std::deque<Cached> s_cache;                  // (the thread's own) newest first, three at most

FILETIME write_time(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return {};
    return fa.ftLastWriteTime;
}

const ImageNote* cached(const std::wstring& path, int maxW, int maxH) {
    FILETIME t = write_time(path);
    for (auto& c : s_cache)
        if (c.path == path && c.maxW == maxW && c.maxH == maxH && CompareFileTime(&c.time, &t) == 0) return &c.img;
    return nullptr;
}

ImageNote load(IWICImagingFactory* wic, const std::wstring& path, int maxW, int maxH) {
    if (const ImageNote* c = cached(path, maxW, maxH)) return *c;
    ImageNote img;
    img.path = path;
    FILETIME t = write_time(path);
    if (decode(wic, path, maxW, maxH, img)) {
        s_cache.push_front({ path, maxW, maxH, t, img });
        if (s_cache.size() > 3) s_cache.pop_back();
    }
    return img;
}

void worker() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    for (;;) {
        std::unique_ptr<Request> req;
        {
            std::unique_lock<std::mutex> lk(s_mx);
            s_cv.wait(lk, [] { return s_req != nullptr; });
            req = std::move(s_req);
        }
        auto* note = new ImageNote(load(wic, req->path, req->maxW, req->maxH));
        note->id = req->id;
        note->path = req->path;
        if (!PostMessageW(req->notify, req->msg, 0, (LPARAM)note)) delete note;
        bool newer;
        {
            std::lock_guard<std::mutex> lk(s_mx);
            newer = s_req != nullptr;
        }
        if (!newer && !req->next.empty()) load(wic, req->next, req->maxW, req->maxH);
    }
    (void)co;                               // (the thread lives as long as the process)
}

} // namespace

bool image_file(const std::wstring& path) {
    const wchar_t* dot = wcsrchr(path.c_str(), L'.');
    if (!dot || wcschr(dot, L'\\')) return false;
    std::call_once(s_extOnce, load_extensions);
    std::wstring e = dot;
    for (auto& c : e) c = (wchar_t)towlower(c);
    return std::find(s_exts.begin(), s_exts.end(), e) != s_exts.end();
}

void image_request(HWND notify, UINT msg, unsigned id, const std::wstring& path, int maxW, int maxH,
                   const std::wstring& next) {
    std::lock_guard<std::mutex> lk(s_mx);
    s_req = std::make_unique<Request>(Request{ notify, msg, id, path, maxW, maxH, next });
    if (!s_running) {
        s_running = true;
        std::thread(worker).detach();
    }
    s_cv.notify_one();
}
