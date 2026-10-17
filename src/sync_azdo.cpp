#include "sync_azdo.h"
#include "crypto.h"
#include "http.h"

namespace sync {

// the "Connect through" choices (persisted as shown)
static const wchar_t kViaRest[] = L"REST API";
static const wchar_t kViaGit[] = L"git.exe";

std::wstring azdo_clone_url(const std::wstring& typed, std::wstring* why) {
    std::wstring u = typed;
    while (!u.empty() && (u.back() == L' ' || u.back() == L'/')) u.pop_back();
    while (!u.empty() && u.front() == L' ') u.erase(0, 1);
    auto bad = [&](const wchar_t* w) { if (why) *why = w; return std::wstring(); };
    if (_wcsnicmp(u.c_str(), L"https://", 8) != 0) return bad(L"The clone URL starts with https://.");
    size_t hostStart = 8, slash = u.find(L'/', hostStart);
    if (slash == std::wstring::npos) return bad(L"That isn't a repository's clone URL.");
    size_t at = u.find(L'@', hostStart);
    if (at != std::wstring::npos && at < slash) { u.erase(hostStart, at + 1 - hostStart); slash = u.find(L'/', hostStart); }
    size_t g = u.find(L"/_git/");
    if (g == std::wstring::npos || g + 6 >= u.size()) return bad(L"An Azure DevOps clone URL has /_git/<repository> in it (Repos > Files > Clone).");
    if (u.find_first_of(L" ?#", hostStart) != std::wstring::npos) return bad(L"The clone URL has no spaces, '?' or '#'.");
    return u;
}

GitOptions azdo_git_options(const std::wstring& url, const std::wstring& pat, const std::wstring& branch, const std::wstring& subfolder,
                            const std::wstring& mirror) {
    GitOptions o;
    o.url = url;
    o.branch = branch.empty() ? L"explorerplus-sync" : branch;
    o.subfolder = subfolder;
    o.mirror = mirror;
    std::string token = ":" + to_utf8(pat);
    std::string b64 = crypto::base64(token.data(), token.size());
    crypto::wipe(token.data(), token.size());
    std::vector<std::pair<std::wstring, std::wstring>> cfg = {
        { L"http." + url + L".extraHeader", L"Authorization: Basic " + from_utf8(b64) },
        { L"credential.helper", L"" },                     // Git Credential Manager neither prompts nor stores
        { L"http.sslBackend", L"schannel" },               // the Windows certificate store (company roots)
    };
    crypto::wipe(b64.data(), b64.size());
    std::wstring proxy = http::proxy_for(url);
    if (!proxy.empty()) cfg.push_back({ L"http." + url + L".proxy", proxy });
    o.env.push_back({ L"GIT_CONFIG_COUNT", std::to_wstring(cfg.size()) });
    for (size_t i = 0; i < cfg.size(); ++i) {
        o.env.push_back({ L"GIT_CONFIG_KEY_" + std::to_wstring(i), cfg[i].first });
        o.env.push_back({ L"GIT_CONFIG_VALUE_" + std::to_wstring(i), cfg[i].second });
    }
    std::string p8 = to_utf8(pat);
    o.classify = azdo_classify;
    o.scrub = [p8](const std::string& s) { return azdo_scrub(s, p8); };
    return o;
}

Result azdo_classify(const std::string& e) {
    auto has = [&](const char* s) { return e.find(s) != std::string::npos; };
    if (has("TF402455"))
        return Result::fail(Status::Rejected, L"This branch requires pull requests (branch policies). Choose a branch without policies, such as explorerplus-sync.");
    size_t tf = e.find("TF");
    while (tf != std::string::npos) {
        size_t d = tf + 2, n = 0;
        while (d + n < e.size() && isdigit((unsigned char)e[d + n])) ++n;
        if (n >= 5) {
            size_t ls = e.rfind('\n', tf), le = e.find('\n', tf);
            std::string line = e.substr(ls == std::string::npos ? 0 : ls + 1, le == std::string::npos ? std::string::npos : le - (ls == std::string::npos ? 0 : ls + 1));
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.compare(0, 8, "remote: ") == 0) line.erase(0, 8);
            return Result::fail(Status::Rejected, L"Azure DevOps refused: " + from_utf8(line));
        }
        tf = e.find("TF", tf + 2);
    }
    if (has("Authentication failed") || has("could not read Username") || has("could not read Password") ||
        has("returned error: 401") || has("returned error: 403") || has("HTTP Basic: Access denied"))
        return Result::fail(Status::AuthFailed, L"Azure DevOps refused the personal access token (expired, revoked, or without Code (Read & write) for this repository).");
    return {};
}

std::string azdo_scrub(const std::string& text, const std::string& pat) {
    std::string s = text;
    auto blank = [&](const std::string& secret) {
        if (secret.size() < 4) return;
        for (size_t i; (i = s.find(secret)) != std::string::npos;) s.replace(i, secret.size(), "***");
    };
    blank(pat);
    std::string token = ":" + pat;
    blank(crypto::base64(token.data(), token.size()));
    // any Authorization header that slipped through
    for (size_t i = 0; (i = s.find("Authorization:", i)) != std::string::npos;) {
        size_t e = s.find('\n', i);
        s.replace(i, (e == std::string::npos ? s.size() : e) - i, "Authorization: ***");
        i += 17;
    }
    return s;
}

Result git_delete_remote_branch(const GitOptions& o) {
    std::wstring git = o.gitExe.empty() ? find_git() : o.gitExe;
    if (git.empty()) return Result::fail(Status::Fatal, L"git.exe wasn't found.");
    proc::Options po;
    po.env = o.env;
    po.env.push_back({ L"GIT_TERMINAL_PROMPT", L"0" });
    po.env.push_back({ L"GCM_INTERACTIVE", L"never" });
    po.timeoutMs = 120000;
    proc::Result r = proc::run(git, { L"push", L"--quiet", o.url, L":refs/heads/" + o.branch }, po);
    if (r.ok()) return {};
    if (o.classify) if (Result c = o.classify(r.err); !c.ok()) return c;
    std::string e = o.scrub ? o.scrub(r.err) : r.err;
    return Result::fail(Status::Fatal, from_utf8(e));
}

void register_azdo_backend() {
    BackendType t;
    t.id = L"azdo";
    t.name = L"Azure DevOps Git";
    t.fields = {
        { L"url", L"Clone URL", ConfigField::Url, true, L"", {}, L"Repos > Files > Clone, e.g. https://dev.azure.com/org/project/_git/repo." },
        { L"pat", L"Personal access token", ConfigField::Secret, true, L"", {}, L"Scope: Code (Read & write), for this organization only, with an expiry." },
        { L"branch", L"Branch", ConfigField::Text, false, L"explorerplus-sync", {}, L"Created by the first sync. Not one with branch policies." },
        { L"subfolder", L"Folder in the repository", ConfigField::Text, false, L"", {}, L"" },
        { L"via", L"Connect through", ConfigField::Choice, false, kViaRest, { kViaRest, kViaGit },
          L"REST API: nothing to install, files up to 18 MB. git.exe: Git for Windows 2.31+, bigger files, a copy of the history on this PC.",
          false },
    };
    t.validate = [](const BackendConfig& c) -> std::wstring {
        std::wstring why;
        if (azdo_clone_url(config_get(c, L"url"), &why).empty()) return why;
        if (config_get(c, L"pat").empty()) return L"Enter a personal access token.";
        return {};
    };
    t.create = [](const BackendConfig& c) -> std::unique_ptr<Backend> {
        std::wstring url = azdo_clone_url(config_get(c, L"url"), nullptr);
        if (url.empty()) return nullptr;
        // both connections keep the same layout on the branch: switching is just a setting
        if (config_get(c, L"via", kViaRest) != kViaGit) {
            AzdoRestOptions o;
            o.url = url;
            o.pat = config_get(c, L"pat");
            o.branch = config_get(c, L"branch", L"explorerplus-sync");
            if (o.branch.empty()) o.branch = L"explorerplus-sync";
            o.subfolder = config_get(c, L"subfolder");
            return make_azdo_rest_backend(std::move(o));
        }
        GitOptions o = azdo_git_options(url, config_get(c, L"pat"), config_get(c, L"branch", L"explorerplus-sync"),
                                        config_get(c, L"subfolder"), config_get(c, L"mirror"));
        return make_git_backend(std::move(o));
    };
    register_backend(std::move(t));
}

} // namespace sync
