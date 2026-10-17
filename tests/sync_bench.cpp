// The sync benchmark (docs/sync-plan.md, phase 3): many small files and one big file through
// an encrypted Folder store, against copying the same files. `explorer-plus-sync-tests --bench
// [small files] [big file MB]` (10,000 and 2048 by default); prints wall and CPU time of each.
#include "sync_test_util.h"
#include "sync_engine.h"
#include "sync_crypt.h"
#include "sync_keys.h"
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

using namespace sync;

namespace {

double cpu_seconds() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto s = [](FILETIME f) { return (((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7; };
    return s(k) + s(u);
}

double wall_seconds() {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / f.QuadPart;
}

// not compressible, cheap to make (xorshift)
struct Noise {
    uint64_t s;
    explicit Noise(uint64_t seed) : s(seed | 1) {}
    void fill(char* p, size_t n) {
        for (size_t i = 0; i + 8 <= n; i += 8) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            memcpy(p + i, &s, 8);
        }
    }
};

bool write_noise(const std::wstring& path, uint64_t size, Noise& nz) {
    HANDLE h = CreateFileW(tu::lp(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<char> buf(1 << 20);
    bool ok = true;
    for (uint64_t left = size; left && ok;) {
        DWORD n = (DWORD)std::min<uint64_t>(left, buf.size()), w = 0;
        nz.fill(buf.data(), n);
        ok = WriteFile(h, buf.data(), n, &w, nullptr) && w == n;
        left -= n;
    }
    CloseHandle(h);
    return ok;
}

void copy_tree(const std::wstring& from, const std::wstring& to) {
    tu::mkdirs(to);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(tu::lp(from + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copy_tree(from + L"\\" + n, to + L"\\" + n);
        else CopyFileW(tu::lp(from + L"\\" + n).c_str(), tu::lp(to + L"\\" + n).c_str(), FALSE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// counts the raw store's calls and their time (what a pass spends in the store)
struct Calls {
    std::mutex mx;
    std::map<std::string, std::pair<size_t, double>> by;
    void add(const char* what, double s) { std::lock_guard<std::mutex> lk(mx); auto& x = by[what]; ++x.first; x.second += s; }
    void print(const char* title) {
        std::lock_guard<std::mutex> lk(mx);
        std::printf("    store calls, %s:", title);
        for (auto& [k, v] : by) std::printf(" %s %zu (%.2f s)", k.c_str(), v.first, v.second);
        std::printf("\n");
        by.clear();
    }
};

struct Timed : Backend {
    std::unique_ptr<Backend> in;
    Calls& calls;
    Timed(std::unique_ptr<Backend> i, Calls& c) : in(std::move(i)), calls(c) {}
    template <class F> Result t(const char* what, F f) {
        double w = wall_seconds();
        Result r = f();
        calls.add(what, wall_seconds() - w);
        return r;
    }
    Caps caps() const override { return in->caps(); }
    Result connect(Progress& p) override { return t("connect", [&] { return in->connect(p); }); }
    Result changes(const std::string& c, ChangeBatch& o, Progress& p) override { return t("changes", [&] { return in->changes(c, o, p); }); }
    Result changed_since(const std::string& tk, bool& ch, std::string& nt, Progress& p) override { return t("changed_since", [&] { return in->changed_since(tk, ch, nt, p); }); }
    Result touch(std::string& nt, Progress& p) override { return t("touch", [&] { return in->touch(nt, p); }); }
    Result stat(const RelPath& path, RemoteEntry& o, Progress& p) override { return t("stat", [&] { return in->stat(path, o, p); }); }
    Result list(const RelPath& path, std::vector<RemoteEntry>& o, Progress& p) override { return t("list", [&] { return in->list(path, o, p); }); }
    Result download(const RemoteEntry& e, ByteSink& o, FileMeta& m, Progress& p) override { return t("download", [&] { return in->download(e, o, m, p); }); }
    Result upload(const RelPath& path, ByteSource& s, const FileMeta& m, const std::string& er, RemoteEntry& o, Progress& p) override {
        return t("upload", [&] { return in->upload(path, s, m, er, o, p); });
    }
    Result remove(const RelPath& path, const std::string& er, Progress& p) override { return t("remove", [&] { return in->remove(path, er, p); }); }
    Result make_dir(const RelPath& path, Progress& p) override { return t("make_dir", [&] { return in->make_dir(path, p); }); }
    Result begin(Progress& p) override { return t("begin", [&] { return in->begin(p); }); }
    Result commit(const std::wstring& s, std::string& nc, Progress& p) override { return t("commit", [&] { return in->commit(s, nc, p); }); }
    void abort() override { in->abort(); }
    Result download_range(const RemoteEntry& e, uint64_t off, uint64_t len, ByteSink& o, Progress& p) override { return in->download_range(e, off, len, o, p); }
    Result peek_hash(const RemoteEntry& e, ContentHash& o, Progress& p) override { return t("peek_hash", [&] { return in->peek_hash(e, o, p); }); }
    ContentHash hash_of_local(const std::array<uint8_t, 32>& h) const override { return in->hash_of_local(h); }
    Result move(const RelPath& f, const RelPath& to, const std::string& er, RemoteEntry& o, Progress& p) override { return t("move", [&] { return in->move(f, to, er, o, p); }); }
    Result remove_dir(const RelPath& path, Progress& p) override { return t("remove_dir", [&] { return in->remove_dir(path, p); }); }
    Result list_versions(const RelPath& path, std::vector<RemoteVersion>& o, Progress& p) override { return in->list_versions(path, o, p); }
};

struct Device {
    Calls* calls = nullptr;
    std::wstring root, stateDir, trash;
    std::unique_ptr<StateStore> st;
    std::unique_ptr<Engine> eng;
    Result open(const BackendConfig& cfg, synckeys::KeyStore& ks, bool create) {
        NoProgress p;
        std::wstring why;
        auto rb = create_raw_backend(L"folder", cfg, &why);
        if (!rb) return Result::fail(Status::Fatal, why);
        if (calls) rb = std::make_unique<Timed>(std::move(rb), *calls);
        synckeys::Vault v;
        if (Result r = synckeys::open_vault(*rb, ks, create, v, p); !r.ok()) return r;
        synckeys::KeyStore* k = &ks;
        auto enc = make_encrypting(std::move(rb), v, [k](Backend& b, synckeys::Vault& nv) { NoProgress np; return synckeys::open_vault(b, *k, false, nv, np); });
        st = std::make_unique<StateStore>(stateDir);
        st->load();
        EngineOptions o;
        o.root = root;
        o.deviceName = L"BENCH";
        o.trashDir = trash;
        o.settleMs = 0;
        eng = std::make_unique<Engine>(o, std::move(enc), *st);
        return {};
    }
};

struct Timing {
    double wall, cpu;
};

Timing measure(const std::function<void()>& f) {
    double w = wall_seconds(), c = cpu_seconds();
    f();
    return { wall_seconds() - w, cpu_seconds() - c };
}

void row(const char* what, Timing t, double mb) {
    std::printf("  %-34s %8.2f s wall %8.2f s CPU %9.1f MB/s\n", what, t.wall, t.cpu, t.wall > 0 ? mb / t.wall : 0.0);
}

} // namespace

int run_sync_bench(int smallFiles, uint64_t bigMB) {
    register_builtin_backends();
    std::wstring src = tu::temp_root(L"bench-src"), copy = tu::temp_root(L"bench-copy"), store = tu::temp_root(L"bench-store");
    Calls calls;
    Device a, b;
    a.calls = b.calls = &calls;
    a.root = src;
    a.stateDir = tu::temp_root(L"bench-a-state");
    a.trash = tu::temp_root(L"bench-a-trash");
    b.root = tu::temp_root(L"bench-b");
    b.stateDir = tu::temp_root(L"bench-b-state");
    b.trash = tu::temp_root(L"bench-b-trash");
    auto cleanup = [&] {
        a.eng.reset(); a.st.reset(); b.eng.reset(); b.st.reset();
        for (auto& d : { src, copy, store, a.stateDir, a.trash, b.root, b.stateDir, b.trash }) tu::rmrf(d);
    };

    std::printf("sync benchmark: %d small files (1-8 KB), one %llu MB file; encrypted Folder store in %ls\n", smallFiles,
                (unsigned long long)bigMB, store.c_str());
    Noise nz(0x5eed);
    uint64_t smallBytes = 0;
    for (int i = 0; i < smallFiles; ++i) {
        std::wstring dir = src + L"\\d" + std::to_wstring(i % 100);
        if (i < 100) tu::mkdirs(dir);
        uint64_t size = 1024 + (i * 7919) % (7 * 1024);
        if (!write_noise(dir + L"\\f" + std::to_wstring(i) + L".bin", size, nz)) { std::printf("couldn't write the test files\n"); cleanup(); return 1; }
        smallBytes += size;
    }
    std::wstring big = src + L"\\big.bin";
    if (bigMB && !write_noise(big, bigMB << 20, nz)) { std::printf("couldn't write the big file (disk space?)\n"); cleanup(); return 1; }
    double smallMB = smallBytes / 1048576.0, totalMB = smallMB + (double)bigMB;

    // the plain copy of the same files: the floor for any sync through a folder
    std::wstring bigLater = store + L"-big.bin";
    if (bigMB) MoveFileW(big.c_str(), bigLater.c_str());
    row("copy, small files", measure([&] { copy_tree(src, copy); }), smallMB);
    if (bigMB) {
        row("copy, big file", measure([&] { CopyFileW(bigLater.c_str(), (copy + L"\\big.bin").c_str(), FALSE); }), (double)bigMB);
        MoveFileW(bigLater.c_str(), big.c_str());
    }
    tu::rmrf(copy);

    std::unique_ptr<synckeys::KeyStore> ks = synckeys::memory_key_store();
    std::string rec;
    BackendConfig cfg = { { L"path", store } };
    int failed = 0;
    if (!synckeys::setup_ring(*ks, rec).ok() || !a.open(cfg, *ks, true).ok() || !b.open(cfg, *ks, false).ok()) {
        std::printf("couldn't open the store\n");
        cleanup();
        return 1;
    }
    NoProgress np;
    PassReport up, down;
    calls.print("opening");
    Timing tu1 = measure([&] { up = a.eng->sync(np); });
    row("sync upload (encrypt + write)", tu1, totalMB);
    calls.print("upload");
    Timing td = measure([&] { down = b.eng->sync(np); });
    row("sync download (read + decrypt)", td, totalMB);
    calls.print("download");
    Timing again = measure([&] { a.eng->sync(np); });
    row("sync pass, nothing changed", again, 0);
    calls.print("nothing changed");
    if (!up.result.ok() || !down.result.ok()) {
        std::printf("a pass failed: %s / %s\n", to_utf8(up.result.message).c_str(), to_utf8(down.result.message).c_str());
        failed = 1;
    } else if (tu::read(src, L"d7\\f7.bin") != tu::read(b.root, L"d7\\f7.bin")) {
        std::printf("the downloaded files differ\n");
        failed = 1;
    }
    // encryption alone: the big file's content through the file format, in memory
    if (bigMB) {
        std::vector<uint8_t> chunk(64 << 20);
        Noise(1).fill((char*)chunk.data(), chunk.size());
        crypto::Key key;
        key.b.fill(7);
        uint8_t tag[crypto::kTag], nonce[crypto::kNonce] = {};
        Timing enc = measure([&] {
            for (uint64_t done = 0; done < (bigMB << 20); done += chunk.size()) crypto::aead_lock_into(key, nonce, nullptr, 0, chunk.data(), chunk.size(), tag);
        });
        row("XChaCha20-Poly1305 alone (memory)", enc, (double)bigMB);
    }
    cleanup();
    return failed;
}
