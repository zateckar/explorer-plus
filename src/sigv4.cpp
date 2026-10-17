#include "sigv4.h"
#include "crypto.h"
#include <windows.h>
#include <algorithm>
#include <cctype>

namespace sigv4 {

std::string uri_encode(const std::string& s, bool keepSlash) {
    static const char* hexd = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keepSlash && c == '/')) o += (char)c;
        else { o += '%'; o += hexd[c >> 4]; o += hexd[c & 15]; }
    }
    return o;
}

std::string sha256_hex(const std::string& data) {
    crypto::Hash32 h = crypto::sha256(data.data(), data.size());
    return crypto::hex(h);
}

std::string now_amz_date() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char b[32];
    snprintf(b, sizeof(b), "%04d%02d%02dT%02d%02d%02dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

std::string canonical_query(const std::vector<std::pair<std::string, std::string>>& q) {
    std::vector<std::pair<std::string, std::string>> e;
    for (auto& [k, v] : q) e.push_back({ uri_encode(k, false), uri_encode(v, false) });
    std::sort(e.begin(), e.end());
    std::string s;
    for (auto& [k, v] : e) {
        if (!s.empty()) s += '&';
        s += k + "=" + v;
    }
    return s;
}

static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string trim_all(const std::string& v) {
    // trimmed, inner runs of blanks as one
    std::string o;
    bool sp = false;
    for (char c : v) {
        if (c == ' ' || c == '\t') { sp = !o.empty(); continue; }
        if (sp) { o += ' '; sp = false; }
        o += c;
    }
    return o;
}

std::string canonical_request(const Request& r, std::string& signedHeaders) {
    std::vector<std::pair<std::string, std::string>> h;
    h.push_back({ "host", r.host });
    for (auto& [k, v] : r.headers) h.push_back({ lower(k), trim_all(v) });
    std::sort(h.begin(), h.end());
    std::string ch;
    signedHeaders.clear();
    for (auto& [k, v] : h) {
        ch += k + ":" + v + "\n";
        if (!signedHeaders.empty()) signedHeaders += ';';
        signedHeaders += k;
    }
    return r.method + "\n" + (r.path.empty() ? "/" : r.path) + "\n" + canonical_query(r.query) + "\n" + ch + "\n" + signedHeaders + "\n" + r.payloadHash;
}

static crypto::Hash32 hmac(const void* k, size_t kn, const std::string& d) { return crypto::hmac_sha256(k, kn, d.data(), d.size()); }

std::string signature(const std::string& creq, const Credentials& c, const std::string& region, const std::string& service, const std::string& amzDate) {
    std::string date = amzDate.substr(0, 8);
    std::string scope = date + "/" + region + "/" + service + "/aws4_request";
    std::string sts = "AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" + sha256_hex(creq);
    std::string k0 = "AWS4" + c.secretKey;
    crypto::Hash32 kd = hmac(k0.data(), k0.size(), date);
    crypto::Hash32 kr = hmac(kd.data(), 32, region);
    crypto::Hash32 ks = hmac(kr.data(), 32, service);
    crypto::Hash32 kk = hmac(ks.data(), 32, "aws4_request");
    crypto::Hash32 sig = hmac(kk.data(), 32, sts);
    crypto::wipe(k0.data(), k0.size());
    crypto::wipe(kk.data(), 32);
    return crypto::hex(sig);
}

void sign(Request& r, const Credentials& c, const std::string& region, const std::string& service, std::string amzDate) {
    if (amzDate.empty()) amzDate = now_amz_date();
    r.headers.push_back({ "x-amz-date", amzDate });
    r.headers.push_back({ "x-amz-content-sha256", r.payloadHash });
    if (!c.sessionToken.empty()) r.headers.push_back({ "x-amz-security-token", c.sessionToken });
    std::string signedHeaders;
    std::string creq = canonical_request(r, signedHeaders);
    std::string sig = signature(creq, c, region, service, amzDate);
    r.headers.push_back({ "Authorization", "AWS4-HMAC-SHA256 Credential=" + c.accessKey + "/" + amzDate.substr(0, 8) + "/" + region + "/" + service +
                                               "/aws4_request, SignedHeaders=" + signedHeaders + ", Signature=" + sig });
}

} // namespace sigv4
