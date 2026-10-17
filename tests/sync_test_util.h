#pragma once
// file helpers for the sync tests: scratch folders, files, tree snapshots
#include <windows.h>
#include <map>
#include <string>
#include <vector>

namespace tu {

// long paths (past MAX_PATH) work everywhere below
inline std::wstring lp(const std::wstring& p) { return p.compare(0, 4, L"\\\\?\\") == 0 ? p : L"\\\\?\\" + p; }

inline std::wstring temp_root(const std::wstring& name) {
    wchar_t t[MAX_PATH];
    GetTempPathW(MAX_PATH, t);
    std::wstring p = std::wstring(t) + L"ep-sync-tests";
    CreateDirectoryW(p.c_str(), nullptr);
    p += L"\\" + name + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64() % 100000);
    CreateDirectoryW(p.c_str(), nullptr);
    return p;
}

inline void rmrf(const std::wstring& p) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(lp(p + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring c = p + L"\\" + n;
            SetFileAttributesW(lp(c).c_str(), FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmrf(c);
            else DeleteFileW(lp(c).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // a just-deleted file can linger (a scanner holds it): the folder goes once it has
    for (int i = 0; i < 50 && !RemoveDirectoryW(lp(p).c_str()) && GetLastError() == ERROR_DIR_NOT_EMPTY; ++i) Sleep(20);
}

inline bool mkdirs(const std::wstring& p) {
    if (CreateDirectoryW(lp(p).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return true;
    size_t s = p.find_last_of(L'\\');
    if (s == std::wstring::npos) return false;
    mkdirs(p.substr(0, s));
    return CreateDirectoryW(lp(p).c_str(), nullptr) != 0;
}

inline void write(const std::wstring& root, const std::wstring& rel, const std::string& text) {
    std::wstring p = root + L"\\" + rel;
    for (auto& c : p) if (c == L'/') c = L'\\';
    mkdirs(p.substr(0, p.find_last_of(L'\\')));
    HANDLE h = CreateFileW(lp(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w;
    WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr);
    CloseHandle(h);
}

inline std::string read(const std::wstring& root, const std::wstring& rel) {
    std::wstring p = root + L"\\" + rel;
    for (auto& c : p) if (c == L'/') c = L'\\';
    HANDLE h = CreateFileW(lp(p).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "<missing>";
    std::string s;
    char b[4096];
    DWORD n;
    while (ReadFile(h, b, sizeof(b), &n, nullptr) && n) s.append(b, n);
    CloseHandle(h);
    return s;
}

inline bool exists(const std::wstring& root, const std::wstring& rel) {
    std::wstring p = root + L"\\" + rel;
    for (auto& c : p) if (c == L'/') c = L'\\';
    return GetFileAttributesW(lp(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline void remove(const std::wstring& root, const std::wstring& rel) {
    std::wstring p = root + L"\\" + rel;
    for (auto& c : p) if (c == L'/') c = L'\\';
    DWORD a = GetFileAttributesW(lp(p).c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) rmrf(p);
    else DeleteFileW(lp(p).c_str());
}

inline bool rename(const std::wstring& root, const std::wstring& from, const std::wstring& to) {
    std::wstring a = root + L"\\" + from, b = root + L"\\" + to;
    for (auto& c : a) if (c == L'/') c = L'\\';
    for (auto& c : b) if (c == L'/') c = L'\\';
    mkdirs(b.substr(0, b.find_last_of(L'\\')));
    return MoveFileExW(lp(a).c_str(), lp(b).c_str(), 0) != 0;
}

// relative path -> content ("<dir>" for folders); hidden temp folders left out
inline void tree_into(const std::wstring& root, const std::wstring& rel, std::map<std::wstring, std::string>& out) {
    WIN32_FIND_DATAW fd;
    std::wstring dir = rel.empty() ? root : root + L"\\" + rel;
    HANDLE h = FindFirstFileW(lp(dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || n.compare(0, 6, L".~sync") == 0) continue;
        std::wstring r = rel.empty() ? n : rel + L"/" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { out[r] = "<dir>"; tree_into(root, rel.empty() ? n : rel + L"\\" + n, out); }
        else out[r] = read(root, r);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
inline std::map<std::wstring, std::string> tree(const std::wstring& root) {
    std::map<std::wstring, std::string> m;
    tree_into(root, L"", m);
    return m;
}
inline std::string dump(const std::map<std::wstring, std::string>& t) {
    std::string s;
    for (auto& [k, v] : t) {
        for (wchar_t c : k) s += c < 128 ? (char)c : '?';
        s += "=" + (v.size() > 40 ? v.substr(0, 40) + "..." : v) + "\n";
    }
    return s;
}

} // namespace tu
