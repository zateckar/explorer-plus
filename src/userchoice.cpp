#include "userchoice.h"
#include <wincrypt.h>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <vector>

namespace {

bool md5(const void* data, size_t bytes, uint32_t out[4]) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH h = 0;
    bool ok = false;
    if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(prov, CALG_MD5, 0, 0, &h)) {
            DWORD n = 16;
            ok = CryptHashData(h, (const BYTE*)data, (DWORD)bytes, 0) && CryptGetHashParam(h, HP_HASHVAL, (BYTE*)out, &n, 0) &&
                 n == 16;
            CryptDestroyHash(h);
        }
        CryptReleaseContext(prov, 0);
    }
    return ok;
}

std::wstring base64(const uint8_t* p, size_t n) {
    static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out += k[v >> 18 & 63];
        out += k[v >> 12 & 63];
        out += i + 1 < n ? k[v >> 6 & 63] : L'=';
        out += i + 2 < n ? k[v & 63] : L'=';
    }
    return out;
}

} // namespace

// (shell32's algorithm, as SetUserFTA and PS-SFTA reproduce it: MD5 of the text, then two
// multiply-and-shift passes over it in pairs of 32-bit words, keyed by the MD5)
std::wstring user_choice_hash(const std::wstring& ext, const std::wstring& sid, const std::wstring& progId,
                              const FILETIME& written) {
    ULONGLONG t = (ULONGLONG)written.dwHighDateTime << 32 | written.dwLowDateTime;
    t -= t % 600000000ull;                      // (to the minute)
    wchar_t time[17];
    swprintf_s(time, L"%08x%08x", (unsigned)(t >> 32), (unsigned)t);
    std::wstring text = ext + sid + progId + time +
                        L"User Choice set via Windows User Experience {D18B6DD5-6124-4341-9318-804003BAFA0B}";
    for (auto& c : text) c = (wchar_t)towlower(c);

    // the UTF-16 text with its terminating NUL, as 32-bit words (zero padded)
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    std::vector<uint32_t> w((bytes + 3) / 4 + 2, 0);
    memcpy(w.data(), text.c_str(), bytes);
    uint32_t m[4];
    if (!md5(text.c_str(), bytes, m)) return {};
    size_t pairs = (bytes >> 2) / 2;            // (an odd last word is left out)
    if (pairs == 0) return {};

    uint32_t out1 = 0, out2 = 0, cache = 0;
    uint32_t k1 = (m[0] | 1) + 0x69FB0000u, k2 = (m[1] | 1) + 0x13DB0000u;
    for (size_t i = 0; i < pairs; ++i) {
        uint32_t r0 = w[2 * i] + out1;
        uint32_t r1 = w[2 * i + 1];
        uint32_t a = r0 * k1 - 0x10FA9605u * (r0 >> 16);
        uint32_t b = 0x79F8A395u * a + 0x689B6B9Fu * (a >> 16);
        uint32_t r3 = 0xEA970001u * b - 0x3C101569u * (b >> 16);
        uint32_t r4 = r3 + r1;
        uint32_t r5 = cache + r3;
        uint32_t c = r4 * k2 - 0x3CE8EC25u * (r4 >> 16);
        uint32_t d = 0x59C3AF2Du * c - 0x2232E0F1u * (c >> 16);
        out1 = 0x1EC90001u * d + 0x35BD1EC9u * (d >> 16);
        out2 = r5 + out1;
        cache = out2;
    }
    uint32_t h0 = out1, h1 = out2;

    out1 = out2 = cache = 0;
    k1 = m[0] | 1;
    k2 = m[1] | 1;
    for (size_t i = 0; i < pairs; ++i) {
        uint32_t r0 = w[2 * i] + out1;
        uint32_t a = r0 * k1;
        uint32_t b = 0xB1110000u * a - 0x30674EEFu * (a >> 16);
        uint32_t c = 0x5B9F0000u * b - 0x78F7A461u * (b >> 16);
        uint32_t d = 0x12CEB96Du * (c >> 16) - 0x46930000u * c;
        uint32_t r3 = 0x1D830000u * d + 0x257E1D83u * (d >> 16);
        uint32_t e = k2 * (r3 + w[2 * i + 1]);
        uint32_t f = 0x16F50000u * e - 0x5D8BE90Bu * (e >> 16);
        uint32_t g = 0x96FF0000u * f - 0x2C7C6901u * (f >> 16);
        uint32_t h = 0x2B890000u * g + 0x7C932B89u * (g >> 16);
        out1 = 0x9F690000u * h - 0x405B6097u * (h >> 16);
        out2 = out1 + cache + r3;
        cache = out2;
    }

    uint32_t v[2] = { out1 ^ h0, out2 ^ h1 };
    return base64((const uint8_t*)v, sizeof(v));
}
