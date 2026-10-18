// git's push protocol, built without git.exe (docs/sync-plan.md, "Azure DevOps Git"): tree
// and commit objects, a pack of them and the receive-pack request a smart HTTP server takes
// (as git.exe sends it: the ref update as a pkt-line, a flush, the pack). The pack holds
// whole objects zlib-"deflated" in stored blocks: no deltas, no compression (the files are
// ciphertext), so it's written as it's sent, of any size, with its length known up front.
#include "sync_azdo.h"
#include "crypto.h"
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <set>

namespace sync {

namespace {
BCRYPT_ALG_HANDLE sha1_alg() {
    static BCRYPT_ALG_HANDLE alg = [] {
        BCRYPT_ALG_HANDLE a = nullptr;
        BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
        return a;
    }();
    return alg;
}
}

Sha1::Sha1() {
    BCRYPT_HASH_HANDLE h = nullptr;
    if (sha1_alg() && BCryptCreateHash(sha1_alg(), &h, nullptr, 0, nullptr, 0, 0) == 0) m_h = h;
}
Sha1::~Sha1() { if (m_h) BCryptDestroyHash((BCRYPT_HASH_HANDLE)m_h); }
void Sha1::update(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    while (n && m_h) {                             // BCrypt takes 32-bit lengths
        ULONG k = (ULONG)std::min<size_t>(n, 1u << 30);
        BCryptHashData((BCRYPT_HASH_HANDLE)m_h, (PUCHAR)p, k, 0);
        p += k;
        n -= k;
    }
}
std::array<uint8_t, 20> Sha1::final() {
    std::array<uint8_t, 20> out{};
    if (m_h) BCryptFinishHash((BCRYPT_HASH_HANDLE)m_h, out.data(), 20, 0);
    return out;
}

std::string git_object_id(const char* type, const void* data, size_t n) {
    std::string head = std::string(type) + " " + std::to_string(n);
    head += '\0';
    Sha1 h;
    h.update(head.data(), head.size());
    h.update(data, n);
    return crypto::hex(h.final());
}

namespace {

std::string parent_dir(const std::string& p) {
    size_t s = p.rfind('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}
std::string leaf(const std::string& p) {
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

struct TreeEnt { std::string name, mode, oid; bool tree = false; };

// git's order: names byte by byte, a tree's as if it ended in '/'
bool tree_less(const TreeEnt& a, const TreeEnt& b) {
    std::string x = a.tree ? a.name + "/" : a.name, y = b.tree ? b.name + "/" : b.name;
    return x < y;
}

} // namespace

std::string git_build_trees(const std::map<std::string, GitEntry>& files, std::vector<GitTree>& trees) {
    trees.clear();
    std::map<std::string, std::vector<TreeEnt>> ents;      // folder -> its entries
    std::set<std::string> dirs = { "" };
    for (auto& [path, e] : files) {
        std::string d = parent_dir(path);
        ents[d].push_back({ leaf(path), e.mode, e.oid, false });
        while (!d.empty() && dirs.insert(d).second) d = parent_dir(d);
    }
    // the deepest folders first: a folder's subtrees are known when it's written
    std::vector<std::string> order(dirs.begin(), dirs.end());
    std::stable_sort(order.begin(), order.end(), [](const std::string& a, const std::string& b) {
        auto depth = [](const std::string& s) { return s.empty() ? 0 : 1 + (int)std::count(s.begin(), s.end(), '/'); };
        return depth(a) > depth(b);
    });
    std::string root;
    for (auto& d : order) {
        std::vector<TreeEnt>& v = ents[d];
        std::sort(v.begin(), v.end(), tree_less);
        std::string data;
        for (auto& e : v) {
            crypto::Bytes raw;
            crypto::unhex(e.oid, raw);
            if (raw.size() != 20) raw.assign(20, 0);
            data += e.mode + " " + e.name;
            data += '\0';
            data.append((const char*)raw.data(), 20);
        }
        std::string oid = git_object_id("tree", data.data(), data.size());
        if (d.empty()) root = oid;
        else ents[parent_dir(d)].push_back({ leaf(d), "40000", oid, true });
        trees.push_back({ d, oid, std::move(data) });
    }
    return root;
}

std::string git_commit_object(const std::string& tree, const std::string& parent, const std::string& who, uint64_t unixTime,
                              const std::string& message) {
    std::string stamp = who + " " + std::to_string(unixTime) + " +0000\n";
    std::string c = "tree " + tree + "\n";
    if (!parent.empty()) c += "parent " + parent + "\n";
    c += "author " + stamp + "committer " + stamp + "\n" + message;
    if (c.back() != '\n') c += '\n';
    return c;
}

namespace {

std::string pkt_line(const std::string& s) {
    char len[5];
    snprintf(len, sizeof(len), "%04x", (unsigned)(s.size() + 4));
    return len + s;
}

constexpr size_t kBlock = 65535;                   // a stored deflate block's most

// the request, produced as it's read
class PackRequest : public ByteSource {
public:
    PackRequest(std::string prefix, std::vector<PackObject> objs) : m_prefix(std::move(prefix)), m_objs(std::move(objs)) {
        m_total = m_prefix.size() + 12 + 20;
        for (auto& o : m_objs) {
            uint64_t n = size_of(o);
            uint64_t blocks = n ? (n + kBlock - 1) / kBlock : 1;
            m_total += header(o.type, n).size() + 2 + blocks * 5 + n + 4;
        }
        rewind();
    }
    uint64_t size() override { return m_total; }
    bool rewind() override {
        m_sha = std::make_unique<Sha1>();
        m_buf = m_prefix;
        m_at = 0;
        m_obj = 0;
        m_off = 0;
        m_inObj = false;
        m_done = false;
        std::string head = "PACK";
        put32(head, 2);
        put32(head, (uint32_t)m_objs.size());
        add(head, true);
        return true;
    }
    bool read(void* out, size_t n, size_t* got) override {
        *got = 0;
        while (*got < n) {
            if (m_at == m_buf.size()) {
                m_buf.clear();
                m_at = 0;
                if (!refill()) return false;
                if (m_buf.empty()) break;
            }
            size_t k = std::min(n - *got, m_buf.size() - m_at);
            memcpy((uint8_t*)out + *got, m_buf.data() + m_at, k);
            m_at += k;
            *got += k;
        }
        return true;
    }

private:
    static uint64_t size_of(const PackObject& o) { return o.file != INVALID_HANDLE_VALUE ? o.size : o.data.size(); }
    static void put32(std::string& s, uint32_t v) { for (int i = 3; i >= 0; --i) s += (char)(v >> (8 * i)); }
    static std::string header(int type, uint64_t n) {
        std::string h;
        uint8_t c = (uint8_t)((type << 4) | (n & 15));
        n >>= 4;
        while (n) { h += (char)(c | 0x80); c = n & 0x7f; n >>= 7; }
        h += (char)c;
        return h;
    }
    void add(const std::string& s, bool hashed) {
        if (hashed) m_sha->update(s.data(), s.size());
        m_buf += s;
    }
    // the next piece: an object's header, one stored block of it, its end; the trailer
    bool refill() {
        if (m_done) return true;
        if (m_obj == m_objs.size()) {
            auto d = m_sha->final();
            m_buf.assign((const char*)d.data(), 20);
            m_done = true;
            return true;
        }
        PackObject& o = m_objs[m_obj];
        uint64_t n = size_of(o);
        if (!m_inObj) {
            add(header(o.type, n) + "\x78\x01", true);    // zlib: deflate, no dictionary, fastest
            m_inObj = true;
            m_off = 0;
            m_a = 1;
            m_b = 0;
            if (o.file != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER z{};
                if (!SetFilePointerEx(o.file, z, nullptr, FILE_BEGIN)) return false;
            }
            return true;
        }
        size_t k = (size_t)std::min<uint64_t>(kBlock, n - m_off);
        std::string blk;
        blk += (char)(m_off + k == n ? 1 : 0);            // BFINAL, BTYPE 00 (stored)
        blk += (char)(k & 0xff);
        blk += (char)(k >> 8);
        blk += (char)(~k & 0xff);
        blk += (char)((~k >> 8) & 0xff);
        size_t at = blk.size();
        blk.resize(at + k);
        if (o.file != INVALID_HANDLE_VALUE) {
            DWORD r = 0;
            if (k && (!ReadFile(o.file, blk.data() + at, (DWORD)k, &r, nullptr) || r != k)) return false;
        } else if (k) {
            memcpy(blk.data() + at, o.data.data() + m_off, k);
        }
        for (size_t i = 0; i < k; ++i) {                  // Adler-32 (64-bit sums: a block can't overflow them)
            m_a += (uint8_t)blk[at + i];
            m_b += m_a;
        }
        m_a %= 65521;
        m_b %= 65521;
        m_off += k;
        add(blk, true);
        if (m_off == n) {
            std::string tail;
            put32(tail, (uint32_t)((m_b << 16) | m_a));
            add(tail, true);
            m_inObj = false;
            ++m_obj;
        }
        return true;
    }

    std::string m_prefix;
    std::vector<PackObject> m_objs;
    uint64_t m_total = 0;
    std::unique_ptr<Sha1> m_sha;
    std::string m_buf;
    size_t m_at = 0, m_obj = 0;
    uint64_t m_off = 0;
    uint64_t m_a = 1, m_b = 0;
    bool m_inObj = false, m_done = false;
};

// pkt-lines of a body (stops at a malformed one); flushes are ""
std::vector<std::string> pkt_lines(const std::string& body, bool* ok = nullptr) {
    std::vector<std::string> out;
    size_t at = 0;
    if (ok) *ok = true;
    while (at + 4 <= body.size()) {
        unsigned len = 0;
        if (sscanf(body.substr(at, 4).c_str(), "%4x", &len) != 1 || (len && len < 4) || at + std::max(len, 4u) > body.size()) {
            if (ok) *ok = false;
            break;
        }
        out.push_back(len ? body.substr(at + 4, len - 4) : std::string());
        at += len ? len : 4;
    }
    return out;
}

} // namespace

std::unique_ptr<ByteSource> git_receive_pack_request(const std::string& oldId, const std::string& newId, const std::string& ref,
                                                     std::vector<PackObject> objects) {
    std::string cmd = (oldId.empty() ? std::string(40, '0') : oldId) + " " + newId + " " + ref;
    cmd += '\0';
    cmd += "report-status agent=explorer-plus\n";
    return std::make_unique<PackRequest>(pkt_line(cmd) + "0000", std::move(objects));
}

bool git_advertised_ref(const std::string& body, const std::string& ref, std::string& oid) {
    oid.clear();
    bool ok = false;
    std::vector<std::string> lines = pkt_lines(body, &ok);
    if (!ok || lines.empty()) return false;
    for (auto& l : lines) {
        std::string s = l.substr(0, l.find('\0'));
        while (!s.empty() && s.back() == '\n') s.pop_back();
        if (s.size() > 41 && s[40] == ' ' && s.substr(41) == ref) oid = s.substr(0, 40);
    }
    return true;
}

Result git_receive_pack_result(const std::string& body, const std::string& ref) {
    std::string unpack, status;
    for (auto& l : pkt_lines(body)) {
        std::string s = l;
        if (!s.empty() && (s[0] == 1 || s[0] == 2 || s[0] == 3)) s.erase(0, 1);      // (side-band, if sent anyway)
        while (!s.empty() && s.back() == '\n') s.pop_back();
        if (s.compare(0, 7, "unpack ") == 0) unpack = s.substr(7);
        else if (s.compare(0, 3, "ok ") == 0 && s.substr(3) == ref) status = "ok";
        else if (s.compare(0, 3, "ng ") == 0 && s.compare(3, ref.size(), ref) == 0) status = s.substr(std::min(s.size(), 4 + ref.size()));
    }
    if (unpack.empty() && status.empty()) return Result::fail(Status::Fatal, L"The server's answer to the push isn't git's.");
    if (unpack != "ok") return Result::fail(Status::Fatal, L"The server couldn't read the pushed data: " + from_utf8(unpack));
    if (status == "ok") return {};
    auto has = [&](const char* w) { return status.find(w) != std::string::npos; };
    if (has("TF401028") || has("fetch first") || has("non-fast-forward") || has("stale") || has("failed to lock") || has("failed to update ref") || has("incorrect old value") || has("already exists"))
        return Result::fail(Status::CommitRaced, L"Another device synced first.");
    if (Result c = azdo_classify(status); !c.ok()) return c;
    return Result::fail(Status::Rejected, L"The server refused the push: " + from_utf8(status.empty() ? std::string("no reason given") : status));
}

} // namespace sync
