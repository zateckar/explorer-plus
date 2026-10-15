// (Direct2D's SVG interfaces are declared for Windows 10 1703 on; asked for at run time)
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000003
#include "imageview.h"
#include "imageformats.h"
#include <propvarutil.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <d2d1_3.h>
#include <d2d1helper.h>
#include <gdiplus.h>
#include <algorithm>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>

namespace {

// ------------------------------------------------------------------
// which files: the extensions of the WIC decoders installed, and the ones drawn here
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
    // drawn here: SVG (Direct2D), metafiles (GDI+), an APNG's own extension, imageformats.cpp's
    for (const wchar_t* e : { L".svg", L".emf", L".wmf", L".apng", L".tga", L".qoi", L".hdr", L".pic", L".psd",
                              L".psb", L".pcx" })
        s_exts.push_back(e);
}

std::wstring ext_of(const std::wstring& path) {
    const wchar_t* dot = wcsrchr(path.c_str(), L'.');
    if (!dot || wcschr(dot, L'\\')) return {};
    std::wstring e = dot;
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

bool read_all(const std::wstring& path, std::vector<uint8_t>& out, size_t max) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && (unsigned long long)size.QuadPart <= max;
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

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

unsigned meta_uint(IWICMetadataQueryReader* q, const wchar_t* name, unsigned def) {
    if (!q) return def;
    PROPVARIANT v;
    PropVariantInit(&v);
    unsigned r = def;
    if (SUCCEEDED(q->GetMetadataByName(name, &v))) {
        if (v.vt == VT_UI1) r = v.bVal;
        else if (v.vt == VT_UI2) r = v.uiVal;
        else if (v.vt == VT_UI4) r = v.ulVal;
    }
    PropVariantClear(&v);
    return r;
}

// `frame` fitted to maxW x maxH (never enlarged), turned upright (EXIF orientation `o`),
// as premultiplied BGRA
bool fit(IWICImagingFactory* wic, IWICBitmapSource* frame, int o, int maxW, int maxH, ImageNote& out) {
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    IWICBitmap* still = nullptr;
    IWICBitmapFlipRotator* turn = nullptr;
    bool ok = false;
    UINT w = 0, h = 0;
    if (SUCCEEDED(frame->GetSize(&w, &h)) && w && h) {
        bool sideways = o >= 5;                     // (5..8: turned a quarter)
        UINT uw = sideways ? h : w, uh = sideways ? w : h;
        double k = std::min({ 1.0, (double)std::max(1, maxW) / uw, (double)std::max(1, maxH) / uh });
        UINT tw = std::max(1u, (UINT)(w * k + 0.5)), th = std::max(1u, (UINT)(h * k + 0.5));
        IWICBitmapSource* src = frame;
        if ((tw != w || th != h) && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant)))
            src = scaler;
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
    release(turn);
    release(still);
    release(conv);
    release(scaler);
    return ok;
}

// premultiplied BGRA pixels fitted to the box
bool fit_pixels(IWICImagingFactory* wic, const uint32_t* px, int w, int h, int maxW, int maxH, ImageNote& out) {
    IWICBitmap* bmp = nullptr;
    if (FAILED(wic->CreateBitmapFromMemory((UINT)w, (UINT)h, GUID_WICPixelFormat32bppPBGRA, (UINT)w * 4,
                                           (UINT)((size_t)w * h * 4), (BYTE*)const_cast<uint32_t*>(px), &bmp)))
        return false;
    bool ok = fit(wic, bmp, 1, maxW, maxH, out);
    bmp->Release();
    return ok;
}

// a frame's own pixels (premultiplied BGRA)
bool frame_pixels(IWICImagingFactory* wic, IWICBitmapSource* frame, std::vector<uint32_t>& px, UINT& w, UINT& h) {
    IWICFormatConverter* conv = nullptr;
    bool ok = SUCCEEDED(frame->GetSize(&w, &h)) && w && h && (unsigned long long)w * h <= (1ull << 28) &&
              SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom));
    if (ok) {
        px.resize((size_t)w * h);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE*)px.data()));
    }
    release(conv);
    return ok;
}

// An animation, frame by frame: each canvas fitted to the box and kept (up to a memory
// budget: a long one loops early rather than taking gigabytes)
struct AnimOut {
    IWICImagingFactory* wic;
    int maxW, maxH;
    ImageNote& out;
    size_t budget = (size_t)1 << 25;            // pixels
    bool add(const std::vector<uint32_t>& canvas, int cw, int ch, int delayMs) {
        ImageNote one;
        if (!fit_pixels(wic, canvas.data(), cw, ch, maxW, maxH, one)) return false;
        size_t n = (size_t)one.w * one.h;
        if (!out.frames.empty() && n > budget) return false;
        budget -= std::min(budget, n);
        if (out.frames.empty()) {
            out.pixels = one.pixels;
            out.w = one.w;
            out.h = one.h;
            out.fullW = one.fullW;
            out.fullH = one.fullH;
        }
        out.frames.push_back(one.pixels);
        out.delays.push_back(anim_delay(delayMs));
        return true;
    }
    void done() {                               // (one frame: a still picture)
        if (out.frames.size() < 2) { out.frames.clear(); out.delays.clear(); }
    }
};

// a GIF's frames: each at its place on the logical screen, kept / cleared / restored after
bool gif_frames(IWICImagingFactory* wic, IWICBitmapDecoder* dec, UINT count, int maxW, int maxH, ImageNote& out) {
    IWICMetadataQueryReader* q = nullptr;
    dec->GetMetadataQueryReader(&q);
    int cw = (int)meta_uint(q, L"/logscrdesc/Width", 0), ch = (int)meta_uint(q, L"/logscrdesc/Height", 0);
    release(q);
    AnimOut anim{ wic, maxW, maxH, out };
    std::unique_ptr<AnimCanvas> canvas;
    std::vector<uint32_t> px;
    for (UINT i = 0; i < count && i < 2000; ++i) {
        IWICBitmapFrameDecode* frame = nullptr;
        UINT fw = 0, fh = 0;
        if (FAILED(dec->GetFrame(i, &frame)) || !frame_pixels(wic, frame, px, fw, fh)) { release(frame); break; }
        frame->GetMetadataQueryReader(&q);
        AnimFrame f;
        f.x = (int)meta_uint(q, L"/imgdesc/Left", 0);
        f.y = (int)meta_uint(q, L"/imgdesc/Top", 0);
        f.w = (int)fw;
        f.h = (int)fh;
        f.px = px.data();
        unsigned disposal = meta_uint(q, L"/grctlext/Disposal", 0);
        f.dispose = disposal == 2 ? AnimFrame::Clear : disposal == 3 ? AnimFrame::Restore : AnimFrame::Keep;
        int delay = (int)meta_uint(q, L"/grctlext/Delay", 0) * 10;
        release(q);
        release(frame);
        if (!canvas) {
            if (cw <= 0 || ch <= 0) { cw = f.x + f.w; ch = f.y + f.h; }
            canvas = std::make_unique<AnimCanvas>(cw, ch);
        }
        if (!anim.add(canvas->add(f), canvas->w(), canvas->h(), delay)) break;
    }
    anim.done();
    return out.pixels != nullptr;
}

// an APNG's frames, each decoded from a PNG of its own
bool apng_frames(IWICImagingFactory* wic, const std::vector<uint8_t>& file, int maxW, int maxH, ImageNote& out) {
    int cw = 0, ch = 0;
    std::vector<ApngFrame> frames;
    if (!apng_split(file.data(), file.size(), cw, ch, frames) || frames.size() < 2) return false;
    AnimOut anim{ wic, maxW, maxH, out };
    AnimCanvas canvas(cw, ch);
    std::vector<uint32_t> px;
    for (auto& fr : frames) {
        IWICStream* s = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        UINT fw = 0, fh = 0;
        bool ok = SUCCEEDED(wic->CreateStream(&s)) && SUCCEEDED(s->InitializeFromMemory(fr.png.data(), (DWORD)fr.png.size())) &&
                  SUCCEEDED(wic->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
                  SUCCEEDED(dec->GetFrame(0, &frame)) && frame_pixels(wic, frame, px, fw, fh) &&
                  (int)fw == fr.w && (int)fh == fr.h;
        release(frame);
        release(dec);
        release(s);
        if (!ok) break;
        AnimFrame f;
        f.x = fr.x; f.y = fr.y; f.w = fr.w; f.h = fr.h;
        f.px = px.data();
        f.dispose = fr.dispose;
        f.over = fr.over;
        if (!anim.add(canvas.add(f), cw, ch, fr.delayMs)) break;
    }
    anim.done();
    if (!out.pixels) return false;
    return true;
}

// ------------------------------------------------------------------
// drawings: SVG (Direct2D), EMF / WMF (GDI+); drawn to fit the box, enlarged up to
// nlarge times
// ------------------------------------------------------------------
void drawing_size(double w, double h, int maxW, int maxH, double enlarge, int& tw, int& th) {
    double k = std::min({ enlarge, std::max(1, maxW) / w, std::max(1, maxH) / h });
    tw = std::max(1, (int)(w * k + 0.5));
    th = std::max(1, (int)(h * k + 0.5));
}

using D2DCreate = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);

bool svg_render(IWICImagingFactory* wic, const std::wstring& path, int maxW, int maxH, double enlarge, ImageNote& out) {
    static D2DCreate create = [] {
        HMODULE m = LoadLibraryExW(L"d2d1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? (D2DCreate)(void*)GetProcAddress(m, "D2D1CreateFactory") : nullptr;
    }();
    std::vector<uint8_t> file;
    double w = 0, h = 0;
    if (!create || !read_all(path, file, 64u << 20) || !svg_size(std::string(file.begin(), file.end()), w, h)) return false;
    int tw = 0, th = 0;
    drawing_size(w, h, maxW, maxH, enlarge, tw, th);
    ID2D1Factory1* f = nullptr;
    IWICBitmap* bmp = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    ID2D1DeviceContext5* dc = nullptr;
    IStream* stream = nullptr;
    ID2D1SvgDocument* doc = nullptr;
    D2D1_FACTORY_OPTIONS opts{};
    bool ok = SUCCEEDED(create(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts, (void**)&f)) &&
              SUCCEEDED(wic->CreateBitmap((UINT)tw, (UINT)th, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp));
    if (ok) {
        D2D1_RENDER_TARGET_PROPERTIES props{};
        props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
        ok = SUCCEEDED(f->CreateWicBitmapRenderTarget(bmp, &props, &rt)) &&
             SUCCEEDED(rt->QueryInterface(__uuidof(ID2D1DeviceContext5), (void**)&dc));      // (Windows 10 1703 on)
    }
    if (ok) ok = (stream = SHCreateMemStream(file.data(), (UINT)file.size())) != nullptr;
    // drawn at the size it asks for, scaled to the bitmap (Direct2D draws a document at its
    // own width / height, whatever the viewport)
    if (ok) ok = SUCCEEDED(dc->CreateSvgDocument(stream, D2D1::SizeF((float)w, (float)h), &doc)) && doc;
    if (ok) {
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        rt->SetTransform(D2D1::Matrix3x2F::Scale((float)(tw / w), (float)(th / h)));
        dc->DrawSvgDocument(doc);
        ok = SUCCEEDED(rt->EndDraw());
    }
    if (ok) {
        auto px = std::make_shared<std::vector<uint32_t>>((size_t)tw * th);
        ok = SUCCEEDED(bmp->CopyPixels(nullptr, (UINT)tw * 4, (UINT)(px->size() * 4), (BYTE*)px->data()));
        if (ok) {
            out.pixels = px;
            out.w = out.fullW = tw;
            out.h = out.fullH = th;
        }
    }
    release(doc);
    release(stream);
    release(dc);
    release(rt);
    release(bmp);
    release(f);
    return ok;
}

// on white (drawings made for paper)
bool metafile_render(const std::wstring& path, int maxW, int maxH, double enlarge, ImageNote& out) {
    Gdiplus::Metafile mf(path.c_str());
    Gdiplus::MetafileHeader hdr;
    if (mf.GetLastStatus() != Gdiplus::Ok || mf.GetMetafileHeader(&hdr) != Gdiplus::Ok) return false;
    Gdiplus::Rect b;
    hdr.GetBounds(&b);
    double dx = hdr.GetDpiX() > 0 ? hdr.GetDpiX() : 96, dy = hdr.GetDpiY() > 0 ? hdr.GetDpiY() : 96;
    double w = b.Width * 96.0 / dx, h = b.Height * 96.0 / dy;
    if (w < 1 || h < 1) return false;
    int tw = 0, th = 0;
    drawing_size(w, h, maxW, maxH, enlarge, tw, th);
    auto px = std::make_shared<std::vector<uint32_t>>((size_t)tw * th);
    {
        Gdiplus::Bitmap bmp(tw, th, tw * 4, PixelFormat32bppPARGB, (BYTE*)px->data());
        Gdiplus::Graphics g(&bmp);
        g.Clear(Gdiplus::Color(255, 255, 255, 255));
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        if (g.DrawImage(&mf, 0, 0, tw, th) != Gdiplus::Ok) return false;
    }
    out.pixels = px;
    out.w = out.fullW = tw;
    out.h = out.fullH = th;
    return true;
}

// nlarge: how far a drawing may grow to fill the box
bool decode(IWICImagingFactory* wic, const std::wstring& path, int maxW, int maxH, int page, ImageNote& out,
            double enlarge = 4.0) {
    if (!wic) return false;
    std::wstring ext = ext_of(path);
    if (ext == L".svg") return svg_render(wic, path, maxW, maxH, enlarge, out);
    if (ext == L".emf" || ext == L".wmf") return metafile_render(path, maxW, maxH, enlarge, out);
    if (own_image_ext(ext)) {
        std::vector<uint8_t> file;
        RawImage raw;
        return read_all(path, file, 1u << 30) && own_image_decode(ext, file.data(), file.size(), raw) &&
               fit_pixels(wic, raw.px.data(), raw.w, raw.h, maxW, maxH, out);
    }
    if (ext == L".png" || ext == L".apng") {
        std::vector<uint8_t> file;
        if (read_all(path, file, 256u << 20) && apng_frames(wic, file, maxW, maxH, out)) return true;
        out = {};
    }
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    bool ok = false;
    UINT count = 0;
    GUID container{};
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrameCount(&count)) && count) {
        dec->GetContainerFormat(&container);
        UINT index = 0;
        if (container == GUID_ContainerFormatGif && count > 1) {
            ok = gif_frames(wic, dec, count, maxW, maxH, out);
            if (ok) { dec->Release(); return true; }
            out = {};
        } else if (container == GUID_ContainerFormatTiff && count > 1) {
            index = (UINT)std::clamp(page, 0, (int)count - 1);
            out.page = (int)index;
            out.pages = (int)count;
        } else if (container == GUID_ContainerFormatIco && count > 1) {
            // the icon's biggest size (the deepest colour of equals: the later)
            unsigned long long best = 0;
            for (UINT i = 0; i < count; ++i) {
                IWICBitmapFrameDecode* f = nullptr;
                UINT w = 0, h = 0;
                if (SUCCEEDED(dec->GetFrame(i, &f)) && SUCCEEDED(f->GetSize(&w, &h)) && (unsigned long long)w * h >= best) {
                    best = (unsigned long long)w * h;
                    index = i;
                }
                release(f);
            }
        }
        if (SUCCEEDED(dec->GetFrame(index, &frame))) ok = fit(wic, frame, orientation(frame), maxW, maxH, out);
    }
    release(frame);
    release(dec);
    return ok;
}

// ------------------------------------------------------------------
// the thread: the newest request, then the next picture ahead
// ------------------------------------------------------------------
struct Request { HWND notify; UINT msg; unsigned id; std::wstring path; int maxW, maxH; std::wstring next; int page; };
struct Cached { std::wstring path; int maxW, maxH, page; FILETIME time; ImageNote img; };
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

const ImageNote* cached(const std::wstring& path, int maxW, int maxH, int page) {
    FILETIME t = write_time(path);
    for (auto& c : s_cache)
        if (c.path == path && c.maxW == maxW && c.maxH == maxH && c.page == page && CompareFileTime(&c.time, &t) == 0)
            return &c.img;
    return nullptr;
}

ImageNote load(IWICImagingFactory* wic, const std::wstring& path, int maxW, int maxH, int page) {
    if (const ImageNote* c = cached(path, maxW, maxH, page)) return *c;
    ImageNote img;
    img.path = path;
    FILETIME t = write_time(path);
    if (decode(wic, path, maxW, maxH, page, img)) {
        img.path = path;
        s_cache.push_front({ path, maxW, maxH, page, t, img });
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
        auto* note = new ImageNote(load(wic, req->path, req->maxW, req->maxH, req->page));
        note->id = req->id;
        note->path = req->path;
        if (!PostMessageW(req->notify, req->msg, 0, (LPARAM)note)) delete note;
        bool newer;
        {
            std::lock_guard<std::mutex> lk(s_mx);
            newer = s_req != nullptr;
        }
        if (!newer && !req->next.empty()) load(wic, req->next, req->maxW, req->maxH, 0);
    }
    (void)co;                               // (the thread lives as long as the process)
}

} // namespace

bool image_file(const std::wstring& path) {
    if (path.find(L"://") != std::wstring::npos) return false;     // (a web address: a stream's)
    std::wstring e = ext_of(path);
    if (e.empty()) return false;
    std::call_once(s_extOnce, load_extensions);
    return std::find(s_exts.begin(), s_exts.end(), e) != s_exts.end();
}

bool image_decode_full(const std::wstring& path, ImageNote& out) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    bool ok = decode(wic, path, 1 << 15, 1 << 15, 0, out, 1.0);      // (a drawing at its own size)
    wic->Release();
    out.path = path;
    out.frames.clear();                     // (the first frame: what's edited)
    out.delays.clear();
    return ok;
}

void image_request(HWND notify, UINT msg, unsigned id, const std::wstring& path, int maxW, int maxH,
                   const std::wstring& next, int page) {
    std::lock_guard<std::mutex> lk(s_mx);
    s_req = std::make_unique<Request>(Request{ notify, msg, id, path, maxW, maxH, next, page });
    if (!s_running) {
        s_running = true;
        std::thread(worker).detach();
    }
    s_cv.notify_one();
}
