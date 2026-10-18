#include "proc.h"
#include <algorithm>
#include <cstring>
#include <thread>

namespace proc {

std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring s = L"\"";
    for (size_t i = 0;; ++i) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { ++bs; ++i; }
        if (i == a.size()) { s.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { s.append(bs * 2 + 1, L'\\'); s += L'"'; }
        else { s.append(bs, L'\\'); s += a[i]; }
    }
    s += L'"';
    return s;
}

std::wstring find_on_path(const std::wstring& exe) {
    wchar_t buf[MAX_PATH];
    DWORD n = SearchPathW(nullptr, exe.c_str(), nullptr, MAX_PATH, buf, nullptr);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

static std::vector<wchar_t> env_block(const Env& extra) {
    std::vector<std::wstring> vars;
    if (wchar_t* e = GetEnvironmentStringsW()) {
        for (wchar_t* p = e; *p; p += wcslen(p) + 1) vars.push_back(p);
        FreeEnvironmentStringsW(e);
    }
    for (auto& [k, v] : extra) {
        std::wstring pre = k + L"=";
        vars.erase(std::remove_if(vars.begin(), vars.end(), [&](const std::wstring& x) {
            return x.size() >= pre.size() && _wcsnicmp(x.c_str(), pre.c_str(), pre.size()) == 0; }), vars.end());
        if (v != kUnset) vars.push_back(pre + v);
    }
    std::sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (auto& v : vars) { block.insert(block.end(), v.begin(), v.end()); block.push_back(0); }
    block.push_back(0);
    // the variables may hold secrets (a PAT): the caller's copy is wiped by its owner
    return block;
}

struct Child {
    HANDLE proc = nullptr, job = nullptr, in = nullptr, out = nullptr, err = nullptr;
    ~Child() {
        for (HANDLE h : { in, out, err }) if (h) CloseHandle(h);
        if (proc) CloseHandle(proc);
        if (job) CloseHandle(job);                       // kills what is left (KILL_ON_JOB_CLOSE)
    }
};

static bool spawn(const std::wstring& exe, const std::vector<std::wstring>& args, const Options& o, Child& c) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE inR = nullptr, outW = nullptr, errW = nullptr;
    if (!CreatePipe(&inR, &c.in, &sa, 1 << 16) || !CreatePipe(&c.out, &outW, &sa, 1 << 16) || !CreatePipe(&c.err, &errW, &sa, 1 << 16))
        return false;
    SetHandleInformation(c.in, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(c.out, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(c.err, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = quote_arg(exe);
    for (auto& a : args) { cmd += L' '; cmd += quote_arg(a); }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inR;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = errW;
    // only its own three pipe ends are inherited: not another thread's child's at the same moment
    // (which would keep that child's pipes open, and its reader waiting, as long as this one runs)
    HANDLE inherit[3] = { inR, outW, errW };
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto* attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
    bool listed = attrSize && InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);
    if (listed) si.lpAttributeList = attrs;
    std::vector<wchar_t> env = env_block(o.env);
    PROCESS_INFORMATION pi{};
    c.job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(c.job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
                                 (listed ? EXTENDED_STARTUPINFO_PRESENT : 0),
                             env.data(), o.cwd.empty() ? nullptr : o.cwd.c_str(), &si.StartupInfo, &pi);
    if (listed) DeleteProcThreadAttributeList(attrs);
    SecureZeroMemory(env.data(), env.size() * sizeof(wchar_t));
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) return false;
    AssignProcessToJobObject(c.job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    c.proc = pi.hProcess;
    return true;
}

Result run(const std::wstring& exe, const std::vector<std::wstring>& args, const Options& o) {
    Result r;
    Child c;
    if (!spawn(exe, args, o, c)) return r;
    r.started = true;
    std::thread errT([&] {
        char b[4096];
        DWORD n;
        while (ReadFile(c.err, b, sizeof(b), &n, nullptr) && n) if (r.err.size() < (1 << 20)) r.err.append(b, n);
    });
    bool sinkStopped = false;
    std::thread outT([&] {
        std::vector<char> b(1 << 16);
        DWORD n;
        while (ReadFile(c.out, b.data(), (DWORD)b.size(), &n, nullptr) && n) {
            if (o.stdoutSink) { if (!sinkStopped && !o.stdoutSink->write(b.data(), n)) sinkStopped = true; }
            else r.out.append(b.data(), n);
        }
    });
    std::thread inT([&] {
        if (o.stdinText) {
            const char* p = o.stdinText->data();
            size_t left = o.stdinText->size();
            DWORD w;
            while (left && WriteFile(c.in, p, (DWORD)std::min<size_t>(left, 1 << 16), &w, nullptr)) { p += w; left -= w; }
        } else if (o.stdinSource) {
            std::vector<char> b(1 << 16);
            for (;;) {
                size_t got = 0;
                if (!o.stdinSource->read(b.data(), b.size(), &got) || !got) break;
                const char* p = b.data();
                DWORD w;
                bool fine = true;
                while (got && (fine = WriteFile(c.in, p, (DWORD)got, &w, nullptr) != 0)) { p += w; got -= w; }
                if (!fine) break;
            }
        }
        CloseHandle(c.in);
        c.in = nullptr;
    });
    ULONGLONG start = GetTickCount64();
    for (;;) {
        if (WaitForSingleObject(c.proc, 100) == WAIT_OBJECT_0) break;
        if (o.cancelled && o.cancelled()) { r.cancelled = true; TerminateJobObject(c.job, 1); }
        if (o.timeoutMs && GetTickCount64() - start > o.timeoutMs) { r.timedOut = true; TerminateJobObject(c.job, 1); }
    }
    inT.join();
    // grandchildren may hold the pipes open: once the child is gone, end them too
    TerminateJobObject(c.job, 0);
    outT.join();
    errT.join();
    GetExitCodeProcess(c.proc, &r.exitCode);
    return r;
}

// ------------------------------------------------------------------ Session
Session::~Session() { stop(); }

bool Session::start(const std::wstring& exe, const std::vector<std::wstring>& args, const Options& o) {
    stop();
    Child c;
    if (!spawn(exe, args, o, c)) return false;
    CloseHandle(c.err);                                    // nothing reads it: let it go
    c.err = nullptr;
    m_proc = c.proc; m_job = c.job; m_in = c.in; m_out = c.out;
    c.proc = c.job = c.in = c.out = nullptr;
    m_buf.clear();
    m_pos = 0;
    return true;
}

void Session::stop() {
    if (m_in) { CloseHandle(m_in); m_in = nullptr; }
    if (m_job) { TerminateJobObject(m_job, 0); CloseHandle(m_job); m_job = nullptr; }
    if (m_out) { CloseHandle(m_out); m_out = nullptr; }
    if (m_proc) { CloseHandle(m_proc); m_proc = nullptr; }
}

bool Session::write(const void* data, size_t n) {
    const char* p = (const char*)data;
    while (n) {
        DWORD w = 0;
        if (!m_in || !WriteFile(m_in, p, (DWORD)n, &w, nullptr)) return false;
        p += w;
        n -= w;
    }
    return true;
}

bool Session::fill() {
    if (m_pos > 0 && m_pos == m_buf.size()) { m_buf.clear(); m_pos = 0; }
    char b[1 << 16];
    DWORD n = 0;
    if (!m_out || !ReadFile(m_out, b, sizeof(b), &n, nullptr) || !n) return false;
    m_buf.insert(m_buf.end(), b, b + n);
    return true;
}

bool Session::read_line(std::string& line) {
    line.clear();
    size_t searched = 0;                                   // (past m_pos: not searched again)
    for (;;) {
        for (size_t i = m_pos + searched; i < m_buf.size(); ++i)
            if (m_buf[i] == '\n') {
                line.assign(m_buf.data() + m_pos, i - m_pos);
                m_pos = i + 1;
                return true;
            }
        searched = m_buf.size() - m_pos;
        if (searched > (16u << 20)) return false;          // (no line is that long)
        if (!fill()) return false;
    }
}

bool Session::read_exact(void* buf, size_t n) {
    char* out = (char*)buf;
    while (n) {
        if (m_pos == m_buf.size() && !fill()) return false;
        size_t k = std::min(n, m_buf.size() - m_pos);
        memcpy(out, m_buf.data() + m_pos, k);
        m_pos += k;
        out += k;
        n -= k;
        if (m_pos > (1 << 20)) { m_buf.erase(m_buf.begin(), m_buf.begin() + m_pos); m_pos = 0; }
    }
    return true;
}

} // namespace proc
