#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Explorer Plus <-> the sync agent (docs/sync-plan.md, "IPC"): a named pipe the agent owns,
// \\.\pipe\ExplorerPlus.Sync.<user SID>.<settings hash>, open to the current user only.
// A message is a 4-byte little-endian length and UTF-8 "key=value" lines (repeatable keys,
// backslash escapes); the first line is "cmd=<request>" in requests, "ok=1" or "error=..."
// in replies. One request, one reply per connection.
namespace syncipc {

using Msg = std::vector<std::pair<std::string, std::string>>;

std::string encode(const Msg& m);
bool decode(const std::string& body, Msg& out);
std::string get(const Msg& m, const std::string& key, const std::string& def = {});
std::vector<std::string> all(const Msg& m, const std::string& key);
inline Msg request(const std::string& cmd) { return { { "cmd", cmd } }; }
inline Msg ok() { return { { "ok", "1" } }; }
inline Msg error(const std::string& why) { return { { "error", why } }; }

std::wstring pipe_name();
std::wstring agent_mutex_name();

// client: sends a request, waits for the reply (false: no agent, or it didn't answer in time)
bool call(const Msg& req, Msg& reply, DWORD timeoutMs = 10000);
bool agent_running();

// server: a thread accepting connections; handler runs on a worker thread per connection
class Server {
public:
    ~Server() { stop(); }
    bool start(std::function<Msg(const Msg&)> handler);
    void stop();
private:
    void loop();
    std::function<Msg(const Msg&)> m_handler;
    std::thread m_thread;
    HANDLE m_stop = nullptr;
    std::vector<std::thread> m_workers;
};

} // namespace syncipc
