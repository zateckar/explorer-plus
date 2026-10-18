#include "appstate.h"
#include "searchbox.h"
#include "navigation.h"
#include "tabs.h"
#include <structuredquerycondition.h>
#include <propvarutil.h>
#include <cwctype>
#include <unordered_set>

// ------------------------------------------------------------------
// conditions: IConditionFactory (structuredquery.h, missing from MinGW)

MIDL_INTERFACE("A5EFE073-B16F-474f-9F3E-9F8B497A3E08")
IConditionFactory : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE MakeNot(ICondition* sub, BOOL simplify, ICondition** out) = 0;
    virtual HRESULT STDMETHODCALLTYPE MakeAndOr(CONDITION_TYPE ct, IEnumUnknown* subs, BOOL simplify, ICondition** out) = 0;
    virtual HRESULT STDMETHODCALLTYPE MakeLeaf(LPCWSTR propName, CONDITION_OPERATION op, LPCWSTR valueType,
                                               const PROPVARIANT* value, IUnknown* nameTerm, IUnknown* opTerm,
                                               IUnknown* valueTerm, BOOL expand, ICondition** out) = 0;
    virtual HRESULT STDMETHODCALLTYPE Resolve(ICondition* c, int options, const SYSTEMTIME* ref, ICondition** out) = 0;
};
static const CLSID kCLSID_ConditionFactory =
    { 0xE03E85B0, 0x7BE3, 0x4000, { 0xBA, 0x98, 0x6C, 0x13, 0xDE, 0x9F, 0xA4, 0x86 } };
static const IID kIID_IConditionFactory =
    { 0xA5EFE073, 0xB16F, 0x474F, { 0x9F, 0x3E, 0x9F, 0x8B, 0x49, 0x7A, 0x3E, 0x08 } };
static const CONDITION_TYPE kAnd = (CONDITION_TYPE)0;          // CT_AND_CONDITION

// minimal IEnumUnknown over the word conditions (for MakeAndOr)
class UnknownList final : public IEnumUnknown {
    LONG m_ref = 1;
    std::vector<IUnknown*> m_items;
    size_t m_pos = 0;
public:
    explicit UnknownList(std::vector<IUnknown*> items) : m_items(std::move(items)) {
        for (auto* p : m_items) p->AddRef();
    }
    ~UnknownList() { for (auto* p : m_items) p->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IEnumUnknown) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG n, IUnknown** out, ULONG* fetched) override {
        ULONG got = 0;
        while (got < n && m_pos < m_items.size()) { out[got] = m_items[m_pos++]; out[got]->AddRef(); ++got; }
        if (fetched) *fetched = got;
        return got == n ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG n) override { m_pos += n; return m_pos <= m_items.size() ? S_OK : S_FALSE; }
    HRESULT STDMETHODCALLTYPE Reset() override { m_pos = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumUnknown** out) override {
        auto* c = new UnknownList(m_items);
        c->m_pos = m_pos;
        *out = c;
        return S_OK;
    }
};

// words ("quoted phrases" stay whole); each must occur in the name; * and ? are wildcards
static std::vector<std::wstring> split_terms(const std::wstring& q) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool quoted = false;
    for (wchar_t c : q) {
        if (c == L'"') { quoted = !quoted; continue; }
        if (!quoted && iswspace(c)) { if (!cur.empty()) out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static HRESULT make_condition(const std::wstring& query, ICondition** out) {
    *out = nullptr;
    com_ptr<IConditionFactory> cf;
    HRESULT hr = CoCreateInstance(kCLSID_ConditionFactory, nullptr, CLSCTX_INPROC_SERVER, kIID_IConditionFactory,
                                  (void**)cf.put());
    if (FAILED(hr) || !cf) return FAILED(hr) ? hr : E_FAIL;
    std::vector<com_ptr<ICondition>> leaves;
    for (auto& term : split_terms(query)) {
        bool wild = term.find_first_of(L"*?") != std::wstring::npos;
        PROPVARIANT pv;
        if (FAILED(InitPropVariantFromString(term.c_str(), &pv))) continue;
        com_ptr<ICondition> leaf;
        hr = cf->MakeLeaf(wild ? L"System.FileName" : L"System.ItemNameDisplay",
                          wild ? COP_DOSWILDCARDS : COP_VALUE_CONTAINS, nullptr, &pv,
                          nullptr, nullptr, nullptr, FALSE, leaf.put());
        PropVariantClear(&pv);
        if (SUCCEEDED(hr) && leaf) leaves.push_back(std::move(leaf));
    }
    if (leaves.empty()) return E_INVALIDARG;
    if (leaves.size() == 1) { *out = leaves[0].detach(); return S_OK; }
    std::vector<IUnknown*> subs;
    for (auto& l : leaves) subs.push_back(l.get());
    UnknownList* list = new UnknownList(subs);
    hr = cf->MakeAndOr(kAnd, list, TRUE, out);
    list->Release();
    return hr;
}

// ------------------------------------------------------------------
// searches started in this session: result folder -> scope + query

struct SearchRec {
    ULONGLONG sig = 0;
    PIDLIST_ABSOLUTE scope = nullptr;
    std::wstring query;
};
static std::vector<SearchRec> s_searches;
static bool s_settingText = false;          // our own SetWindowText: no new search

static const SearchRec* find_rec(PCIDLIST_ABSOLUTE abs) {
    if (!abs) return nullptr;
    ULONGLONG sig = pidl_sig(abs);
    for (auto& r : s_searches) if (r.sig == sig) return &r;
    return nullptr;
}

bool search_is_results(PCIDLIST_ABSOLUTE abs) { return find_rec(abs) != nullptr; }
PCIDLIST_ABSOLUTE search_scope_of(PCIDLIST_ABSOLUTE abs) {
    const SearchRec* r = find_rec(abs);
    return r ? r->scope : nullptr;
}

PIDLIST_ABSOLUTE search_real_location(PCIDLIST_ABSOLUTE resultItem) {
    com_ptr<IShellItem> si;
    if (FAILED(SHCreateItemFromIDList(resultItem, IID_PPV_ARGS(si.put()))) || !si) return nullptr;
    PWSTR path = nullptr;
    if (FAILED(si->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &path)) || !path) return nullptr;
    PIDLIST_ABSOLUTE real = nullptr;
    if (FAILED(SHParseDisplayName(path, nullptr, &real, 0, nullptr))) real = nullptr;
    CoTaskMemFree(path);
    return real;
}

static std::wstring box_text() {
    int n = GetWindowTextLengthW(g_search);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(g_search, s.data(), n + 1);
    s.resize(n);
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

// result folder for `query` under `scope` (nullptr on failure)
static PIDLIST_ABSOLUTE make_search(PCIDLIST_ABSOLUTE scope, const std::wstring& query) {
    com_ptr<ICondition> cond;
    if (FAILED(make_condition(query, cond.put())) || !cond) return nullptr;
    com_ptr<ISearchFolderItemFactory> f;
    if (FAILED(CoCreateInstance(CLSID_SearchFolderItemFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(f.put()))) || !f)
        return nullptr;
    com_ptr<IShellItemArray> arr;
    if (FAILED(SHCreateShellItemArrayFromIDLists(1, (PCIDLIST_ABSOLUTE_ARRAY)&scope, arr.put())) || !arr)
        return nullptr;
    std::wstring name = L"Search Results in " + pidl_abs_dispname(scope);
    PIDLIST_ABSOLUTE out = nullptr;
    if (FAILED(f->SetScope(arr.get())) || FAILED(f->SetCondition(cond.get())) ||
        FAILED(f->SetDisplayName(name.c_str())) || FAILED(f->GetIDList(&out)))
        return nullptr;
    return out;
}

void search_submit() {
    KillTimer(g_main, TIMER_SEARCH);
    if (!g_search) return;
    std::wstring q = box_text();
    if (q.empty()) { search_cancel(); return; }
    const SearchRec* cur = find_rec(g_pane->current);
    if (cur && cur->query == q) return;                    // already showing it
    const bool refining = cur != nullptr;
    PCIDLIST_ABSOLUTE scope = refining ? cur->scope : g_pane->current;
    PIDLIST_ABSOLUTE results = make_search(scope, q);
    if (!results) { MessageBeep(MB_ICONASTERISK); return; }
    SearchRec r;
    r.sig = pidl_sig(results);
    r.scope = pidl_clone(scope);
    r.query = q;
    s_searches.push_back(r);                               // (cur / scope may dangle from here on)
    if (s_searches.size() > 64) {
        // oldest first, but none a tab shows or has in its history (Back to it would find
        // no search box text and no scope, and the tab was saved with a name that doesn't parse)
        std::vector<PCIDLIST_ABSOLUTE> used;
        tabmodel_all_folders(used);
        std::unordered_set<ULONGLONG> held;
        for (auto p : used) held.insert(pidl_sig(p));
        for (size_t i = 0; i + 1 < s_searches.size() && s_searches.size() > 64;) {
            if (held.count(s_searches[i].sig)) { ++i; continue; }
            CoTaskMemFree(s_searches[i].scope);
            s_searches.erase(s_searches.begin() + i);
        }
    }
    if (refining) {
        // refining a search replaces the results in place (no extra history entry)
        navigate_in_place(results);
    } else {
        navigate_owned(results);
    }
}

void search_cancel() {
    KillTimer(g_main, TIMER_SEARCH);
    const SearchRec* cur = find_rec(g_pane->current);
    if (!cur) return;
    // back to the searched folder: through history when it is right behind us
    TabState* t = tabmodel_active();
    if (t && tabmodel_can_back() && pidl_equal(t->nav().back.back(), cur->scope)) navigation_go_back();
    else navigate_to(cur->scope);
}

void search_text_changed() {
    if (s_settingText) return;
    // search as you type, after a short pause (Enter searches at once)
    SetTimer(g_main, TIMER_SEARCH, 700, nullptr);
}

void search_on_timer() {
    KillTimer(g_main, TIMER_SEARCH);
    if (GetFocus() != g_search) return;                    // the user moved on
    if (box_text().empty()) search_cancel();
    else search_submit();
}

void search_on_navigated() {
    if (!g_search) return;
    const SearchRec* r = find_rec(g_pane->current);
    std::wstring want = r ? r->query : std::wstring();
    if (box_text() != want) {
        s_settingText = true;
        SetWindowTextW(g_search, want.c_str());
        s_settingText = false;
    }
    search_update_cue();
}
