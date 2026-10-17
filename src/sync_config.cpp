#include "sync_config.h"
#include "credstore.h"
#include "crypto.h"
#include "sync_paths.h"

namespace syncconf {

using sync::from_utf8;
using sync::to_utf8;

// (only a path: reading it creates nothing, so a user who never syncs gets no Sync folder)
std::wstring pairs_file() {
    std::wstring ini = sync_settings_file();
    size_t slash = ini.find_last_of(L"\\/");
    return ini.empty() || slash == std::wstring::npos ? std::wstring() : ini.substr(0, slash) + L"\\Sync\\pairs.ini";
}

std::string new_id() {
    uint8_t b[4];
    crypto::random(b, 4);
    return crypto::hex(b, 4);
}

// one value per line: backslash escapes for line breaks
static std::string esc(const std::wstring& v) {
    std::string s = to_utf8(v), o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}
static std::wstring unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            o += n == 'n' ? '\n' : n == 'r' ? '\r' : n;
        } else o += s[i];
    }
    return from_utf8(o);
}

std::string serialize(const std::vector<Pair>& pairs) {
    std::string s = "; Explorer Plus sync pairs (secrets are in Credential Manager)\n";
    for (auto& p : pairs) {
        s += "\n[pair." + p.id + "]\n";
        s += "name=" + esc(p.name) + "\n";
        s += "root=" + esc(p.root) + "\n";
        s += "type=" + esc(p.type) + "\n";
        s += std::string("paused=") + (p.paused ? "1" : "0") + "\n";
        if (p.create) s += "create=1\n";
        for (auto& x : p.excluded) s += "exclude=" + esc(x) + "\n";
        for (auto& [k, v] : p.cfg) s += "cfg." + esc(k) + "=" + esc(v) + "\n";
    }
    return s;
}

std::vector<Pair> parse(const std::string& text) {
    std::vector<Pair> out;
    Pair* cur = nullptr;
    size_t i = 0;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i);
        i = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            cur = nullptr;
            size_t c = line.find(']');
            std::string sec = line.substr(1, c == std::string::npos ? std::string::npos : c - 1);
            if (sec.compare(0, 5, "pair.") == 0 && sec.size() > 5) {
                out.push_back({});
                cur = &out.back();
                cur->id = sec.substr(5);
            }
            continue;
        }
        if (!cur) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "name") cur->name = unesc(v);
        else if (k == "root") cur->root = unesc(v);
        else if (k == "type") cur->type = unesc(v);
        else if (k == "paused") cur->paused = v == "1";
        else if (k == "create") cur->create = v == "1";
        else if (k == "exclude") cur->excluded.push_back(unesc(v));
        else if (k.compare(0, 4, "cfg.") == 0) cur->cfg.push_back({ unesc(k.substr(4)), unesc(v) });
    }
    return out;
}

std::vector<Pair> load() {
    std::wstring f = pairs_file();
    if (f.empty()) return {};
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string text;
    char buf[65536];
    DWORD n;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) text.append(buf, n);
    CloseHandle(h);
    return parse(text);
}

bool save(const std::vector<Pair>& pairs) {
    std::wstring f = pairs_file();
    if (f.empty() || sync_home().empty()) return false;          // (sync_home: the folder made)
    std::string text = serialize(pairs);
    std::wstring tmp = f + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w;
    bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr) && w == text.size() && FlushFileBuffers(h);
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static std::vector<std::wstring> secret_keys(const std::wstring& type) {
    std::vector<std::wstring> keys;
    sync::register_builtin_backends();
    if (const sync::BackendType* t = sync::find_backend(type))
        for (auto& f : t->fields) if (f.kind == sync::ConfigField::Secret) keys.push_back(f.key);
    return keys;
}

void split_secrets(const std::wstring& type, sync::BackendConfig& cfg, sync::BackendConfig& secrets) {
    auto keys = secret_keys(type);
    sync::BackendConfig plain;
    for (auto& kv : cfg) {
        bool secret = false;
        for (auto& k : keys) if (k == kv.first) secret = true;
        (secret ? secrets : plain).push_back(kv);
    }
    cfg = plain;
}

sync::BackendConfig full_config(const Pair& p) {
    sync::BackendConfig c = p.cfg;
    for (auto& k : secret_keys(p.type)) {
        std::wstring v = credstore::get_text(L"pair." + from_utf8(p.id) + L"." + k);
        if (!v.empty()) c.push_back({ k, v });
    }
    return c;
}

bool set_secret(const std::string& pairId, const std::wstring& key, const std::wstring& value) {
    return credstore::put_text(L"pair." + from_utf8(pairId) + L"." + key, value);
}

void forget_secrets(const std::string& pairId) { credstore::erase_prefix(L"pair." + from_utf8(pairId) + L"."); }

std::wstring state_dir(const std::string& pairId) {
    std::wstring h = sync_home();
    if (h.empty()) return h;
    std::wstring d = h + L"\\pairs";
    CreateDirectoryW(d.c_str(), nullptr);
    d += L"\\" + from_utf8(pairId);
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

static void remove_tree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!lstrcmpW(fd.cFileName, L".") || !lstrcmpW(fd.cFileName, L"..")) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) remove_tree(p);
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);       // (git's objects are read-only)
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
                else DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir.c_str());
}

void remove_state_dir(const std::string& pairId) {
    std::wstring h = sync_home();
    if (h.empty() || pairId.empty() || pairId.find_first_of("\\/.:") != std::string::npos) return;
    remove_tree(L"\\\\?\\" + h + L"\\pairs\\" + from_utf8(pairId));
}

const Pair* pair_of(const std::vector<Pair>& pairs, const std::wstring& path, sync::RelPath* rel) {
    const Pair* best = nullptr;
    std::wstring lp = syncpath::case_key(path);
    while (!lp.empty() && (lp.back() == L'\\' || lp.back() == L'/')) lp.pop_back();
    for (auto& p : pairs) {
        std::wstring r = syncpath::case_key(p.root);
        while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
        if (r.empty()) continue;
        bool in = lp == r || (lp.size() > r.size() && lp.compare(0, r.size(), r) == 0 && (lp[r.size()] == L'\\' || lp[r.size()] == L'/'));
        if (in && (!best || r.size() > best->root.size())) best = &p;
    }
    if (best && rel) {
        std::wstring root = best->root;
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        std::wstring p2 = path;
        while (!p2.empty() && (p2.back() == L'\\' || p2.back() == L'/')) p2.pop_back();
        std::wstring r = p2.size() > root.size() ? p2.substr(root.size() + 1) : L"";
        for (auto& c : r) if (c == L'\\') c = L'/';
        *rel = r;
    }
    return best;
}

} // namespace syncconf
