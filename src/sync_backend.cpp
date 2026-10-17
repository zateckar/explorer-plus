#include "sync_backend.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

namespace sync {

const wchar_t* status_name(Status s) {
    switch (s) {
        case Status::Ok: return L"ok";
        case Status::NotFound: return L"not found";
        case Status::PreconditionFailed: return L"changed meanwhile";
        case Status::CommitRaced: return L"another device committed first";
        case Status::AuthFailed: return L"sign-in refused";
        case Status::Offline: return L"offline";
        case Status::Throttled: return L"throttled";
        case Status::TooLarge: return L"too large";
        case Status::InvalidName: return L"invalid name";
        case Status::Unsupported: return L"unsupported";
        case Status::Rejected: return L"refused by the server";
        case Status::Locked: return L"locked";
        case Status::Tampered: return L"tampered";
        case Status::Cancelled: return L"cancelled";
        case Status::Fatal: return L"failed";
    }
    return L"?";
}

// ------------------------------------------------------------------ streams
bool MemorySource::read(void* buf, size_t n, size_t* got) {
    size_t k = std::min(n, data.size() - pos);
    if (k) memcpy(buf, data.data() + pos, k);
    pos += k;
    *got = k;
    return true;
}

bool MemorySink::write(const void* buf, size_t n) {
    if (data.size() + n > limit) {
        size_t k = limit > data.size() ? limit - data.size() : 0;
        data.insert(data.end(), (const uint8_t*)buf, (const uint8_t*)buf + k);
        return false;
    }
    data.insert(data.end(), (const uint8_t*)buf, (const uint8_t*)buf + n);
    return true;
}

FileSource::FileSource(HANDLE file) : h(file) {
    LARGE_INTEGER li{};
    if (GetFileSizeEx(h, &li)) len = (uint64_t)li.QuadPart;
}
bool FileSource::read(void* buf, size_t n, size_t* got) {
    DWORD r = 0;
    if (!ReadFile(h, buf, (DWORD)std::min<size_t>(n, 1u << 30), &r, nullptr)) { *got = 0; return false; }
    *got = r;
    return true;
}
bool FileSource::rewind() {
    LARGE_INTEGER z{};
    return SetFilePointerEx(h, z, nullptr, FILE_BEGIN) != 0;
}
bool FileSink::write(const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    while (n) {
        DWORD w = 0;
        if (!WriteFile(h, p, (DWORD)std::min<size_t>(n, 1u << 30), &w, nullptr) || !w) return false;
        p += w;
        n -= w;
    }
    return true;
}

Result pump(ByteSource& in, ByteSink& out, Progress& prog) {
    std::vector<uint8_t> buf(256 * 1024);
    uint64_t done = 0, total = in.size();
    for (;;) {
        if (prog.cancelled()) return Result::fail(Status::Cancelled);
        size_t got = 0;
        if (!in.read(buf.data(), buf.size(), &got)) return Result::fail(Status::Fatal, L"read failed");
        if (!got) break;
        if (!out.write(buf.data(), got)) return Result::fail(Status::Cancelled, L"write stopped");
        done += got;
        prog.bytes(done, total);
    }
    return {};
}

Result Backend::list(const RelPath& prefix, std::vector<RemoteEntry>& out, Progress& prog) {
    ChangeBatch b;
    Result r = changes("", b, prog);
    if (!r.ok()) return r;
    out.clear();
    for (auto& e : b.upserts)
        if (prefix.empty() || (e.path.size() > prefix.size() && e.path.compare(0, prefix.size(), prefix) == 0 && e.path[prefix.size()] == L'/'))
            out.push_back(std::move(e));
    return {};
}

Result Backend::stat(const RelPath& p, RemoteEntry& out, Progress& prog) {
    std::vector<RemoteEntry> v;
    Result r = list(parent_of(p), v, prog);
    if (!r.ok()) return r;
    for (auto& e : v) if (e.path == p) { out = e; return {}; }
    return Result::fail(Status::NotFound);
}

// ------------------------------------------------------------------ registry
static std::mutex s_regMx;
static std::vector<std::unique_ptr<BackendType>>& registry() {
    static std::vector<std::unique_ptr<BackendType>> r;
    return r;
}

void register_backend(BackendType t) {
    std::lock_guard<std::mutex> lk(s_regMx);
    for (auto& r : registry()) if (r->id == t.id) { *r = std::move(t); return; }
    registry().push_back(std::make_unique<BackendType>(std::move(t)));
}

const BackendType* find_backend(const std::wstring& id) {
    register_builtin_backends();
    std::lock_guard<std::mutex> lk(s_regMx);
    for (auto& r : registry()) if (r->id == id) return r.get();
    return nullptr;
}

std::vector<const BackendType*> backend_types() {
    register_builtin_backends();
    std::lock_guard<std::mutex> lk(s_regMx);
    std::vector<const BackendType*> v;
    for (auto& r : registry()) v.push_back(r.get());
    return v;
}

void register_folder_backend();
void register_s3_backend();
void register_git_backend();
void register_azdo_backend();

void register_builtin_backends() {
    static std::once_flag once;
    std::call_once(once, [] {
        register_folder_backend();
        register_s3_backend();
        register_git_backend();
        register_azdo_backend();
    });
}

std::wstring config_get(const BackendConfig& c, const std::wstring& key, const std::wstring& def) {
    for (auto& [k, v] : c) if (_wcsicmp(k.c_str(), key.c_str()) == 0) return v;
    return def;
}

std::unique_ptr<Backend> create_raw_backend(const std::wstring& typeId, const BackendConfig& cfg, std::wstring* why) {
    const BackendType* t = find_backend(typeId);
    if (!t) { if (why) *why = L"Unknown storage type: " + typeId; return nullptr; }
    for (auto& f : t->fields)
        if (f.required && config_get(cfg, f.key).empty()) { if (why) *why = f.label + L" is required."; return nullptr; }
    if (t->validate) {
        std::wstring w = t->validate(cfg);
        if (!w.empty()) { if (why) *why = w; return nullptr; }
    }
    auto b = t->create(cfg);
    if (!b && why && why->empty()) *why = L"The storage couldn't be opened.";
    return b;
}

// ------------------------------------------------------------------ decorators
namespace {

// forwards everything; decorators override what they change
struct Forwarding : Backend {
    std::unique_ptr<Backend> in;
    explicit Forwarding(std::unique_ptr<Backend> i) : in(std::move(i)) {}
    Caps caps() const override { return in->caps(); }
    Result connect(Progress& p) override { return in->connect(p); }
    Result changes(const std::string& c, ChangeBatch& o, Progress& p) override { return in->changes(c, o, p); }
    Result changed_since(const std::string& t, bool& ch, std::string& nt, Progress& p) override { return in->changed_since(t, ch, nt, p); }
    Result touch(std::string& nt, Progress& p) override { return in->touch(nt, p); }
    Result stat(const RelPath& path, RemoteEntry& o, Progress& p) override { return in->stat(path, o, p); }
    Result list(const RelPath& path, std::vector<RemoteEntry>& o, Progress& p) override { return in->list(path, o, p); }
    Result download(const RemoteEntry& e, ByteSink& o, FileMeta& m, Progress& p) override { return in->download(e, o, m, p); }
    Result download_range(const RemoteEntry& e, uint64_t off, uint64_t len, ByteSink& o, Progress& p) override { return in->download_range(e, off, len, o, p); }
    Result peek_hash(const RemoteEntry& e, ContentHash& o, Progress& p) override { return in->peek_hash(e, o, p); }
    ContentHash hash_of_local(const std::array<uint8_t, 32>& h) const override { return in->hash_of_local(h); }
    Result upload(const RelPath& path, ByteSource& s, const FileMeta& m, const std::string& er, RemoteEntry& o, Progress& p) override { return in->upload(path, s, m, er, o, p); }
    Result remove(const RelPath& path, const std::string& er, Progress& p) override { return in->remove(path, er, p); }
    Result move(const RelPath& f, const RelPath& t, const std::string& er, RemoteEntry& o, Progress& p) override { return in->move(f, t, er, o, p); }
    Result make_dir(const RelPath& path, Progress& p) override { return in->make_dir(path, p); }
    Result remove_dir(const RelPath& path, Progress& p) override { return in->remove_dir(path, p); }
    Result begin(Progress& p) override { return in->begin(p); }
    Result commit(const std::wstring& s, std::string& nc, Progress& p) override { return in->commit(s, nc, p); }
    void abort() override { in->abort(); }
    Result list_versions(const RelPath& path, std::vector<RemoteVersion>& o, Progress& p) override { return in->list_versions(path, o, p); }
};

struct Retrying : Forwarding {
    int attempts, firstDelay;
    Retrying(std::unique_ptr<Backend> i, int a, int d) : Forwarding(std::move(i)), attempts(a), firstDelay(d) {}

    template <class F> Result again(Progress& p, F f) {
        int delay = firstDelay;
        Result r;
        for (int i = 0; i < attempts; ++i) {
            r = f();
            if (!transient(r.st) || p.cancelled()) return r;
            int wait = r.retryAfterMs > 0 ? r.retryAfterMs : delay;
            for (int slept = 0; slept < wait && !p.cancelled(); slept += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            delay = std::min(delay * 2, 30000);
        }
        return r;
    }
    Result connect(Progress& p) override { return again(p, [&] { return in->connect(p); }); }
    Result changes(const std::string& c, ChangeBatch& o, Progress& p) override { return again(p, [&] { o = {}; return in->changes(c, o, p); }); }
    Result changed_since(const std::string& t, bool& ch, std::string& nt, Progress& p) override { return again(p, [&] { return in->changed_since(t, ch, nt, p); }); }
    Result download(const RemoteEntry& e, ByteSink& o, FileMeta& m, Progress& p) override {
        // a sink can't be rewound: only retry when nothing reached it yet
        struct Counting : ByteSink { ByteSink& s; uint64_t n = 0; explicit Counting(ByteSink& x) : s(x) {}
            bool write(const void* b, size_t k) override { n += k; return s.write(b, k); } } c(o);
        return again(p, [&] { Result r = in->download(e, c, m, p); if (c.n && transient(r.st)) r.st = Status::Fatal; return r; });
    }
    Result peek_hash(const RemoteEntry& e, ContentHash& o, Progress& p) override { return again(p, [&] { return in->peek_hash(e, o, p); }); }
    Result stat(const RelPath& path, RemoteEntry& o, Progress& p) override { return again(p, [&] { return in->stat(path, o, p); }); }
    Result list(const RelPath& path, std::vector<RemoteEntry>& o, Progress& p) override { return again(p, [&] { o.clear(); return in->list(path, o, p); }); }
    Result touch(std::string& nt, Progress& p) override { return again(p, [&] { return in->touch(nt, p); }); }
    Result upload(const RelPath& path, ByteSource& s, const FileMeta& m, const std::string& er, RemoteEntry& o, Progress& p) override {
        return again(p, [&] { s.rewind(); return in->upload(path, s, m, er, o, p); });
    }
    Result remove(const RelPath& path, const std::string& er, Progress& p) override { return again(p, [&] { return in->remove(path, er, p); }); }
    Result move(const RelPath& f, const RelPath& t, const std::string& er, RemoteEntry& o, Progress& p) override { return again(p, [&] { return in->move(f, t, er, o, p); }); }
    Result commit(const std::wstring& s, std::string& nc, Progress& p) override { return again(p, [&] { return in->commit(s, nc, p); }); }
};

// a token bucket shared by the transfers of one pair
struct Throttled : Forwarding {
    std::shared_ptr<std::atomic<uint64_t>> rate;
    std::mutex mx;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    double budget = 0;
    Throttled(std::unique_ptr<Backend> i, std::shared_ptr<std::atomic<uint64_t>> r) : Forwarding(std::move(i)), rate(std::move(r)) {}

    void take(size_t n, Progress& p) {
        for (;;) {
            uint64_t bps = rate ? rate->load() : 0;
            if (!bps || p.cancelled()) return;
            {
                std::lock_guard<std::mutex> lk(mx);
                auto now = std::chrono::steady_clock::now();
                budget = std::min<double>(budget + std::chrono::duration<double>(now - last).count() * (double)bps, (double)bps);
                last = now;
                if (budget >= (double)n || budget >= (double)bps) { budget -= (double)n; return; }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    Result download(const RemoteEntry& e, ByteSink& o, FileMeta& m, Progress& p) override {
        struct S : ByteSink { Throttled& t; ByteSink& s; Progress& p;
            S(Throttled& a, ByteSink& b, Progress& c) : t(a), s(b), p(c) {}
            bool write(const void* b, size_t k) override { t.take(k, p); return s.write(b, k); } } s(*this, o, p);
        return in->download(e, s, m, p);
    }
    Result upload(const RelPath& path, ByteSource& src, const FileMeta& m, const std::string& er, RemoteEntry& o, Progress& p) override {
        struct S : ByteSource { Throttled& t; ByteSource& s; Progress& p;
            S(Throttled& a, ByteSource& b, Progress& c) : t(a), s(b), p(c) {}
            uint64_t size() override { return s.size(); }
            bool read(void* b, size_t n, size_t* got) override { bool ok = s.read(b, std::min<size_t>(n, 64 * 1024), got); t.take(*got, p); return ok; }
            bool rewind() override { return s.rewind(); } } s(*this, src, p);
        return in->upload(path, s, m, er, o, p);
    }
};

} // namespace

std::unique_ptr<Backend> make_retrying(std::unique_ptr<Backend> inner, int attempts, int firstDelayMs) {
    return std::make_unique<Retrying>(std::move(inner), attempts, firstDelayMs);
}
std::unique_ptr<Backend> make_throttled(std::unique_ptr<Backend> inner, std::shared_ptr<std::atomic<uint64_t>> bps) {
    return std::make_unique<Throttled>(std::move(inner), std::move(bps));
}

// ------------------------------------------------------------------ text
std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring from_utf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::wstring parent_of(const RelPath& p) {
    size_t i = p.find_last_of(L'/');
    return i == std::wstring::npos ? std::wstring() : p.substr(0, i);
}
std::wstring leaf_of(const RelPath& p) {
    size_t i = p.find_last_of(L'/');
    return i == std::wstring::npos ? p : p.substr(i + 1);
}
std::wstring join_path(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + L"/" + b;
}

} // namespace sync
