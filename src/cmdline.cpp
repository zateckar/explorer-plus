#include "cmdline.h"
#include <windows.h>
#include <shellapi.h>
#include <cwctype>

CommandLine parse_command_line(const wchar_t* cmdline) {
    CommandLine out;
    int argc = 0;
    LPWSTR* argv = cmdline && *cmdline ? CommandLineToArgvW(cmdline, &argc) : nullptr;
    for (int i = 1; argv && i < argc; ++i) {
        if (lstrcmpiW(argv[i], L"--new-window") == 0) { out.newWindow = true; continue; }
        if (lstrcmpiW(argv[i], L"--play") == 0) { out.play = true; continue; }
        // a quoted path ending in a backslash ("D:\" from a "%1" verb) arrives as D:"
        std::wstring a = argv[i];
        if (!a.empty() && a.back() == L'"') { a.pop_back(); a += L'\\'; }
        out.path += (out.path.empty() ? L"" : L" ") + a;   // an unquoted path with spaces
    }
    if (argv) LocalFree(argv);
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
