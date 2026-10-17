#pragma once
#include "sync_backend.h"
#include <functional>
#include <string>
#include <vector>

// Child processes without a console window (the git backends drive git.exe): stdin fed
// from memory or a ByteSource, stdout to memory or a ByteSink, stderr to memory, the
// environment extended per call, the whole process tree killed on cancel (job object).
namespace proc {

using Env = std::vector<std::pair<std::wstring, std::wstring>>;   // set (an empty value too; kUnset: removed)
inline const std::wstring kUnset = L"\x01unset";

struct Options {
    std::wstring cwd;
    Env env;
    const std::string* stdinText = nullptr;
    sync::ByteSource* stdinSource = nullptr;
    sync::ByteSink* stdoutSink = nullptr;      // else collected in Result::out
    std::function<bool()> cancelled;
    DWORD timeoutMs = 0;                        // 0: none
};

struct Result {
    bool started = false, cancelled = false, timedOut = false;
    DWORD exitCode = 0;
    std::string out, err;
    bool ok() const { return started && !cancelled && !timedOut && exitCode == 0; }
};

Result run(const std::wstring& exe, const std::vector<std::wstring>& args, const Options& o = {});
std::wstring quote_arg(const std::wstring& a);              // CommandLineToArgvW rules
std::wstring find_on_path(const std::wstring& exe);         // "" when not found

// A long-running child spoken to line by line (git cat-file --batch)
class Session {
public:
    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    bool start(const std::wstring& exe, const std::vector<std::wstring>& args, const Options& o);
    bool running() const { return m_proc != nullptr; }
    bool write(const void* data, size_t n);
    bool read_line(std::string& line);                      // up to '\n' (not included)
    bool read_exact(void* buf, size_t n);
    void stop();
private:
    bool fill();
    HANDLE m_proc = nullptr, m_job = nullptr, m_in = nullptr, m_out = nullptr;
    std::vector<char> m_buf;
    size_t m_pos = 0;
};

} // namespace proc
