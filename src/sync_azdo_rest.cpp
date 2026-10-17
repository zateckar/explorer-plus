// Azure DevOps Git through its REST API (docs/sync-plan.md, "Azure DevOps Git"): refs for
// the cheap poll, a recursive tree for the listing, raw blobs for downloads and one push per
// pass, made against the commit the pass started from (oldObjectId), so a push that lost a
// race with another device is refused whole (TF401028) and the engine re-plans.
#include "sync_azdo.h"
#include "crypto.h"
#include "http.h"
#include "json.h"
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>

namespace sync {

std::string git_blob_id(const void* data, size_t n) {
    static BCRYPT_ALG_HANDLE alg = [] {
        BCRYPT_ALG_HANDLE a = nullptr;
        BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
        return a;
    }();
    std::string head = "blob " + std::to_string(n);
    head += '\0';
    BCRYPT_HASH_HANDLE h = nullptr;
    uint8_t out[20] = {};
    if (!alg || BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0) return {};
    BCryptHashData(h, (PUCHAR)head.data(), (ULONG)head.size(), 0);
    const uint8_t* p = (const uint8_t*)data;
    while (n) {                                    // BCrypt takes 32-bit lengths
        ULONG k = (ULONG)std::min<size_t>(n, 1u << 30);
        BCryptHashData(h, (PUCHAR)p, k, 0);
        p += k;
        n -= k;
    }
    BCryptFinishHash(h, out, 20, 0);
    BCryptDestroyHash(h);
    return crypto::hex(out, 20);
}

std::wstring azdo_api_url(const std::wstring& cloneUrl) {
    size_t g = cloneUrl.find(L"/_git/");
    if (g == std::wstring::npos || g + 6 >= cloneUrl.size()) return {};
    return cloneUrl.substr(0, g) + L"/_apis/git/repositories/" + cloneUrl.substr(g + 6);
}

Result azdo_rest_failure(int status, const std::string& body, const std::wstring& what) {
    std::string msg, type;
    json::Value v;
    if (json::parse(body, v)) { msg = v["message"].str(); type = v["typeKey"].str(); }
    auto has = [&](const char* s) { return msg.find(s) != std::string::npos || type.find(s) != std::string::npos; };
    std::wstring text = from_utf8(msg);
    if (text.size() > 600) text = text.substr(0, 600) + L"...";
    if (has("TF401028") || has("GitReferenceStaleException")) return Result::fail(Status::CommitRaced, L"Another device synced first.");
    if (has("TF402455"))
        return Result::fail(Status::Rejected, L"This branch requires pull requests (branch policies). Choose a branch without policies, such as explorerplus-sync.");
    if (has("maximum request size")) return Result::fail(Status::TooLarge, L"Azure DevOps refused a push this big: " + text);
    if (status == 203 || status == 401)            // 203: the sign-in page instead of an answer
        return Result::fail(Status::AuthFailed, L"Azure DevOps refused the personal access token (expired, revoked, or without Code (Read & write) for this repository).");
    for (size_t tf = msg.find("TF"); tf != std::string::npos; tf = msg.find("TF", tf + 2)) {
        size_t n = 0;
        while (tf + 2 + n < msg.size() && isdigit((unsigned char)msg[tf + 2 + n])) ++n;
        if (n >= 5) return Result::fail(Status::Rejected, L"Azure DevOps refused: " + text);
    }
    if (status == 403) return Result::fail(Status::AuthFailed, L"Azure DevOps refused the access: " + text);
    if (status == 404) return Result::fail(Status::NotFound, text.empty() ? what + L": not found." : text);
    if (status == 409 || status == 412) return Result::fail(Status::PreconditionFailed, text);
    if (status == 413) return Result::fail(Status::TooLarge, L"Azure DevOps refused a push this big.");
    if (status == 429 || status == 503) return Result::fail(Status::Throttled, L"Azure DevOps asks to slow down. " + text);
    if (status >= 500) return Result::fail(Status::Offline, L"Azure DevOps had a problem (" + std::to_wstring(status) + L"). " + text);
    return Result::fail(Status::Fatal, what + L" failed (" + std::to_wstring(status) + L")" + (text.empty() ? L"." : L": " + text));
}

namespace {

constexpr char kZero[] = "0000000000000000000000000000000000000000";
constexpr const wchar_t* kApi = L"api-version=5.0";      // Azure DevOps Server 2019 and later, and Services

// "2026-10-17T22:49:15.123Z" -> FILETIME (zero when unreadable)
FILETIME parse_time(const std::string& s) {
    SYSTEMTIME st{};
    FILETIME ft{};
    if (sscanf(s.c_str(), "%4hu-%2hu-%2huT%2hu:%2hu:%2hu", &st.wYear, &st.wMonth, &st.wDay, &st.wHour, &st.wMinute, &st.wSecond) == 6)
        SystemTimeToFileTime(&st, &ft);
    return ft;
}
std::string now_iso() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char b[32];
    snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

class AzdoRestBackend : public Backend {
public:
    explicit AzdoRestBackend(AzdoRestOptions o) : m_o(std::move(o)) {
        m_api = azdo_api_url(m_o.url);
        while (!m_o.subfolder.empty() && m_o.subfolder.back() == L'/') m_o.subfolder.pop_back();
        while (!m_o.subfolder.empty() && m_o.subfolder.front() == L'/') m_o.subfolder.erase(0, 1);
        m_prefix = m_o.subfolder.empty() ? std::string() : to_utf8(m_o.subfolder) + "/";
        std::string token = ":" + to_utf8(m_o.pat);
        m_auth = "Basic " + crypto::base64(token.data(), token.size());
        crypto::wipe(token.data(), token.size());
        m_pat8 = to_utf8(m_o.pat);
        crypto::wipe(m_o.pat.data(), m_o.pat.size() * sizeof(wchar_t));
        m_o.pat.clear();
    }
    ~AzdoRestBackend() override {
        crypto::wipe(m_auth.data(), m_auth.size());
        crypto::wipe(m_pat8.data(), m_pat8.size());
    }

    Caps caps() const override {
        Caps c;
        c.changeFeed = true;                  // its own change token (the branch's tip)
        c.conditionalWrite = true;
        c.transactional = true;
        c.serverMove = true;
        c.versions = true;
        c.caseSensitive = true;
        c.emptyDirs = false;
        c.parallelism = 4;                    // downloads; uploads only gather the push
        c.maxNameLength = 255;
        c.maxPathLength = 4096;
        c.maxFileSize = max_file();
        c.maxCommitBytes = m_o.maxPushBytes;
        c.hash = HashKind::GitBlobSha1;
        c.pollSeconds = m_o.pollSeconds;
        return c;
    }

    Result connect(Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        if (m_api.empty()) return Result::fail(Status::Fatal, L"That isn't an Azure DevOps clone URL.");
        http::Response r = call("GET", L"", p);
        if (r.status != 200) return failure(r, L"Opening the repository");
        json::Value v;
        if (!json::parse(r.body, v) || v["id"].str().empty())
            return Result::fail(Status::Fatal, L"The server's answer isn't Azure DevOps' (check the clone URL).");
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
        if (Result r = load_tree(m_tip, p); !r.ok()) return r;
        out.full = true;
        for (auto& [path, b] : m_tree) if (under_prefix(path)) out.upserts.push_back(entry_of(path, b.oid, b.size));
        return {};
    }

    Result stat(const RelPath& path, RemoteEntry& out, Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        if (Result r = fetch(p); !r.ok()) return r;
        if (m_tip.empty()) return Result::fail(Status::NotFound);
        if (Result r = load_tree(m_tip, p); !r.ok()) return r;
        std::string full = m_prefix + to_utf8(path);
        auto it = m_tree.find(full);
        if (it != m_tree.end()) { out = entry_of(full, it->second.oid, it->second.size); return {}; }
        auto below = m_tree.lower_bound(full + "/");
        if (below != m_tree.end() && below->first.compare(0, full.size() + 1, full + "/") == 0) {
            out = {};
            out.path = path;
            out.isDir = true;
            return {};
        }
        return Result::fail(Status::NotFound);
    }

    Result list(const RelPath& prefix, std::vector<RemoteEntry>& out, Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        out.clear();
        if (Result r = fetch(p); !r.ok()) return r;
        if (m_tip.empty()) return {};
        if (Result r = load_tree(m_tip, p); !r.ok()) return r;
        std::string start = m_prefix + (prefix.empty() ? std::string() : to_utf8(prefix) + "/");
        for (auto it = m_tree.lower_bound(start); it != m_tree.end() && it->first.compare(0, start.size(), start) == 0; ++it)
            if (under_prefix(it->first)) out.push_back(entry_of(it->first, it->second.oid, it->second.size));
        return {};
    }

    // no lock held: downloads run side by side
    Result download(const RemoteEntry& e, ByteSink& sink, FileMeta&, Progress& p) override {
        if (e.rev.size() != 40) return Result::fail(Status::NotFound);
        http::Request q = request("GET", L"/blobs/" + from_utf8(e.rev) + L"?$format=octetstream", p);
        q.headers.push_back({ "Accept", "application/octet-stream" });
        q.sink = &sink;
        q.timeoutMs = 120000;
        http::Response r = http::send(q);
        if (r.sinkStopped) return Result::fail(Status::Cancelled, L"write stopped");
        if (r.status != 200) return failure(r, L"Downloading");
        return {};
    }

    Result begin(Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        if (!m_fetched) if (Result r = fetch(p); !r.ok()) return r;
        m_staging = true;
        m_base = m_tip;
        m_staged.clear();
        m_bytes = 0;
        return {};
    }

    Result upload(const RelPath& path, ByteSource& in, const FileMeta&, const std::string& expectedRev, RemoteEntry& out,
                  Progress& p) override {
        if (in.size() > max_file())
            return Result::fail(Status::TooLarge, L"Azure DevOps' REST API takes files up to " + std::to_wstring(max_file() >> 20) +
                                                  L" MB. Bigger ones need the git.exe connection (the pair's settings).");
        // read and encode outside the lock (several transfer threads gather one push)
        std::vector<uint8_t> data((size_t)in.size());
        size_t at = 0;
        for (;;) {
            if (p.cancelled()) return Result::fail(Status::Cancelled);
            size_t got = 0;
            if (at == data.size()) {                       // the end, or a source longer than it said
                uint8_t extra;
                if (!in.read(&extra, 1, &got)) return Result::fail(Status::Fatal, L"The file couldn't be read.");
                if (got) return Result::fail(Status::Fatal, L"The file changed while it was read.");
                break;
            }
            if (!in.read(data.data() + at, data.size() - at, &got)) return Result::fail(Status::Fatal, L"The file couldn't be read.");
            if (!got) return Result::fail(Status::Fatal, L"The file changed while it was read.");
            at += got;
        }
        Staged s;
        s.oid = git_blob_id(data.data(), data.size());
        s.size = data.size();
        s.b64 = crypto::base64(data.data(), data.size());
        s.content = true;
        p.bytes(data.size(), data.size());
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        if (!m_staging) return Result::fail(Status::Fatal, L"upload outside a transaction");
        std::string full = m_prefix + to_utf8(path);
        if (in_rename(full)) if (Result r = push_mid(p); !r.ok()) return r;
        if (Result r = check(full, expectedRev, p); !r.ok()) return r;
        if (m_bytes + s.b64.size() + 4096 > m_o.maxPushBytes && !m_staged.empty())
            if (Result r = push_mid(p); !r.ok()) return r;        // a big pass goes up in parts
        out = entry_of(full, s.oid, s.size);
        m_bytes += s.b64.size() + 512;
        m_staged[full] = std::move(s);
        return {};
    }

    Result remove(const RelPath& path, const std::string& expectedRev, Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        if (!m_staging) return Result::fail(Status::Fatal, L"remove outside a transaction");
        std::string full = m_prefix + to_utf8(path);
        if (in_rename(full)) if (Result r = push_mid(p); !r.ok()) return r;
        std::string cur;
        if (Result r = current(full, cur, p); !r.ok()) return r;
        if (cur.empty()) return Result::fail(Status::NotFound);
        if (Result r = check(full, expectedRev, p); !r.ok()) return r;
        m_staged[full] = Staged{};
        m_bytes += 512;
        return {};
    }

    Result move(const RelPath& from, const RelPath& to, const std::string& expectedRev, RemoteEntry& out, Progress& p) override {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        if (!m_staging) return Result::fail(Status::Fatal, L"move outside a transaction");
        std::string f = m_prefix + to_utf8(from), t = m_prefix + to_utf8(to);
        // a rename is made against the commit the push starts from: anything gathered on
        // either name goes up first
        if (m_staged.count(f) || m_staged.count(t)) if (Result r = push_mid(p); !r.ok()) return r;
        std::string cur, there;
        if (Result r = current(f, cur, p); !r.ok()) return r;
        if (cur.empty()) return Result::fail(Status::NotFound);
        if (Result r = check(f, expectedRev, p); !r.ok()) return r;
        if (Result r = current(t, there, p); !r.ok()) return r;
        if (!there.empty()) return Result::fail(Status::PreconditionFailed);
        uint64_t size = 0;
        if (auto b = m_tree.find(f); b != m_tree.end()) size = b->second.size;
        Staged gone;
        gone.renamedAway = true;
        m_staged[f] = gone;
        Staged moved;
        moved.oid = cur;
        moved.size = size;
        moved.renamedFrom = f;
        m_staged[t] = moved;
        m_bytes += 1024;
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
        std::wstring item = from_utf8(http::url_encode("/" + m_prefix + to_utf8(path), true));
        http::Response r = call("GET", L"/commits?searchCriteria.itemPath=" + item + L"&searchCriteria.itemVersion.version=" + from_utf8(m_tip) +
                                           L"&searchCriteria.itemVersion.versionType=commit&searchCriteria.$top=50", p);
        if (r.status != 200) return failure(r, L"Reading the history");
        json::Value v;
        if (!json::parse(r.body, v)) return Result::fail(Status::Fatal, L"Azure DevOps gave an unreadable history.");
        const json::Value& list = v["value"];
        for (size_t i = 0; i < list.size(); ++i) {
            const json::Value& c = list[i];
            // the file's blob at that commit (a commit that deleted it has none)
            http::Response ir = call("GET", L"/items?path=" + item + L"&versionDescriptor.version=" + from_utf8(c["commitId"].str()) +
                                                L"&versionDescriptor.versionType=commit&$format=json", p);
            if (ir.status == 404) continue;
            if (ir.status != 200) return failure(ir, L"Reading the history");
            json::Value iv;
            if (!json::parse(ir.body, iv) || iv["isFolder"].b) continue;
            RemoteVersion rv;
            rv.rev = iv["objectId"].str();
            if (rv.rev.size() != 40) continue;
            rv.when = parse_time(c["committer"]["date"].str());
            rv.note = from_utf8(c["comment"].str());
            http::Response br = call("GET", L"/blobs/" + from_utf8(rv.rev), p);
            json::Value bv;
            if (br.status == 200 && json::parse(br.body, bv)) rv.size = bv["size"].u64();
            out.push_back(std::move(rv));
        }
        return {};
    }

    // the run's branch goes away (tests)
    Result delete_branch(Progress& p) {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        std::string tip;
        if (Result r = remote_tip(tip, p); !r.ok()) return r;
        if (tip.empty()) return {};
        std::string body = "[{\"name\":" + json::quote("refs/heads/" + to_utf8(m_o.branch)) + ",\"oldObjectId\":" + json::quote(tip) +
                           ",\"newObjectId\":\"" + kZero + "\"}]";
        http::Response r = call("POST", L"/refs", p, &body);
        if (r.status != 200) return failure(r, L"Deleting the branch");
        json::Value v;
        if (!json::parse(r.body, v) || !v["value"][0]["success"].b) return Result::fail(Status::Fatal, L"Azure DevOps didn't delete the branch.");
        return {};
    }

private:
    struct Blob { std::string oid; uint64_t size = 0; };
    struct Staged {
        std::string oid;               // "" = removed
        uint64_t size = 0;
        bool content = false;          // b64 holds the bytes
        std::string b64;
        std::string renamedFrom;       // a rename's new name: the old one
        bool renamedAway = false;      // a rename's old name
    };

    uint64_t max_file() const { return (m_o.maxPushBytes - 4096) / 4 * 3; }

    http::Request request(const char* method, const std::wstring& rel, Progress& p) const {
        http::Request q;
        q.method = method;
        q.url = m_api + rel + (rel.find(L'?') == std::wstring::npos ? L"?" : L"&") + kApi;
        q.headers = { { "Authorization", m_auth }, { "Accept", "application/json" },
                      { "X-TFS-FedAuthRedirect", "Suppress" } };       // 401, not a sign-in page
        q.progress = &p;
        return q;
    }
    http::Response call(const char* method, const std::wstring& rel, Progress& p, const std::string* body = nullptr,
                        ByteSink* sink = nullptr, DWORD timeoutMs = 60000) const {
        http::Request q = request(method, rel, p);
        if (body) { q.bodyText = body; q.headers.push_back({ "Content-Type", "application/json" }); }
        q.sink = sink;
        q.timeoutMs = timeoutMs;
        return http::send(q);
    }
    Result failure(const http::Response& r, const std::wstring& what) const {
        if (r.error == ERROR_CANCELLED) return Result::fail(Status::Cancelled);
        if (!r.status) return Result::fail(Status::Offline, L"Azure DevOps can't be reached (" + std::to_wstring(r.error) + L").");
        Result x = azdo_rest_failure(r.status, r.body, what);
        x.message = from_utf8(azdo_scrub(to_utf8(x.message), m_pat8));
        if (x.st == Status::Throttled) {
            std::string ra = r.header("Retry-After");
            if (!ra.empty()) x.retryAfterMs = atoi(ra.c_str()) * 1000;
        }
        return x;
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

    Result remote_tip(std::string& tip, Progress& p) {
        tip.clear();
        std::string name = "refs/heads/" + to_utf8(m_o.branch);
        // the filter is a prefix ("heads/a" also lists "heads/ab"): the exact name is picked
        http::Response r = call("GET", L"/refs?filter=" + from_utf8(http::url_encode("heads/" + to_utf8(m_o.branch), true)), p);
        if (r.status != 200) return failure(r, L"Reading the branch");
        json::Value v;
        if (!json::parse(r.body, v)) return Result::fail(Status::Fatal, L"Azure DevOps gave an unreadable answer (refs).");
        const json::Value& list = v["value"];
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i]["name"].str() == name) tip = list[i]["objectId"].str();
        if (!tip.empty() && tip.size() != 40) return Result::fail(Status::Fatal, L"Azure DevOps gave an unreadable answer (refs).");
        return {};
    }

    Result fetch(Progress& p) {
        std::string tip;
        if (Result r = remote_tip(tip, p); !r.ok()) return r;
        m_fetched = true;
        m_tip = tip;
        return {};
    }

    // a commit's whole tree, the files only (folders are implied)
    Result load_tree(const std::string& commit, Progress& p) {
        if (m_treeOf == commit) return {};
        http::Response c = call("GET", L"/commits/" + from_utf8(commit), p);
        if (c.status != 200) return failure(c, L"Reading the commit");
        json::Value cv;
        if (!json::parse(c.body, cv) || cv["treeId"].str().size() != 40) return Result::fail(Status::Fatal, L"Azure DevOps gave an unreadable commit.");
        MemorySink sink;
        sink.limit = 1u << 30;
        http::Response t = call("GET", L"/trees/" + from_utf8(cv["treeId"].str()) + L"?recursive=true", p, nullptr, &sink, 300000);
        if (t.status != 200) return failure(t, L"Listing the files");
        json::Value tv;
        if (!json::parse(std::string_view((const char*)sink.data.data(), sink.data.size()), tv))
            return Result::fail(Status::Fatal, L"Azure DevOps gave an unreadable tree.");
        std::map<std::string, Blob> tree;
        const json::Value& list = tv["treeEntries"];
        for (size_t i = 0; i < list.size(); ++i) {
            const json::Value& e = list[i];
            if (e["gitObjectType"].str() != "blob") continue;
            tree[e["relativePath"].str()] = { e["objectId"].str(), e["size"].u64() };
        }
        m_tree = std::move(tree);
        m_treeOf = commit;
        return {};
    }

    Result current(const std::string& full, std::string& oid, Progress& p) {
        auto s = m_staged.find(full);
        if (s != m_staged.end()) { oid = s->second.oid; return {}; }
        oid.clear();
        if (m_base.empty()) return {};
        if (Result r = load_tree(m_base, p); !r.ok()) return r;
        auto b = m_tree.find(full);
        if (b != m_tree.end()) oid = b->second.oid;
        return {};
    }

    Result check(const std::string& full, const std::string& expectedRev, Progress& p) {
        if (expectedRev == "*") return {};
        std::string cur;
        if (Result r = current(full, cur, p); !r.ok()) return r;
        if (expectedRev.empty() ? !cur.empty() : cur != expectedRev) return Result::fail(Status::PreconditionFailed);
        return {};
    }

    bool in_rename(const std::string& full) const {
        auto s = m_staged.find(full);
        return s != m_staged.end() && (s->second.renamedAway || !s->second.renamedFrom.empty());
    }

    // what's gathered goes up now and the transaction carries on from that commit
    Result push_mid(Progress& p) {
        std::string nc;
        Result r = push_staged(L"sync", nc, p);
        if (r.st == Status::CommitRaced) return Result::fail(Status::PreconditionFailed, L"Another device synced meanwhile.");
        if (!r.ok()) return r;
        m_staged.clear();
        m_bytes = 0;
        return {};
    }

    Result push_staged(const std::wstring& summary, std::string& newCursor, Progress& p) {
        newCursor.clear();
        if (m_staged.empty()) return {};
        if (!m_base.empty()) if (Result r = load_tree(m_base, p); !r.ok()) return r;
        auto inBase = [&](const std::string& path) { return !m_base.empty() && m_tree.count(path) != 0; };
        std::string changes;
        auto add = [&](const std::string& c) { if (!changes.empty()) changes += ','; changes += c; };
        for (auto& [path, s] : m_staged) {
            std::string item = "{\"path\":" + json::quote("/" + path) + "}";
            if (!s.renamedFrom.empty())
                add("{\"changeType\":\"rename\",\"sourceServerItem\":" + json::quote("/" + s.renamedFrom) + ",\"item\":" + item + "}");
            else if (s.content)
                add(std::string("{\"changeType\":\"") + (inBase(path) ? "edit" : "add") + "\",\"item\":" + item +
                    ",\"newContent\":{\"content\":\"" + s.b64 + "\",\"contentType\":\"base64encoded\"}}");
            else if (!s.renamedAway && inBase(path))
                add("{\"changeType\":\"delete\",\"item\":" + item + "}");
        }
        if (changes.empty()) return {};
        std::string who = "{\"name\":" + json::quote(to_utf8(m_o.author)) + ",\"email\":" + json::quote(to_utf8(m_o.email)) +
                          ",\"date\":\"" + now_iso() + "\"}";
        std::string body = "{\"refUpdates\":[{\"name\":" + json::quote("refs/heads/" + to_utf8(m_o.branch)) + ",\"oldObjectId\":\"" +
                           (m_base.empty() ? std::string(kZero) : m_base) + "\"}],\"commits\":[{\"comment\":" +
                           json::quote(to_utf8(summary.empty() ? L"sync" : summary)) + ",\"author\":" + who + ",\"committer\":" + who +
                           ",\"changes\":[" + changes + "]}]}";
        changes.clear();
        http::Response r = call("POST", L"/pushes", p, &body, nullptr, 600000);
        crypto::wipe(body.data(), body.size());
        if (r.status != 200 && r.status != 201) return failure(r, L"Pushing");
        json::Value v;
        std::string commit;
        if (json::parse(r.body, v)) commit = v["refUpdates"][0]["newObjectId"].str();
        if (commit.size() != 40) return Result::fail(Status::Fatal, L"Azure DevOps accepted the push but gave no commit.");
        // the new commit's tree is the old one with what went up (saves asking for it)
        if (m_treeOf == m_base || m_base.empty()) {
            if (m_base.empty()) m_tree.clear();
            for (auto& [path, s] : m_staged) {
                if (s.oid.empty()) m_tree.erase(path);
                else m_tree[path] = { s.oid, s.size };
            }
            m_treeOf = commit;
        }
        bool ours = m_base == m_listed;            // nothing between the engine's listing and this push
        m_tip = commit;
        m_base = commit;
        if (ours) { m_listed = commit; newCursor = commit; }
        return {};
    }

    AzdoRestOptions m_o;
    std::wstring m_api;
    std::string m_auth, m_pat8, m_prefix;
    std::recursive_mutex m_mx;
    bool m_fetched = false;
    std::string m_tip, m_listed, m_base;
    std::string m_treeOf;                          // the commit m_tree is of
    std::map<std::string, Blob> m_tree;            // full path -> blob
    bool m_staging = false;
    std::map<std::string, Staged> m_staged;        // full path -> what goes up
    uint64_t m_bytes = 0;
};

} // namespace

std::unique_ptr<Backend> make_azdo_rest_backend(AzdoRestOptions o) { return std::make_unique<AzdoRestBackend>(std::move(o)); }

Result azdo_rest_delete_branch(const AzdoRestOptions& o) {
    AzdoRestBackend b(o);
    NoProgress np;
    return b.delete_branch(np);
}

} // namespace sync
