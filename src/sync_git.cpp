#include "sync_git.h"
#include "credstore.h"
#include "crypto.h"
#include <map>
#include <mutex>

namespace sync {

// ------------------------------------------------------------------ parsers
// "<mode> SP <type> SP <oid> SP+ <size> TAB <path> NUL" (size "-" for trees)
bool parse_ls_tree(const std::string& z, std::vector<GitTreeEntry>& out) {
    size_t i = 0;
    while (i < z.size()) {
        size_t end = z.find('\0', i);
        if (end == std::string::npos) return false;
        std::string rec = z.substr(i, end - i);
        i = end + 1;
        size_t tab = rec.find('\t');
        if (tab == std::string::npos) return false;
        GitTreeEntry e;
        e.path = rec.substr(tab + 1);
        std::string head = rec.substr(0, tab);
        size_t a = head.find(' '), b = a == std::string::npos ? a : head.find(' ', a + 1);
        if (b == std::string::npos) return false;
        e.mode = head.substr(0, a);
        e.type = head.substr(a + 1, b - a - 1);
        size_t c = head.find(' ', b + 1);
        e.oid = head.substr(b + 1, c == std::string::npos ? std::string::npos : c - b - 1);
        if (c != std::string::npos) {
            std::string sz = head.substr(c + 1);
            size_t s = sz.find_first_not_of(' ');
            sz = s == std::string::npos ? "" : sz.substr(s);
            e.size = sz == "-" || sz.empty() ? 0 : strtoull(sz.c_str(), nullptr, 10);
        }
        if (e.oid.size() < 40) return false;
        out.push_back(std::move(e));
    }
    return true;
}

// ":<old mode> <new mode> <old oid> <new oid> <status> NUL <path> NUL"
bool parse_diff_tree(const std::string& z, std::vector<GitDiffEntry>& out) {
    size_t i = 0;
    while (i < z.size()) {
        if (z[i] != ':') {                       // a leading commit id line (diff-tree <commit>)
            size_t e = z.find('\0', i);
            if (e == std::string::npos) return false;
            i = e + 1;
            continue;
        }
        size_t e = z.find('\0', i);
        if (e == std::string::npos) return false;
        std::string head = z.substr(i + 1, e - i - 1);
        i = e + 1;
        size_t p = z.find('\0', i);
        if (p == std::string::npos) return false;
        GitDiffEntry d;
        d.path = z.substr(i, p - i);
        i = p + 1;
        std::vector<std::string> f;
        size_t s = 0;
        while (s <= head.size()) {
            size_t n = head.find(' ', s);
            if (n == std::string::npos) n = head.size();
            f.push_back(head.substr(s, n - s));
            s = n + 1;
        }
        if (f.size() < 5 || f[4].empty()) return false;
        d.oldMode = f[0]; d.newMode = f[1]; d.oldOid = f[2]; d.newOid = f[3]; d.status = f[4][0];
        out.push_back(std::move(d));
    }
    return true;
}

bool git_version_at_least(const std::string& v, int major, int minor) {
    size_t p = v.find_first_of("0123456789");
    if (p == std::string::npos) return false;
    int ma = atoi(v.c_str() + p);
    size_t dot = v.find('.', p);
    int mi = dot == std::string::npos ? 0 : atoi(v.c_str() + dot + 1);
    return ma > major || (ma == major && mi >= minor);
}

std::wstring find_git() {
    std::wstring g = proc::find_on_path(L"git.exe");
    if (!g.empty()) return g;
    wchar_t pf[MAX_PATH];
    if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) {
        std::wstring p = std::wstring(pf) + L"\\Git\\cmd\\git.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return {};
}

namespace {

constexpr char kZero[] = "0000000000000000000000000000000000000000";

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

class GitBackend : public Backend {
public:
    explicit GitBackend(GitOptions o) : m_o(std::move(o)) {
        if (m_o.gitExe.empty()) m_o.gitExe = find_git();
        if (m_o.mirror.empty()) {
            std::string key = to_utf8(m_o.url + L"|" + m_o.branch + L"|" + m_o.subfolder);
            crypto::Hash32 h = crypto::blake2b(key.data(), key.size());
            std::string hx = crypto::hex(h.data(), 8);
            m_o.mirror = sync_home() + L"\\git\\" + std::wstring(hx.begin(), hx.end()) + L".git";
        }
        while (!m_o.subfolder.empty() && m_o.subfolder.back() == L'/') m_o.subfolder.pop_back();
        while (!m_o.subfolder.empty() && m_o.subfolder.front() == L'/') m_o.subfolder.erase(0, 1);
        m_prefix = m_o.subfolder.empty() ? std::string() : to_utf8(m_o.subfolder) + "/";
    }
    ~GitBackend() override { m_cat.stop(); }

    Caps caps() const override {
        Caps c;
        c.changeFeed = true;
        c.conditionalWrite = true;
        c.transactional = true;
        c.serverMove = true;
        c.versions = true;
        c.caseSensitive = true;
        c.emptyDirs = false;
        c.parallelism = 1;
        c.maxNameLength = 255;
        c.maxPathLength = 4096;
        c.maxFileSize = m_o.maxFileSize;
        c.maxCommitBytes = m_o.maxCommitBytes;
        c.hash = HashKind::GitBlobSha1;
        c.pollSeconds = m_o.pollSeconds;
        return c;
    }

    Result connect(Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        if (m_o.gitExe.empty()) return Result::fail(Status::Fatal, L"Git for Windows isn't installed (git.exe wasn't found).");
        proc::Result v = git({ L"--version" }, p, false);
        if (!v.started) return Result::fail(Status::Fatal, L"git.exe couldn't be started.");
        if (!git_version_at_least(v.out, 2, 31)) return Result::fail(Status::Fatal, L"Git for Windows 2.31 or newer is needed.");
        if (Result r = ensure_mirror(p); !r.ok()) return r;
        std::string tip;
        return remote_tip(tip, p);
    }

    Result changed_since(const std::string& token, bool& changed, std::string& nt, Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        std::string tip;
        if (Result r = remote_tip(tip, p); !r.ok()) return r;
        nt = tip.empty() ? "none" : tip;
        changed = token != nt;
        return {};
    }

    Result changes(const std::string& cursor, ChangeBatch& out, Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        out = {};
        if (Result r = fetch(p); !r.ok()) return r;
        out.cursor = m_tip.empty() ? "none" : m_tip;
        m_listed = m_tip;
        if (m_tip.empty()) { out.full = true; return {}; }
        if (cursor == m_tip) return {};
        bool incremental = !cursor.empty() && cursor != "none" && git({ L"cat-file", L"-e", w(cursor) + L"^{commit}" }, p).ok();
        if (!incremental) {
            out.full = true;
            std::vector<GitTreeEntry> tree;
            if (Result r = ls_tree(m_tip, "", tree, p); !r.ok()) return r;
            for (auto& e : tree) if (e.type == "blob") out.upserts.push_back(entry_of(e.path, e.oid, e.size));
            return {};
        }
        std::vector<std::wstring> args = { L"diff-tree", L"-r", L"-z", L"--raw", L"--no-renames", L"--no-abbrev", w(cursor), w(m_tip) };
        if (!m_prefix.empty()) { args.push_back(L"--"); args.push_back(from_utf8(m_prefix)); }
        proc::Result d = git(args, p);
        if (!d.ok()) return fail(d, L"git diff-tree");
        std::vector<GitDiffEntry> diff;
        if (!parse_diff_tree(d.out, diff)) return Result::fail(Status::Fatal, L"git diff-tree gave an unreadable answer.");
        std::vector<std::string> need;
        for (auto& e : diff) if (e.status != 'D' && e.newMode != "040000" && e.newMode != "160000") need.push_back(e.newOid);
        std::map<std::string, uint64_t> sizes;
        if (Result r = sizes_of(need, sizes, p); !r.ok()) return r;
        for (auto& e : diff) {
            if (!under_prefix(e.path)) continue;
            RelPath rel = from_utf8(e.path.substr(m_prefix.size()));
            if (e.status == 'D') out.removals.push_back(rel);
            else if (e.newMode != "040000" && e.newMode != "160000") out.upserts.push_back(entry_of(e.path, e.newOid, sizes[e.newOid]));
        }
        return {};
    }

    Result stat(const RelPath& path, RemoteEntry& out, Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        if (Result r = fetch(p); !r.ok()) return r;
        if (m_tip.empty()) return Result::fail(Status::NotFound);
        std::vector<GitTreeEntry> t;
        if (Result r = ls_tree(m_tip, to_utf8(path), t, p, false); !r.ok()) return r;
        for (auto& e : t)
            if (e.path == m_prefix + to_utf8(path)) {
                out = entry_of(e.path, e.oid, e.size);
                out.isDir = e.type == "tree";
                return {};
            }
        return Result::fail(Status::NotFound);
    }

    Result list(const RelPath& prefix, std::vector<RemoteEntry>& out, Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        out.clear();
        if (Result r = fetch(p); !r.ok()) return r;
        if (m_tip.empty()) return {};
        std::vector<GitTreeEntry> t;
        if (Result r = ls_tree(m_tip, prefix.empty() ? "" : to_utf8(prefix) + "/", t, p); !r.ok()) return r;
        for (auto& e : t) if (e.type == "blob") out.push_back(entry_of(e.path, e.oid, e.size));
        return {};
    }

    Result download(const RemoteEntry& e, ByteSink& sink, FileMeta&, Progress& p) override {
        std::lock_guard<std::mutex> lk(m_catMx);
        if (e.rev.size() < 40) return Result::fail(Status::NotFound);
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!m_cat.running()) {
                proc::Options o;
                o.env = env();
                if (!m_cat.start(m_o.gitExe, { L"cat-file", L"--batch" }, o)) return Result::fail(Status::Fatal, L"git cat-file couldn't be started.");
            }
            std::string q = e.rev + "\n", head;
            if (!m_cat.write(q.data(), q.size()) || !m_cat.read_line(head)) { m_cat.stop(); continue; }
            if (head.find(" missing") != std::string::npos) return Result::fail(Status::NotFound);
            size_t sp = head.rfind(' ');
            if (sp == std::string::npos || head.find(" blob ") == std::string::npos) { m_cat.stop(); return Result::fail(Status::NotFound); }
            uint64_t size = strtoull(head.c_str() + sp + 1, nullptr, 10), done = 0;
            std::vector<char> buf(1 << 16);
            bool stopped = false;
            while (done < size) {
                size_t k = (size_t)std::min<uint64_t>(buf.size(), size - done);
                if (!m_cat.read_exact(buf.data(), k)) { m_cat.stop(); return Result::fail(Status::Fatal, L"git cat-file stopped."); }
                done += k;
                if (!stopped && !sink.write(buf.data(), k)) stopped = true;     // keep reading: the session stays in step
                p.bytes(done, size);
            }
            char nl;
            m_cat.read_exact(&nl, 1);
            return stopped ? Result::fail(Status::Cancelled, L"write stopped") : Result{};
        }
        return Result::fail(Status::Fatal, L"git cat-file failed.");
    }

    Result begin(Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        if (m_tip.empty() && !m_fetched) if (Result r = fetch(p); !r.ok()) return r;
        m_staging = true;
        m_base = m_tip;
        m_staged.clear();
        m_bytes = 0;
        m_baseTree.clear();
        m_baseTreeLoaded = false;
        return {};
    }

    Result upload(const RelPath& path, ByteSource& in, const FileMeta&, const std::string& expectedRev, RemoteEntry& out,
                  Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        if (!m_staging) return Result::fail(Status::Fatal, L"upload outside a transaction");
        if (in.size() > m_o.maxFileSize) return Result::fail(Status::TooLarge, L"The file is bigger than this repository's limit.");
        std::string full = m_prefix + to_utf8(path);
        if (Result r = check(full, expectedRev, p); !r.ok()) return r;
        proc::Options o;
        o.env = env();
        o.stdinSource = &in;
        o.cancelled = [&p] { return p.cancelled(); };
        proc::Result h = proc::run(m_o.gitExe, { L"hash-object", L"-w", L"--no-filters", L"--stdin" }, o);
        if (h.cancelled) return Result::fail(Status::Cancelled);
        if (!h.ok()) return fail(h, L"git hash-object");
        std::string oid = trim(h.out);
        m_staged[full] = oid;
        m_bytes += in.size();
        out = entry_of(full, oid, in.size());
        if (m_bytes > m_o.maxCommitBytes) {                 // a big pass goes up in parts
            std::string nc;
            Result r = push_staged(L"sync", nc, p);
            if (r.st == Status::CommitRaced) return Result::fail(Status::PreconditionFailed, L"Another device pushed meanwhile.");
            if (!r.ok()) return r;
            m_staging = true;
            m_base = m_tip;
            m_staged.clear();
            m_bytes = 0;
            m_baseTreeLoaded = false;
        }
        return {};
    }

    Result remove(const RelPath& path, const std::string& expectedRev, Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        if (!m_staging) return Result::fail(Status::Fatal, L"remove outside a transaction");
        std::string full = m_prefix + to_utf8(path);
        std::string cur;
        if (Result r = current(full, cur, p); !r.ok()) return r;
        if (cur.empty()) return Result::fail(Status::NotFound);
        if (Result r = check(full, expectedRev, p); !r.ok()) return r;
        m_staged[full] = "";
        return {};
    }

    Result move(const RelPath& from, const RelPath& to, const std::string& expectedRev, RemoteEntry& out, Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        if (!m_staging) return Result::fail(Status::Fatal, L"move outside a transaction");
        std::string f = m_prefix + to_utf8(from), t = m_prefix + to_utf8(to);
        std::string cur, there;
        if (Result r = current(f, cur, p); !r.ok()) return r;
        if (cur.empty()) return Result::fail(Status::NotFound);
        if (Result r = check(f, expectedRev, p); !r.ok()) return r;
        if (Result r = current(t, there, p); !r.ok()) return r;
        if (!there.empty()) return Result::fail(Status::PreconditionFailed);
        m_staged[f] = "";
        m_staged[t] = cur;
        uint64_t size = 0;
        auto bt = m_baseTree.find(f);
        if (bt != m_baseTree.end()) size = bt->second.second;
        out = entry_of(t, cur, size);
        return {};
    }

    Result remove_dir(const RelPath&, Progress&) override { return {}; }   // folders are implied

    Result commit(const std::wstring& summary, std::string& newCursor, Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        newCursor.clear();
        if (!m_staging) return {};
        Result r = push_staged(summary, newCursor, p);
        m_staging = false;
        m_staged.clear();
        return r;
    }
    void abort() override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        m_staging = false;
        m_staged.clear();
    }

    Result list_versions(const RelPath& path, std::vector<RemoteVersion>& out, Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        out.clear();
        if (Result r = fetch(p); !r.ok()) return r;
        if (m_tip.empty()) return Result::fail(Status::NotFound);
        std::string full = m_prefix + to_utf8(path);
        proc::Result l = git({ L"log", L"-n", L"50", L"--no-abbrev", L"--raw", L"--no-renames", L"--format=%x01%ct %s", w(m_tip), L"--", from_utf8(full) }, p);
        if (!l.ok()) return fail(l, L"git log");
        RemoteVersion cur;
        bool have = false;
        std::vector<std::string> need;
        size_t i = 0;
        while (i < l.out.size()) {
            size_t e = l.out.find('\n', i);
            if (e == std::string::npos) e = l.out.size();
            std::string line = l.out.substr(i, e - i);
            i = e + 1;
            if (!line.empty() && line[0] == '\x01') {
                have = true;
                uint64_t t = strtoull(line.c_str() + 1, nullptr, 10);
                uint64_t ft = (t + 11644473600ull) * 10000000ull;
                cur = {};
                cur.when.dwLowDateTime = (DWORD)ft;
                cur.when.dwHighDateTime = (DWORD)(ft >> 32);
                size_t sp = line.find(' ');
                cur.note = sp == std::string::npos ? L"" : from_utf8(line.substr(sp + 1));
            } else if (have && line.size() > 1 && line[0] == ':') {
                std::vector<GitDiffEntry> d;
                std::string z = line;
                size_t tab = z.find('\t');
                if (tab == std::string::npos) continue;
                z[tab] = '\0';
                z += '\0';
                if (!parse_diff_tree(z, d) || d.empty() || d[0].status == 'D') continue;
                cur.rev = d[0].newOid;
                need.push_back(cur.rev);
                out.push_back(cur);
            }
        }
        std::map<std::string, uint64_t> sizes;
        sizes_of(need, sizes, p);
        for (auto& v : out) v.size = sizes[v.rev];
        return {};
    }

private:
    static std::wstring w(const std::string& s) { return from_utf8(s); }

    proc::Env env() const {
        proc::Env e = {
            { L"GIT_DIR", m_o.mirror },
            { L"GIT_TERMINAL_PROMPT", L"0" },
            { L"GIT_ASKPASS", proc::kUnset },
            { L"SSH_ASKPASS", proc::kUnset },
            { L"GIT_CONFIG_PARAMETERS", proc::kUnset },
            { L"GCM_INTERACTIVE", L"never" },
            { L"GIT_INDEX_FILE", m_o.mirror + L"\\sync-index" },
            { L"LC_ALL", L"C" },
            { L"LANG", L"C" },
            { L"GIT_AUTHOR_NAME", m_o.author },
            { L"GIT_AUTHOR_EMAIL", m_o.email },
            { L"GIT_COMMITTER_NAME", m_o.author },
            { L"GIT_COMMITTER_EMAIL", m_o.email },
            { L"GIT_OPTIONAL_LOCKS", L"0" },
            // a connection that stalls (under 1 KB/s for a minute) fails instead of hanging the pass
            { L"GIT_HTTP_LOW_SPEED_LIMIT", L"1000" },
            { L"GIT_HTTP_LOW_SPEED_TIME", L"60" },
        };
        for (auto& x : m_o.env) e.push_back(x);
        return e;
    }

    proc::Result git(const std::vector<std::wstring>& args, Progress& p, bool inMirror = true, const std::string* stdinText = nullptr) {
        // (a URL git would take for an option: never run)
        if (!m_o.url.empty() && m_o.url[0] == L'-') { proc::Result r; r.started = true; r.exitCode = 1; r.err = "fatal: bad URL"; return r; }
        proc::Options o;
        if (inMirror) o.env = env();
        else o.env = m_o.env;
        o.stdinText = stdinText;
        o.cancelled = [&p] { return p.cancelled(); };
        return proc::run(m_o.gitExe, args, o);
    }

    // git's messages without secrets: the backend's own, and any URL's user:password@
    std::string scrub(const std::string& text) const {
        std::string s = m_o.scrub ? m_o.scrub(text) : text;
        for (size_t i = 0; (i = s.find("://", i)) != std::string::npos;) {
            i += 3;
            size_t end = s.find_first_of("/ \t\r\n'\"", i), at = s.find('@', i);
            if (at != std::string::npos && (end == std::string::npos || at < end)) s.replace(i, at - i, "***");
        }
        return s;
    }

    Result fail(const proc::Result& r, const std::wstring& what) const {
        if (r.cancelled) return Result::fail(Status::Cancelled);
        if (r.timedOut) return Result::fail(Status::Offline, what + L" timed out.");
        if (!r.started) return Result::fail(Status::Fatal, L"git.exe couldn't be started.");
        if (m_o.classify) {
            Result c = m_o.classify(r.err);
            if (!c.ok()) return c;
        }
        std::string e = r.err;
        auto has = [&](const char* s) { return e.find(s) != std::string::npos; };
        std::wstring text = from_utf8(scrub(trim(e)));
        if (text.size() > 600) text = text.substr(0, 600) + L"...";
        if (has("Authentication failed") || has("could not read Username") || has("could not read Password") ||
            has("HTTP Basic: Access denied") || has("returned error: 401") || has("returned error: 403") || has("Permission denied (publickey"))
            return Result::fail(Status::AuthFailed, L"The repository refused the sign-in. " + text);
        if (has("Could not resolve host") || has("Failed to connect") || has("Connection timed out") || has("Connection refused") ||
            has("Operation timed out") || has("unable to access") || has("Could not read from remote repository") || has("early EOF"))
            return Result::fail(Status::Offline, L"The repository can't be reached. " + text);
        if (has("returned error: 429") || has("returned error: 503")) return Result::fail(Status::Throttled, text);
        return Result::fail(Status::Fatal, what + L" failed: " + text);
    }

    Result ensure_mirror(Progress& p) {
        if (m_mirrorReady) return {};
        if (GetFileAttributesW((m_o.mirror + L"\\HEAD").c_str()) == INVALID_FILE_ATTRIBUTES) {
            size_t s = m_o.mirror.find_last_of(L'\\');
            if (s != std::wstring::npos) SHCreateDirectory(m_o.mirror.substr(0, s));
            proc::Result r = git({ L"init", L"--bare", L"--quiet", m_o.mirror }, p, false);
            if (!r.ok()) return fail(r, L"git init");
            // a private store: no automatic gc while syncing, no reflogs
            git({ L"config", L"gc.auto", L"0" }, p);
            git({ L"config", L"core.logAllRefUpdates", L"false" }, p);
        }
        m_mirrorReady = true;
        return {};
    }
    static void SHCreateDirectory(const std::wstring& d) {
        if (CreateDirectoryW(d.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return;
        size_t s = d.find_last_of(L'\\');
        if (s == std::wstring::npos) return;
        SHCreateDirectory(d.substr(0, s));
        CreateDirectoryW(d.c_str(), nullptr);
    }

    std::wstring ref() const { return L"refs/heads/" + m_o.branch; }

    Result remote_tip(std::string& tip, Progress& p) {
        if (Result r = ensure_mirror(p); !r.ok()) return r;
        proc::Result r = git({ L"ls-remote", m_o.url, ref() }, p);
        if (!r.ok()) return fail(r, L"git ls-remote");
        tip.clear();
        size_t tab = r.out.find('\t');
        if (tab != std::string::npos && tab >= 40) tip = r.out.substr(0, tab);
        return {};
    }

    Result fetch(Progress& p) {
        if (Result r = ensure_mirror(p); !r.ok()) return r;
        std::string tip;
        if (Result r = remote_tip(tip, p); !r.ok()) return r;
        m_fetched = true;
        if (tip.empty()) { m_tip.clear(); return {}; }
        if (tip != m_tip && !git({ L"cat-file", L"-e", w(tip) + L"^{commit}" }, p).ok()) {
            proc::Result r = git({ L"fetch", L"--quiet", L"--no-tags", L"--no-write-fetch-head", m_o.url, L"+" + ref() + L":refs/sync/tip" }, p);
            if (!r.ok()) return fail(r, L"git fetch");
        }
        m_tip = tip;
        return {};
    }

    Result ls_tree(const std::string& commit, const std::string& sub, std::vector<GitTreeEntry>& out, Progress& p, bool recursive = true) {
        std::vector<std::wstring> args = { L"ls-tree", L"-z", L"--long", L"--full-tree" };
        if (recursive) args.insert(args.begin() + 1, L"-r");
        args.push_back(w(commit));
        std::string path = m_prefix + sub;
        if (!path.empty()) { args.push_back(L"--"); args.push_back(from_utf8(path)); }
        proc::Result r = git(args, p);
        if (!r.ok()) return fail(r, L"git ls-tree");
        std::vector<GitTreeEntry> all;
        if (!parse_ls_tree(r.out, all)) return Result::fail(Status::Fatal, L"git ls-tree gave an unreadable answer.");
        for (auto& e : all) if (under_prefix(e.path)) out.push_back(std::move(e));
        return {};
    }

    bool under_prefix(const std::string& path) const {
        return m_prefix.empty() || (path.size() > m_prefix.size() && path.compare(0, m_prefix.size(), m_prefix) == 0);
    }

    RemoteEntry entry_of(const std::string& full, const std::string& oid, uint64_t size) const {
        RemoteEntry e;
        e.path = from_utf8(full.substr(std::min(full.size(), m_prefix.size())));
        e.rev = oid;
        e.size = size;
        e.hash.kind = HashKind::GitBlobSha1;
        crypto::Bytes b;
        if (crypto::unhex(oid, b) && b.size() == 20) std::copy(b.begin(), b.end(), e.hash.bytes.begin());
        return e;
    }

    Result sizes_of(const std::vector<std::string>& oids, std::map<std::string, uint64_t>& out, Progress& p) {
        if (oids.empty()) return {};
        std::string in;
        for (auto& o : oids) in += o + "\n";
        proc::Result r = git({ L"cat-file", L"--batch-check" }, p, true, &in);
        if (!r.ok()) return fail(r, L"git cat-file");
        size_t i = 0;
        while (i < r.out.size()) {
            size_t e = r.out.find('\n', i);
            if (e == std::string::npos) e = r.out.size();
            std::string line = r.out.substr(i, e - i);
            i = e + 1;
            size_t a = line.find(' '), b = line.rfind(' ');
            if (a == std::string::npos || b == a) continue;
            out[line.substr(0, a)] = strtoull(line.c_str() + b + 1, nullptr, 10);
        }
        return {};
    }

    // the base commit's tree, path -> (oid, size)
    Result load_base_tree(Progress& p) {
        if (m_baseTreeLoaded) return {};
        m_baseTree.clear();
        if (!m_base.empty()) {
            std::vector<GitTreeEntry> t;
            if (Result r = ls_tree(m_base, "", t, p); !r.ok()) return r;
            for (auto& e : t) if (e.type == "blob") m_baseTree[e.path] = { e.oid, e.size };
        }
        m_baseTreeLoaded = true;
        return {};
    }

    Result current(const std::string& full, std::string& oid, Progress& p) {
        auto s = m_staged.find(full);
        if (s != m_staged.end()) { oid = s->second; return {}; }
        if (Result r = load_base_tree(p); !r.ok()) return r;
        auto b = m_baseTree.find(full);
        oid = b == m_baseTree.end() ? std::string() : b->second.first;
        return {};
    }

    Result check(const std::string& full, const std::string& expectedRev, Progress& p) {
        if (expectedRev == "*") return {};
        std::string cur;
        if (Result r = current(full, cur, p); !r.ok()) return r;
        if (expectedRev.empty() ? !cur.empty() : cur != expectedRev) return Result::fail(Status::PreconditionFailed);
        return {};
    }

    Result push_staged(const std::wstring& summary, std::string& newCursor, Progress& p) {
        newCursor.clear();
        if (m_staged.empty()) return {};
        std::wstring index = m_o.mirror + L"\\sync-index";
        DeleteFileW(index.c_str());
        proc::Result r = m_base.empty() ? git({ L"read-tree", L"--empty" }, p) : git({ L"read-tree", w(m_base) }, p);
        if (!r.ok()) return fail(r, L"git read-tree");
        std::string info;
        for (auto& [path, oid] : m_staged) {
            if (oid.empty()) info += std::string("0 ") + kZero + "\t" + path + '\0';
            else info += "100644 " + oid + "\t" + path + '\0';
        }
        r = git({ L"update-index", L"-z", L"--index-info" }, p, true, &info);
        if (!r.ok()) return fail(r, L"git update-index");
        r = git({ L"write-tree" }, p);
        if (!r.ok()) return fail(r, L"git write-tree");
        std::string tree = trim(r.out);
        std::vector<std::wstring> args = { L"commit-tree", L"--no-gpg-sign", w(tree), L"-m", summary.empty() ? L"sync" : summary };
        if (!m_base.empty()) { args.push_back(L"-p"); args.push_back(w(m_base)); }
        r = git(args, p);
        if (!r.ok()) return fail(r, L"git commit-tree");
        std::string commit = trim(r.out);
        r = git({ L"push", L"--quiet", L"--no-verify", m_o.url, w(commit) + L":" + ref() }, p);
        DeleteFileW(index.c_str());
        if (!r.ok()) {
            const std::string& e = r.err;
            if (e.find("non-fast-forward") != std::string::npos || e.find("fetch first") != std::string::npos ||
                e.find("stale info") != std::string::npos || e.find("[rejected]") != std::string::npos ||
                e.find("cannot lock ref") != std::string::npos || e.find("failed to update ref") != std::string::npos)
                return Result::fail(Status::CommitRaced, L"Another device pushed first.");
            return fail(r, L"git push");
        }
        git({ L"update-ref", L"refs/sync/tip", w(commit) }, p);
        bool ours = m_base == m_listed;            // nothing between the engine's listing and this commit
        m_tip = commit;
        if (ours) { m_listed = commit; newCursor = commit; }
        if (++m_commits % 50 == 0) git({ L"gc", L"--auto", L"--quiet" }, p);
        return {};
    }

    GitOptions m_o;
    std::string m_prefix;
    std::recursive_mutex m_mx;
    std::mutex m_catMx;
    proc::Session m_cat;
    bool m_mirrorReady = false, m_fetched = false;
    std::string m_tip, m_listed, m_base;
    bool m_staging = false;
    std::map<std::string, std::string> m_staged;                 // full path -> oid ("" = removed)
    std::map<std::string, std::pair<std::string, uint64_t>> m_baseTree;
    bool m_baseTreeLoaded = false;
    uint64_t m_bytes = 0;
    int m_commits = 0;
};

} // namespace

std::unique_ptr<Backend> make_git_backend(GitOptions o) { return std::make_unique<GitBackend>(std::move(o)); }

void register_git_backend() {
    BackendType t;
    t.id = L"git";
    t.name = L"Git repository";
    t.fields = {
        { L"url", L"Repository URL", ConfigField::Url, true, L"", {}, L"Anything git can push to: https://..., git@..., or a path." },
        { L"branch", L"Branch", ConfigField::Text, false, L"explorerplus-sync", {}, L"Created by the first sync." },
        { L"subfolder", L"Folder in the repository", ConfigField::Text, false, L"", {}, L"" },
        { L"maxFileMB", L"Largest file (MB)", ConfigField::Number, false, L"50", {}, L"Hosts refuse big files (GitHub: 100 MB)." },
    };
    t.validate = [](const BackendConfig& c) -> std::wstring {
        std::wstring url = config_get(c, L"url");
        if (url.empty()) return L"Enter the repository's URL.";
        if (url[0] == L'-') return L"The URL can't start with '-'.";        // (git would read it as an option)
        std::wstring branch = config_get(c, L"branch");
        if (!branch.empty() && branch[0] == L'-') return L"The branch can't start with '-'.";
        return {};
    };
    t.create = [](const BackendConfig& c) -> std::unique_ptr<Backend> {
        GitOptions o;
        o.url = config_get(c, L"url");
        o.branch = config_get(c, L"branch", L"explorerplus-sync");
        o.subfolder = config_get(c, L"subfolder");
        o.mirror = config_get(c, L"mirror");
        uint64_t mb = wcstoull(config_get(c, L"maxFileMB", L"50").c_str(), nullptr, 10);
        if (mb) o.maxFileSize = mb << 20;
        return make_git_backend(std::move(o));
    };
    register_backend(std::move(t));
}

} // namespace sync
