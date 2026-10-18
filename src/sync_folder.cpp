// The Folder store (docs/sync-plan.md, "Folder"): a local or UNC folder holding the
// encrypted files (a NAS share, a USB disk; the tests' store). The smallest backend.
#include "sync_backend.h"
#include "sync_local.h"
#include "sync_paths.h"
#include "crypto.h"

namespace sync {
namespace {

std::string rev_of(uint64_t size, uint64_t mtime, uint64_t id) {
    char b[64];
    snprintf(b, sizeof(b), "%llx-%llx-%llx", (unsigned long long)size, (unsigned long long)mtime, (unsigned long long)id);
    return b;
}

class FolderBackend : public Backend {
public:
    explicit FolderBackend(std::wstring root) : m_root(std::move(root)) {
        while (!m_root.empty() && (m_root.back() == L'\\' || m_root.back() == L'/')) m_root.pop_back();
    }
    Caps caps() const override {
        Caps c;
        c.conditionalWrite = true;              // checked just before the rename (best effort)
        c.serverMove = true;
        c.rangeReads = true;
        c.caseSensitive = false;
        c.emptyDirs = true;
        c.parallelism = 1;      // (measured: 4 threads writing small files were 7-20x slower, docs/sync-plan.md "Benchmark")
        c.maxNameLength = 255;
        c.maxPathLength = 30000;
        c.pollSeconds = 30;
        return c;
    }
    Result connect(Progress&) override {
        DWORD a = GetFileAttributesW(full(L"").c_str());
        if (a == INVALID_FILE_ATTRIBUTES) {
            if (!make_dirs(full(L""))) return Result::fail(Status::Offline, L"The storage folder can't be reached: " + m_root);
        } else if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return Result::fail(Status::Fatal, L"Not a folder: " + m_root);
        return {};
    }
    Result changes(const std::string&, ChangeBatch& out, Progress& p) override {
        out = {};
        out.full = true;
        if (GetFileAttributesW(full(L"").c_str()) == INVALID_FILE_ATTRIBUTES) return Result::fail(Status::Offline, L"The storage folder can't be reached.");
        if (!walk(L"", out.upserts, p)) return Result::fail(Status::Offline, L"The storage folder couldn't be listed.");
        return {};
    }
    Result list(const RelPath& prefix, std::vector<RemoteEntry>& out, Progress& p) override {
        out.clear();
        DWORD a = GetFileAttributesW(full(prefix).c_str());
        if (a == INVALID_FILE_ATTRIBUTES) return {};
        if (!walk(prefix, out, p)) return Result::fail(Status::Offline);
        return {};
    }
    Result stat(const RelPath& path, RemoteEntry& out, Progress&) override {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (!GetFileAttributesExW(full(path).c_str(), GetFileExInfoStandard, &d)) return Result::fail(Status::NotFound);
        out.path = path;
        out.isDir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out.size = out.isDir ? 0 : ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
        out.rev = out.isDir ? "dir" : current_rev(path);
        return {};
    }
    Result download(const RemoteEntry& e, ByteSink& out, FileMeta&, Progress& p) override {
        HANDLE h = CreateFileW(full(e.path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) return Result::fail(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? Status::NotFound : Status::Offline);
        FileSource src(h);
        Result r = pump(src, out, p);
        CloseHandle(h);
        return r;
    }
    Result download_range(const RemoteEntry& e, uint64_t off, uint64_t len, ByteSink& out, Progress& p) override {
        HANDLE h = CreateFileW(full(e.path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return Result::fail(Status::NotFound);
        LARGE_INTEGER at;
        at.QuadPart = (LONGLONG)off;
        SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
        std::vector<uint8_t> buf((size_t)std::min<uint64_t>(len, 1 << 20));
        Result r;
        while (len) {
            DWORD got = 0;
            if (!ReadFile(h, buf.data(), (DWORD)std::min<uint64_t>(len, buf.size()), &got, nullptr)) { r = Result::fail(Status::Offline); break; }
            if (!got) break;
            if (!out.write(buf.data(), got)) break;
            len -= got;
        }
        CloseHandle(h);
        return r;
    }
    Result upload(const RelPath& path, ByteSource& in, const FileMeta&, const std::string& expectedRev, RemoteEntry& out, Progress& p) override {
        if (expectedRev != "*" && current_rev(path) != expectedRev) return Result::fail(Status::PreconditionFailed);
        std::wstring tmpDir = full(L".~ep-tmp");
        CreateDirectoryW(tmpDir.c_str(), nullptr);
        uint8_t r8[8];
        crypto::random(r8, 8);
        std::string hx = crypto::hex(r8, 8);
        std::wstring tmp = tmpDir + L"\\" + std::wstring(hx.begin(), hx.end());
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return Result::fail(Status::Offline, L"The storage folder can't be written.");
        FileSink sink(h);
        Result r = pump(in, sink, p);
        bool flushed = FlushFileBuffers(h) != 0;
        CloseHandle(h);
        if (r.ok() && !flushed) r = Result::fail(Status::Offline, L"The storage folder can't be written.");
        if (!r.ok()) { DeleteFileW(tmp.c_str()); return r; }
        std::wstring target = full(path);
        make_dirs(target.substr(0, target.find_last_of(L'\\')));
        // the condition once more, just before the file goes in
        if (expectedRev != "*" && current_rev(path) != expectedRev) { DeleteFileW(tmp.c_str()); return Result::fail(Status::PreconditionFailed); }
        DWORD flags = MOVEFILE_WRITE_THROUGH | (expectedRev.empty() ? 0 : MOVEFILE_REPLACE_EXISTING);
        if (!MoveFileExW(tmp.c_str(), target.c_str(), flags)) {
            DWORD e = GetLastError();
            DeleteFileW(tmp.c_str());
            return Result::fail(e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS ? Status::PreconditionFailed : Status::Offline);
        }
        return stat(path, out, p);
    }
    Result remove(const RelPath& path, const std::string& expectedRev, Progress&) override {
        std::string cur = current_rev(path);
        if (cur.empty()) return Result::fail(Status::NotFound);
        if (expectedRev != "*" && cur != expectedRev) return Result::fail(Status::PreconditionFailed);
        if (!DeleteFileW(full(path).c_str())) return Result::fail(Status::Offline);
        prune(parent_of(path));
        return {};
    }
    Result move(const RelPath& from, const RelPath& to, const std::string& expectedRev, RemoteEntry& out, Progress& p) override {
        if (expectedRev != "*" && current_rev(from) != expectedRev) return Result::fail(Status::PreconditionFailed);
        if (!current_rev(to).empty()) return Result::fail(Status::PreconditionFailed);
        std::wstring target = full(to);
        make_dirs(target.substr(0, target.find_last_of(L'\\')));
        if (!MoveFileExW(full(from).c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) return Result::fail(Status::Offline);
        prune(parent_of(from));
        return stat(to, out, p);
    }
    Result make_dir(const RelPath& path, Progress&) override {
        return make_dirs(full(path)) ? Result() : Result::fail(Status::Offline);
    }
    Result remove_dir(const RelPath& path, Progress&) override {
        RemoveDirectoryW(full(path).c_str());          // only when empty; else another device filled it
        return {};
    }

private:
    std::wstring full(const RelPath& rel) const { return syncpath::to_local(m_root, rel); }
    std::string current_rev(const RelPath& path) const {
        HANDLE h = CreateFileW(full(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        uint64_t s = 0, m = 0, id = 0;
        file_identity(h, s, m, id);
        CloseHandle(h);
        return rev_of(s, m, id);
    }
    // folders that only held what was just removed go too (they exist through their files)
    void prune(RelPath dir) {
        (void)dir;                                      // explicit folders stay: emptyDirs
    }
    bool walk(const RelPath& rel, std::vector<RemoteEntry>& out, Progress& p) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((full(rel) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
        std::vector<RelPath> dirs;
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || (rel.empty() && n == L".~ep-tmp")) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            RemoteEntry e;
            e.path = join_path(rel, n);
            e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (e.isDir) { e.rev = "dir"; dirs.push_back(e.path); }
            else {
                e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                e.rev = current_rev(e.path);
                if (e.rev.empty()) continue;            // gone meanwhile
            }
            out.push_back(std::move(e));
        } while (FindNextFileW(h, &fd));
        bool complete = GetLastError() == ERROR_NO_MORE_FILES;      // (a share that dropped: not a short listing)
        FindClose(h);
        if (!complete || p.cancelled()) return false;
        for (auto& d : dirs) if (!walk(d, out, p)) return false;
        return true;
    }
    std::wstring m_root;
};

} // namespace

void register_folder_backend() {
    BackendType t;
    t.id = L"folder";
    t.name = L"Folder (a NAS share, a USB disk)";
    ConfigField f;
    f.key = L"path";
    f.label = L"Storage folder";
    f.kind = ConfigField::Folder;
    f.required = true;
    f.help = L"Where the encrypted copies go, e.g. \\\\nas\\backup\\sync or E:\\Sync";
    t.fields.push_back(f);
    t.validate = [](const BackendConfig& c) -> std::wstring {
        std::wstring p = config_get(c, L"path");
        if (p.size() < 3 || (p[1] != L':' && p.compare(0, 2, L"\\\\") != 0)) return L"Give the storage folder's full path.";
        return {};
    };
    t.create = [](const BackendConfig& c) -> std::unique_ptr<Backend> { return std::make_unique<FolderBackend>(config_get(c, L"path")); };
    register_backend(std::move(t));
}

} // namespace sync
