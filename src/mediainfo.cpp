#include "mediainfo.h"
#include "mediadata.h"
#include <propkey.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

// ------------------------------------------------------------------
// tags
// ------------------------------------------------------------------
std::wstring prop_string(IPropertyStore* ps, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(ps->GetValue(key, &v))) {
        if (v.vt == VT_LPWSTR && v.pwszVal) out = v.pwszVal;
        else if (v.vt == (VT_VECTOR | VT_LPWSTR))
            for (ULONG i = 0; i < v.calpwstr.cElems; ++i)
                if (v.calpwstr.pElems[i] && *v.calpwstr.pElems[i])
                    out += (out.empty() ? L"" : L"; ") + std::wstring(v.calpwstr.pElems[i]);
    }
    PropVariantClear(&v);
    while (!out.empty() && iswspace(out.back())) out.pop_back();
    return out;
}

unsigned prop_uint(IPropertyStore* ps, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    unsigned out = 0;
    if (SUCCEEDED(ps->GetValue(key, &v))) {
        if (v.vt == VT_UI4) out = v.ulVal;
        else if (v.vt == VT_UI8) out = (unsigned)v.uhVal.QuadPart;
        else if (v.vt == VT_I4 && v.lVal > 0) out = (unsigned)v.lVal;
    }
    PropVariantClear(&v);
    return out;
}

void read_tags(const std::wstring& path, MediaInfo& info) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
        info.size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    IPropertyStore* ps = nullptr;
    if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps))) || !ps) return;
    auto add = [&](const wchar_t* label, const std::wstring& v) { if (!v.empty()) info.tags.push_back({ label, v }); };
    std::wstring artist = prop_string(ps, PKEY_Music_Artist), albumArtist = prop_string(ps, PKEY_Music_AlbumArtist);
    if (albumArtist != artist) add(L"Album artist", albumArtist);
    if (unsigned y = prop_uint(ps, PKEY_Media_Year)) add(L"Year", std::to_wstring(y));
    add(L"Genre", prop_string(ps, PKEY_Music_Genre));
    if (unsigned t = prop_uint(ps, PKEY_Music_TrackNumber)) add(L"Track", std::to_wstring(t));
    std::wstring disc = prop_string(ps, PKEY_Music_PartOfSet);      // "1/2"
    if (disc.empty())
        if (unsigned d = prop_uint(ps, PKEY_Music_DiscNumber)) disc = std::to_wstring(d);
    for (auto& c : disc) if (c == L'/') c = L' ';
    if (size_t sp = disc.find(L' '); sp != std::wstring::npos) disc = disc.substr(0, sp) + L" of" + disc.substr(sp);
    add(L"Disc", disc);
    add(L"Composer", prop_string(ps, PKEY_Music_Composer));
    add(L"Conductor", prop_string(ps, PKEY_Music_Conductor));
    add(L"Publisher", prop_string(ps, PKEY_Media_Publisher));
    if (unsigned bpm = prop_uint(ps, PKEY_Music_BeatsPerMinute)) add(L"BPM", std::to_wstring(bpm));
    else add(L"BPM", prop_string(ps, PKEY_Music_BeatsPerMinute));
    info.bitrate = prop_uint(ps, PKEY_Audio_EncodingBitrate);
    ps->Release();
}

// ------------------------------------------------------------------
// cover
// ------------------------------------------------------------------
// a picture file decoded to premultiplied BGRA, no side over `maxPx`
bool decode_picture(IWICImagingFactory* wic, const std::wstring& file, int maxPx, MediaInfo& info) {
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    UINT w = 0, h = 0;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h) {
        double k = std::min(1.0, (double)maxPx / std::max(w, h));
        UINT tw = std::max(1u, (UINT)(w * k + 0.5)), th = std::max(1u, (UINT)(h * k + 0.5));
        IWICBitmapSource* src = frame;
        if ((tw != w || th != h) && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant)))
            src = scaler;
        else { tw = w; th = h; }
        if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom))) {
            auto px = std::make_shared<std::vector<uint32_t>>((size_t)tw * th);
            if (SUCCEEDED(conv->CopyPixels(nullptr, tw * 4, tw * th * 4, (BYTE*)px->data()))) {
                info.cover = px;
                info.coverW = (int)tw;
                info.coverH = (int)th;
                ok = true;
            }
        }
    }
    if (conv) conv->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return ok;
}

// the art in the file itself (ID3, MP4, FLAC, WMA tags), as Explorer's thumbnail shows it
bool embedded_picture(const std::wstring& path, int maxPx, MediaInfo& info) {
    IShellItemImageFactory* f = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))) || !f) return false;
    HBITMAP bmp = nullptr;
    HRESULT hr = f->GetImage({ maxPx, maxPx }, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bmp);
    f->Release();
    if (FAILED(hr) || !bmp) return false;
    BITMAP bm{};
    GetObjectW(bmp, sizeof(bm), &bm);
    bool ok = false;
    if (bm.bmWidth > 0 && bm.bmHeight > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), bm.bmWidth, -bm.bmHeight, 1, 32, BI_RGB };
        auto px = std::make_shared<std::vector<uint32_t>>((size_t)bm.bmWidth * bm.bmHeight);
        HDC dc = GetDC(nullptr);
        if (GetDIBits(dc, bmp, 0, bm.bmHeight, px->data(), &bi, DIB_RGB_COLORS) == bm.bmHeight) {
            bool anyAlpha = false;
            for (uint32_t p : *px) if (p >> 24) { anyAlpha = true; break; }
            if (!anyAlpha) for (uint32_t& p : *px) p |= 0xFF000000;      // (no alpha: opaque)
            info.cover = px;
            info.coverW = bm.bmWidth;
            info.coverH = bm.bmHeight;
            ok = true;
        }
        ReleaseDC(nullptr, dc);
    }
    DeleteObject(bmp);
    return ok;
}

std::wstring folder_cover(const std::wstring& path) {
    std::wstring dir = path.substr(0, path.find_last_of(L"\\/") + 1);
    if (dir.empty()) return {};
    std::vector<ImageFile> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return {};
    do {
        // (hidden ones too: Windows Media Player's Folder.jpg is hidden)
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && is_image_name(fd.cFileName))
            files.push_back({ fd.cFileName, ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow });
    } while (files.size() < 5000 && FindNextFileW(h, &fd));
    FindClose(h);
    int i = pick_cover(files);
    return i >= 0 ? dir + files[(size_t)i].name : std::wstring();
}

void read_cover(IWICImagingFactory* wic, const std::wstring& path, int maxPx, MediaInfo& info) {
    // the same picture as for the previous track (an album): not decoded again
    static std::wstring lastFrom;
    static FILETIME lastTime{};
    static MediaInfo last;
    std::wstring pic = folder_cover(path);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!pic.empty() && GetFileAttributesExW(pic.c_str(), GetFileExInfoStandard, &fa) && pic == lastFrom &&
        CompareFileTime(&fa.ftLastWriteTime, &lastTime) == 0 && last.cover) {
        info.cover = last.cover;
        info.coverW = last.coverW;
        info.coverH = last.coverH;
        info.coverFrom = pic;
        return;
    }
    if (!pic.empty() && wic && decode_picture(wic, pic, maxPx, info)) {
        info.coverFrom = pic;
        lastFrom = pic;
        lastTime = fa.ftLastWriteTime;
        last.cover = info.cover;
        last.coverW = info.coverW;
        last.coverH = info.coverH;
        return;
    }
    if (embedded_picture(path, maxPx, info)) info.coverFrom = path;
}

// ------------------------------------------------------------------
// the thread: the newest request only
// ------------------------------------------------------------------
struct Job { HWND notify; UINT msg; unsigned id; std::wstring path; int maxPx; };
std::mutex s_mx;
std::condition_variable s_cv;
std::unique_ptr<Job> s_job;
bool s_running = false;

void worker() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    for (;;) {
        std::unique_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lk(s_mx);
            s_cv.wait(lk, [] { return s_job != nullptr; });
            job = std::move(s_job);
        }
        auto* note = new MediaInfoNote{ job->id, {} };
        read_tags(job->path, note->info);
        read_cover(wic, job->path, job->maxPx, note->info);
        if (!PostMessageW(job->notify, job->msg, 0, (LPARAM)note)) delete note;
    }
    (void)co;                               // (the thread lives as long as the process)
}

} // namespace

void media_info_request(HWND notify, UINT msg, unsigned id, const std::wstring& path, int maxCoverPx) {
    std::lock_guard<std::mutex> lk(s_mx);
    s_job = std::make_unique<Job>(Job{ notify, msg, id, path, maxCoverPx });
    if (!s_running) {
        s_running = true;
        std::thread(worker).detach();
    }
    s_cv.notify_one();
}
