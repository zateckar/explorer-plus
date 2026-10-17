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

} // namespace sync
