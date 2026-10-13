#include "media.h"
#include "mediadata.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <evr.h>
#include <mmdeviceapi.h>
#include <audiosessiontypes.h>
#include <propkey.h>
#include <shlwapi.h>
#include <atomic>
#include <cwchar>
#include <mutex>

// ------------------------------------------------------------------
// Media Foundation, loaded on first use (mf.dll alone pulls in a dozen DLLs: none of
// that belongs in the app's startup). MFStartup is never paired with MFShutdown: the
// process ends with _Exit.
// ------------------------------------------------------------------

// (not in MinGW's headers / libmfuuid)
static const GUID kVideoRenderService = { 0x1092a86c, 0xab1a, 0x459a, { 0xa3, 0x36, 0x83, 0x1f, 0xbc, 0x4d, 0x11, 0xff } };
static const DWORD kStandardQueue = 0x00000001;     // MFASYNC_CALLBACK_QUEUE_STANDARD

static struct {
    HRESULT (WINAPI* Startup)(ULONG, DWORD);
    HRESULT (WINAPI* PutWorkItem)(DWORD, IMFAsyncCallback*, IUnknown*);
    HRESULT (WINAPI* CreateMFByteStreamOnStream)(IStream*, IMFByteStream**);
    HRESULT (WINAPI* CreateMediaSession)(IMFAttributes*, IMFMediaSession**);
    HRESULT (WINAPI* CreateSourceResolver)(IMFSourceResolver**);
    HRESULT (WINAPI* CreateMediaType)(IMFMediaType**);
    HRESULT (WINAPI* CreateTopology)(IMFTopology**);
    HRESULT (WINAPI* CreateTopologyNode)(MF_TOPOLOGY_TYPE, IMFTopologyNode**);
    HRESULT (WINAPI* CreateAudioRendererActivate)(IMFActivate**);
    HRESULT (WINAPI* CreateVideoRendererActivate)(HWND, IMFActivate**);
    HRESULT (WINAPI* GetService)(IUnknown*, REFGUID, REFIID, LPVOID*);
} s_mf;
static int s_loaded = 0;            // 0 not tried, 1 ready, -1 unavailable
static std::wstring s_why;

template <class F> static bool proc(HMODULE m, const char* name, F& out) {
    out = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    return out != nullptr;
}

bool media_available(std::wstring* why) {
    if (!s_loaded) {
        s_loaded = -1;
        HMODULE plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE mf = LoadLibraryExW(L"mf.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!plat || !mf ||
            !proc(plat, "MFStartup", s_mf.Startup) ||
            !proc(plat, "MFPutWorkItem", s_mf.PutWorkItem) ||
            !proc(plat, "MFCreateMFByteStreamOnStream", s_mf.CreateMFByteStreamOnStream) ||
            !proc(plat, "MFCreateSourceResolver", s_mf.CreateSourceResolver) ||
            !proc(plat, "MFCreateMediaType", s_mf.CreateMediaType) ||
            !proc(mf, "MFCreateMediaSession", s_mf.CreateMediaSession) ||
            !proc(mf, "MFCreateTopology", s_mf.CreateTopology) ||
            !proc(mf, "MFCreateTopologyNode", s_mf.CreateTopologyNode) ||
            !proc(mf, "MFCreateAudioRendererActivate", s_mf.CreateAudioRendererActivate) ||
            !proc(mf, "MFCreateVideoRendererActivate", s_mf.CreateVideoRendererActivate) ||
            !proc(mf, "MFGetService", s_mf.GetService)) {
            s_why = L"Media playback isn't installed on this computer (Windows N editions: add the Media Feature Pack).";
        } else if (HRESULT hr = s_mf.Startup(MF_VERSION, MFSTARTUP_LITE); FAILED(hr)) {    // (no network sources)
            WCHAR b[96];
            swprintf_s(b, L"Media playback couldn't start (error 0x%08lX).", (unsigned long)hr);
            s_why = b;
        } else {
            s_loaded = 1;
        }
    }
    if (why) *why = s_why;
    return s_loaded > 0;
}

std::wstring media_error_text(HRESULT hr) {
    switch (hr) {
        case MF_E_PLATFORM_NOT_INITIALIZED: {
            std::wstring why;
            if (!media_available(&why)) return why;
            break;
        }
        case MF_E_UNSUPPORTED_BYTESTREAM_TYPE: case MF_E_UNSUPPORTED_SCHEME:
        case MF_E_UNSUPPORTED_FORMAT: case MF_E_INVALIDMEDIATYPE: case MF_E_INVALID_FILE_FORMAT:
            return L"This file type can't be played.";
        case MF_E_TOPO_CODEC_NOT_FOUND: case MF_E_TOPO_UNSUPPORTED:
            return L"The codec this file needs isn't installed.";
        case MF_E_NO_AUDIO_PLAYBACK_DEVICE:
            return L"There is no audio output device.";
        case MF_E_AUDIO_PLAYBACK_DEVICE_INVALIDATED:
            return L"The audio output device was removed.";
        case MF_E_AUDIO_SERVICE_NOT_RUNNING:
            return L"The Windows Audio service isn't running.";
        case HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND): case HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND):
        case HRESULT_FROM_WIN32(ERROR_BAD_NETPATH):
            return L"The file isn't there anymore.";
        case HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED): case HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION):
            return L"The file can't be opened.";
    }
    if (hr == E_FAIL || hr == S_OK) return L"This file can't be played.";
    WCHAR b[96];
    swprintf_s(b, L"This file can't be played (error 0x%08lX).", (unsigned long)hr);
    return b;
}

// ------------------------------------------------------------------
// notes from Media Foundation's threads to the window
// ------------------------------------------------------------------
struct MediaNote {
    enum Kind { Source, Event } kind = Event;
    UINT gen = 0;                   // the open it belongs to (stale ones are dropped)
    HRESULT hr = S_OK;              // the event's status / the resolver's result
    MediaEventType type = MEUnknown;
    UINT32 topoStatus = 0;          // MESessionTopologyStatus
    IUnknown* source = nullptr;     // Source: the media source (owned)
    std::wstring title, artist, album;     // Source: the file's tags
};

static void release_note(MediaNote* n) {
    if (n->source) {
        IMFMediaSource* s = nullptr;
        if (SUCCEEDED(n->source->QueryInterface(IID_PPV_ARGS(&s)))) { s->Shutdown(); s->Release(); }
        n->source->Release();
    }
    delete n;
}

static void post_note(HWND hwnd, UINT msg, MediaNote* n) {
    if (!PostMessageW(hwnd, msg, 0, (LPARAM)n)) release_note(n);    // (the window is gone)
}

struct CallbackBase : IMFAsyncCallback {
    std::atomic<ULONG> refs{ 1 };
    HWND hwnd; UINT msg, gen;
    CallbackBase(HWND h, UINT m, UINT g) : hwnd(h), msg(m), gen(g) {}
    virtual ~CallbackBase() = default;
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IMFAsyncCallback)) { *ppv = static_cast<IMFAsyncCallback*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = --refs; if (!r) delete this; return r; }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
};

// The session's events: passed on to the window; MESessionClosed (the last one) signals
// `closed`. (The session holds this while an event is asked for, and lets go at
// Shutdown, so holding the session back makes no cycle.)
struct SessionCallback : CallbackBase {
    IMFMediaSession* session;
    HANDLE closed;
    SessionCallback(HWND h, UINT m, UINT g, IMFMediaSession* s) : CallbackBase(h, m, g), session(s) {
        session->AddRef();
        closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~SessionCallback() override { session->Release(); CloseHandle(closed); }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        IMFMediaEvent* ev = nullptr;
        if (FAILED(session->EndGetEvent(result, &ev)) || !ev) return S_OK;
        auto* n = new MediaNote;
        n->gen = gen;
        ev->GetType(&n->type);
        ev->GetStatus(&n->hr);
        if (n->type == MESessionTopologyStatus) ev->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, &n->topoStatus);
        ev->Release();
        if (n->type == MESessionClosed) {
            delete n;
            SetEvent(closed);
            return S_OK;
        }
        session->BeginGetEvent(this, nullptr);
        post_note(hwnd, msg, n);
        return S_OK;
    }
};

static std::wstring prop_text(IPropertyStore* ps, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(ps->GetValue(key, &v))) {
        if (v.vt == VT_LPWSTR && v.pwszVal) out = v.pwszVal;
        else if (v.vt == (VT_VECTOR | VT_LPWSTR))
            for (ULONG i = 0; i < v.calpwstr.cElems; ++i)
                if (v.calpwstr.pElems[i]) out += (out.empty() ? L"" : L"; ") + std::wstring(v.calpwstr.pElems[i]);
    }
    PropVariantClear(&v);
    return out;
}

// Opening a file, on a work queue thread (a file on a slow share mustn't hold up the
// window): the file is opened here, shared for reading, writing and deleting, so the file
// manager can still rename, move or delete a file that is playing; then the source
// resolver makes the media source from it (the path tells it the file type). The tags
// come from the shell's property store, as Explorer shows them (a source made from a
// stream has no property handler of its own).
struct OpenCallback : CallbackBase {
    std::wstring path, title, artist, album;
    IMFSourceResolver* resolver;
    bool resolving = false;
    OpenCallback(HWND h, UINT m, UINT g, IMFSourceResolver* r, std::wstring p)
        : CallbackBase(h, m, g), path(std::move(p)), resolver(r) { resolver->AddRef(); }
    ~OpenCallback() override { resolver->Release(); }
    void answer(HRESULT hr, IUnknown* source) {
        auto* n = new MediaNote;
        n->kind = MediaNote::Source;
        n->gen = gen;
        n->hr = hr;
        n->source = source;
        n->title = std::move(title);
        n->artist = std::move(artist);
        n->album = std::move(album);
        post_note(hwnd, msg, n);
    }
    void read_tags() {
        IPropertyStore* ps = nullptr;
        if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps))) || !ps)
            return;
        title = prop_text(ps, PKEY_Title);
        artist = prop_text(ps, PKEY_Music_Artist);
        album = prop_text(ps, PKEY_Music_AlbumTitle);
        ps->Release();
    }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        static const DWORD kFlags = MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE;
        if (resolving) {
            MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
            IUnknown* source = nullptr;
            HRESULT hr = resolver->EndCreateObjectFromByteStream(result, &type, &source);
            answer(hr, source);
            return S_OK;
        }
        read_tags();
        IStream* file = nullptr;
        IMFByteStream* bytes = nullptr;
        HRESULT hr = SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL,
                                            FALSE, nullptr, &file);
        if (SUCCEEDED(hr)) hr = s_mf.CreateMFByteStreamOnStream(file, &bytes);
        if (SUCCEEDED(hr)) {
            resolving = true;
            hr = resolver->BeginCreateObjectFromByteStream(bytes, path.c_str(), kFlags, nullptr, nullptr, this, nullptr);
        }
        if (bytes) bytes->Release();
        if (file) file->Release();
        if (FAILED(hr)) answer(hr, nullptr);
        return S_OK;
    }
};

// ------------------------------------------------------------------
// player
// ------------------------------------------------------------------
template <class T> static void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct MediaPlayer::Impl {
    HWND notify, video;
    UINT msg;
    UINT gen = 0;
    IMFMediaSession* session = nullptr;
    IMFMediaSource* source = nullptr;
    SessionCallback* events = nullptr;
    IMFVideoDisplayControl* display = nullptr;
    IMFAudioStreamVolume* volume = nullptr;
    IMFPresentationClock* clock = nullptr;
    bool playWhenReady = false;
    LONGLONG startAt = 0;           // where the file starts once open
    bool wantPaused = false;        // the user's choice: a seek keeps it
    int pending = 0;                // Start / Pause / Stop calls not answered yet
    LONGLONG queuedSeek = -1;       // asked for while `pending`
    LONGLONG shownPos = -1;         // a seek's target until the session has got there
    float amplitude = 1.0f;
};

MediaPlayer::MediaPlayer(HWND notify, UINT msg, HWND video) : d(new Impl) {
    d->notify = notify;
    d->msg = msg;
    d->video = video;
}

MediaPlayer::~MediaPlayer() {
    close();
    delete d;
}

// Waits for MESessionClosed. The video renderer may send messages to the video window
// (this thread's) while it shuts down: those are answered meanwhile, nothing else is.
static void wait_closed(HANDLE ev, DWORD ms) {
    DWORD end = GetTickCount() + ms;
    for (;;) {
        DWORD now = GetTickCount();
        DWORD left = (LONG)(end - now) > 0 ? end - now : 0;
        DWORD r = MsgWaitForMultipleObjects(1, &ev, FALSE, left, QS_SENDMESSAGE);
        if (r != WAIT_OBJECT_0 + 1) return;          // closed, timed out or failed
        MSG m;
        PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
}

void MediaPlayer::close() {
    ++d->gen;                                        // whatever is still on its way is stale
    release(d->display);
    release(d->volume);
    release(d->clock);
    if (d->session && d->events && SUCCEEDED(d->session->Close())) {
        m_closing = true;
        wait_closed(d->events->closed, 2000);
        m_closing = false;
    }
    if (d->source) d->source->Shutdown();
    if (d->session) d->session->Shutdown();
    release(d->source);
    release(d->session);
    release(d->events);
    d->playWhenReady = d->wantPaused = false;
    d->startAt = 0;
    d->pending = 0;
    d->queuedSeek = d->shownPos = -1;
    m_state = State::Closed;
    m_duration = 0;
    m_hasVideo = false;
    m_error = S_OK;
    m_title.clear(); m_artist.clear(); m_album.clear();
    m_audio = {};
    if (d->video) InvalidateRect(d->video, nullptr, TRUE);
}

void MediaPlayer::fail(HRESULT hr) {
    m_error = FAILED(hr) ? hr : E_FAIL;
    m_state = State::Failed;
    d->playWhenReady = false;
}

bool MediaPlayer::open(const std::wstring& path, bool play, LONGLONG startAt) {
    close();
    if (!media_available()) { fail(MF_E_PLATFORM_NOT_INITIALIZED); return false; }
    UINT gen = d->gen;
    IMFSourceResolver* resolver = nullptr;
    HRESULT hr = s_mf.CreateMediaSession(nullptr, &d->session);
    if (SUCCEEDED(hr)) {
        d->events = new SessionCallback(d->notify, d->msg, gen, d->session);
        hr = d->session->BeginGetEvent(d->events, nullptr);
    }
    if (SUCCEEDED(hr)) hr = s_mf.CreateSourceResolver(&resolver);
    if (SUCCEEDED(hr)) {
        auto* cb = new OpenCallback(d->notify, d->msg, gen, resolver, path);
        hr = s_mf.PutWorkItem(kStandardQueue, cb, nullptr);
        cb->Release();
    }
    release(resolver);
    if (FAILED(hr)) { fail(hr); return false; }
    m_state = State::Opening;
    d->playWhenReady = play;
    d->startAt = startAt > 0 ? startAt : 0;
    d->wantPaused = !play && d->startAt > 0;         // (shown paused there)
    return true;
}

// Opus in MP4: Windows' MP4 source labels the stream with the sample entry's box type
// ('Opus', read big-endian) where Windows' own Opus decoder takes MFAudioFormat_Opus with
// an OpusHead (as Matroska / WebM carry it). The stream is relabelled for the decoder:
// the OpusHead is made from the sample description's 'dOps' box (opus_head_from_dops).
// (The source accepts the type though IsMediaTypeSupported says otherwise.)
static void fix_opus_type(IMFMediaTypeHandler* th) {
    IMFMediaType* mt = nullptr;
    if (FAILED(th->GetCurrentMediaType(&mt)) || !mt) return;
    GUID sub = GUID_NULL;
    mt->GetGUID(MF_MT_SUBTYPE, &sub);
    UINT8* desc = nullptr;
    UINT32 n = 0;
    UINT8 head[19];
    if ((sub.Data1 == 0x4F707573 || sub.Data1 == 0x7375704F) &&
        SUCCEEDED(mt->GetAllocatedBlob(MF_MT_MPEG4_SAMPLE_DESCRIPTION, &desc, &n))) {
        IMFMediaType* fixed = nullptr;
        if (opus_head_from_dops(desc, n, head) && SUCCEEDED(s_mf.CreateMediaType(&fixed))) {
            GUID opus = MFAudioFormat_Base;
            opus.Data1 = 0x704F;                      // MFAudioFormat_Opus
            if (SUCCEEDED(mt->CopyAllItems(fixed)) && SUCCEEDED(fixed->SetGUID(MF_MT_SUBTYPE, opus)) &&
                SUCCEEDED(fixed->SetBlob(MF_MT_USER_DATA, head, sizeof(head))))
                th->SetCurrentMediaType(fixed);
            fixed->Release();
        }
        CoTaskMemFree(desc);
    }
    mt->Release();
}

// the first selected audio stream's format (after fix_opus_type)
static AudioInfo audio_info_of(IMFMediaTypeHandler* th) {
    AudioInfo a;
    IMFMediaType* mt = nullptr;
    // (some sources, WAV's among them, have no current type before playback: their first one)
    if ((FAILED(th->GetCurrentMediaType(&mt)) || !mt) && (FAILED(th->GetMediaTypeByIndex(0, &mt)) || !mt)) return a;
    GUID sub = GUID_NULL;
    if (SUCCEEDED(mt->GetGUID(MF_MT_SUBTYPE, &sub))) a.format = sub.Data1;
    UINT32 v = 0;
    if (SUCCEEDED(mt->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &v))) a.rate = v;
    if (SUCCEEDED(mt->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &v))) a.channels = v;
    if (SUCCEEDED(mt->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &v))) a.bits = v;
    if (SUCCEEDED(mt->GetUINT32(MF_MT_AVG_BITRATE, &v))) a.bitrate = v;
    else if (SUCCEEDED(mt->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &v))) a.bitrate = v * 8;
    mt->Release();
    return a;
}

static bool audio_output_present() {
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&en))))
        return true;                                 // (can't tell: the renderer finds out)
    IMMDevice* dev = nullptr;
    bool ok = SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev;
    if (dev) dev->Release();
    en->Release();
    return ok;
}

void MediaPlayer::source_ready(IUnknown* obj, HRESULT hr) {
    if (SUCCEEDED(hr) && obj) hr = obj->QueryInterface(IID_PPV_ARGS(&d->source));
    if (FAILED(hr) || !d->source) { fail(hr); return; }

    IMFPresentationDescriptor* pd = nullptr;
    IMFTopology* topo = nullptr;
    hr = d->source->CreatePresentationDescriptor(&pd);
    if (SUCCEEDED(hr)) {
        UINT64 dur = 0;
        if (SUCCEEDED(pd->GetUINT64(MF_PD_DURATION, &dur))) m_duration = (LONGLONG)dur;
        hr = s_mf.CreateTopology(&topo);
    }
    bool audioOut = audio_output_present(), audio = false, video = false, audioOnly = true;
    DWORD count = 0;
    if (SUCCEEDED(hr)) hr = pd->GetStreamDescriptorCount(&count);
    for (DWORD i = 0; SUCCEEDED(hr) && i < count; ++i) {
        BOOL selected = FALSE;
        IMFStreamDescriptor* sd = nullptr;
        if (FAILED(pd->GetStreamDescriptorByIndex(i, &selected, &sd))) continue;
        GUID major = GUID_NULL;
        IMFMediaTypeHandler* th = nullptr;
        if (SUCCEEDED(sd->GetMediaTypeHandler(&th))) {
            th->GetMajorType(&major);
            if (selected && major == MFMediaType_Audio) {
                fix_opus_type(th);
                if (!m_audio.format) m_audio = audio_info_of(th);
            }
            th->Release();
        }
        IMFActivate* sink = nullptr;
        if (major == MFMediaType_Video) audioOnly = false;
        // one audio and one video stream (the first ones the file selects); without an
        // audio device a video plays silently
        if (selected && major == MFMediaType_Audio && audioOut && !audio) {
            if (SUCCEEDED(s_mf.CreateAudioRendererActivate(&sink))) {
                sink->SetUINT32(MF_AUDIO_RENDERER_ATTRIBUTE_STREAM_CATEGORY, AudioCategory_Media);
                audio = true;
            }
        } else if (selected && major == MFMediaType_Video && !video && d->video) {
            if (SUCCEEDED(s_mf.CreateVideoRendererActivate(d->video, &sink))) video = true;
        }
        if (!sink) {
            if (selected) pd->DeselectStream(i);
            sd->Release();
            continue;
        }
        IMFTopologyNode *in = nullptr, *out = nullptr;
        hr = s_mf.CreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &in);
        if (SUCCEEDED(hr)) hr = in->SetUnknown(MF_TOPONODE_SOURCE, d->source);
        if (SUCCEEDED(hr)) hr = in->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pd);
        if (SUCCEEDED(hr)) hr = in->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, sd);
        if (SUCCEEDED(hr)) hr = topo->AddNode(in);
        if (SUCCEEDED(hr)) hr = s_mf.CreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &out);
        if (SUCCEEDED(hr)) hr = out->SetObject(sink);
        if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_TOPONODE_STREAMID, 0);
        if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_TOPONODE_NOSHUTDOWN_ON_REMOVE, FALSE);
        if (SUCCEEDED(hr)) hr = topo->AddNode(out);
        if (SUCCEEDED(hr)) hr = in->ConnectOutput(0, out, 0);
        release(in);
        release(out);
        sink->Release();
        sd->Release();
    }
    if (SUCCEEDED(hr) && !audio && !video)
        hr = !audioOut && audioOnly ? MF_E_NO_AUDIO_PLAYBACK_DEVICE : MF_E_UNSUPPORTED_FORMAT;
    if (SUCCEEDED(hr)) hr = d->session->SetTopology(0, topo);
    release(topo);
    release(pd);
    if (FAILED(hr)) { fail(hr); return; }
    m_hasVideo = video;
}

void MediaPlayer::topology_ready() {
    release(d->display);
    release(d->volume);
    release(d->clock);
    if (m_hasVideo && SUCCEEDED(s_mf.GetService(d->session, kVideoRenderService, IID_PPV_ARGS(&d->display))) && d->display) {
        d->display->SetAspectRatioMode(MFVideoARMode_PreservePicture);
        d->display->SetBorderColor(RGB(0, 0, 0));
        video_resized();
    }
    s_mf.GetService(d->session, MR_STREAM_VOLUME_SERVICE, IID_PPV_ARGS(&d->volume));
    set_volume(d->amplitude);
    IMFClock* c = nullptr;
    if (SUCCEEDED(d->session->GetClock(&c)) && c) {
        c->QueryInterface(IID_PPV_ARGS(&d->clock));
        c->Release();
    }
    if (m_state != State::Opening) return;           // (a format change: the session goes on)
    m_state = State::Stopped;
    if (d->playWhenReady || d->startAt > 0) {
        d->shownPos = d->startAt > 0 ? d->startAt : -1;
        start_at(d->startAt, false);
    }
}

void MediaPlayer::start_at(LONGLONG hns, bool resume) {
    if (!d->session) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (!resume) { v.vt = VT_I8; v.hVal.QuadPart = hns; }
    if (SUCCEEDED(d->session->Start(&GUID_NULL, &v))) ++d->pending;
    // (paused: shows the frame there and stays; the session runs the calls in order)
    if (d->wantPaused && SUCCEEDED(d->session->Pause())) ++d->pending;
}

void MediaPlayer::play() {
    d->wantPaused = false;
    switch (m_state) {
        case State::Opening: d->playWhenReady = true; break;
        case State::Paused:
            if (!d->pending) start_at(0, true);
            else d->queuedSeek = position();
            break;
        case State::Stopped: case State::Ended: start_at(0, false); break;
        case State::Playing:            // (a Pause on its way: started again once it's answered)
            if (d->pending) d->queuedSeek = position();
            break;
        default: break;
    }
}

void MediaPlayer::pause() {
    d->wantPaused = true;
    if (m_state == State::Opening) { d->playWhenReady = false; return; }
    if (m_state != State::Playing || !d->session) return;
    if (d->pending) { d->queuedSeek = position(); return; }
    if (SUCCEEDED(d->session->Pause())) ++d->pending;
}

void MediaPlayer::stop() {
    d->playWhenReady = d->wantPaused = false;
    d->queuedSeek = d->shownPos = -1;
    if (!d->session || (m_state != State::Playing && m_state != State::Paused)) return;
    if (SUCCEEDED(d->session->Stop())) ++d->pending;
}

void MediaPlayer::seek(LONGLONG hns) {
    if (m_state != State::Playing && m_state != State::Paused && m_state != State::Stopped && m_state != State::Ended)
        return;
    if (hns < 0) hns = 0;
    if (m_duration > 0 && hns > m_duration) hns = m_duration;
    d->shownPos = hns;
    if (m_state != State::Playing) d->wantPaused = true;    // (seeking a stopped file shows it paused there)
    if (d->pending) { d->queuedSeek = hns; return; }
    start_at(hns, false);
}

bool MediaPlayer::will_play() const {
    switch (m_state) {
        case State::Opening: return d->playWhenReady;
        case State::Playing: case State::Paused: return !d->wantPaused;
        default: return false;
    }
}

LONGLONG MediaPlayer::position() {
    if (d->shownPos >= 0) return d->shownPos;
    if (m_state == State::Ended) return m_duration;
    if (m_state != State::Playing && m_state != State::Paused) return 0;
    MFTIME t = 0;
    if (d->clock && SUCCEEDED(d->clock->GetTime(&t))) return t;
    return 0;
}

void MediaPlayer::set_volume(float amplitude) {
    d->amplitude = amplitude < 0 ? 0 : amplitude > 1 ? 1 : amplitude;
    if (!d->volume) return;
    UINT32 n = 0;
    if (FAILED(d->volume->GetChannelCount(&n)) || !n || n > 32) return;
    float levels[32];
    for (UINT32 i = 0; i < n; ++i) levels[i] = d->amplitude;
    d->volume->SetAllVolumes(n, levels);
}

void MediaPlayer::video_resized() {
    if (!d->display || !d->video) return;
    RECT rc{};
    GetClientRect(d->video, &rc);
    d->display->SetVideoPosition(nullptr, &rc);
}

bool MediaPlayer::video_paint() {
    return d->display && SUCCEEDED(d->display->RepaintVideo());
}

MediaPlayer::Change MediaPlayer::handle(MediaNote* n) {
    if (!n) return Change::None;
    if (n->gen != d->gen) { release_note(n); return Change::None; }    // an earlier file's
    if (n->kind == MediaNote::Source) {
        IUnknown* src = n->source;
        n->source = nullptr;
        HRESULT hr = n->hr;
        m_title = std::move(n->title);
        m_artist = std::move(n->artist);
        m_album = std::move(n->album);
        delete n;
        source_ready(src, hr);
        if (src) src->Release();
        return m_state == State::Failed ? Change::Failed : Change::Opened;
    }
    MediaEventType type = n->type;
    HRESULT hr = n->hr;
    UINT32 topo = n->topoStatus;
    delete n;
    if (hr == MF_E_AUDIO_PLAYBACK_DEVICE_INVALIDATED) return Change::DeviceLost;
    if (FAILED(hr) || type == MEError) {
        fail(hr);
        return Change::Failed;
    }
    State was = m_state;
    auto answered = [&] {
        if (d->pending > 0) --d->pending;
        if (d->pending) return;
        d->shownPos = -1;
        if (d->queuedSeek >= 0) {
            LONGLONG t = d->queuedSeek;
            d->queuedSeek = -1;
            d->shownPos = t;
            start_at(t, false);
        }
    };
    switch (type) {
        case MESessionTopologyStatus:
            if (topo == MF_TOPOSTATUS_READY) topology_ready();
            break;
        case MESessionStarted: m_state = State::Playing; answered(); break;
        case MESessionPaused:  m_state = State::Paused; answered(); break;
        case MESessionStopped: m_state = State::Stopped; answered(); break;
        case MESessionEnded:
            m_state = State::Ended;
            d->pending = 0;
            d->queuedSeek = d->shownPos = -1;
            break;
        default: break;
    }
    return m_state != was ? Change::State : Change::None;
}

// ------------------------------------------------------------------
// decoding for analysis
// ------------------------------------------------------------------
static HRESULT (WINAPI* s_createReader)(IMFMediaSource*, IMFAttributes*, IMFSourceReader**) = nullptr;
static std::once_flag s_readerOnce;

AudioDecoder::~AudioDecoder() {
    if (m_reader) m_reader->Release();
    if (m_source) { m_source->Shutdown(); m_source->Release(); }
}

bool AudioDecoder::open(const std::wstring& path, LONGLONG from) {
    if (s_loaded <= 0) return false;                     // (media_available said no, or wasn't asked)
    std::call_once(s_readerOnce, [] {
        if (HMODULE rw = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            proc(rw, "MFCreateSourceReaderFromMediaSource", s_createReader);
    });
    if (!s_createReader) return false;
    // the source as playback makes it: the file shared for deleting, Opus in MP4 relabelled
    IStream* file = nullptr;
    IMFByteStream* bytes = nullptr;
    IMFSourceResolver* resolver = nullptr;
    IUnknown* obj = nullptr;
    MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
    HRESULT hr = SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE,
                                        nullptr, &file);
    if (SUCCEEDED(hr)) hr = s_mf.CreateMFByteStreamOnStream(file, &bytes);
    if (SUCCEEDED(hr)) hr = s_mf.CreateSourceResolver(&resolver);
    if (SUCCEEDED(hr))
        hr = resolver->CreateObjectFromByteStream(bytes, path.c_str(),
                                                  MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE,
                                                  nullptr, &type, &obj);
    if (SUCCEEDED(hr) && obj) hr = obj->QueryInterface(IID_PPV_ARGS(&m_source));
    release(obj);
    release(resolver);
    release(bytes);
    release(file);
    if (FAILED(hr) || !m_source) return false;
    IMFPresentationDescriptor* pd = nullptr;
    if (SUCCEEDED(m_source->CreatePresentationDescriptor(&pd))) {
        UINT64 dur = 0;
        if (SUCCEEDED(pd->GetUINT64(MF_PD_DURATION, &dur))) m_duration = (LONGLONG)dur;
        DWORD count = 0;
        pd->GetStreamDescriptorCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            BOOL selected = FALSE;
            IMFStreamDescriptor* sd = nullptr;
            if (FAILED(pd->GetStreamDescriptorByIndex(i, &selected, &sd))) continue;
            IMFMediaTypeHandler* th = nullptr;
            GUID major = GUID_NULL;
            if (SUCCEEDED(sd->GetMediaTypeHandler(&th))) {
                if (SUCCEEDED(th->GetMajorType(&major)) && major == MFMediaType_Audio) fix_opus_type(th);
                th->Release();
            }
            sd->Release();
        }
        pd->Release();
    }
    if (FAILED(s_createReader(m_source, nullptr, &m_reader)) || !m_reader) return false;
    const DWORD audio = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    m_reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(m_reader->SetStreamSelection(audio, TRUE))) return false;
    IMFMediaType* want = nullptr;
    if (FAILED(s_mf.CreateMediaType(&want))) return false;
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    hr = m_reader->SetCurrentMediaType(audio, nullptr, want);
    want->Release();
    IMFMediaType* got = nullptr;
    if (FAILED(hr) || FAILED(m_reader->GetCurrentMediaType(audio, &got)) || !got) return false;
    UINT32 ch = 0, rate = 0, bits = 0;
    got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    got->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    got->Release();
    if (!ch || !rate || bits != 32) return false;
    m_channels = ch;
    m_rate = rate;
    if (from > 0) seek(from);
    return true;
}

bool AudioDecoder::seek(LONGLONG to) {
    if (!m_reader) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = to;
    return SUCCEEDED(m_reader->SetCurrentPosition(GUID_NULL, v));
}

bool AudioDecoder::read(std::vector<float>& out, LONGLONG* time) {
    if (!m_reader) return false;
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* s = nullptr;
        HRESULT hr = m_reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &s);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) {
            release(s);
            return false;
        }
        if (!s) continue;                                  // (a gap, a format change)
        IMFMediaBuffer* b = nullptr;
        bool ok = false;
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b)) && b) {
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
                out.insert(out.end(), (const float*)p, (const float*)p + len / sizeof(float));
                b->Unlock();
                ok = true;
            }
            b->Release();
        }
        s->Release();
        if (time) *time = ts;
        return ok;
    }
}
