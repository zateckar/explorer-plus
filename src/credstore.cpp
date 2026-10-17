#include "credstore.h"
#include "crypto.h"
#include <windows.h>
#include <wincred.h>
#include <shlobj.h>
#include <mutex>

std::wstring sync_settings_file() {
    DWORD need = GetEnvironmentVariableW(L"EXPLORERPLUS_SETTINGS", nullptr, 0);
    if (need) {
        std::wstring over(need, L'\0');
        DWORD n = GetEnvironmentVariableW(L"EXPLORERPLUS_SETTINGS", over.data(), need);
        over.resize(n < need ? n : 0);
        return over;
    }
    WCHAR dir[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, dir))) return {};
    return std::wstring(dir) + L"\\ExplorerPlus\\settings.ini";
}

std::wstring sync_home() {
    std::wstring ini = sync_settings_file();
    size_t slash = ini.find_last_of(L"\\/");
    if (ini.empty() || slash == std::wstring::npos) return {};
    std::wstring dir = ini.substr(0, slash);
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\Sync";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring sync_namespace() {
    static std::wstring ns;
    static std::once_flag once;
    std::call_once(once, [] {
        std::wstring ini = sync_settings_file();
        size_t slash = ini.find_last_of(L"\\/");
        std::wstring dir = slash == std::wstring::npos ? ini : ini.substr(0, slash);
        CharLowerBuffW(dir.data(), (DWORD)dir.size());
        crypto::Hash32 h = crypto::blake2b(dir.data(), dir.size() * sizeof(wchar_t));
        std::string hx = crypto::hex(h.data(), 4);
        ns.assign(hx.begin(), hx.end());
    });
    return ns;
}

namespace credstore {

static std::wstring target(const std::wstring& name) { return L"ExplorerPlus:" + sync_namespace() + L":" + name; }

bool put(const std::wstring& name, const std::vector<uint8_t>& blob) {
    if (blob.size() > kMaxBlob) return false;
    std::wstring t = target(name);
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = t.data();
    c.CredentialBlobSize = (DWORD)blob.size();
    c.CredentialBlob = (LPBYTE)blob.data();
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    wchar_t user[] = L"Explorer Plus sync";
    c.UserName = user;
    return CredWriteW(&c, 0) != 0;
}

bool get(const std::wstring& name, std::vector<uint8_t>& blob) {
    blob.clear();
    PCREDENTIALW c = nullptr;
    if (!CredReadW(target(name).c_str(), CRED_TYPE_GENERIC, 0, &c)) return false;
    blob.assign(c->CredentialBlob, c->CredentialBlob + c->CredentialBlobSize);
    SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
    CredFree(c);
    return true;
}

bool put_text(const std::wstring& name, const std::wstring& text) {
    std::vector<uint8_t> b((const uint8_t*)text.data(), (const uint8_t*)(text.data() + text.size()));
    bool ok = put(name, b);
    crypto::wipe(b.data(), b.size());
    return ok;
}

std::wstring get_text(const std::wstring& name) {
    std::vector<uint8_t> b;
    if (!get(name, b)) return {};
    std::wstring s((const wchar_t*)b.data(), b.size() / sizeof(wchar_t));
    crypto::wipe(b.data(), b.size());
    return s;
}

bool erase(const std::wstring& name) { return CredDeleteW(target(name).c_str(), CRED_TYPE_GENERIC, 0) != 0; }

int erase_prefix(const std::wstring& prefix) {
    std::wstring filter = target(prefix) + L"*";
    DWORD n = 0;
    PCREDENTIALW* list = nullptr;
    if (!CredEnumerateW(filter.c_str(), 0, &n, &list)) return 0;
    int done = 0;
    for (DWORD i = 0; i < n; ++i)
        if (CredDeleteW(list[i]->TargetName, CRED_TYPE_GENERIC, 0)) ++done;
    CredFree(list);
    return done;
}

} // namespace credstore
