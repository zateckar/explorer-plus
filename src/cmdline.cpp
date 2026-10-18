#include "cmdline.h"
#include <windows.h>
#include <shellapi.h>
#include <cwctype>

// the arguments after the exe's
static std::vector<std::wstring> split_args(const wchar_t* cmdline, bool skipExe) {
    std::vector<std::wstring> out;
    int argc = 0;
    LPWSTR* argv = cmdline && *cmdline ? CommandLineToArgvW(cmdline, &argc) : nullptr;
    for (int i = skipExe ? 1 : 0; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        // a quoted path ending in a backslash ("D:\" from a "%1" verb) arrives as D:", and
        // what followed it ("C:\My Folder\" --new-window) in the same argument
        size_t q = a.find(L'"');
        if (q != std::wstring::npos) {
            std::wstring rest = a.substr(q + 1);
            out.push_back(a.substr(0, q) + L'\\');
            size_t b = rest.find_first_not_of(L' ');
            if (b != std::wstring::npos) {
                // (a leading word for the exe: CommandLineToArgvW reads the first one its own way)
                for (auto& r : split_args((L"x " + rest.substr(b)).c_str(), true)) out.push_back(r);
            }
            continue;
        }
        out.push_back(a);
    }
    if (argv) LocalFree(argv);
    return out;
}

static bool absolute_path(const std::wstring& a) {
    return (a.size() >= 3 && iswalpha(a[0]) && a[1] == L':' && a[2] == L'\\') || a.rfind(L"\\\\", 0) == 0;
}

CommandLine parse_command_line(const wchar_t* cmdline) {
    CommandLine out;
    std::vector<std::wstring> args = split_args(cmdline, true);
    bool morePaths = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const wchar_t* a = args[i].c_str();
        if (lstrcmpiW(a, L"--new-window") == 0) { out.newWindow = true; continue; }
        if (lstrcmpiW(a, L"--play") == 0) { out.play = true; continue; }
        if (lstrcmpiW(a, L"--notepad") == 0) { out.notepad = true; continue; }
        if (lstrcmpiW(a, L"--sync") == 0) { out.sync = true; continue; }
        if (lstrcmpiW(a, L"--headless") == 0) { out.headless = true; continue; }
        if (lstrcmpiW(a, L"--sync-folders") == 0) { out.syncFolders = true; continue; }
        if (lstrcmpiW(a, L"--sync-cli") == 0) {
            out.syncCli = true;
            for (++i; i < args.size(); ++i) out.syncArgs.push_back(args[i]);
            break;
        }
        // an unquoted path with spaces comes in pieces: joined again. A second path of its
        // own (C:\a C:\b) isn't run into the first: the first is opened.
        if (morePaths) continue;
        if (!out.path.empty() && absolute_path(args[i])) { morePaths = true; continue; }
        out.path += (out.path.empty() ? L"" : L" ") + args[i];
    }
    return out;
}

std::wstring quote_command_arg(const std::wstring& s) {
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; q += c; continue; }
        if (c == L'"') q.append(slashes + 1, L'\\');      // (not in paths; kept correct anyway)
        slashes = 0;
        q += c;
    }
    q.append(slashes, L'\\');
    return q + L"\"";
}

std::wstring typed_text_clean(const std::wstring& text) {
    std::wstring t = text;
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    size_t st = 0; while (st < t.size() && iswspace(t[st])) ++st;
    t.erase(0, st);
    // Strip quotes often pasted from command lines
    if (t.size() >= 2 && t.front() == L'"' && t.back() == L'"') t = t.substr(1, t.size() - 2);
    // %USERPROFILE%\... and friends
    if (t.find(L'%') != std::wstring::npos) {
        WCHAR exp[4096]{};
        DWORD n = ExpandEnvironmentStringsW(t.c_str(), exp, ARRAYSIZE(exp));
        if (n && n <= ARRAYSIZE(exp)) t = exp;
    }
    return t;
}
