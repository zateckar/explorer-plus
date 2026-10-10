#include "acc.h"

namespace {

// child ids: CHILDID_SELF (0) is the window, i + 1 the control's child i
class AccObject final : public IAccessible {
public:
    AccObject(HWND hwnd, const AccSource* src) : hwnd_(hwnd), src_(src) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == IID_IAccessible) {
            *ppv = static_cast<IAccessible*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = InterlockedDecrement(&refs_);
        if (!n) delete this;
        return n;
    }

    // IDispatch (MSAA clients use the vtable)
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* n) override { if (n) *n = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override {
        return E_NOTIMPL;
    }

    // IAccessible
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** out) override {
        if (!out) return E_POINTER;
        if (!alive()) return E_FAIL;
        return AccessibleObjectFromWindow(hwnd_, (DWORD)OBJID_WINDOW, IID_IDispatch, (void**)out);
    }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* out) override {
        if (!out) return E_POINTER;
        *out = alive() ? count() : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch** out) override {
        if (out) *out = nullptr;
        return S_FALSE;                     // children are simple elements
    }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT v, BSTR* out) override {
        return text(v, out, [](const AccChild& c) { return c.name; }, [this] { return src_->name; });
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT v, BSTR* out) override {
        return text(v, out, [](const AccChild& c) { return c.value; },
                    [this] { return src_->value ? src_->value() : std::wstring(); });
    }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT, BSTR* out) override {
        if (out) *out = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT v, VARIANT* out) override {
        if (!out) return E_POINTER;
        int i = 0;
        if (!resolve(v, &i)) return E_INVALIDARG;
        out->vt = VT_I4;
        out->lVal = i < 0 ? src_->role : src_->child(i).role;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT v, VARIANT* out) override {
        if (!out) return E_POINTER;
        int i = 0;
        if (!resolve(v, &i)) return E_INVALIDARG;
        long s = 0;
        if (!IsWindowVisible(hwnd_)) s |= STATE_SYSTEM_INVISIBLE;
        if (i < 0) {
            if (GetFocus() == hwnd_) s |= STATE_SYSTEM_FOCUSED;
        } else {
            s |= src_->child(i).state;
            if (GetFocus() == hwnd_ && src_->focus && src_->focus() == i) s |= STATE_SYSTEM_FOCUSED;
        }
        out->vt = VT_I4;
        out->lVal = s;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR* out) override {
        if (out) *out = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* file, VARIANT, long*) override {
        if (file) *file = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT v, BSTR* out) override {
        return text(v, out, [](const AccChild& c) { return c.shortcut; }, [] { return std::wstring(); });
    }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* out) override {
        if (!out) return E_POINTER;
        VariantInit(out);
        if (!alive() || GetFocus() != hwnd_) return S_FALSE;
        int f = src_->focus ? src_->focus() : -1;
        out->vt = VT_I4;
        out->lVal = f >= 0 && f < count() ? f + 1 : CHILDID_SELF;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* out) override {
        if (out) VariantInit(out);
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT v, BSTR* out) override {
        return text(v, out, [](const AccChild& c) { return c.action; }, [] { return std::wstring(); });
    }
    HRESULT STDMETHODCALLTYPE accSelect(long, VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accLocation(long* x, long* y, long* w, long* h, VARIANT v) override {
        if (!x || !y || !w || !h) return E_POINTER;
        int i = 0;
        if (!resolve(v, &i)) return E_INVALIDARG;
        RECT r{};
        if (i < 0) {
            GetWindowRect(hwnd_, &r);
        } else {
            r = src_->child(i).rc;
            MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&r), 2);
        }
        *x = r.left; *y = r.top; *w = r.right - r.left; *h = r.bottom - r.top;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(long dir, VARIANT start, VARIANT* out) override {
        if (!out) return E_POINTER;
        VariantInit(out);
        int i = 0;
        if (!resolve(start, &i)) return E_INVALIDARG;
        int n = count(), to = -2;
        switch (dir) {
            case NAVDIR_FIRSTCHILD: if (i < 0 && n) to = 0; break;
            case NAVDIR_LASTCHILD:  if (i < 0 && n) to = n - 1; break;
            case NAVDIR_NEXT: case NAVDIR_RIGHT: case NAVDIR_DOWN:
                if (i >= 0 && i + 1 < n) to = i + 1;
                break;
            case NAVDIR_PREVIOUS: case NAVDIR_LEFT: case NAVDIR_UP:
                if (i > 0) to = i - 1;
                break;
        }
        if (to < 0) return S_FALSE;
        out->vt = VT_I4;
        out->lVal = to + 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accHitTest(long x, long y, VARIANT* out) override {
        if (!out) return E_POINTER;
        VariantInit(out);
        if (!alive()) return E_FAIL;
        POINT pt{ x, y };
        RECT wr{};
        GetWindowRect(hwnd_, &wr);
        if (!PtInRect(&wr, pt)) return S_FALSE;
        ScreenToClient(hwnd_, &pt);
        out->vt = VT_I4;
        out->lVal = CHILDID_SELF;
        for (int i = 0, n = count(); i < n; ++i) {
            RECT r = src_->child(i).rc;
            if (PtInRect(&r, pt)) { out->lVal = i + 1; break; }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT v) override {
        int i = 0;
        if (!resolve(v, &i)) return E_INVALIDARG;
        if (i < 0 || !src_->action || src_->child(i).action.empty()) return DISP_E_MEMBERNOTFOUND;
        if (src_->child(i).state & STATE_SYSTEM_UNAVAILABLE) return E_FAIL;
        src_->action(i);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }

private:
    bool alive() const { return IsWindow(hwnd_) != FALSE; }
    int count() const { return src_->count ? src_->count() : 0; }

    // VT_I4 child id -> -1 (self) or 0-based index
    bool resolve(const VARIANT& v, int* i) const {
        if (v.vt != VT_I4 || !alive()) return false;
        if (v.lVal == CHILDID_SELF) { *i = -1; return true; }
        if (v.lVal < 1 || v.lVal > count()) return false;
        *i = v.lVal - 1;
        return true;
    }

    template <class FChild, class FSelf>
    HRESULT text(const VARIANT& v, BSTR* out, FChild ofChild, FSelf ofSelf) {
        if (!out) return E_POINTER;
        *out = nullptr;
        int i = 0;
        if (!resolve(v, &i)) return E_INVALIDARG;
        std::wstring s = i < 0 ? ofSelf() : ofChild(src_->child(i));
        if (s.empty()) return S_FALSE;
        *out = SysAllocStringLen(s.data(), (UINT)s.size());
        return *out ? S_OK : E_OUTOFMEMORY;
    }

    LONG refs_ = 1;
    HWND hwnd_;
    const AccSource* src_;
};

}  // namespace

LRESULT acc_get_object(HWND hwnd, WPARAM wp, LPARAM lp, const AccSource* src) {
    if ((LONG)lp != (LONG)OBJID_CLIENT || !src) return DefWindowProcW(hwnd, WM_GETOBJECT, wp, lp);
    auto* obj = new AccObject(hwnd, src);
    LRESULT r = LresultFromObject(IID_IAccessible, wp, obj);
    obj->Release();
    return r;
}

void acc_notify(HWND hwnd, DWORD event, int child) {
    if (hwnd) NotifyWinEvent(event, hwnd, OBJID_CLIENT, child < 0 ? CHILDID_SELF : child + 1);
}
