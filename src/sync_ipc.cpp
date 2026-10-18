#include "sync_ipc.h"
#include "credstore.h"
#include <sddl.h>
#include <atomic>

namespace syncipc {

// ------------------------------------------------------------------ codec
static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '=') o += "\\e";
        else o += c;
    }
    return o;
}
static std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            o += n == 'n' ? '\n' : n == 'r' ? '\r' : n == 'e' ? '=' : n;
        } else o += s[i];
    }
    return o;
}

std::string encode(const Msg& m) {
    std::string s;
    for (auto& [k, v] : m) s += esc(k) + "=" + esc(v) + "\n";
    return s;
}

bool decode(const std::string& body, Msg& out) {
    out.clear();
    size_t i = 0;
    while (i < body.size()) {
        size_t e = body.find('\n', i);
        if (e == std::string::npos) e = body.size();
        std::string line = body.substr(i, e - i);
        i = e + 1;
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) return false;
        out.push_back({ unesc(line.substr(0, eq)), unesc(line.substr(eq + 1)) });
    }
    return true;
}

std::string get(const Msg& m, const std::string& key, const std::string& def) {
    for (auto& [k, v] : m) if (k == key) return v;
    return def;
}
std::vector<std::string> all(const Msg& m, const std::string& key) {
    std::vector<std::string> v;
    for (auto& [k, x] : m) if (k == key) v.push_back(x);
    return v;
}

// ------------------------------------------------------------------ names
static std::wstring user_sid() {
    HANDLE tok = nullptr;
    std::wstring sid = L"user";
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        BYTE buf[256];
        DWORD n = 0;
        if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n)) {
            LPWSTR s = nullptr;
            if (ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &s)) { sid = s; LocalFree(s); }
        }
        CloseHandle(tok);
    }
    return sid;
}

std::wstring pipe_name() { return L"\\\\.\\pipe\\ExplorerPlus.Sync." + user_sid() + L"." + sync_namespace(); }
// (the user's: another account in the same session, a runas, has its own)
std::wstring agent_mutex_name() { return L"Local\\ExplorerPlus.Sync." + user_sid() + L"." + sync_namespace(); }

// the pipe's server runs as this user: a pipe of that name made first by another account (pipe
// names are machine-wide) never gets the requests, which carry secrets
static bool server_is_this_user(HANDLE pipe) {
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid)) return false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;
    HANDLE tok = nullptr;
    bool same = false;
    if (OpenProcessToken(proc, TOKEN_QUERY, &tok)) {
        BYTE buf[256];
        DWORD n = 0;
        if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n)) {
            LPWSTR s = nullptr;
            if (ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &s)) { same = user_sid() == s; LocalFree(s); }
        }
        CloseHandle(tok);
    }
    CloseHandle(proc);
    return same;
}

// ------------------------------------------------------------------ framing
static bool write_all(HANDLE h, const void* p, DWORD n) {
    const char* c = (const char*)p;
    while (n) {
        DWORD w = 0;
        if (!WriteFile(h, c, n, &w, nullptr) || !w) return false;
        c += w;
        n -= w;
    }
    return true;
}
static bool read_all(HANDLE h, void* p, DWORD n) {
    char* c = (char*)p;
    while (n) {
        DWORD r = 0;
        if (!ReadFile(h, c, n, &r, nullptr) && GetLastError() != ERROR_MORE_DATA) return false;
        if (!r) return false;
        c += r;
        n -= r;
    }
    return true;
}
static bool send_msg(HANDLE h, const Msg& m) {
    std::string body = encode(m);
    uint32_t len = (uint32_t)body.size();
    uint8_t hdr[4] = { (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
    return write_all(h, hdr, 4) && write_all(h, body.data(), len);
}
static bool recv_msg(HANDLE h, Msg& m) {
    uint8_t hdr[4];
    if (!read_all(h, hdr, 4)) return false;
    uint32_t len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (len > (16u << 20)) return false;
    std::string body(len, '\0');
    if (len && !read_all(h, body.data(), len)) return false;
    return decode(body, m);
}

// ------------------------------------------------------------------ client
bool call(const Msg& req, Msg& reply, DWORD timeoutMs) {
    std::wstring name = pipe_name();
    ULONGLONG start = GetTickCount64();
    HANDLE h = INVALID_HANDLE_VALUE;
    for (;;) {
        h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY || GetTickCount64() - start > timeoutMs) return false;
        WaitNamedPipeW(name.c_str(), 200);
    }
    if (!server_is_this_user(h)) { CloseHandle(h); return false; }
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
    // a reply that never comes (a hung agent) mustn't hang the caller
    std::atomic<bool> done{ false };
    HANDLE th = h;
    std::thread watchdog([&] {
        ULONGLONG t0 = GetTickCount64();
        while (!done && GetTickCount64() - t0 < timeoutMs) Sleep(20);
        if (!done) CancelIoEx(th, nullptr);
    });
    bool ok = send_msg(h, req) && recv_msg(h, reply);
    done = true;
    watchdog.join();
    CloseHandle(h);
    return ok;
}

bool agent_running() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, agent_mutex_name().c_str());
    if (!m) return false;
    CloseHandle(m);
    return true;
}

// ------------------------------------------------------------------ server
bool Server::start(std::function<Msg(const Msg&)> handler) {
    m_handler = std::move(handler);
    m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_thread = std::thread([this] { loop(); });
    return true;
}

void Server::stop() {
    if (!m_stop) return;
    SetEvent(m_stop);
    // wake a ConnectNamedPipe that waits
    HANDLE h = CreateFileW(pipe_name().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (m_thread.joinable()) m_thread.join();
    for (auto& w : m_workers) if (w.th.joinable()) w.th.join();
    m_workers.clear();
    CloseHandle(m_stop);
    m_stop = nullptr;
}

void Server::loop() {
    // the current user only, no network clients
    std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(user_sid()) + L")";
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
    // no descriptor (the user's SID couldn't be read): no pipe, rather than the default
    // one, which lets every local account read it
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr))
        return;
    bool first = true;
    while (WaitForSingleObject(m_stop, 0) != WAIT_OBJECT_0) {
        HANDLE p = CreateNamedPipeW(pipe_name().c_str(),
                                    PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
        if (p == INVALID_HANDLE_VALUE) { Sleep(200); continue; }
        first = false;
        BOOL connected = ConnectNamedPipe(p, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (WaitForSingleObject(m_stop, 0) == WAIT_OBJECT_0) { CloseHandle(p); break; }
        if (!connected) { CloseHandle(p); continue; }
        // finished workers are joined as new ones start (a long "watch" mustn't hold up the rest)
        for (auto it = m_workers.begin(); it != m_workers.end();) {
            if (*it->done) { it->th.join(); it = m_workers.erase(it); }
            else ++it;
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        m_workers.push_back({ std::thread([this, p, done] {
                                  Msg req, reply;
                                  if (recv_msg(p, req)) {
                                      reply = m_handler(req);
                                      send_msg(p, reply);
                                      FlushFileBuffers(p);
                                  }
                                  DisconnectNamedPipe(p);
                                  CloseHandle(p);
                                  *done = true;
                              }),
                              done });
    }
    LocalFree(sa.lpSecurityDescriptor);
}

} // namespace syncipc
