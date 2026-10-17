#pragma once
#include "proc.h"
#include "sync_backend.h"

// git as an object store (docs/sync-plan.md, "git"): a bare mirror on this machine, a
// private index per transaction, one commit and one push per sync pass, `ls-remote` as the
// cheap poll and `diff-tree` as the change feed. The sync folder is never a work tree.
namespace sync {

struct GitOptions {
    std::wstring url;                         // anything git can push to (https, ssh, a path)
    std::wstring branch = L"explorerplus-sync";
    std::wstring subfolder;                   // in the repository ('/'-separated, optional)
    std::wstring gitExe;                      // "" = PATH, then %ProgramFiles%\Git\cmd\git.exe
    std::wstring mirror;                      // the bare mirror ("" = Sync\git\<hash> in the sync home)
    std::wstring author = L"Explorer Plus", email = L"sync@localhost";
    uint64_t maxFileSize = 50ull << 20;       // GitHub refuses 100 MB
    uint64_t maxCommitBytes = 1ull << 30;     // bigger passes go up as several commits
    int pollSeconds = 60;
    proc::Env env;                            // added to every git call (Azure DevOps: the PAT header)
    // a store's own reading of git's failures first (Azure DevOps: TF402455...); Ok: not recognised
    std::function<Result(const std::string& stderrText)> classify;
    // what goes into messages and logs (Azure DevOps blanks the token)
    std::function<std::string(const std::string&)> scrub;
};

std::unique_ptr<Backend> make_git_backend(GitOptions o);
std::wstring find_git();                      // "" when there's none
bool git_version_at_least(const std::string& versionOutput, int major, int minor);

// pure parsers of git's -z outputs (unit-tested)
struct GitTreeEntry { std::string mode, type, oid; uint64_t size = 0; std::string path; };
bool parse_ls_tree(const std::string& z, std::vector<GitTreeEntry>& out);        // ls-tree -r -z --long
struct GitDiffEntry { std::string oldMode, newMode, oldOid, newOid; char status = 0; std::string path; };
bool parse_diff_tree(const std::string& z, std::vector<GitDiffEntry>& out);      // diff-tree -r -z --raw --no-renames

} // namespace sync
