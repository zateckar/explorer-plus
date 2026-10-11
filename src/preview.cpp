#include "preview.h"
#include "theme.h"

// The Preview pane hosts the shell preview handler registered for the file's type
// (the text previewer for .txt / .cpp / .xml, the HTML previewer, Office, PDF readers,
// ...). Like Explorer, handlers run out of process (prevhost.exe), so a crashing handler
// cannot take the app down; only a handler that can't run anywhere else (or asks for
// it) is loaded into the app itself.

static const wchar_t kPreviewHandlerKey[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";

static com_ptr<IPreviewHandler> s_handler;
static HWND s_hostWnd = nullptr;

static bool handler_for(const std::wstring& ext, CLSID* clsid) {
    WCHAR buf[64]{};
    DWORD n = ARRAYSIZE(buf);
    if (FAILED(AssocQueryStringW(ASSOCF_INIT_DEFAULTTOSTAR, ASSOCSTR_SHELLEXTENSION, ext.c_str(),
                                 kPreviewHandlerKey, buf, &n)))
        return false;
    return SUCCEEDED(CLSIDFromString(buf, clsid));
}

// May the handler run inside this process? Only when it has no surrogate to run in (no
// AppID: out of process it can't start at all) or its registration asks for it. A
// handler that failed to start out of process for another reason (prevhost couldn't
// start, RPC trouble) is not loaded in here instead.
static bool inproc_allowed(const CLSID& clsid) {
    WCHAR s[64]{};
    if (!StringFromGUID2(clsid, s, ARRAYSIZE(s))) return false;
    std::wstring key = std::wstring(L"CLSID\\") + s;
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), L"DisableProcessIsolation", RRF_RT_REG_DWORD, nullptr,
                     &v, &cb) == ERROR_SUCCESS && v)
        return true;
    WCHAR appId[64]{};
    cb = sizeof(appId);
    return RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), L"AppID", RRF_RT_REG_SZ, nullptr, appId, &cb) != ERROR_SUCCESS;
}

// hand the file to the handler the way it asks for it: stream, shell item or path
static HRESULT initialize(IPreviewHandler* h, IShellItem* si, const std::wstring& path) {
    com_ptr<IInitializeWithStream> ws;
    if (SUCCEEDED(h->QueryInterface(IID_PPV_ARGS(ws.put()))) && ws) {
        com_ptr<IStream> stm;
        HRESULT hr = SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE,
                                            nullptr, stm.put());
        return SUCCEEDED(hr) ? ws->Initialize(stm.get(), STGM_READ) : hr;
    }
    com_ptr<IInitializeWithItem> wi;
    if (SUCCEEDED(h->QueryInterface(IID_PPV_ARGS(wi.put()))) && wi) return wi->Initialize(si, STGM_READ);
    com_ptr<IInitializeWithFile> wf;
    if (SUCCEEDED(h->QueryInterface(IID_PPV_ARGS(wf.put()))) && wf) return wf->Initialize(path.c_str(), STGM_READ);
    return E_NOINTERFACE;
}

static void apply_visuals(IPreviewHandler* h) {
    com_ptr<IPreviewHandlerVisuals> v;
    if (FAILED(h->QueryInterface(IID_PPV_ARGS(v.put()))) || !v) return;
    const Palette& p = pal();
    v->SetBackgroundColor(p.content);
    v->SetTextColor(p.text);
    LOGFONTW lf{};
    if (GetObjectW(g_font, sizeof(lf), &lf)) v->SetFont(&lf);
}

// ------------------------------------------------------------------
// The frame the handler talks back to. Keys pressed inside the preview (whose window
// belongs to prevhost.exe: the app's message loop never sees them) come here when the
// handler doesn't use them itself: the app's shortcuts, and Tab / F6 to leave.

// TranslateAccelerator here runs inside the handler's call: the command is posted (run
// after the call returns) rather than sent, so it can close or replace the handler.
static HWND s_relay = nullptr;

static LRESULT CALLBACK relay_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND) { PostMessageW(g_main, WM_COMMAND, wp, lp); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND relay_window() {
    if (s_relay) return s_relay;
    WNDCLASSW wc{};
    wc.lpfnWndProc = relay_proc;
    wc.hInstance = g_hinst;
    wc.lpszClassName = L"ExplorerPlusAccelRelay";
    RegisterClassW(&wc);
    s_relay = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hinst, nullptr);
    return s_relay;
}

struct PreviewFrame : IPreviewHandlerFrame {
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IPreviewHandlerFrame) { *ppv = this; return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }     // static instance
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP GetWindowContext(PREVIEWHANDLERFRAMEINFO* info) override {
        if (!info) return E_POINTER;
        info->haccel = app_accelerators(&info->cAccelEntries);
        return info->haccel ? S_OK : E_FAIL;
    }
    STDMETHODIMP TranslateAccelerator(MSG* msg) override {
        if (!msg || (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN)) return S_FALSE;
        bool ctrl = GetKeyState(VK_CONTROL) & 0x8000, alt = GetKeyState(VK_MENU) & 0x8000;
        if ((msg->wParam == VK_TAB || msg->wParam == VK_F6) && !ctrl && !alt) {
            // the end of the preview's own tab order: back to the file list
            PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(IDM_FOCUS_LIST, 0), 0);
            return S_OK;
        }
        HACCEL acc = app_accelerators(nullptr);
        HWND relay = relay_window();
        return acc && relay && ::TranslateAcceleratorW(relay, acc, msg) ? S_OK : S_FALSE;
    }
};
static PreviewFrame s_frame;

static void set_site(IPreviewHandler* h, IUnknown* site) {
    com_ptr<IObjectWithSite> ows;
    if (SUCCEEDED(h->QueryInterface(IID_PPV_ARGS(ows.put()))) && ows) ows->SetSite(site);
}

bool preview_open(HWND host, PCIDLIST_ABSOLUTE item) {
    preview_close();
    if (!host || !item) return false;
    com_ptr<IShellItem> si;
    if (FAILED(SHCreateItemFromIDList(item, IID_PPV_ARGS(si.put()))) || !si) return false;
    SFGAOF a = 0;
    si->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM | SFGAO_FILESYSTEM, &a);
    if ((a & SFGAO_FOLDER) && !(a & SFGAO_STREAM)) return false;      // folders: nothing to preview
    PWSTR p = nullptr;
    if (FAILED(si->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return false;
    std::wstring path = p;
    CoTaskMemFree(p);
    CLSID clsid{};
    if (!handler_for(PathFindExtensionW(path.c_str()), &clsid)) return false;

    com_ptr<IPreviewHandler> h;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(h.put()))) || !h) {
        h.reset();
        if (!inproc_allowed(clsid) ||
            FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(h.put()))) || !h)
            return false;
    }
    set_site(h.get(), &s_frame);
    if (FAILED(initialize(h.get(), si.get(), path))) { set_site(h.get(), nullptr); return false; }
    apply_visuals(h.get());
    RECT rc{};
    GetClientRect(host, &rc);
    if (FAILED(h->SetWindow(host, &rc)) || FAILED(h->DoPreview())) {
        h->Unload();
        set_site(h.get(), nullptr);
        return false;
    }
    s_handler = std::move(h);
    s_hostWnd = host;
    return true;
}

void preview_close() {
    if (!s_handler) return;
    s_handler->Unload();
    set_site(s_handler.get(), nullptr);
    s_handler.reset();
    s_hostWnd = nullptr;
}

bool preview_active() { return s_handler.get() != nullptr; }

void preview_resize() {
    if (!s_handler || !s_hostWnd) return;
    RECT rc{};
    GetClientRect(s_hostWnd, &rc);
    s_handler->SetRect(&rc);
}

void preview_colors_changed() {
    if (s_handler) apply_visuals(s_handler.get());
}

bool preview_has_focus() {
    HWND f = GetFocus();
    return s_handler && s_hostWnd && f && (f == s_hostWnd || IsChild(s_hostWnd, f));
}

bool preview_focus() {
    return s_handler && SUCCEEDED(s_handler->SetFocus());
}

bool preview_translate_accelerator(MSG* msg) {
    // an in-process handler's window gets its keys through the app's loop: it sees them
    // first (as it would in Explorer), out of process it gets them itself
    return preview_has_focus() && s_handler->TranslateAccelerator(msg) == S_OK;
}
