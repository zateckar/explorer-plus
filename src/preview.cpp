#include "preview.h"
#include "theme.h"

// The Preview pane hosts the shell preview handler registered for the file's type
// (the text previewer for .txt / .cpp / .xml, the HTML previewer, Office, PDF readers,
// ...). Like Explorer, handlers run out of process (prevhost.exe) where their
// registration allows it, so a crashing handler cannot take the app down.

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
        if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(h.put()))) || !h)
            return false;
    }
    if (FAILED(initialize(h.get(), si.get(), path))) return false;
    apply_visuals(h.get());
    RECT rc{};
    GetClientRect(host, &rc);
    if (FAILED(h->SetWindow(host, &rc)) || FAILED(h->DoPreview())) {
        h->Unload();
        return false;
    }
    s_handler = std::move(h);
    s_hostWnd = host;
    return true;
}

void preview_close() {
    if (!s_handler) return;
    s_handler->Unload();
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
