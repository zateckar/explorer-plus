#include "sync_s3.h"
#include "http.h"
#include "sigv4.h"

namespace sync {

// ------------------------------------------------------------------ XML (the few answers S3 gives)
std::string xml_unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos) { o += s[i]; continue; }
        std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "amp") o += '&';
        else if (ent == "lt") o += '<';
        else if (ent == "gt") o += '>';
        else if (ent == "quot") o += '"';
        else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, nullptr, 16) : strtoul(ent.c_str() + 1, nullptr, 10);
            if (cp < 0x80) o += (char)cp;
            else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
            else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        } else { o += s.substr(i, e - i + 1); }
        i = e;
    }
    return o;
}

std::string xml_text(const std::string& x, const std::string& tag, size_t from, size_t* end) {
    std::string open = "<" + tag + ">", close = "</" + tag + ">";
    size_t a = x.find(open, from);
    if (a == std::string::npos) { if (end) *end = std::string::npos; return {}; }
    size_t b = x.find(close, a);
    if (b == std::string::npos) { if (end) *end = std::string::npos; return {}; }
    if (end) *end = b + close.size();
    return xml_unescape(x.substr(a + open.size(), b - a - open.size()));
}

bool parse_list_objects(const std::string& x, std::vector<S3Object>& out, bool& truncated, std::string& next) {
    if (x.find("<ListBucketResult") == std::string::npos) return false;
    size_t pos = 0;
    for (;;) {
        size_t a = x.find("<Contents>", pos);
        if (a == std::string::npos) break;
        size_t b = x.find("</Contents>", a);
        if (b == std::string::npos) return false;
        std::string c = x.substr(a, b - a);
        S3Object o;
        o.key = xml_text(c, "Key");
        o.size = strtoull(xml_text(c, "Size").c_str(), nullptr, 10);
        o.etag = xml_text(c, "ETag");
        if (o.etag.size() >= 2 && o.etag.front() == '"' && o.etag.back() == '"') o.etag = o.etag.substr(1, o.etag.size() - 2);
        out.push_back(o);
        pos = b;
    }
    truncated = xml_text(x, "IsTruncated") == "true";
    next = xml_text(x, "NextContinuationToken");
    return true;
}

namespace {

std::string unquote(std::string e) {
    if (e.size() >= 2 && e.front() == '"' && e.back() == '"') e = e.substr(1, e.size() - 2);
    return e;
}

class S3Backend : public Backend {
public:
    explicit S3Backend(S3Options o) : m_o(std::move(o)) {
        if (m_o.endpoint.empty()) m_o.endpoint = L"https://s3." + from_utf8(m_o.region) + L".amazonaws.com";
        while (!m_o.endpoint.empty() && m_o.endpoint.back() == L'/') m_o.endpoint.pop_back();
        size_t s = m_o.endpoint.find(L"://");
        m_scheme = s == std::wstring::npos ? L"https" : m_o.endpoint.substr(0, s);
        m_host = to_utf8(s == std::wstring::npos ? m_o.endpoint : m_o.endpoint.substr(s + 3));
        while (!m_o.prefix.empty() && m_o.prefix.front() == '/') m_o.prefix.erase(0, 1);
        if (!m_o.prefix.empty() && m_o.prefix.back() != '/') m_o.prefix += '/';
    }

    Caps caps() const override {
        Caps c;
        c.conditionalWrite = true;
        c.serverMove = true;
        c.rangeReads = true;
        c.caseSensitive = true;
        c.emptyDirs = false;
        c.parallelism = 4;
        c.maxNameLength = 255;
        c.maxPathLength = 1024 - (int)m_o.prefix.size();
        c.maxFileSize = 0xFFFFFFFFull;           // one PUT with a known length (WinHTTP: 32 bits)
        c.hash = HashKind::Md5;
        c.pollSeconds = 60;
        return c;
    }

    Result connect(Progress& p) override {
        std::vector<S3Object> v;
        return list_raw(m_o.prefix, v, p, 1);
    }

    Result changes(const std::string&, ChangeBatch& out, Progress& p) override {
        out = {};
        out.full = true;
        std::vector<S3Object> v;
        if (Result r = list_raw(m_o.prefix, v, p); !r.ok()) return r;
        for (auto& o : v) if (auto e = entry(o); !e.path.empty()) out.upserts.push_back(e);
        return {};
    }

    Result list(const RelPath& prefix, std::vector<RemoteEntry>& out, Progress& p) override {
        out.clear();
        std::vector<S3Object> v;
        if (Result r = list_raw(m_o.prefix + (prefix.empty() ? "" : to_utf8(prefix) + "/"), v, p); !r.ok()) return r;
        for (auto& o : v) if (auto e = entry(o); !e.path.empty()) out.push_back(e);
        return {};
    }

    Result stat(const RelPath& path, RemoteEntry& out, Progress& p) override {
        http::Response r = call("HEAD", key(path), {}, {}, nullptr, nullptr, p);
        if (r.status != 200) return fail(r);
        out = {};
        out.path = path;
        out.size = strtoull(r.header("Content-Length").c_str(), nullptr, 10);
        out.rev = unquote(r.header("ETag"));
        return {};
    }

    Result download(const RemoteEntry& e, ByteSink& sink, FileMeta&, Progress& p) override {
        http::Response r = call("GET", key(e.path), {}, {}, nullptr, &sink, p);
        if (r.status == 200) {
            if (!e.rev.empty() && unquote(r.header("ETag")) != e.rev) return Result::fail(Status::PreconditionFailed, L"The file changed in the store.");
            return r.sinkStopped ? Result::fail(Status::Cancelled, L"write stopped") : Result{};
        }
        return fail(r);
    }

    Result download_range(const RemoteEntry& e, uint64_t off, uint64_t len, ByteSink& sink, Progress& p) override {
        if (!len) return {};
        http::Headers h = { { "Range", "bytes=" + std::to_string(off) + "-" + std::to_string(off + len - 1) } };
        http::Response r = call("GET", key(e.path), {}, h, nullptr, &sink, p);
        if (r.status == 206 || r.status == 200) return {};
        return fail(r);
    }

    Result upload(const RelPath& path, ByteSource& in, const FileMeta&, const std::string& expectedRev, RemoteEntry& out, Progress& p) override {
        if (in.size() > 0xFFFFFFFFull) return Result::fail(Status::TooLarge, L"Files over 4 GB can't go to S3 in one piece yet.");
        http::Headers h = { { "Content-Type", "application/octet-stream" } };
        if (expectedRev.empty()) h.push_back({ "If-None-Match", "*" });
        else if (expectedRev != "*") h.push_back({ "If-Match", "\"" + expectedRev + "\"" });
        http::Response r = call("PUT", key(path), {}, h, &in, nullptr, p);
        if (r.status != 200) return fail(r);
        out = {};
        out.path = path;
        out.size = in.size();
        out.rev = unquote(r.header("ETag"));
        return {};
    }

    Result remove(const RelPath& path, const std::string& expectedRev, Progress& p) override {
        if (expectedRev != "*") {
            RemoteEntry cur;
            Result s = stat(path, cur, p);
            if (!s.ok()) return s;
            if (!expectedRev.empty() && cur.rev != expectedRev) return Result::fail(Status::PreconditionFailed);
        }
        http::Response r = call("DELETE", key(path), {}, {}, nullptr, nullptr, p);
        if (r.status == 204 || r.status == 200) return {};
        return fail(r);
    }

    Result move(const RelPath& from, const RelPath& to, const std::string& expectedRev, RemoteEntry& out, Progress& p) override {
        RemoteEntry cur;
        if (Result s = stat(from, cur, p); !s.ok()) return s;
        if (expectedRev != "*" && cur.rev != expectedRev) return Result::fail(Status::PreconditionFailed);
        if (cur.size > (5ull << 30)) return Result::fail(Status::Unsupported);
        http::Headers h = { { "x-amz-copy-source", "/" + m_o.bucket + "/" + sigv4::uri_encode(key(from), true) },
                            { "x-amz-copy-source-if-match", "\"" + cur.rev + "\"" },
                            { "If-None-Match", "*" } };
        http::Response r = call("PUT", key(to), {}, h, nullptr, nullptr, p);
        if (r.status != 200 || r.body.find("<Error>") != std::string::npos) return fail(r);
        std::string etag = unquote(xml_text(r.body, "ETag"));
        http::Response d = call("DELETE", key(from), {}, {}, nullptr, nullptr, p);
        if (d.status != 204 && d.status != 200) return fail(d);
        out = {};
        out.path = to;
        out.size = cur.size;
        out.rev = etag.empty() ? cur.rev : etag;
        return {};
    }

private:
    std::string key(const RelPath& p) const { return m_o.prefix + to_utf8(p); }

    RemoteEntry entry(const S3Object& o) const {
        RemoteEntry e;
        if (o.key.size() <= m_o.prefix.size() || o.key.back() == '/') return e;      // "folder" markers
        e.path = from_utf8(o.key.substr(m_o.prefix.size()));
        e.size = o.size;
        e.rev = o.etag;
        return e;
    }

    Result fail(const http::Response& r) const {
        Status s = http::status_of(r);
        std::string code = xml_text(r.body, "Code"), msg = xml_text(r.body, "Message");
        if (code == "ConditionalRequestConflict") s = Status::PreconditionFailed;
        if (code == "NoSuchBucket") s = Status::Fatal;
        if (code == "SlowDown") s = Status::Throttled;
        std::wstring text;
        if (!r.status) text = L"The storage can't be reached.";
        else if (!code.empty()) text = from_utf8(code + (msg.empty() ? "" : ": " + msg));
        else text = L"HTTP " + std::to_wstring(r.status);
        Result res = Result::fail(s, text);
        if (s == Status::Throttled) res.retryAfterMs = atoi(r.header("Retry-After").c_str()) * 1000;
        return res;
    }

    http::Response call(const std::string& method, const std::string& objKey, const std::vector<std::pair<std::string, std::string>>& query,
                        const http::Headers& extra, ByteSource* body, ByteSink* sink, Progress& p) {
        sigv4::Request sr;
        sr.method = method;
        std::string path;
        if (m_o.pathStyle) {
            sr.host = m_host;
            path = "/" + m_o.bucket + (objKey.empty() ? "" : "/" + sigv4::uri_encode(objKey, true));
        } else {
            sr.host = m_o.bucket + "." + m_host;
            path = "/" + sigv4::uri_encode(objKey, true);
        }
        sr.path = path;
        sr.query = query;
        sr.headers = extra;
        sr.payloadHash = body ? "UNSIGNED-PAYLOAD" : sigv4::sha256_hex("");
        sigv4::sign(sr, m_o.cred, m_o.region, "s3");
        http::Request hr;
        hr.method = method;
        std::string q = sigv4::canonical_query(query);
        hr.url = m_scheme + L"://" + from_utf8(sr.host) + from_utf8(path) + (q.empty() ? L"" : L"?" + from_utf8(q));
        hr.headers = sr.headers;
        hr.body = body;
        hr.sink = sink;
        hr.progress = &p;
        hr.timeoutMs = 120000;
        return http::send(hr);
    }

    Result list_raw(const std::string& prefix, std::vector<S3Object>& out, Progress& p, int maxKeys = 1000) {
        std::string token;
        for (;;) {
            std::vector<std::pair<std::string, std::string>> q = { { "list-type", "2" }, { "prefix", prefix }, { "max-keys", std::to_string(maxKeys) } };
            if (!token.empty()) q.push_back({ "continuation-token", token });
            http::Response r = call("GET", "", q, {}, nullptr, nullptr, p);
            if (r.status != 200) return fail(r);
            bool truncated = false;
            std::string next;
            if (!parse_list_objects(r.body, out, truncated, next)) return Result::fail(Status::Fatal, L"The storage's listing couldn't be read.");
            if (!truncated || maxKeys == 1) return {};
            if (next.empty()) return Result::fail(Status::Fatal, L"The storage's listing couldn't be read.");   // (truncated, no way on)
            token = next;
            if (p.cancelled()) return Result::fail(Status::Cancelled);
        }
    }

    S3Options m_o;
    std::wstring m_scheme;
    std::string m_host;
};

} // namespace

std::unique_ptr<Backend> make_s3_backend(S3Options o) { return std::make_unique<S3Backend>(std::move(o)); }

void register_s3_backend() {
    BackendType t;
    t.id = L"s3";
    t.name = L"Amazon S3 or compatible";
    t.fields = {
        { L"bucket", L"Bucket", ConfigField::Text, true, L"", {}, L"" },
        { L"prefix", L"Folder in the bucket", ConfigField::Text, false, L"", {}, L"" },
        { L"region", L"Region", ConfigField::Text, false, L"us-east-1", {}, L"" },
        { L"endpoint", L"Endpoint", ConfigField::Url, false, L"", {}, L"Empty for Amazon S3; e.g. https://<account>.r2.cloudflarestorage.com" },
        { L"accessKey", L"Access key ID", ConfigField::Text, true, L"", {}, L"" },
        { L"secretKey", L"Secret access key", ConfigField::Secret, true, L"", {}, L"A key for this bucket only." },
        { L"pathStyle", L"Path-style addresses", ConfigField::Bool, false, L"0", {}, L"For MinIO and other servers without bucket host names." },
    };
    t.validate = [](const BackendConfig& c) -> std::wstring {
        if (config_get(c, L"bucket").empty()) return L"Enter the bucket's name.";
        if (config_get(c, L"accessKey").empty() || config_get(c, L"secretKey").empty()) return L"Enter the access key and its secret.";
        return {};
    };
    t.create = [](const BackendConfig& c) -> std::unique_ptr<Backend> {
        S3Options o;
        o.bucket = to_utf8(config_get(c, L"bucket"));
        o.prefix = to_utf8(config_get(c, L"prefix"));
        o.region = to_utf8(config_get(c, L"region", L"us-east-1"));
        o.endpoint = config_get(c, L"endpoint");
        o.cred.accessKey = to_utf8(config_get(c, L"accessKey"));
        o.cred.secretKey = to_utf8(config_get(c, L"secretKey"));
        o.pathStyle = config_get(c, L"pathStyle") == L"1";
        return make_s3_backend(std::move(o));
    };
    register_backend(std::move(t));
}

} // namespace sync
