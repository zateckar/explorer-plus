#include "http.h"
#include <winhttp.h>
#include <mutex>

namespace http {

static HINTERNET session() {
    static HINTERNET s = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        s = WinHttpOpen(L"ExplorerPlus-Sync/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s)                                              // before Windows 8.1: the default proxy
            s = WinHttpOpen(L"ExplorerPlus-Sync/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x2000 /* TLS 1.3 */;
        if (!WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        }
    });
    return s;
}

struct H { HINTERNET h = nullptr; ~H() { if (h) WinHttpCloseHandle(h); } };

std::string Response::header(const std::string& name) const {
    for (auto& [k, v] : headers) if (_stricmp(k.c_str(), name.c_str()) == 0) return v;
    return {};
}

static void parse_headers(const std::wstring& raw, Headers& out) {
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t e = raw.find(L"\r\n", pos);
        if (e == std::wstring::npos) e = raw.size();
        std::wstring line = raw.substr(pos, e - pos);
        pos = e + 2;
        size_t c = line.find(L':');
        if (c == std::wstring::npos) continue;
        std::wstring v = line.substr(c + 1);
        while (!v.empty() && v[0] == L' ') v.erase(0, 1);
        out.push_back({ sync::to_utf8(line.substr(0, c)), sync::to_utf8(v) });
    }
}

Response send(const Request& r) {
    Response resp;
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256], path[8192];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 8192;
    wchar_t extra[8192];
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 8192;
    if (!WinHttpCrackUrl(r.url.c_str(), 0, 0, &uc)) { resp.error = GetLastError(); return resp; }
    std::wstring object = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
    H con, req;
    con.h = WinHttpConnect(session(), std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    if (!con.h) { resp.error = GetLastError(); return resp; }
    std::wstring method = sync::from_utf8(r.method);
    req.h = WinHttpOpenRequest(con.h, method.c_str(), object.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req.h) { resp.error = GetLastError(); return resp; }
    WinHttpSetTimeouts(req.h, (int)r.timeoutMs, (int)r.timeoutMs, (int)r.timeoutMs, (int)r.timeoutMs);
    DWORD noRedirect = WINHTTP_DISABLE_REDIRECTS;            // a signed request must not wander off
    WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirect, sizeof(noRedirect));
    std::wstring hdrs;
    for (auto& [k, v] : r.headers) hdrs += sync::from_utf8(k) + L": " + sync::from_utf8(v) + L"\r\n";
    uint64_t len = r.bodyText ? r.bodyText->size() : r.body ? r.body->size() : 0;
    BOOL ok = WinHttpSendRequest(req.h, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(), (DWORD)-1L,
                                 r.bodyText ? (LPVOID)r.bodyText->data() : WINHTTP_NO_REQUEST_DATA,
                                 r.bodyText ? (DWORD)r.bodyText->size() : 0,
                                 len > 0xFFFFFFFFull ? WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH : (DWORD)len, 0);
    if (!ok) { resp.error = GetLastError(); return resp; }
    if (r.body && len) {
        std::vector<uint8_t> buf(256 * 1024);
        uint64_t sent = 0;
        for (;;) {
            if (r.progress && r.progress->cancelled()) { resp.error = ERROR_CANCELLED; return resp; }
            size_t got = 0;
            if (!r.body->read(buf.data(), buf.size(), &got)) { resp.error = ERROR_READ_FAULT; return resp; }
            if (!got) break;
            DWORD w = 0;
            if (!WinHttpWriteData(req.h, buf.data(), (DWORD)got, &w)) { resp.error = GetLastError(); return resp; }
            sent += got;
            if (r.progress) r.progress->bytes(sent, len);
        }
    }
    if (!WinHttpReceiveResponse(req.h, nullptr)) { resp.error = GetLastError(); return resp; }
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    resp.status = (int)status;
    sz = 0;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &sz, WINHTTP_NO_HEADER_INDEX);
    if (sz) {
        std::wstring raw(sz / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &sz, WINHTTP_NO_HEADER_INDEX))
            parse_headers(raw, resp.headers);
    }
    bool toSink = r.sink && status >= 200 && status < 300;
    uint64_t total = 0, got = 0;
    std::string cl = resp.header("Content-Length");
    if (!cl.empty()) total = _strtoui64(cl.c_str(), nullptr, 10);
    std::vector<char> buf(256 * 1024);
    // (a body cut short is no response: a 200 with half a listing must never pass for a whole one)
    for (;;) {
        if (r.progress && r.progress->cancelled()) { resp.error = ERROR_CANCELLED; resp.status = 0; return resp; }
        DWORD n = 0;
        if (!WinHttpReadData(req.h, buf.data(), (DWORD)buf.size(), &n)) { resp.error = GetLastError(); resp.status = 0; return resp; }
        if (!n) break;
        got += n;
        if (toSink) {
            if (!r.sink->write(buf.data(), n)) { resp.sinkStopped = true; break; }
            if (r.progress) r.progress->bytes(got, total);
        } else if (resp.body.size() < (64u << 20)) resp.body.append(buf.data(), n);
        else { resp.error = ERROR_BUFFER_OVERFLOW; resp.status = 0; return resp; }   // (not cut short silently)
    }
    return resp;
}

std::string url_encode(const std::string& s, bool keepSlash) {
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' ||
            (keepSlash && c == '/'))
            o += (char)c;
        else { o += '%'; o += hx[c >> 4]; o += hx[c & 15]; }
    }
    return o;
}

sync::Status status_of(const Response& r) {
    if (r.error == ERROR_CANCELLED) return sync::Status::Cancelled;
    if (!r.status) return sync::Status::Offline;
    switch (r.status) {
        case 401: case 403: return sync::Status::AuthFailed;
        case 404: return sync::Status::NotFound;
        case 409: case 412: return sync::Status::PreconditionFailed;
        case 413: return sync::Status::TooLarge;
        case 429: case 503: return sync::Status::Throttled;
        case 500: case 502: case 504: return sync::Status::Offline;
    }
    return sync::Status::Fatal;
}

// ------------------------------------------------------------------ proxy
static bool wild_match(const wchar_t* p, const wchar_t* s) {
    if (!*p) return !*s;
    if (*p == L'*') return wild_match(p + 1, s) || (*s && wild_match(p, s + 1));
    return *s && towlower(*p) == towlower(*s) && wild_match(p + 1, s + 1);
}

static std::wstring pick_proxy(const std::wstring& list) {
    // "host:port", or "http=host:port;https=host:port" (and blanks between)
    std::wstring first, https;
    size_t i = 0;
    while (i < list.size()) {
        size_t e = list.find_first_of(L"; ", i);
        if (e == std::wstring::npos) e = list.size();
        std::wstring item = list.substr(i, e - i);
        i = e + 1;
        if (item.empty()) continue;
        size_t eq = item.find(L'=');
        if (eq == std::wstring::npos) { if (first.empty()) first = item; continue; }
        if (_wcsicmp(item.substr(0, eq).c_str(), L"https") == 0) https = item.substr(eq + 1);
    }
    std::wstring p = https.empty() ? first : https;
    if (!p.empty() && p.find(L"://") == std::wstring::npos) p = L"http://" + p;
    return p;
}

std::wstring proxy_for(const std::wstring& url) {
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return {};
    static std::mutex mx;
    static std::vector<std::pair<std::wstring, std::wstring>> cache;      // host -> proxy (auto detection is slow)
    {
        std::lock_guard<std::mutex> lk(mx);
        for (auto& [h, p] : cache) if (_wcsicmp(h.c_str(), host) == 0) return p;
    }
    std::wstring proxy;
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie{};
    bool haveIe = WinHttpGetIEProxyConfigForCurrentUser(&ie) != 0;
    bool decided = false;
    if (!haveIe || ie.fAutoDetect || ie.lpszAutoConfigUrl) {
        WINHTTP_AUTOPROXY_OPTIONS ao{};
        if (haveIe && ie.lpszAutoConfigUrl) {
            ao.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
            ao.lpszAutoConfigUrl = ie.lpszAutoConfigUrl;
        } else {
            ao.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
            ao.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
        }
        ao.fAutoLogonIfChallenged = TRUE;
        WINHTTP_PROXY_INFO pi{};
        if (WinHttpGetProxyForUrl(session(), url.c_str(), &ao, &pi)) {
            decided = true;
            if (pi.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && pi.lpszProxy) proxy = pick_proxy(pi.lpszProxy);
            if (pi.lpszProxy) GlobalFree(pi.lpszProxy);
            if (pi.lpszProxyBypass) GlobalFree(pi.lpszProxyBypass);
        }
    }
    if (!decided && haveIe && ie.lpszProxy) {
        bool bypass = false;
        if (ie.lpszProxyBypass) {
            std::wstring list = ie.lpszProxyBypass;
            size_t i = 0;
            while (i < list.size() && !bypass) {
                size_t e = list.find_first_of(L"; ", i);
                if (e == std::wstring::npos) e = list.size();
                std::wstring item = list.substr(i, e - i);
                i = e + 1;
                if (item == L"<local>") bypass = wcschr(host, L'.') == nullptr;
                else if (!item.empty()) bypass = wild_match(item.c_str(), host);
            }
        }
        if (!bypass) proxy = pick_proxy(ie.lpszProxy);
    }
    if (ie.lpszAutoConfigUrl) GlobalFree(ie.lpszAutoConfigUrl);
    if (ie.lpszProxy) GlobalFree(ie.lpszProxy);
    if (ie.lpszProxyBypass) GlobalFree(ie.lpszProxyBypass);
    std::lock_guard<std::mutex> lk(mx);
    cache.push_back({ host, proxy });
    return proxy;
}
} // namespace http
