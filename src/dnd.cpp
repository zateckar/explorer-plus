#include "dnd.h"
#include "listview.h"
#include "tree.h"
#include "detailspane.h"

// ---------------------------------------------------------------
// IDropSource

class DropSource final : public IDropSource {
    LONG m_ref = 1;
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return (ULONG)r;
    }
    IFACEMETHODIMP QueryContinueDrag(BOOL esc, DWORD keys) override {
        if (esc) return DRAGDROP_S_CANCEL;
        // dropping happens when the button that started the drag is released
        if (!(keys & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
        return S_OK;
    }
    IFACEMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

// ---------------------------------------------------------------
// IDropTarget for the list and tree panes. Each hover target (a folder row, a tree
// node or the list background) is resolved to the shell's own IDropTarget, and the
// OLE calls are forwarded to it — so default effects (move on the same volume, copy
// across volumes), right-drag menus, drops on archives/programs and progress UI are
// exactly Explorer's.

class PaneDropTarget final : public IDropTarget {
    LONG m_ref = 1;
    HWND m_hwnd;
    bool m_isTree;
    com_ptr<IDataObject> m_pdo;
    com_ptr<IDropTargetHelper> m_helper;
    com_ptr<IDropTarget> m_target;       // shell target under the cursor
    ULONGLONG m_targetKey = 0;           // identity of the hover target
    DWORD m_keys = 0;
    HTREEITEM m_treeHover = nullptr;

public:
    PaneDropTarget(HWND hwnd, bool isTree) : m_hwnd(hwnd), m_isTree(isTree) {
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(m_helper.put()));
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return (ULONG)r;
    }

private:
    // Drop target of `childAbs` (an item) or, with childAbs == nullptr, of the
    // current folder's background.
    static HRESULT target_for_item(PCIDLIST_ABSOLUTE abs, IDropTarget** out) {
        *out = nullptr;
        if (pidl_is_desktop(abs)) {
            com_ptr<IShellFolder> desk;
            if (FAILED(SHGetDesktopFolder(desk.put()))) return E_FAIL;
            return desk->CreateViewObject(g_main, IID_PPV_ARGS(out));
        }
        com_ptr<IShellFolder> parent;
        PCUITEMID_CHILD child = nullptr;
        HRESULT hr = bind_parent(abs, parent.put(), &child);
        if (FAILED(hr)) return hr;
        return parent->GetUIObjectOf(g_main, 1, &child, IID_IDropTarget, nullptr, (void**)out);
    }
    static HRESULT target_for_background(IDropTarget** out) {
        *out = nullptr;
        com_ptr<IShellFolder> psf;
        HRESULT hr = bind_list(g_pane->current, IID_PPV_ARGS(psf.put()));
        if (FAILED(hr)) return hr;
        return psf->CreateViewObject(g_main, IID_PPV_ARGS(out));
    }

    // resolve the target under pt; returns a key identifying it (0 = none)
    ULONGLONG resolve(POINTL pt, com_ptr<IDropTarget>& out) {
        POINT c{ pt.x, pt.y };
        ScreenToClient(m_hwnd, &c);
        if (m_isTree) {
            TVHITTESTINFO th{}; th.pt = c;
            HTREEITEM h = TreeView_HitTest(m_hwnd, &th);
            if (h != m_treeHover) { TreeView_SelectDropTarget(m_hwnd, h); m_treeHover = h; }
            if (!h || !(th.flags & TVHT_ONITEM)) return 0;
            PIDLIST_ABSOLUTE abs = tree_get_pidl_of_item(h);
            if (!abs) return 0;         // a separator (nullptr isn't the Desktop here)
            ULONGLONG key = pidl_sig(abs) | 1;
            if (key != m_targetKey) target_for_item(abs, out.put());
            if (abs) CoTaskMemFree(abs);
            return key;
        }
        PaneScope ps(pane_of_list(m_hwnd));      // (either pane's list)
        LVHITTESTINFO ht{}; ht.pt = c;
        int idx = ListView_HitTest(m_hwnd, &ht);
        PIDLIST_ABSOLUTE abs = nullptr;
        // only rows that accept drops (folders, archives, programs) are targets, and
        // never the rows being dragged
        if (idx >= 0 && (ht.flags & LVHT_ONITEM) &&
            !(ListView_GetItemState(m_hwnd, idx, LVIS_SELECTED) & LVIS_SELECTED)) {
            std::lock_guard<std::mutex> lk(g_pane->itemsMx);
            if (idx < (int)g_pane->items.size() && (g_pane->items[idx]->attrs & SFGAO_DROPTARGET))
                abs = ILCombine(g_pane->current, g_pane->items[idx]->pidl);
        }
        ULONGLONG key = abs ? (pidl_sig(abs) | 1) : 2;     // 2 = background
        if (key != m_targetKey) {
            if (abs) target_for_item(abs, out.put());
            else target_for_background(out.put());
        }
        if (abs) CoTaskMemFree(abs);
        return key;
    }

    // switch the forwarded-to target when the hover target changes
    void retarget(POINTL pt, DWORD keys, DWORD* peffect) {
        com_ptr<IDropTarget> fresh;
        ULONGLONG key = resolve(pt, fresh);
        if (key == m_targetKey) return;
        if (m_target) m_target->DragLeave();
        m_target = std::move(fresh);
        m_targetKey = key;
        if (m_target) {
            DWORD eff = *peffect;
            if (FAILED(m_target->DragEnter(m_pdo.get(), keys, pt, &eff))) m_target.reset();
        }
    }

    void end_hover() {
        if (m_isTree) { TreeView_SelectDropTarget(m_hwnd, nullptr); m_treeHover = nullptr; }
        m_target.reset();
        m_targetKey = 0;
    }

public:
    IFACEMETHODIMP DragEnter(IDataObject* pdo, DWORD keys, POINTL pt, DWORD* peffect) override {
        if (!peffect) return E_INVALIDARG;
        m_pdo.attach(pdo); if (pdo) pdo->AddRef();
        m_keys = keys;
        DWORD allowed = *peffect;
        m_targetKey = 0;
        retarget(pt, keys, peffect);
        DWORD eff = allowed;
        if (m_target) m_target->DragOver(keys, pt, &eff); else eff = DROPEFFECT_NONE;
        *peffect = eff;
        if (m_helper) { POINT p{ pt.x, pt.y }; m_helper->DragEnter(m_hwnd, pdo, &p, eff); }
        return S_OK;
    }

    IFACEMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* peffect) override {
        if (!peffect) return E_INVALIDARG;
        m_keys = keys;
        DWORD allowed = *peffect;
        retarget(pt, keys, peffect);
        DWORD eff = allowed;
        if (m_target) m_target->DragOver(keys, pt, &eff); else eff = DROPEFFECT_NONE;
        *peffect = eff;
        if (m_helper) { POINT p{ pt.x, pt.y }; m_helper->DragOver(&p, eff); }
        return S_OK;
    }

    IFACEMETHODIMP DragLeave() override {
        if (m_helper) m_helper->DragLeave();
        if (m_target) m_target->DragLeave();
        end_hover();
        m_pdo.reset();
        return S_OK;
    }

    IFACEMETHODIMP Drop(IDataObject* pdo, DWORD keys, POINTL pt, DWORD* peffect) override {
        if (!peffect) return E_INVALIDARG;
        if (m_helper) { POINT p{ pt.x, pt.y }; m_helper->Drop(pdo, &p, *peffect); }
        DWORD allowed = *peffect;
        retarget(pt, m_keys, peffect);
        HRESULT hr = S_OK;
        if (m_target) {
            DWORD eff = allowed;
            detailspane_release_files();    // a moved file may be the one previewed
            hr = m_target->Drop(pdo, keys, pt, &eff);
            *peffect = eff;
        } else {
            *peffect = DROPEFFECT_NONE;
        }
        end_hover();
        m_pdo.reset();
        return hr;
    }
};

// ---------------------------------------------------------------
// registration

static bool s_registered = false;

void dnd_register_list(HWND list) {
    if (!s_registered || !list) return;
    auto* lt = new PaneDropTarget(list, false);
    RegisterDragDrop(list, lt);           // OLE holds its own references
    lt->Release();
}

void dnd_register_sources() {
    if (!g_tree) return;
    s_registered = true;
    for (Pane& p : g_panes) dnd_register_list(p.list);
    auto* tt = new PaneDropTarget(g_tree, true);
    RegisterDragDrop(g_tree, tt);
    tt->Release();
}

void dnd_unregister_all() {
    for (Pane& p : g_panes)
        if (p.list) RevokeDragDrop(p.list);
    if (g_tree) RevokeDragDrop(g_tree);
}

// begin dragging the list selection outward
void dnd_begin_list_drag() {
    auto sel = list_selected_pidls();
    if (sel.empty()) return;
    com_ptr<IShellFolder> psf;
    if (FAILED(bind_list(g_pane->current, IID_PPV_ARGS(psf.put()))) || !psf) return;
    com_ptr<IDataObject> pdo;
    if (FAILED(dataobject_from_children(g_main, psf.get(), sel, pdo.put())) || !pdo) return;
    DWORD effect = 0;
    DropSource* src = new DropSource;
    detailspane_release_files();        // the drop may move the previewed file elsewhere
    SHDoDragDrop(g_main, pdo.get(), src, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    src->Release();
}
