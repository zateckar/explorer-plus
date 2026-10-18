#include "sync_local.h"
#include "crypto.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <vector>

namespace sync {

uint64_t ft64(const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }
FILETIME ft_of(uint64_t v) { FILETIME f; f.dwLowDateTime = (DWORD)v; f.dwHighDateTime = (DWORD)(v >> 32); return f; }
uint64_t now_ft() { FILETIME f; GetSystemTimeAsFileTime(&f); return ft64(f); }

static void scan_dir(const std::wstring& root, const RelPath& rel, const syncpath::Ignore& ig, ScanResult& out) {
    std::wstring full = syncpath::to_local(root, rel);
    HANDLE h = CreateFileW(full.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (rel.empty()) out.rootMissing = true;
        else {
            out.problems.push_back({ rel, L"The folder can't be read." });
            out.unknown.push_back(rel);
        }
        return;
    }
    std::vector<uint8_t> buf(64 * 1024);
    std::vector<RelPath> dirs;
    FILE_INFO_BY_HANDLE_CLASS cls = FileIdBothDirectoryRestartInfo;
    for (;;) {
        if (!GetFileInformationByHandleEx(h, cls, buf.data(), (DWORD)buf.size())) {
            if (GetLastError() != ERROR_NO_MORE_FILES) {
                if (rel.empty()) out.rootIncomplete = true;
                else {
                    out.problems.push_back({ rel, L"The folder couldn't be listed completely." });
                    out.unknown.push_back(rel);
                }
            }
            break;
        }
        cls = FileIdBothDirectoryInfo;
        auto* p = (FILE_ID_BOTH_DIR_INFO*)buf.data();
        for (;;) {
            std::wstring name(p->FileName, p->FileNameLength / sizeof(wchar_t));
            if (name != L"." && name != L"..") {
                bool isDir = (p->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                bool link = false;
                if (p->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    DWORD tag = p->EaSize;                   // holds the reparse tag for reparse points
                    link = tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT || tag == 0x80000023 /* AF_UNIX */;
                }
                std::wstring nfcName = syncpath::nfc(name);
                RelPath child = join_path(rel, nfcName);
                if (link && !ig.ignored(child, isDir)) out.unknown.push_back(child);   // (what it replaced stays as it is)
                else if (nfcName != name && !ig.ignored(child, isDir)) {
                    // a name not in the composed form (NFD: from a Mac): its paths here aren't the
                    // ones the storage has, so it isn't synced, and neither side is touched there
                    out.problems.push_back({ child, L"The name's accents are stored decomposed (as a Mac writes them): "
                                                    L"not synced. Renaming it here fixes that." });
                    out.unknown.push_back(child);
                } else if (!link && !ig.ignored(child, isDir)) {
                    LocalItem it;
                    it.path = child;
                    it.isDir = isDir;
                    it.size = isDir ? 0 : (uint64_t)p->EndOfFile.QuadPart;
                    it.mtime = isDir ? 0 : (uint64_t)p->LastWriteTime.QuadPart;
                    it.fileId = (uint64_t)p->FileId.QuadPart;
                    it.attrs = p->FileAttributes;
                    out.items[key_of(child)] = it;
                    if (isDir) dirs.push_back(child);
                }
            }
            if (!p->NextEntryOffset) break;
            p = (FILE_ID_BOTH_DIR_INFO*)((uint8_t*)p + p->NextEntryOffset);
        }
    }
    CloseHandle(h);
    for (auto& d : dirs) scan_dir(root, d, ig, out);
}

ScanResult scan_local(const std::wstring& root, const syncpath::Ignore& ignore) {
    ScanResult r;
    DWORD a = GetFileAttributesW(syncpath::to_local(root, L"").c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { r.rootMissing = true; return r; }
    scan_dir(root, L"", ignore, r);
    return r;
}

std::string root_identity(const std::wstring& root) {
    HANDLE h = CreateFileW(syncpath::to_local(root, L"").c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION fi{};
    bool ok = GetFileInformationByHandle(h, &fi) != 0;
    CloseHandle(h);
    if (!ok) return {};
    char buf[64];
    snprintf(buf, sizeof(buf), "%08lx-%08lx%08lx", fi.dwVolumeSerialNumber, fi.nFileIndexHigh, fi.nFileIndexLow);
    return buf;
}

HANDLE open_for_read(const std::wstring& full) {
    // (deleting or renaming it meanwhile stays possible: the next pass sees that)
    return CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                       nullptr);
}

bool file_identity(HANDLE h, uint64_t& size, uint64_t& mtime, uint64_t& fileId, uint32_t* attrs) {
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(h, &fi)) return false;
    size = ((uint64_t)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
    mtime = ft64(fi.ftLastWriteTime);
    fileId = ((uint64_t)fi.nFileIndexHigh << 32) | fi.nFileIndexLow;
    if (attrs) *attrs = fi.dwFileAttributes;
    return true;
}

bool hash_handle(HANDLE h, Hash& out, uint64_t* size) {
    LARGE_INTEGER z{};
    if (!SetFilePointerEx(h, z, nullptr, FILE_BEGIN)) return false;
    crypto::Blake2b b;
    std::vector<uint8_t> buf(1 << 20);
    uint64_t n = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr)) return false;
        if (!got) break;
        b.update(buf.data(), got);
        n += got;
    }
    out = b.final();
    if (size) *size = n;
    return SetFilePointerEx(h, z, nullptr, FILE_BEGIN) != 0;
}

bool make_dirs(const std::wstring& full) {
    if (CreateDirectoryW(full.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return true;
    if (GetLastError() != ERROR_PATH_NOT_FOUND) return false;
    size_t s = full.find_last_of(L'\\');
    if (s == std::wstring::npos || s < 7) return false;
    if (!make_dirs(full.substr(0, s))) return false;
    return CreateDirectoryW(full.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

namespace {
// refuses what the shell would delete for good instead of recycling (no Recycle Bin on that
// drive, a policy, a file too big for it): with FOF_NO_UI it would do so without asking
struct RecycleOnly : IFileOperationProgressSink {
    bool refused = false;
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IFileOperationProgressSink) { *ppv = this; return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }          // (on the stack)
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP PreDeleteItem(DWORD flags, IShellItem*) override {
        if (flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) return S_OK;
        refused = true;
        return E_ABORT;
    }
    STDMETHODIMP StartOperations() override { return S_OK; }
    STDMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
    STDMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    STDMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    STDMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    STDMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return S_OK; }
    STDMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    STDMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    STDMETHODIMP ResetTimer() override { return S_OK; }
    STDMETHODIMP PauseTimer() override { return S_OK; }
    STDMETHODIMP ResumeTimer() override { return S_OK; }
};

// (on a thread of its own: IFileOperation wants a single-threaded apartment)
bool shell_recycle(const std::wstring& path) {
    bool ok = false;
    std::thread([&] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return;
        IFileOperation* op = nullptr;
        IShellItem* item = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) &&
            SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            RecycleOnly sink;
            DWORD cookie = 0;
            op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NO_UI | FOFX_RECYCLEONDELETE);
            if (SUCCEEDED(op->Advise(&sink, &cookie))) {
                BOOL aborted = FALSE;
                bool done = SUCCEEDED(op->DeleteItem(item, nullptr)) && SUCCEEDED(op->PerformOperations()) &&
                            SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && !aborted;
                op->Unadvise(cookie);
                ok = done && !sink.refused && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
            }
        }
        if (item) item->Release();
        if (op) op->Release();
        CoUninitialize();
    }).join();
    return ok;
}
} // namespace

bool recycle(const std::wstring& full, const std::wstring& trashDir, const RelPath& rel) {
    if (!trashDir.empty()) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[32];
        swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::wstring dest = syncpath::to_local(trashDir + L"\\" + stamp, rel);
        make_dirs(dest.substr(0, dest.find_last_of(L'\\')));
        return MoveFileExW(full.c_str(), dest.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING) != 0;
    }
    // the shell's Recycle Bin: shell items take ordinary paths (up to MAX_PATH)
    std::wstring p = full;
    if (p.compare(0, 8, L"\\\\?\\UNC\\") == 0) p = L"\\\\" + p.substr(8);
    else if (p.compare(0, 4, L"\\\\?\\") == 0) p = p.substr(4);
    if (p.size() < MAX_PATH && shell_recycle(p)) return true;
    // too long for the shell, or no Recycle Bin on that drive: a deleted file is gone for
    // good only through the pair's trash folder, never straight away
    return false;
}

// ------------------------------------------------------------------ watch
bool DirWatch::start(const std::wstring& root, std::function<void()> changed, std::function<void(const std::wstring&)> touched) {
    stop();
    m_root = root;
    m_changed = std::move(changed);
    m_touched = std::move(touched);
    m_dir = CreateFileW(syncpath::to_local(root, L"").c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (m_dir == INVALID_HANDLE_VALUE) return false;
    m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_thread = std::thread([this] { run(); });
    return true;
}

void DirWatch::stop() {
    if (m_stop) SetEvent(m_stop);
    if (m_thread.joinable()) m_thread.join();
    if (m_dir != INVALID_HANDLE_VALUE) { CloseHandle(m_dir); m_dir = INVALID_HANDLE_VALUE; }
    if (m_stop) { CloseHandle(m_stop); m_stop = nullptr; }
}

void DirWatch::run() {
    std::vector<uint8_t> buf(64 * 1024);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool pending = false;
    ULONGLONG firstAt = 0;
    for (;;) {
        ResetEvent(ov.hEvent);
        DWORD got = 0;
        if (!ReadDirectoryChangesW(m_dir, buf.data(), (DWORD)buf.size(), TRUE,
                                   FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                   FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES,
                                   nullptr, &ov, nullptr))
            break;
        HANDLE waits[2] = { m_stop, ov.hEvent };
        for (;;) {
            // a burst settles for 2 s (at most 10 s from its start) before the pass runs
            DWORD timeout = pending ? 2000 : INFINITE;
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, timeout);
            if (w == WAIT_OBJECT_0) { CancelIoEx(m_dir, &ov); GetOverlappedResult(m_dir, &ov, &got, TRUE); CloseHandle(ov.hEvent); return; }
            if (w == WAIT_TIMEOUT || (pending && GetTickCount64() - firstAt > 10000)) {
                pending = false;
                if (m_changed) m_changed();
                if (w == WAIT_TIMEOUT) continue;
            }
            if (w == WAIT_OBJECT_0 + 1) break;
        }
        if (!GetOverlappedResult(m_dir, &ov, &got, FALSE)) break;
        bool counts = got == 0;                               // overflow: something changed
        for (DWORD off = 0; got && off < got;) {
            auto* n = (FILE_NOTIFY_INFORMATION*)(buf.data() + off);
            std::wstring name(n->FileName, n->FileNameLength / sizeof(wchar_t));
            if (name.compare(0, 6, L".~sync") != 0) {
                counts = true;
                if (m_touched) m_touched(name);
            }
            if (!n->NextEntryOffset) break;
            off += n->NextEntryOffset;
        }
        if (counts && !pending) { pending = true; firstAt = GetTickCount64(); }
    }
    CloseHandle(ov.hEvent);
}

} // namespace sync
