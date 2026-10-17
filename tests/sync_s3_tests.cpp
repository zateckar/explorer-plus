// S3: SigV4 against AWS' published example, the XML listing, and the store against an
// in-process S3 server (signatures checked on every request) — raw operations, then two
// encrypted devices syncing through it.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "sync_tests_common.h"
#include "sync_test_util.h"
#include "sync_crypt.h"
#include "sync_engine.h"
#include "sync_keys.h"
#include "sync_s3.h"
#include "crypto.h"
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

using namespace sync;

namespace {

void test_sigv4() {
    // AWS S3 docs, "Signature calculations ... Example: GET Object"
    sigv4::Request r;
    r.method = "GET";
    r.host = "examplebucket.s3.amazonaws.com";
    r.path = "/test.txt";
    r.headers = { { "Range", "bytes=0-9" } };
    r.payloadHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    sigv4::Credentials c{ "AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "" };
    sigv4::sign(r, c, "us-east-1", "s3", "20130524T000000Z");
    std::string auth;
    for (auto& [k, v] : r.headers) if (k == "Authorization") auth = v;
    CHECK(auth == "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
                  "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
                  "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
    // "Example: GET Bucket Lifecycle" (a query parameter without a value)
    sigv4::Request l;
    l.method = "GET";
    l.host = "examplebucket.s3.amazonaws.com";
    l.path = "/";
    l.query = { { "lifecycle", "" } };
    l.payloadHash = r.payloadHash;
    sigv4::sign(l, c, "us-east-1", "s3", "20130524T000000Z");
    for (auto& [k, v] : l.headers) if (k == "Authorization") auth = v;
    CHECK(auth.find("Signature=fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543") != std::string::npos);
    // "Example: Get Bucket (List Objects)"
    sigv4::Request g;
    g.method = "GET";
    g.host = "examplebucket.s3.amazonaws.com";
    g.path = "/";
    g.query = { { "max-keys", "2" }, { "prefix", "J" } };
    g.payloadHash = r.payloadHash;
    sigv4::sign(g, c, "us-east-1", "s3", "20130524T000000Z");
    for (auto& [k, v] : g.headers) if (k == "Authorization") auth = v;
    CHECK(auth.find("Signature=34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7") != std::string::npos);
    CHECK(sigv4::uri_encode("a b/c~d*", true) == "a%20b/c~d%2A");
}

void test_xml() {
    std::string x = "<?xml version=\"1.0\"?><ListBucketResult><IsTruncated>true</IsTruncated>"
                    "<Contents><Key>a/b&amp;c</Key><Size>12</Size><ETag>&quot;abc&quot;</ETag></Contents>"
                    "<Contents><Key>x</Key><Size>0</Size><ETag>\"def\"</ETag></Contents>"
                    "<NextContinuationToken>tok/1</NextContinuationToken></ListBucketResult>";
    std::vector<S3Object> v;
    bool t = false;
    std::string next;
    CHECK(parse_list_objects(x, v, t, next));
    CHECK(v.size() == 2 && v[0].key == "a/b&c" && v[0].size == 12 && v[0].etag == "abc" && v[1].etag == "def" && t && next == "tok/1");
    CHECK(xml_unescape("&#x160;koda &#65;") == "\xC5\xA0koda A");
    CHECK(!parse_list_objects("<Error><Code>AccessDenied</Code></Error>", v, t, next));
}

// ------------------------------------------------------------------ an S3 server for the tests
std::string pct_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) { o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else o += s[i];
    }
    return o;
}

class MockS3 {
public:
    sigv4::Credentials cred{ "AKTEST", "secret/test+key", "" };
    std::string bucket = "bkt";
    std::mutex mx;
    std::map<std::string, std::string> objs, etags;
    std::atomic<int> requests{ 0 }, badSignatures{ 0 }, lists{ 0 }, heads{ 0 };
    int port = 0;

    bool start() {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        m_ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(m_ls, (sockaddr*)&a, sizeof(a)) || listen(m_ls, 16)) return false;
        int len = sizeof(a);
        getsockname(m_ls, (sockaddr*)&a, &len);
        port = ntohs(a.sin_port);
        m_th = std::thread([this] { loop(); });
        return true;
    }
    ~MockS3() {
        m_stop = true;
        closesocket(m_ls);
        if (m_th.joinable()) m_th.join();
        for (auto& t : m_conns) if (t.joinable()) t.join();
    }

private:
    void loop() {
        while (!m_stop) {
            SOCKET c = accept(m_ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            m_conns.emplace_back([this, c] { serve(c); });
        }
    }
    static bool recv_all(SOCKET c, std::string& buf, size_t want) {
        char b[65536];
        while (buf.size() < want) {
            int n = recv(c, b, sizeof(b), 0);
            if (n <= 0) return false;
            buf.append(b, n);
        }
        return true;
    }
    void serve(SOCKET c) {
        for (;;) {
            std::string buf;
            size_t he;
            char b[65536];
            while ((he = buf.find("\r\n\r\n")) == std::string::npos) {
                int n = recv(c, b, sizeof(b), 0);
                if (n <= 0) { closesocket(c); return; }
                buf.append(b, n);
            }
            std::string head = buf.substr(0, he);
            std::string body = buf.substr(he + 4);
            std::vector<std::pair<std::string, std::string>> hdrs;
            size_t le = head.find("\r\n");
            std::string line = head.substr(0, le);
            size_t i = le + 2;
            while (i < head.size()) {
                size_t e = head.find("\r\n", i);
                if (e == std::string::npos) e = head.size();
                std::string h = head.substr(i, e - i);
                i = e + 2;
                size_t col = h.find(':');
                if (col == std::string::npos) continue;
                std::string k = h.substr(0, col), v = h.substr(col + 1);
                for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
                while (!v.empty() && v[0] == ' ') v.erase(0, 1);
                hdrs.push_back({ k, v });
            }
            auto hv = [&](const std::string& k) { for (auto& [a, v] : hdrs) if (a == k) return v; return std::string(); };
            size_t cl = strtoull(hv("content-length").c_str(), nullptr, 10);
            if (!recv_all(c, body, cl)) { closesocket(c); return; }
            body.resize(cl);
            std::string resp = handle(line, hdrs, hv, body);
            send(c, resp.data(), (int)resp.size(), 0);
            if (hv("connection") == "close") break;
        }
        closesocket(c);
    }

    template <class HV>
    std::string handle(const std::string& line, const std::vector<std::pair<std::string, std::string>>& hdrs, HV hv, const std::string& body) {
        ++requests;
        size_t s1 = line.find(' '), s2 = line.rfind(' ');
        std::string method = line.substr(0, s1), target = line.substr(s1 + 1, s2 - s1 - 1);
        std::string path = target, rawQuery;
        size_t q = target.find('?');
        if (q != std::string::npos) { path = target.substr(0, q); rawQuery = target.substr(q + 1); }
        std::vector<std::pair<std::string, std::string>> query;
        for (size_t i = 0; i < rawQuery.size();) {
            size_t e = rawQuery.find('&', i);
            if (e == std::string::npos) e = rawQuery.size();
            std::string kv = rawQuery.substr(i, e - i);
            size_t eq = kv.find('=');
            query.push_back({ pct_decode(kv.substr(0, eq)), eq == std::string::npos ? "" : pct_decode(kv.substr(eq + 1)) });
            i = e + 1;
        }
        // the signature, as S3 checks it
        std::string auth = hv("authorization");
        size_t sh = auth.find("SignedHeaders="), sg = auth.find("Signature=");
        bool good = false;
        if (sh != std::string::npos && sg != std::string::npos) {
            std::string signedList = auth.substr(sh + 14, auth.find(',', sh) - sh - 14);
            sigv4::Request r;
            r.method = method;
            r.host = hv("host");
            r.path = path;
            r.query = query;
            r.payloadHash = hv("x-amz-content-sha256");
            for (size_t i = 0; i <= signedList.size();) {
                size_t e = signedList.find(';', i);
                if (e == std::string::npos) e = signedList.size();
                std::string k = signedList.substr(i, e - i);
                i = e + 1;
                if (k != "host") r.headers.push_back({ k, hv(k) });
            }
            std::string sl;
            std::string creq = sigv4::canonical_request(r, sl);
            good = sl == signedList && sigv4::signature(creq, cred, "us-east-1", "s3", hv("x-amz-date")) == auth.substr(sg + 10) &&
                   auth.find("Credential=" + cred.accessKey + "/") != std::string::npos;
        }
        if (!good) { ++badSignatures; return reply(403, "<Error><Code>SignatureDoesNotMatch</Code></Error>"); }
        std::string pre = "/" + bucket;
        if (path.compare(0, pre.size(), pre) != 0) return reply(404, "<Error><Code>NoSuchBucket</Code></Error>");
        std::string key = path.size() > pre.size() ? pct_decode(path.substr(pre.size() + 1)) : "";
        std::lock_guard<std::mutex> lk(mx);
        if (key.empty() && method == "GET") {
            ++lists;
            std::string prefix, token;
            for (auto& [k, v] : query) { if (k == "prefix") prefix = v; if (k == "continuation-token") token = v; }
            std::string x = "<ListBucketResult>";
            int n = 0;
            bool more = false;
            std::string last;
            for (auto it = objs.upper_bound(token); it != objs.end(); ++it) {
                if (it->first.compare(0, prefix.size(), prefix) != 0) continue;
                if (n == 3) { more = true; break; }                // small pages: the paging is tested too
                x += "<Contents><Key>" + it->first + "</Key><Size>" + std::to_string(it->second.size()) + "</Size><ETag>&quot;" + etags[it->first] + "&quot;</ETag></Contents>";
                last = it->first;
                ++n;
            }
            x += std::string("<IsTruncated>") + (more ? "true" : "false") + "</IsTruncated>";
            if (more) x += "<NextContinuationToken>" + last + "</NextContinuationToken>";
            return reply(200, x + "</ListBucketResult>");
        }
        auto it = objs.find(key);
        if (method == "HEAD" || method == "GET") {
            if (method == "HEAD") ++heads;
            if (it == objs.end()) return reply(404, method == "HEAD" ? "" : "<Error><Code>NoSuchKey</Code></Error>");
            std::string range = hv("range");
            std::string data = it->second;
            int code = 200;
            if (method == "GET" && range.compare(0, 6, "bytes=") == 0) {
                uint64_t a = strtoull(range.c_str() + 6, nullptr, 10), b = strtoull(range.c_str() + range.find('-') + 1, nullptr, 10);
                data = a < data.size() ? data.substr(a, b - a + 1) : "";
                code = 206;
            }
            return reply(code, method == "HEAD" ? "" : data, { { "ETag", "\"" + etags[key] + "\"" } }, method == "HEAD" ? it->second.size() : SIZE_MAX);
        }
        if (method == "PUT") {
            std::string inm = hv("if-none-match"), im = hv("if-match"), src = hv("x-amz-copy-source");
            if (inm == "*" && it != objs.end()) return reply(412, "<Error><Code>PreconditionFailed</Code></Error>");
            if (!im.empty() && (it == objs.end() || "\"" + etags[key] + "\"" != im)) return reply(412, "<Error><Code>PreconditionFailed</Code></Error>");
            std::string data = body;
            if (!src.empty()) {
                std::string sk = pct_decode(src.substr(pre.size() + 1));
                auto s = objs.find(sk);
                if (s == objs.end()) return reply(404, "<Error><Code>NoSuchKey</Code></Error>");
                if (!hv("x-amz-copy-source-if-match").empty() && hv("x-amz-copy-source-if-match") != "\"" + etags[sk] + "\"")
                    return reply(412, "<Error><Code>PreconditionFailed</Code></Error>");
                data = s->second;
            }
            crypto::Hash32 h = crypto::blake2b(data.data(), data.size());
            objs[key] = data;
            etags[key] = crypto::hex(h.data(), 16);
            if (!src.empty()) return reply(200, "<CopyObjectResult><ETag>&quot;" + etags[key] + "&quot;</ETag></CopyObjectResult>");
            return reply(200, "", { { "ETag", "\"" + etags[key] + "\"" } });
        }
        if (method == "DELETE") {
            objs.erase(key);
            etags.erase(key);
            return reply(204, "");
        }
        return reply(405, "");
    }

    static std::string reply(int code, const std::string& body, const std::vector<std::pair<std::string, std::string>>& extra = {}, size_t length = SIZE_MAX) {
        std::string r = "HTTP/1.1 " + std::to_string(code) + " X\r\nContent-Length: " + std::to_string(length == SIZE_MAX ? body.size() : length) + "\r\n";
        for (auto& [k, v] : extra) r += k + ": " + v + "\r\n";
        return r + "\r\n" + body;
    }

    SOCKET m_ls = INVALID_SOCKET;
    std::thread m_th;
    std::vector<std::thread> m_conns;
    std::atomic<bool> m_stop{ false };
};

S3Options opts(MockS3& m, const std::string& prefix = "pfx") {
    S3Options o;
    o.endpoint = L"http://127.0.0.1:" + std::to_wstring(m.port);
    o.bucket = m.bucket;
    o.prefix = prefix;
    o.cred = m.cred;
    o.pathStyle = true;
    return o;
}

Result put(Backend& b, const RelPath& p, const std::string& text, const std::string& rev, std::string* out = nullptr) {
    NoProgress np;
    MemorySource s(std::vector<uint8_t>(text.begin(), text.end()));
    RemoteEntry e;
    Result r = b.upload(p, s, FileMeta{}, rev, e, np);
    if (out) *out = e.rev;
    return r;
}

void test_raw() {
    MockS3 m;
    CHECK(m.start());
    auto s = make_s3_backend(opts(m));
    NoProgress np;
    CHECK(s->connect(np).ok());
    std::string r1, r2;
    CHECK(put(*s, L"a.txt", "alpha", "", &r1).ok() && !r1.empty());
    CHECK(put(*s, L"a.txt", "again", "").st == Status::PreconditionFailed);
    CHECK(put(*s, L"a.txt", "alpha 2", "wrong").st == Status::PreconditionFailed);
    CHECK(put(*s, L"a.txt", "alpha 2", r1, &r2).ok() && r2 != r1);
    for (int i = 0; i < 6; ++i) CHECK(put(*s, L"d/f" + std::to_wstring(i), "f", "").ok());
    ChangeBatch cb;
    CHECK(s->changes("", cb, np).ok() && cb.full && cb.upserts.size() == 7);   // three pages
    std::vector<RemoteEntry> l;
    CHECK(s->list(L"d", l, np).ok() && l.size() == 6);
    RemoteEntry st;
    CHECK(s->stat(L"a.txt", st, np).ok() && st.rev == r2 && st.size == 7);
    MemorySink ms;
    FileMeta fm;
    CHECK(s->download(st, ms, fm, np).ok() && std::string(ms.data.begin(), ms.data.end()) == "alpha 2");
    MemorySink rs;
    CHECK(s->download_range(st, 2, 3, rs, np).ok() && std::string(rs.data.begin(), rs.data.end()) == "pha");
    RemoteEntry mv;
    CHECK(s->move(L"a.txt", L"b/a.txt", "stale", mv, np).st == Status::PreconditionFailed);
    CHECK(s->move(L"a.txt", L"b/a.txt", r2, mv, np).ok() && mv.rev == r2);
    CHECK(s->stat(L"a.txt", st, np).st == Status::NotFound);
    CHECK(s->remove(L"b/a.txt", "stale", np).st == Status::PreconditionFailed);
    CHECK(s->remove(L"b/a.txt", r2, np).ok());
    CHECK(m.badSignatures == 0);
    // a wrong secret: refused as a sign-in problem
    S3Options bad = opts(m);
    bad.cred.secretKey = "nope";
    auto b = make_s3_backend(bad);
    CHECK(b->connect(np).st == Status::AuthFailed);
}

void test_encrypted() {
    MockS3 m;
    CHECK(m.start());
    struct Dev {
        std::wstring root, stateDir;
        std::unique_ptr<synckeys::KeyStore> ks = synckeys::memory_key_store();
        std::unique_ptr<StateStore> st;
        std::unique_ptr<Engine> eng;
    } a, b;
    a.root = tu::temp_root(L"s3a"); a.stateDir = tu::temp_root(L"s3a-state");
    b.root = tu::temp_root(L"s3b"); b.stateDir = tu::temp_root(L"s3b-state");
    NoProgress np;
    auto open = [&](Dev& d, bool create) -> Result {
        d.eng.reset();
        d.st.reset();
        auto raw = make_s3_backend(opts(m, "vaults/one"));
        synckeys::Vault v;
        Result r = synckeys::open_vault(*raw, *d.ks, create, v, np);
        if (!r.ok()) return r;
        d.st = std::make_unique<StateStore>(d.stateDir);
        d.st->load();
        EngineOptions eo;
        eo.root = d.root;
        eo.deviceName = L"DEV";
        eo.trashDir = d.stateDir + L"\\trash";
        eo.settleMs = 0;
        d.eng = std::make_unique<Engine>(eo, make_encrypting(std::move(raw), v), *d.st);
        return {};
    };
    auto sync = [](Dev& d) {
        NoProgress p;
        PassReport r = d.eng->sync(p);
        if (!r.result.ok()) std::printf("  sync: %s\n", to_utf8(r.result.message).c_str());
        return r;
    };
    std::string rec;
    CHECK(synckeys::setup_ring(*a.ks, rec).ok());
    CHECK(open(a, true).ok());
    for (int i = 0; i < 8; ++i) tu::write(a.root, L"docs/file " + std::to_wstring(i) + L".txt", "S3-SECRET " + std::to_string(i));
    tu::write(a.root, L"big.bin", std::string(300000, 'Q'));
    CHECK(sync(a).result.ok());
    {
        auto raw = make_s3_backend(opts(m, "vaults/one"));
        CHECK(synckeys::redeem_recovery(*raw, *b.ks, rec, np).ok());
    }
    CHECK(open(b, false).ok());
    CHECK(sync(b).result.ok());
    CHECK(tu::read(b.root, L"docs/file 7.txt") == "S3-SECRET 7" && tu::read(b.root, L"big.bin").size() == 300000);
    // a poll with nothing new: one HEAD of the marker, no listing
    int listsBefore = m.lists;
    PassReport idle = sync(b);
    CHECK(idle.log.empty() && m.lists == listsBefore);
    // B renames a folder's file and edits another; A follows
    CHECK(tu::rename(b.root, L"docs/file 1.txt", L"docs/renamed.txt"));
    tu::write(b.root, L"docs/file 2.txt", "S3-SECRET 2, edited");
    CHECK(sync(b).result.ok());
    CHECK(sync(a).result.ok());
    CHECK(tu::read(a.root, L"docs/renamed.txt") == "S3-SECRET 1" && tu::read(a.root, L"docs/file 2.txt") == "S3-SECRET 2, edited");
    CHECK(tu::tree(a.root) == tu::tree(b.root));
    {
        std::lock_guard<std::mutex> lk(m.mx);
        bool opaque = true;
        for (auto& [k, v] : m.objs)
            if (k.find("docs") != std::string::npos || k.find("file") != std::string::npos || v.find("S3-SECRET") != std::string::npos ||
                v.find("QQQQQQQQ") != std::string::npos) opaque = false;
        CHECK(opaque);
    }
    CHECK(m.badSignatures == 0);
    a.eng.reset(); a.st.reset(); b.eng.reset(); b.st.reset();
    tu::rmrf(a.root); tu::rmrf(a.stateDir); tu::rmrf(b.root); tu::rmrf(b.stateDir);
}

} // namespace

void run_s3_tests() {
    test_sigv4();
    test_xml();
    test_raw();
    test_encrypted();
}
