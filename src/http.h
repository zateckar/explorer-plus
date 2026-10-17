#pragma once
#include "sync_backend.h"
#include <functional>
#include <string>
#include <vector>

// HTTPS through WinHTTP: the Windows proxy settings (automatic proxy, PAC), the Windows
// certificate store, request and response bodies streamed. Reused by every HTTP store.
namespace http {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Request {
    std::string method = "GET";
    std::wstring url;                           // https://host[:port]/path?query (already encoded)
    Headers headers;
    const std::string* bodyText = nullptr;
    sync::ByteSource* body = nullptr;           // streamed (Content-Length from size())
    sync::ByteSink* sink = nullptr;             // the response body (2xx only); else Response::body
    sync::Progress* progress = nullptr;
    DWORD timeoutMs = 60000;
};

struct Response {
    int status = 0;                             // 0: no response (error says why)
    DWORD error = 0;                            // WinHTTP / Win32 error
    Headers headers;
    std::string body;
    bool sinkStopped = false;
    std::string header(const std::string& name) const;     // "" when absent (case-insensitive)
};

Response send(const Request& r);
std::string url_encode(const std::string& s, bool keepSlash);   // RFC 3986 unreserved kept
// sync::Status for a failed exchange (no response: Offline; 401/403: AuthFailed; 404: NotFound;
// 409/412: PreconditionFailed; 429/503: Throttled; else Fatal)
sync::Status status_of(const Response& r);

// the proxy Windows would use for a URL (the user's settings, automatic detection, PAC):
// "http://host:port", or "" for a direct connection
std::wstring proxy_for(const std::wstring& url);

} // namespace http
