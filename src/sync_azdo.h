#pragma once
#include "sync_git.h"

// Azure DevOps Git (docs/sync-plan.md, "Azure DevOps Git"): the git store with a personal
// access token handed to git in the child's environment only (an HTTP header scoped to the
// repository URL, never on a command line, never in the mirror's config), the Windows
// certificate store and proxy, and Azure DevOps' refusals read as such.
namespace sync {

// "https://[org@]host/.../_git/repo" -> the URL git is given (no user part, no trailing
// slash); "" and a reason when it isn't an Azure DevOps clone URL
std::wstring azdo_clone_url(const std::wstring& typed, std::wstring* why);

// the git options for a repository and token (the mirror: "" = the default place)
GitOptions azdo_git_options(const std::wstring& cloneUrl, const std::wstring& pat, const std::wstring& branch,
                            const std::wstring& subfolder, const std::wstring& mirror = {});

// git's stderr from Azure DevOps -> a status (Ok: nothing Azure DevOps-specific)
Result azdo_classify(const std::string& stderrText);
// the token and its header blanked out of a text (logs, messages)
std::string azdo_scrub(const std::string& text, const std::string& pat);

// deletes a branch of the remote (tests clean up their run's branch; "remove the pair's data")
Result git_delete_remote_branch(const GitOptions& o);

// ------------------------------------------------------------------ the REST API store
// The same repository layout as the git store (blob ids as revs, commit ids as cursors, one
// commit per pass), so devices can mix the two; no git.exe and no local copy of the history.
// Azure DevOps refuses a push request over 25 MiB: bigger passes go up as several pushes,
// and a single file over about 18 MB can't go up this way.
struct AzdoRestOptions {
    std::wstring url;                          // the clone URL (azdo_clone_url)
    std::wstring pat;
    std::wstring branch = L"explorerplus-sync";
    std::wstring subfolder;
    std::wstring author = L"Explorer Plus", email = L"sync@localhost";
    uint64_t maxPushBytes = 24ull << 20;       // one push's request body
    int pollSeconds = 60;
};
std::unique_ptr<Backend> make_azdo_rest_backend(AzdoRestOptions o);
// ".../_git/repo" -> ".../_apis/git/repositories/repo" ("" when it isn't a clone URL)
std::wstring azdo_api_url(const std::wstring& cloneUrl);
// a failed REST exchange -> a status and the server's own words (TF401028: CommitRaced...)
Result azdo_rest_failure(int httpStatus, const std::string& body, const std::wstring& what);
// the git blob id of some bytes (SHA-1 of "blob <size>\0" + the bytes), lowercase hex
std::string git_blob_id(const void* data, size_t n);
Result azdo_rest_delete_branch(const AzdoRestOptions& o);

} // namespace sync
