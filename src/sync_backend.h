#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Sync folders: the storage abstraction (docs/sync-plan.md, "The backend abstraction").
// The engine owns every sync decision; a backend is a dumb, honest object store that
// reports versions and refuses writes made against a stale version. Encryption is a
// layer above the stores (sync_crypt.h), so a store never sees plaintext and needs no
// crypto code.
namespace sync {

// '/'-separated, relative to the pair's root (or the store's root), case preserved
using RelPath = std::wstring;

enum class HashKind : uint8_t { None, Blake2b, GitBlobSha1, Md5, Fingerprint };
struct ContentHash {
    HashKind kind = HashKind::None;
    std::array<uint8_t, 32> bytes{};
    bool operator==(const ContentHash& o) const { return kind != HashKind::None && kind == o.kind && bytes == o.bytes; }
};

// what travels with a file besides its bytes (encrypted, inside the stored file)
struct FileMeta {
    FILETIME mtime{};
    uint32_t attributes = 0;
    uint64_t version = 0;                   // grows with each upload of the path (rollback detection)
    std::array<uint8_t, 32> plainHash{};    // BLAKE2b of the plaintext (the uploader's)
};

struct RemoteEntry {
    RelPath     path;
    bool        isDir = false;
    uint64_t    size = 0;
    std::string rev;            // opaque version: changes iff the stored bytes do
    ContentHash hash;           // when known without a download
};

struct ChangeBatch {
    std::vector<RemoteEntry> upserts;
    std::vector<RelPath>     removals;
    bool        full = false;   // upserts is the complete tree: everything else is gone
    std::string cursor;         // handed back on the next call
    // stored items that couldn't be read (a name that doesn't decrypt...): reported, skipped
    std::vector<std::pair<RelPath, std::wstring>> problems;
};

struct RemoteVersion {
    std::string rev;            // pass to download as RemoteEntry::rev
    FILETIME    when{};
    uint64_t    size = 0;
    std::wstring note;          // e.g. the commit message
};

struct Caps {
    bool changeFeed = false;
    bool conditionalWrite = false;
    bool transactional = false;
    bool serverMove = false;
    bool rangeReads = false;
    bool versions = false;
    bool caseSensitive = true;
    bool emptyDirs = false;
    int  parallelism = 1;
    int  maxNameLength = 1024;
    int  maxPathLength = 1024;
    uint64_t maxFileSize = ~0ull;
    uint64_t maxCommitBytes = ~0ull;
    HashKind hash = HashKind::None;
    int  pollSeconds = 60;
};

enum class Status { Ok, NotFound, PreconditionFailed, CommitRaced, AuthFailed, Offline, Throttled,
                    TooLarge, InvalidName, Unsupported, Rejected, Locked, Tampered, Cancelled, Fatal };
const wchar_t* status_name(Status s);

struct Result {
    Status st = Status::Ok;
    std::wstring message;
    int retryAfterMs = 0;
    bool ok() const { return st == Status::Ok; }
    static Result fail(Status s, std::wstring m = {}) { Result r; r.st = s; r.message = std::move(m); return r; }
};
// transient: worth retrying the same request
inline bool transient(Status s) { return s == Status::Offline || s == Status::Throttled; }

struct Progress {
    virtual ~Progress() = default;
    virtual void bytes(uint64_t done, uint64_t total) {}
    virtual bool cancelled() { return false; }
};
struct NoProgress : Progress {};

struct ByteSource {
    virtual ~ByteSource() = default;
    virtual uint64_t size() = 0;
    virtual bool read(void* buf, size_t n, size_t* got) = 0;   // *got == 0: the end
    virtual bool rewind() = 0;
};
struct ByteSink {
    virtual ~ByteSink() = default;
    virtual bool write(const void* buf, size_t n) = 0;         // false: stop (the reader gives up)
};

// helpers
struct MemorySource : ByteSource {
    std::vector<uint8_t> data;
    size_t pos = 0;
    MemorySource() = default;
    explicit MemorySource(std::vector<uint8_t> d) : data(std::move(d)) {}
    uint64_t size() override { return data.size(); }
    bool read(void* buf, size_t n, size_t* got) override;
    bool rewind() override { pos = 0; return true; }
};
struct MemorySink : ByteSink {
    std::vector<uint8_t> data;
    size_t limit = SIZE_MAX;                // more than this: stop
    bool write(const void* buf, size_t n) override;
};
struct FileSource : ByteSource {            // a file opened by the caller (not closed here)
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t len = 0;
    explicit FileSource(HANDLE file);
    uint64_t size() override { return len; }
    bool read(void* buf, size_t n, size_t* got) override;
    bool rewind() override;
};
struct FileSink : ByteSink {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit FileSink(HANDLE file) : h(file) {}
    bool write(const void* buf, size_t n) override;
};
// copy a source to a sink in blocks, reporting progress
Result pump(ByteSource& in, ByteSink& out, Progress& prog);

class Backend {
public:
    virtual ~Backend() = default;
    virtual Caps   caps() const = 0;
    virtual Result connect(Progress&) = 0;
    // "" cursor: the full tree (full = true)
    virtual Result changes(const std::string& cursor, ChangeBatch& out, Progress&) = 0;
    // one cheap request: did anything change since `token`? (default: always yes, no token)
    virtual Result changed_since(const std::string& token, bool& changed, std::string& newToken, Progress&) {
        changed = true; newToken.clear(); return {};
    }
    // after this device changed something: let the others' changed_since notice
    virtual Result touch(std::string& newToken, Progress&) { newToken.clear(); return {}; }
    // one item (NotFound when absent)
    virtual Result stat(const RelPath& p, RemoteEntry& out, Progress& prog);
    // everything below a folder ("" = all), recursively (default: a full changes() filtered)
    virtual Result list(const RelPath& prefix, std::vector<RemoteEntry>& out, Progress& prog);
    virtual Result download(const RemoteEntry& e, ByteSink& out, FileMeta& meta, Progress&) = 0;
    virtual Result download_range(const RemoteEntry& e, uint64_t off, uint64_t len, ByteSink& out, Progress&) {
        return Result::fail(Status::Unsupported);
    }
    virtual Result peek_hash(const RemoteEntry& e, ContentHash& out, Progress&) { return Result::fail(Status::Unsupported); }
    // a local file's hash in the store's terms (compared with peek_hash's)
    virtual ContentHash hash_of_local(const std::array<uint8_t, 32>& plainBlake2b) const {
        ContentHash h; h.kind = HashKind::Blake2b; h.bytes = plainBlake2b; return h;
    }
    // expectedRev: "" = must not exist yet, else the rev last seen
    virtual Result upload(const RelPath& p, ByteSource& in, const FileMeta& meta,
                          const std::string& expectedRev, RemoteEntry& out, Progress&) = 0;
    virtual Result remove(const RelPath& p, const std::string& expectedRev, Progress&) = 0;
    virtual Result move(const RelPath& from, const RelPath& to, const std::string& expectedRev,
                        RemoteEntry& out, Progress&) { return Result::fail(Status::Unsupported); }
    virtual Result make_dir(const RelPath& p, Progress&) { return Result::fail(Status::Unsupported); }
    virtual Result remove_dir(const RelPath& p, Progress&) { return {}; }
    // transactional stores gather everything between begin and commit; newCursor: the
    // cursor that includes this commit (empty: ask changes() again)
    virtual Result begin(Progress&) { return {}; }
    virtual Result commit(const std::wstring& summary, std::string& newCursor, Progress&) { newCursor.clear(); return {}; }
    virtual void   abort() {}
    virtual Result list_versions(const RelPath& p, std::vector<RemoteVersion>& out, Progress&) {
        return Result::fail(Status::Unsupported);
    }
};

// ------------------------------------------------------------------ registry
struct ConfigField {
    std::wstring key;            // persisted name ("bucket")
    std::wstring label;          // "Bucket"
    enum Kind { Text, Secret, Bool, Choice, Url, Folder, Number } kind = Text;
    bool required = false;
    std::wstring def;
    std::vector<std::wstring> choices;
    std::wstring help;
};
using BackendConfig = std::vector<std::pair<std::wstring, std::wstring>>;
std::wstring config_get(const BackendConfig& c, const std::wstring& key, const std::wstring& def = {});

struct BackendType {
    std::wstring id;             // persisted, never renamed
    std::wstring name;           // shown
    std::vector<ConfigField> fields;
    std::function<std::wstring(const BackendConfig&)> validate;     // empty: fine
    std::function<std::unique_ptr<Backend>(const BackendConfig&)> create;
};
void register_backend(BackendType t);
const BackendType* find_backend(const std::wstring& id);
std::vector<const BackendType*> backend_types();
void register_builtin_backends();          // folder, s3, git, azdo (idempotent)

// The raw store of a backend type. Only the vault / key code (sync_keys) and tests use
// it directly; pairs always go through open_pair_backend (sync_crypt.h), which encrypts.
std::unique_ptr<Backend> create_raw_backend(const std::wstring& typeId, const BackendConfig& cfg, std::wstring* why);

// ------------------------------------------------------------------ decorators
// retries transient failures with backoff (sources rewind; Offline / Throttled only)
std::unique_ptr<Backend> make_retrying(std::unique_ptr<Backend> inner, int attempts = 4, int firstDelayMs = 500);
// bandwidth limit for uploads and downloads (bytes per second, 0: none)
std::unique_ptr<Backend> make_throttled(std::unique_ptr<Backend> inner, std::shared_ptr<std::atomic<uint64_t>> bytesPerSecond);

// text helpers shared by the stores
std::string  to_utf8(const std::wstring& w);
std::wstring from_utf8(const std::string& s);
std::wstring parent_of(const RelPath& p);              // "" for top-level
std::wstring leaf_of(const RelPath& p);
std::wstring join_path(const std::wstring& a, const std::wstring& b);

} // namespace sync
