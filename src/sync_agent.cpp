#include "sync_agent.h"
#include "credstore.h"
#include "crypto.h"
#include "resource.h"
#include "sync_config.h"
#include "sync_crypt.h"
#include "sync_engine.h"
#include "sync_ipc.h"
#include "sync_keys.h"
#include "theme.h"
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

using namespace sync;

namespace {

std::wstring computer_name() {
    wchar_t n[256];
    DWORD len = 256;
    return GetComputerNameW(n, &len) ? std::wstring(n) : std::wstring(L"PC");
}

std::string u8(const std::wstring& w) { return to_utf8(w); }
std::wstring w16(const std::string& s) { return from_utf8(s); }

// a pair's backend config with its secrets and its git mirror in the pair's own folder
BackendConfig pair_config(const syncconf::Pair& p) {
    BackendConfig c = syncconf::full_config(p);
    if (p.type == L"git" || p.type == L"azdo") c.push_back({ L"mirror", syncconf::state_dir(p.id) + L"\\repo.git" });
    return c;
}

struct Runner {
    syncconf::Pair pair;
    // set and dropped on the runner's thread (and by stop()), with opMx and liveMx held; read
    // by others with either (liveMx: the quick calls, status and such, while a pass runs)
    std::unique_ptr<StateStore> state;
    std::unique_ptr<Engine> eng;
    PairState openState = PairState::Idle;          // while there's no engine
    std::wstring openMessage;
    std::mutex liveMx;

    // what the tray and the requests show: the engine's status, else why it isn't open
    struct View { bool open = false; EngineStatus st; PairState state = PairState::Idle; std::wstring message; };
    View view() {
        std::lock_guard<std::mutex> lk(liveMx);
        View v;
        v.open = eng != nullptr;
        if (eng) { v.st = eng->status(); v.state = v.st.state; v.message = v.st.message; }
        else { v.state = openState; v.message = openMessage; }
        return v;
    }
    // f(engine) when it is open (a quick call: the engine's own lock does the rest)
    template <class F> bool with_engine(F f) {
        std::lock_guard<std::mutex> lk(liveMx);
        if (!eng) return false;
        f(*eng);
        return true;
    }
    DirWatch watch;
    std::thread th;
    std::mutex mx;                                  // the fields below
    std::condition_variable cv;
    bool wake = false, quit = false, reopen = false;
    uint64_t started = 0, passes = 0;               // rounds of the loop begun, finished
    std::mutex opMx;                                // one thing at a time on the store (a pass, a key operation)
    std::atomic<bool> cancel{ false };
    int pollSeconds = 60;

    void poke(bool reopenIt = false) {
        std::lock_guard<std::mutex> lk(mx);
        wake = true;
        if (reopenIt) reopen = true;
        cv.notify_all();
    }
};

struct CancelProgress : Progress {
    std::atomic<bool>& c;
    explicit CancelProgress(std::atomic<bool>& x) : c(x) {}
    bool cancelled() override { return c; }
};

class Agent {
public:
    explicit Agent(bool headless) : m_headless(headless), m_ks(synckeys::credential_key_store()) {}

    int run() {
        register_builtin_backends();
        std::string failed;
        if (!crypto::self_test(&failed)) {
            MessageBoxW(nullptr, (L"The encryption self-test failed (" + w16(failed) + L"). Sync stays off.").c_str(), L"Explorer Plus sync", MB_ICONERROR);
            return 3;
        }
        if (!m_headless) theme_init();      // (the tray menu in the system's light or dark)
        create_window();
        reload();
        m_server.start([this](const syncipc::Msg& m) { return handle(m); });
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        m_stopping = true;                              // (long polls give up)
        m_server.stop();
        stop_all();
        if (!m_headless) { NOTIFYICONDATAW n = icon_data(); Shell_NotifyIconW(NIM_DELETE, &n); }
        return 0;
    }

private:
    // ------------------------------------------------------------------ pairs
    void reload() {
        std::vector<syncconf::Pair> pairs = syncconf::load();
        std::lock_guard<std::mutex> lk(m_mx);
        // gone or changed: stopped
        for (auto it = m_runners.begin(); it != m_runners.end();) {
            const syncconf::Pair* now = nullptr;
            for (auto& p : pairs) if (p.id == (*it)->pair.id) now = &p;
            bool same = now && now->root == (*it)->pair.root && now->type == (*it)->pair.type && now->cfg == (*it)->pair.cfg &&
                        now->excluded == (*it)->pair.excluded;
            if (same) {
                (*it)->pair.name = now->name;
                if (now->paused != (*it)->pair.paused) {
                    (*it)->pair.paused = now->paused;
                    (*it)->with_engine([&](Engine& e) { e.set_paused(now->paused); });
                    (*it)->poke();
                }
                ++it;
                continue;
            }
            stop(**it);
            it = m_runners.erase(it);
        }
        for (auto& p : pairs) {
            bool have = false;
            for (auto& r : m_runners) if (r->pair.id == p.id) have = true;
            if (have) continue;
            auto r = std::make_shared<Runner>();
            r->pair = p;
            Runner* rp = r.get();
            r->th = std::thread([this, rp] { loop(*rp); });
            m_runners.push_back(std::move(r));
        }
    }

    void stop(Runner& r) {
        r.cancel = true;
        {
            std::lock_guard<std::mutex> lk(r.mx);
            r.quit = true;
            r.cv.notify_all();
        }
        r.watch.stop();
        if (r.th.joinable()) r.th.join();
        std::lock_guard<std::mutex> op(r.opMx);         // (a request working on it finishes first)
        std::lock_guard<std::mutex> live(r.liveMx);
        r.eng.reset();
        r.state.reset();
    }
    void stop_all() {
        std::lock_guard<std::mutex> lk(m_mx);
        for (auto& r : m_runners) stop(*r);
        m_runners.clear();
    }

    Result open(Runner& r) {
        CancelProgress prog(r.cancel);
        std::unique_ptr<Backend> b;
        std::wstring sd = syncconf::state_dir(r.pair.id);
        Result res = open_pair_backend(r.pair.type, pair_config(r.pair), *m_ks, r.pair.create, b, prog,
                                       sd.empty() ? sd : sd + L"\\vault.pin");     // what it saw of its vault
        if (!res.ok()) return res;
        if (r.pair.create) {                            // the vault exists now: never make another
            syncconf::update([&](std::vector<syncconf::Pair>& pairs) { for (auto& p : pairs) if (p.id == r.pair.id) p.create = false; });
            r.pair.create = false;
        }
        r.pollSeconds = std::max(15, b->caps().pollSeconds);
        CreateDirectoryW(r.pair.root.c_str(), nullptr);
        auto state = std::make_unique<StateStore>(syncconf::state_dir(r.pair.id));
        if (!state->load() && state->unreadable())         // (in use, a disk error: tried again later)
            return Result::fail(Status::Offline, L"The sync folder's state couldn't be read.");
        EngineOptions o;
        o.root = r.pair.root;
        o.deviceName = computer_name();
        for (auto& x : r.pair.excluded) o.excluded.push_back(x);
        wchar_t trash[64];
        if (GetEnvironmentVariableW(L"EXPLORERPLUS_SYNC_TRASH", trash, 64)) o.trashDir = syncconf::state_dir(r.pair.id) + L"\\trash";   // tests: no Recycle Bin
        auto eng = std::make_unique<Engine>(o, std::move(b), *state);
        eng->set_paused(r.pair.paused);
        {
            std::lock_guard<std::mutex> live(r.liveMx);
            r.state = std::move(state);
            r.eng = std::move(eng);
        }
        Runner* rp = &r;
        r.watch.start(r.pair.root, [rp] { rp->poke(); });
        return {};
    }

    void loop(Runner& r) {
        ULONGLONG retryAt = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(r.mx);
                if (r.quit) return;
                ++r.started;
                if (r.reopen) {
                    r.reopen = false;
                    lk.unlock();
                    std::lock_guard<std::mutex> op(r.opMx);
                    r.watch.stop();
                    std::lock_guard<std::mutex> live(r.liveMx);
                    r.eng.reset();
                    r.state.reset();
                    retryAt = 0;
                    lk.lock();
                }
            }
            bool didPass = false;
            if (!r.eng && GetTickCount64() >= retryAt) {
                std::lock_guard<std::mutex> op(r.opMx);
                Result res = open(r);
                std::lock_guard<std::mutex> live(r.liveMx);
                if (!res.ok()) {
                    r.openState = res.st == Status::Locked ? PairState::Locked : res.st == Status::AuthFailed ? PairState::AuthNeeded
                                : res.st == Status::Offline || res.st == Status::Throttled ? PairState::Offline : PairState::Error;
                    r.openMessage = res.message.empty() ? status_name(res.st) : res.message;
                    // locked / refused: until something changes (unlock, a new secret); offline: a while
                    retryAt = r.openState == PairState::Offline ? GetTickCount64() + 60000 : r.openState == PairState::Error ? GetTickCount64() + 300000 : ~0ull;
                } else {
                    r.openState = PairState::Idle;
                    r.openMessage.clear();
                }
            }
            if (r.eng) {
                std::lock_guard<std::mutex> op(r.opMx);
                CancelProgress prog(r.cancel);
                PassReport rep = r.eng->sync(prog);
                didPass = true;
                (void)rep;
            }
            {
                std::unique_lock<std::mutex> lk(r.mx);
                (void)didPass;
                ++r.passes;
                r.cv.notify_all();
                if (r.quit) return;
                // the next pass: a change here (the watch), a request, or the poll interval;
                // a pair that waits on something (offline...) sooner
                int secs = r.pollSeconds;
                if (r.eng) {
                    PairState s = r.eng->status().state;
                    if (s == PairState::Offline || s == PairState::Error) secs = std::min(secs, 60);
                } else if (retryAt != ~0ull) secs = (int)std::max<int64_t>(1, ((int64_t)retryAt - (int64_t)GetTickCount64()) / 1000);
                else secs = 3600;
                r.cv.wait_for(lk, std::chrono::seconds(secs), [&] { return r.wake || r.quit || r.reopen; });
                r.wake = false;
            }
        }
    }

    std::shared_ptr<Runner> find(const std::string& id) {
        for (auto& r : m_runners) if (r->pair.id == id) return r;
        return nullptr;
    }

    // ------------------------------------------------------------------ requests
    static void put_status(syncipc::Msg& m, Runner& r, const std::string& prefix) {
        Runner::View v = r.view();
        PairState s = v.state;
        std::wstring msg = v.message;
        m.push_back({ prefix + "id", r.pair.id });
        m.push_back({ prefix + "name", u8(r.pair.name) });
        m.push_back({ prefix + "root", u8(r.pair.root) });
        m.push_back({ prefix + "type", u8(r.pair.type) });
        m.push_back({ prefix + "state", u8(pair_state_name(s)) });
        m.push_back({ prefix + "message", u8(msg) });
        if (v.open) {
            const EngineStatus& st = v.st;
            m.push_back({ prefix + "pendingFiles", std::to_string(st.pendingFiles) });
            m.push_back({ prefix + "pendingBytes", std::to_string(st.pendingBytes) });
            m.push_back({ prefix + "pendingDeletes", std::to_string(st.pendingDeletes) });
            m.push_back({ prefix + "conflicts", std::to_string(st.conflicts.size()) });
            m.push_back({ prefix + "problems", std::to_string(st.problems.size()) });
            m.push_back({ prefix + "lastSync", std::to_string(st.lastSync) });
        }
    }

    // a folder's status and its children's states (the file list)
    syncipc::Msg folder_status(const std::wstring& folder) {
        std::shared_ptr<Runner> r;
        RelPath rel;
        if (!(r = runner_of(folder, rel))) return syncipc::error("not in a sync folder");
        std::lock_guard<std::mutex> lk(m_mx);
        syncipc::Msg m = syncipc::ok();
        put_status(m, *r, "");
        m.push_back({ "rel", u8(rel) });
        r->with_engine([&](Engine& e) {
            for (auto& [name, st] : e.children(rel)) m.push_back({ "item", u8(name) + "\t" + u8(item_state_name(st)) });
        });
        return m;
    }

    // the runner of the pair a local path is in (shared: a reload may drop it from the list
    // meanwhile; stop() waits for the request's opMx before the engine goes)
    std::shared_ptr<Runner> runner_of(const std::wstring& path, RelPath& rel) {
        std::lock_guard<std::mutex> lk(m_mx);
        std::vector<syncconf::Pair> ps;
        for (auto& r : m_runners) ps.push_back(r->pair);
        const syncconf::Pair* p = syncconf::pair_of(ps, path, &rel);
        return p ? find(p->id) : nullptr;
    }

    // a raw store of the pair (key operations), with the pair's store lock held by the caller
    std::unique_ptr<Backend> raw_of(Runner& r, Result& res) {
        std::wstring why;
        auto b = create_raw_backend(r.pair.type, pair_config(r.pair), &why);
        if (!b) { res = Result::fail(Status::Fatal, why); return nullptr; }
        NoProgress np;
        res = b->connect(np);
        return res.ok() ? std::move(b) : nullptr;
    }

    syncipc::Msg fail(const Result& r) {
        syncipc::Msg m = syncipc::error(u8(r.message.empty() ? status_name(r.st) : r.message));
        m.push_back({ "status", u8(status_name(r.st)) });
        return m;
    }

    syncipc::Msg handle(const syncipc::Msg& q) {
        std::string cmd = syncipc::get(q, "cmd");
        NoProgress np;
        if (cmd == "ping") { auto m = syncipc::ok(); m.push_back({ "pid", std::to_string(GetCurrentProcessId()) }); return m; }
        if (cmd == "quit") { PostMessageW(m_hwnd, WM_CLOSE, 0, 0); return syncipc::ok(); }
        if (cmd == "reload") { reload(); return syncipc::ok(); }
        if (cmd == "pairs") {
            std::lock_guard<std::mutex> lk(m_mx);
            syncipc::Msg m = syncipc::ok();
            int i = 0;
            for (auto& r : m_runners) put_status(m, *r, "p" + std::to_string(i++) + ".");
            m.push_back({ "count", std::to_string(i) });
            return m;
        }
        if (cmd == "status") return folder_status(w16(syncipc::get(q, "folder")));
        if (cmd == "watch") {
            // a long poll: the folder's status (or every pair's) once it differs from the
            // one the caller has (`since`, the hash of the last reply), or after `wait` ms
            std::wstring folder = w16(syncipc::get(q, "folder"));
            std::string since = syncipc::get(q, "since");
            DWORD wait = (DWORD)std::clamp<unsigned long>(strtoul(syncipc::get(q, "wait", "20000").c_str(), nullptr, 10), 0, 25000);
            ULONGLONG until = GetTickCount64() + wait;
            for (;;) {
                syncipc::Msg m = folder.empty() ? handle(syncipc::request("pairs")) : folder_status(folder);
                // (lastSync left out: a pass that changed nothing visible isn't news)
                syncipc::Msg shown;
                for (auto& kv : m)
                    if (kv.first.size() < 8 || kv.first.compare(kv.first.size() - 8, 8, "lastSync") != 0) shown.push_back(kv);
                std::string body = syncipc::encode(shown);
                crypto::Hash32 h = crypto::blake2b(body.data(), body.size());
                std::string hx = crypto::hex(h.data(), 8);
                if (hx != since || GetTickCount64() >= until || m_stopping) {
                    m.push_back({ "hash", hx });
                    return m;
                }
                Sleep(250);
            }
        }
        if (cmd == "versions" || cmd == "restore") {
            std::shared_ptr<Runner> r;
            RelPath rel;
            if (!(r = runner_of(w16(syncipc::get(q, "path")), rel)) || rel.empty()) return syncipc::error("not in a sync folder");
            std::lock_guard<std::mutex> op(r->opMx);
            if (!r->eng) return syncipc::error(u8(r->openMessage.empty() ? L"The sync folder isn't open." : r->openMessage));
            if (cmd == "restore") {
                Result res = r->eng->restore_version(rel, syncipc::get(q, "rev"), np);
                if (!res.ok()) return fail(res);
                r->poke();
                return syncipc::ok();
            }
            std::vector<RemoteVersion> vs;
            Result res = r->eng->versions(rel, vs, np);
            if (res.st == Status::Unsupported) return syncipc::error("This storage keeps no versions (git and Azure DevOps do).");
            if (!res.ok()) return fail(res);
            syncipc::Msg m = syncipc::ok();
            for (auto& v : vs) {
                uint64_t t = ((uint64_t)v.when.dwHighDateTime << 32) | v.when.dwLowDateTime;
                m.push_back({ "version", v.rev + "\t" + std::to_string(t) + "\t" + std::to_string(v.size) + "\t" + u8(v.note) });
            }
            return m;
        }
        if (cmd == "reencrypt") {
            std::shared_ptr<Runner> r;
            {
                std::lock_guard<std::mutex> lk(m_mx);
                r = find(syncipc::get(q, "pair"));
            }
            if (!r) return syncipc::error("no such pair");
            std::lock_guard<std::mutex> op(r->opMx);
            if (!r->eng) return syncipc::error(u8(r->openMessage.empty() ? L"The sync folder isn't open." : r->openMessage));
            CancelProgress prog(r->cancel);
            Engine::Reencrypted done;
            Result res = r->eng->reencrypt(prog, done);
            if (!res.ok()) return fail(res);
            syncipc::Msg m = syncipc::ok();
            m.push_back({ "files", std::to_string(done.files) });
            m.push_back({ "skipped", std::to_string(done.skipped) });
            r->poke();
            return m;
        }
        if (cmd == "exclude") {
            // leaves a folder out of sync (on=1) or takes it back (on=0): pairs.ini, then a reload
            std::wstring path = w16(syncipc::get(q, "path"));
            bool on = syncipc::get(q, "on", "1") == "1";
            bool inPair = false;
            bool saved = syncconf::update([&](std::vector<syncconf::Pair>& pairs) {
                RelPath rel;
                syncconf::Pair* p = const_cast<syncconf::Pair*>(syncconf::pair_of(pairs, path, &rel));
                if (!p || rel.empty()) return;
                inPair = true;
                std::wstring k = key_of(rel);
                std::erase_if(p->excluded, [&](const std::wstring& x) { return key_of(x) == k; });
                if (on) p->excluded.push_back(rel);
            });
            if (saved && !inPair) return syncipc::error("not in a sync folder");
            if (!saved) return syncipc::error("pairs.ini couldn't be read or written");
            reload();
            return syncipc::ok();
        }
        if (cmd == "sync-now" || cmd == "pause" || cmd == "resume" || cmd == "confirm-deletes" || cmd == "dismiss-conflicts") {
            std::string id = syncipc::get(q, "pair");
            bool waitIt = syncipc::get(q, "wait") == "1";
            std::vector<std::shared_ptr<Runner>> targets;
            {
                std::lock_guard<std::mutex> lk(m_mx);
                for (auto& r : m_runners) if (id.empty() || r->pair.id == id) targets.push_back(r);
            }
            if (targets.empty()) return syncipc::error("no such pair");
            if (cmd == "pause" || cmd == "resume") {
                syncconf::update([&](std::vector<syncconf::Pair>& pairs) {
                    for (auto& p : pairs) if (id.empty() || p.id == id) p.paused = cmd == "pause";
                });
                for (auto& r : targets) { r->pair.paused = cmd == "pause"; r->with_engine([&](Engine& e) { e.set_paused(r->pair.paused); }); r->poke(); }
                return syncipc::ok();
            }
            for (auto& r : targets) {
                bool open = r->with_engine([&](Engine& e) {
                    if (cmd == "confirm-deletes") e.confirm_deletes(syncipc::get(q, "yes") == "1");
                    if (cmd == "dismiss-conflicts") e.dismiss_conflicts();
                });
                if (cmd == "sync-now" && !open) r->poke(true);
            }
            // a pass that's running now saw the old state: wait for the first one that begins
            // after this request (rounds run one after another: round n is done when passes >= n)
            for (auto& r : targets) {
                if (!waitIt) { r->poke(); continue; }
                uint64_t target;
                { std::lock_guard<std::mutex> lk(r->mx); target = r->started + 1; }
                r->poke();
                std::unique_lock<std::mutex> lk(r->mx);
                r->cv.wait_for(lk, std::chrono::seconds(600), [&] { return r->passes >= target || r->quit; });
            }
            syncipc::Msg m = syncipc::ok();
            if (targets.size() == 1) put_status(m, *targets[0], "");
            return m;
        }
        if (cmd == "conflicts") {
            std::lock_guard<std::mutex> lk(m_mx);
            syncipc::Msg m = syncipc::ok();
            for (auto& r : m_runners)
                if (Runner::View v = r->view(); v.open) {
                    const EngineStatus& st = v.st;
                    for (auto& c : st.conflicts) m.push_back({ "conflict", r->pair.id + "\t" + u8(c) });
                    for (auto& [p, why] : st.problems) m.push_back({ "problem", r->pair.id + "\t" + u8(p) + "\t" + u8(why) });
                }
            return m;
        }
        if (cmd == "test-connection") {
            BackendConfig cfg;
            for (auto& [k, v] : q) if (k.compare(0, 4, "cfg.") == 0) cfg.push_back({ w16(k.substr(4)), w16(v) });
            std::wstring type = w16(syncipc::get(q, "type")), why;
            const BackendType* t = find_backend(type);
            if (!t) return syncipc::error("unknown storage type");
            if (t->validate) if (std::wstring v = t->validate(cfg); !v.empty()) return syncipc::error(u8(v));
            auto b = create_raw_backend(type, cfg, &why);
            if (!b) return syncipc::error(u8(why));
            Result r = b->connect(np);
            if (!r.ok()) return fail(r);
            crypto::Bytes vb;
            Result v = synckeys::get_object(*b, L".ep-vault/vault", vb, nullptr, np);
            syncipc::Msg m = syncipc::ok();
            if (v.ok()) m.push_back({ "vault", "exists" });
            else if (v.st == Status::NotFound) {
                std::vector<RemoteEntry> all;
                b->list(L"", all, np);
                m.push_back({ "vault", all.empty() ? "none" : "plaintext" });
            } else return fail(v);
            return m;
        }
        if (cmd == "set-secret") {
            std::string id = syncipc::get(q, "pair");
            syncconf::set_secret(id, w16(syncipc::get(q, "key")), w16(syncipc::get(q, "value")));
            std::lock_guard<std::mutex> lk(m_mx);
            if (auto r = find(id)) r->poke(true);
            return syncipc::ok();
        }
        // ---- keys
        if (cmd == "keys-status") {
            synckeys::Ring ring;
            syncipc::Msg m = syncipc::ok();
            m.push_back({ "ring", m_ks->load_ring(ring) ? "1" : "0" });
            synckeys::DeviceKey d = synckeys::device_key(*m_ks);
            m.push_back({ "device", d.id });
            m.push_back({ "deviceName", u8(d.name) });
            return m;
        }
        if (cmd == "setup-keys") {
            synckeys::Ring ring;
            if (m_ks->load_ring(ring)) return syncipc::error("This device already has keys.");
            Result r = synckeys::setup_ring(*m_ks, syncipc::get(q, "password"));
            if (!r.ok()) return fail(r);
            reopen_all();
            return syncipc::ok();
        }
        if (cmd == "check-password") {
            Result r = synckeys::check_password(*m_ks, syncipc::get(q, "password"));
            return r.ok() ? syncipc::ok() : fail(r);
        }
        if (cmd == "change-password") {
            // the current one first: whoever sits at an unlocked PC can't take the user's place
            if (Result c = synckeys::check_password(*m_ks, syncipc::get(q, "old")); !c.ok()) return fail(c);
            Result r = synckeys::change_password(*m_ks, syncipc::get(q, "password"));
            if (!r.ok()) return fail(r);
            std::lock_guard<std::mutex> lk(m_mx);
            for (auto& rn : m_runners) {
                std::lock_guard<std::mutex> op(rn->opMx);
                Result rr;
                if (auto b = raw_of(*rn, rr)) synckeys::refresh_vault_recovery(*b, *m_ks, np);
                rn->poke(true);                              // its encrypting layer: the new key version
            }
            return syncipc::ok();
        }
        if (cmd == "unlock" || cmd == "pair-offer" || cmd == "devices" || cmd == "remove-device") {
            std::shared_ptr<Runner> r;
            {
                std::lock_guard<std::mutex> lk(m_mx);
                r = find(syncipc::get(q, "pair"));
                if (!r && !m_runners.empty() && syncipc::get(q, "pair").empty()) r = m_runners.front();
            }
            if (!r) return syncipc::error("no such pair");
            syncipc::Msg m = syncipc::ok();
            Result res;
            {
                std::lock_guard<std::mutex> op(r->opMx);
                auto b = raw_of(*r, res);
                if (!b) return fail(res);
                if (cmd == "unlock") {
                    std::string code = syncipc::get(q, "code"), mode = syncipc::get(q, "mode");
                    // mode=password (an old recovery key too: mode=recovery), or a pairing code
                    res = mode == "password" || mode == "recovery" ? synckeys::redeem_recovery(*b, *m_ks, code, np)
                                                                   : synckeys::redeem_pairing(*b, *m_ks, code, np);
                } else if (cmd == "pair-offer") {
                    std::string code;
                    res = synckeys::create_pairing(*b, *m_ks, code, np);
                    m.push_back({ "code", code });
                    m.push_back({ "minutes", "15" });
                } else if (cmd == "devices") {
                    std::vector<synckeys::DeviceInfo> devs;
                    res = synckeys::list_devices(*b, *m_ks, devs, np);
                    for (auto& d : devs) {
                        uint64_t t = ((uint64_t)d.added.dwHighDateTime << 32) | d.added.dwLowDateTime;
                        m.push_back({ "device", d.id + "\t" + u8(d.name) + "\t" + std::to_string(t) + "\t" + (d.thisDevice ? "1" : "0") });
                    }
                } else if (Result c = synckeys::check_password(*m_ks, syncipc::get(q, "old")); !c.ok()) {
                    res = c;                                   // (as for change-password)
                } else {
                    res = synckeys::remove_device(*b, *m_ks, syncipc::get(q, "device"), syncipc::get(q, "password"), np);
                }
            }
            if (!res.ok()) return fail(res);
            if (cmd == "unlock" || cmd == "remove-device") reopen_all();
            return m;
        }
        return syncipc::error("unknown request: " + cmd);
    }

    void reopen_all() {
        std::lock_guard<std::mutex> lk(m_mx);
        for (auto& r : m_runners) r->poke(true);
    }

    // ------------------------------------------------------------------ tray
    static constexpr UINT WM_TRAY = WM_APP + 1;
    enum { ID_OPEN = 1, ID_SYNC, ID_PAUSE, ID_RESUME, ID_QUIT, ID_FOLDER0 = 100 };

    NOTIFYICONDATAW icon_data() {
        NOTIFYICONDATAW n{ sizeof(n) };
        n.hWnd = m_hwnd;
        n.uID = 1;
        return n;
    }

    void create_window() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            Agent* a = (Agent*)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (msg == WM_NCCREATE) {
                a = (Agent*)((CREATESTRUCTW*)lp)->lpCreateParams;
                SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)a);
            }
            if (a) return a->proc(h, msg, wp, lp);
            return DefWindowProcW(h, msg, wp, lp);
        };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ExplorerPlusSyncAgent";
        RegisterClassW(&wc);
        m_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Explorer Plus sync", 0, 0, 0, 0, 0, m_headless ? HWND_MESSAGE : nullptr, nullptr, wc.hInstance, this);
        if (!m_headless) {
            m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
            add_icon();
            SetTimer(m_hwnd, 1, 2000, nullptr);
        }
    }

    // the tray icon: sync's (up to date, syncing), a red "!" (something needs the user: a
    // file that didn't sync, a conflict, an error, a locked or offline folder) or paused
    enum class Look { Normal, Alert, Paused };
    static HICON look_icon(Look l) {
        static HICON icons[3]{};
        int i = (int)l;
        if (!icons[i])
            icons[i] = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(l == Look::Alert ? IDI_SYNC_ALERT : l == Look::Paused ? IDI_SYNC_PAUSED : IDI_SYNC),
                                         IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        return icons[i];
    }

    void add_icon() {
        NOTIFYICONDATAW n = icon_data();
        n.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        n.uCallbackMessage = WM_TRAY;
        n.hIcon = look_icon(m_look);
        wcscpy_s(n.szTip, L"Explorer Plus sync");
        Shell_NotifyIconW(NIM_ADD, &n);
    }

    // one pair's state in words ("Up to date, 1 file not synced"); attention: it needs the user
    static std::wstring pair_text(PairState s, const EngineStatus* st, const std::wstring& openMessage, bool& attention) {
        std::wstring t;
        attention = true;
        switch (s) {
            case PairState::Error: t = L"Error" + (openMessage.empty() ? std::wstring() : L": " + openMessage); break;
            case PairState::Locked: t = L"Locked (unlock it in Sync folders)"; break;
            case PairState::AuthNeeded: t = L"Sign-in needed"; break;
            case PairState::RootMissing: t = L"Folder missing"; break;
            case PairState::ConfirmDeletes: t = L"Deletions wait for you"; break;
            case PairState::Offline: t = L"Offline"; break;
            default: attention = false; break;
        }
        if (t.empty()) {
            if (s == PairState::Paused) t = L"Paused";
            else if (st && st->pendingFiles) t = L"Syncing " + std::to_wstring(st->pendingFiles) + (st->pendingFiles == 1 ? L" file" : L" files");
            else if (s == PairState::Syncing) t = L"Syncing";
            else t = L"Up to date";
        }
        if (st && !st->problems.empty()) {
            attention = true;
            size_t n = st->problems.size();
            t += L", " + std::to_wstring(n) + (n == 1 ? L" file not synced" : L" files not synced");
        }
        if (st && !st->conflicts.empty()) {
            attention = true;
            size_t n = st->conflicts.size();
            t += L", " + std::to_wstring(n) + (n == 1 ? L" conflict" : L" conflicts");
        }
        return t;
    }

    // the tray's tip and icon, and problems not reported before (a notification each round)
    struct TrayView { Look look = Look::Normal; std::wstring tip, newProblem; };
    TrayView tray_view() {
        TrayView v;
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_runners.empty()) { v.tip = L"No sync folders"; return v; }
        size_t pending = 0, paused = 0;
        std::wstring alerts;
        std::set<std::wstring> problems;
        for (auto& r : m_runners) {
            Runner::View rv = r->view();
            const EngineStatus& st = rv.st;
            PairState s = rv.state;
            bool attention = false;
            std::wstring t = pair_text(s, rv.open ? &st : nullptr, rv.message, attention);
            if (attention) alerts += (alerts.empty() ? L"" : L"\n") + r->pair.name + L": " + t;
            if (s == PairState::Paused) ++paused;
            pending += st.pendingFiles;
            for (auto& [path, why] : st.problems) {
                std::wstring key = w16(r->pair.id) + L"|" + path;
                problems.insert(key);
                if (!m_reported.count(key) && v.newProblem.empty()) v.newProblem = r->pair.name + L"\\" + path + L": " + why;
            }
        }
        m_reported = std::move(problems);           // (one that comes back is reported again)
        if (!alerts.empty()) { v.look = Look::Alert; v.tip = alerts; }
        else if (paused == m_runners.size()) { v.look = Look::Paused; v.tip = L"Paused"; }
        else if (pending) v.tip = L"Syncing " + std::to_wstring(pending) + (pending == 1 ? L" file" : L" files");
        else v.tip = L"Up to date";
        return v;
    }

    void update_tray() {
        TrayView v = tray_view();
        NOTIFYICONDATAW n = icon_data();
        n.uFlags = NIF_TIP;
        std::wstring tip = L"Explorer Plus sync: " + v.tip;
        if (tip.size() >= ARRAYSIZE(n.szTip)) tip = tip.substr(0, ARRAYSIZE(n.szTip) - 4) + L"...";
        wcsncpy_s(n.szTip, tip.c_str(), _TRUNCATE);
        if (v.look != m_look) {
            m_look = v.look;
            n.uFlags |= NIF_ICON;
            n.hIcon = look_icon(v.look);
        }
        if (!v.newProblem.empty()) {
            n.uFlags |= NIF_INFO;
            n.dwInfoFlags = NIIF_WARNING;
            wcsncpy_s(n.szInfoTitle, L"A file didn't sync", _TRUNCATE);
            wcsncpy_s(n.szInfo, v.newProblem.c_str(), _TRUNCATE);
        }
        Shell_NotifyIconW(NIM_MODIFY, &n);
    }

    void menu() {
        HMENU m = CreatePopupMenu();
        std::vector<std::wstring> roots;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            for (auto& r : m_runners) {
                Runner::View rv = r->view();
                bool attention = false;
                std::wstring label = r->pair.name + L"\t" + pair_text(rv.state, rv.open ? &rv.st : nullptr, rv.message, attention);
                AppendMenuW(m, MF_STRING, ID_FOLDER0 + roots.size(), label.c_str());
                roots.push_back(r->pair.root);
            }
        }
        if (!roots.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, ID_SYNC, L"Sync now");
        AppendMenuW(m, MF_STRING, ID_PAUSE, L"Pause");
        AppendMenuW(m, MF_STRING, ID_RESUME, L"Resume");
        AppendMenuW(m, MF_STRING, ID_OPEN, L"Sync folders...");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit sync");
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(m_hwnd);
        UINT id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hwnd, nullptr);
        DestroyMenu(m);
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (id >= ID_FOLDER0 && id < ID_FOLDER0 + roots.size()) ShellExecuteW(nullptr, nullptr, exe, (L"\"" + roots[id - ID_FOLDER0] + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        else if (id == ID_OPEN) ShellExecuteW(nullptr, nullptr, exe, L"--sync-folders", nullptr, SW_SHOWNORMAL);
        else if (id == ID_QUIT) PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        else if (id == ID_SYNC || id == ID_PAUSE || id == ID_RESUME) {
            syncipc::Msg q = syncipc::request(id == ID_SYNC ? "sync-now" : id == ID_PAUSE ? "pause" : "resume");
            std::thread([this, q] { handle(q); }).detach();
        }
    }

    LRESULT proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == m_taskbarCreated && m_taskbarCreated) { add_icon(); return 0; }
        switch (msg) {
            case WM_TRAY:
                if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) menu();
                else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
                    wchar_t exe[MAX_PATH];
                    GetModuleFileNameW(nullptr, exe, MAX_PATH);
                    ShellExecuteW(nullptr, nullptr, exe, L"--sync-folders", nullptr, SW_SHOWNORMAL);
                }
                return 0;
            case WM_TIMER:
                update_tray();
                return 0;
            case WM_SETTINGCHANGE:              // popup menus follow a light / dark switch
                if (lp && lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) theme_reload();
                break;
            case WM_CLOSE:
                DestroyWindow(h);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    bool m_headless;
    std::atomic<bool> m_stopping{ false };
    std::unique_ptr<synckeys::KeyStore> m_ks;
    std::mutex m_mx;
    std::vector<std::shared_ptr<Runner>> m_runners;
    syncipc::Server m_server;
    HWND m_hwnd = nullptr;
    UINT m_taskbarCreated = 0;
    Look m_look = Look::Normal;
    std::set<std::wstring> m_reported;      // "<pair id>|<path>" of the problems notified (tray thread)
};

// ------------------------------------------------------------------ CLI output
void out(const std::string& s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) h = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    DWORD w;
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr);
}

bool start_agent_process(bool headless) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --sync" + (headless ? L" --headless" : L"");
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    for (int i = 0; i < 100; ++i) {                     // up to 10 s for the pipe
        syncipc::Msg r;
        if (syncipc::call(syncipc::request("ping"), r, 1000)) return true;
        Sleep(100);
    }
    return false;
}

} // namespace

int sync_agent_main(bool headless) {
    HANDLE mx = CreateMutexW(nullptr, TRUE, syncipc::agent_mutex_name().c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) { if (mx) CloseHandle(mx); return 0; }
    if (sync_home().empty()) return 1;
    int rc;
    {
        Agent a(headless);
        rc = a.run();
    }
    ReleaseMutex(mx);
    CloseHandle(mx);
    return rc;
}

bool sync_agent_ensure_running() {
    if (syncipc::agent_running()) return true;
    if (syncconf::load().empty()) return false;
    return start_agent_process(false);
}

bool sync_agent_start() { return syncipc::agent_running() || start_agent_process(false); }

int sync_cli_main(const std::vector<std::wstring>& args) {
    if (args.empty()) { out("usage: --sync-cli <command> [key=value ...]\n"); return 1; }
    std::string cmd = u8(args[0]);
    syncipc::Msg q = syncipc::request(cmd);
    for (size_t i = 1; i < args.size(); ++i) {
        std::string a = u8(args[i]);
        size_t eq = a.find('=');
        if (eq == std::string::npos) q.push_back({ a, "1" });
        else q.push_back({ a.substr(0, eq), a.substr(eq + 1) });
    }
    if (cmd == "pairs-file") { out(u8(syncconf::pairs_file()) + "\n"); return 0; }
    if (cmd == "add-pair") {
        register_builtin_backends();
        syncconf::Pair p;
        p.id = syncipc::get(q, "id", syncconf::new_id());
        p.root = w16(syncipc::get(q, "root"));
        p.type = w16(syncipc::get(q, "type"));
        while (p.root.size() > 3 && (p.root.back() == L'\\' || p.root.back() == L'/')) p.root.pop_back();
        p.name = w16(syncipc::get(q, "name", u8(p.root.substr(p.root.find_last_of(L"\\/") + 1))));
        p.create = syncipc::get(q, "create") == "1";
        for (auto& x : syncipc::all(q, "exclude")) p.excluded.push_back(w16(x));
        BackendConfig cfg, secrets;
        for (auto& [k, v] : q) if (k.compare(0, 4, "cfg.") == 0) cfg.push_back({ w16(k.substr(4)), w16(v) });
        const BackendType* t = find_backend(p.type);
        if (!t || p.root.empty()) { out("error=unknown type or no root\n"); return 1; }
        syncconf::split_secrets(p.type, cfg, secrets);
        p.cfg = cfg;
        for (auto& [k, v] : secrets) {
            // "env:NAME": from this process' environment (a token kept off command lines)
            std::wstring val = v;
            if (val.compare(0, 4, L"env:") == 0) {
                DWORD n = GetEnvironmentVariableW(val.c_str() + 4, nullptr, 0);
                std::wstring e(n, L'\0');
                if (n) e.resize(GetEnvironmentVariableW(val.c_str() + 4, e.data(), n));
                val = e;
            }
            syncconf::set_secret(p.id, k, val);
        }
        if (!syncconf::update([&](std::vector<syncconf::Pair>& pairs) { pairs.push_back(p); })) {
            out("error=pairs.ini couldn't be read or written\n");
            return 1;
        }
        CreateDirectoryW(p.root.c_str(), nullptr);
        syncipc::Msg r;
        syncipc::call(syncipc::request("reload"), r, 5000);
        out("ok=1\nid=" + p.id + "\n");
        return 0;
    }
    if (cmd == "remove-pair") {
        std::string id = syncipc::get(q, "pair");
        if (!syncconf::update([&](std::vector<syncconf::Pair>& pairs) { std::erase_if(pairs, [&](const syncconf::Pair& p) { return p.id == id; }); })) {
            out("error=pairs.ini couldn't be read or written\n");
            return 1;
        }
        syncconf::forget_secrets(id);
        syncipc::Msg r;
        syncipc::call(syncipc::request("reload"), r, 60000);
        syncconf::remove_state_dir(id);
        out("ok=1\n");
        return 0;
    }
    if (cmd == "forget-keys") {
        // this settings folder's keys and secrets out of Credential Manager (tests clean up;
        // the agent must be stopped)
        if (syncipc::agent_running()) { out("error=quit the agent first\n"); return 1; }
        for (auto& p : syncconf::load()) syncconf::forget_secrets(p.id);
        synckeys::credential_key_store()->forget();
        credstore::erase(L"recovery-key");                    // (kept by the UI for Show recovery key)
        out("ok=1\n");
        return 0;
    }
    if (cmd == "start-agent") {
        bool ok = syncipc::agent_running() || start_agent_process(syncipc::get(q, "headless") == "1");
        out(ok ? "ok=1\n" : "error=the agent didn't start\n");
        return ok ? 0 : 1;
    }
    syncipc::Msg reply;
    DWORD timeout = syncipc::get(q, "wait") == "1" ? 600000 : 60000;
    if (!syncipc::call(q, reply, timeout)) { out("error=no sync agent\n"); return 2; }
    // key=value lines as they are (only a line break in a value is written \n)
    std::string text;
    for (auto& [k, v] : reply) {
        std::string one = v;
        for (size_t at; (at = one.find('\n')) != std::string::npos;) one.replace(at, 1, "\\n");
        text += k + "=" + one + "\n";
    }
    out(text);
    return syncipc::get(reply, "ok") == "1" ? 0 : 1;
}
