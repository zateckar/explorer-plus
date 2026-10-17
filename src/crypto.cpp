#include "crypto.h"
#include "../third_party/monocypher/monocypher.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace crypto {

Key::~Key() { crypto_wipe(b.data(), b.size()); }
bool Key::operator==(const Key& o) const { return crypto_verify32(b.data(), o.b.data()) == 0; }

void wipe(void* p, size_t n) { if (p && n) crypto_wipe(p, n); }

void random(void* out, size_t n) {
    if (!n) return;
    if (BCryptGenRandom(nullptr, (PUCHAR)out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        std::abort();                                    // no randomness: nothing safe to do
}

Key random_key() { Key k; random(k.data(), 32); return k; }

bool equal(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

// ------------------------------------------------------------------ BLAKE2b
Hash32 blake2b(const void* data, size_t n) {
    Hash32 h;
    crypto_blake2b(h.data(), 32, (const uint8_t*)data, n);
    return h;
}
Hash32 blake2b_keyed(const Key& k, const void* data, size_t n) {
    Hash32 h;
    crypto_blake2b_keyed(h.data(), 32, k.data(), 32, (const uint8_t*)data, n);
    return h;
}
std::array<uint8_t, 64> blake2b_512(const void* data, size_t n) {
    std::array<uint8_t, 64> h;
    crypto_blake2b(h.data(), 64, (const uint8_t*)data, n);
    return h;
}

static_assert(sizeof(crypto_blake2b_ctx) <= 256, "Blake2b::ctx too small");
Blake2b::Blake2b() { crypto_blake2b_init((crypto_blake2b_ctx*)ctx, 32); }
Blake2b::Blake2b(const Key& k) { crypto_blake2b_keyed_init((crypto_blake2b_ctx*)ctx, 32, k.data(), 32); }
Blake2b::~Blake2b() { crypto_wipe(ctx, sizeof(ctx)); }
void Blake2b::update(const void* data, size_t n) { crypto_blake2b_update((crypto_blake2b_ctx*)ctx, (const uint8_t*)data, n); }
Hash32 Blake2b::final() { Hash32 h; crypto_blake2b_final((crypto_blake2b_ctx*)ctx, h.data()); return h; }

Key kdf(const Key& k, std::string_view context, const void* salt, size_t saltLen) {
    Blake2b b(k);
    uint8_t len = (uint8_t)std::min<size_t>(context.size(), 255);
    b.update("ep-kdf", 6);
    b.update(&len, 1);                                   // the context's length: no ambiguity with the salt
    b.update(context.data(), len);
    if (salt && saltLen) b.update(salt, saltLen);
    Key out;
    Hash32 h = b.final();
    std::memcpy(out.data(), h.data(), 32);
    crypto_wipe(h.data(), 32);
    return out;
}

// ------------------------------------------------------------------ AEAD
Bytes aead_lock(const Key& k, const uint8_t nonce[kNonce], const void* ad, size_t adLen, const void* plain, size_t n) {
    Bytes out(n + kTag);
    crypto_aead_lock(out.data(), out.data() + n, k.data(), nonce, (const uint8_t*)ad, adLen, (const uint8_t*)plain, n);
    return out;
}

bool aead_unlock(const Key& k, const uint8_t nonce[kNonce], const void* ad, size_t adLen,
                 const void* cipher, size_t n, Bytes& plain) {
    if (n < kTag) return false;
    plain.resize(n - kTag);
    const uint8_t* c = (const uint8_t*)cipher;
    if (crypto_aead_unlock(plain.data(), c + n - kTag, k.data(), nonce, (const uint8_t*)ad, adLen, c, n - kTag) != 0) {
        crypto_wipe(plain.data(), plain.size());
        plain.clear();
        return false;
    }
    return true;
}

void aead_lock_into(const Key& k, const uint8_t nonce[kNonce], const void* ad, size_t adLen, uint8_t* buf, size_t n, uint8_t tag[kTag]) {
    crypto_aead_lock(buf, tag, k.data(), nonce, (const uint8_t*)ad, adLen, buf, n);
}

bool aead_unlock_into(const Key& k, const uint8_t nonce[kNonce], const void* ad, size_t adLen, uint8_t* buf, size_t n, const uint8_t tag[kTag]) {
    return crypto_aead_unlock(buf, tag, k.data(), nonce, (const uint8_t*)ad, adLen, buf, n) == 0;
}

Bytes seal(const Key& k, std::string_view ad, const void* plain, size_t n) {
    uint8_t nonce[kNonce];
    random(nonce, kNonce);
    Bytes c = aead_lock(k, nonce, ad.data(), ad.size(), plain, n);
    Bytes out(nonce, nonce + kNonce);
    out.insert(out.end(), c.begin(), c.end());
    return out;
}

bool open(const Key& k, std::string_view ad, const Bytes& sealed, Bytes& plain) {
    if (sealed.size() < kNonce + kTag) return false;
    return aead_unlock(k, sealed.data(), ad.data(), ad.size(), sealed.data() + kNonce, sealed.size() - kNonce, plain);
}

void xchacha20(const Key& k, const uint8_t nonce[kNonce], const uint8_t* in, uint8_t* out, size_t n) {
    crypto_chacha20_x(out, in, n, k.data(), nonce, 0);
}

// ------------------------------------------------------------------ Argon2id
Argon2Params argon2_default() {
    Argon2Params p;
    wchar_t buf[32];
    // tests only, and only with a settings file of their own (EXPLORERPLUS_SETTINGS): the
    // parameters travel with each wrap, so smaller ones stay readable, but a stray variable
    // must not weaken a real recovery wrap or pairing grant
    if (GetEnvironmentVariableW(L"EXPLORERPLUS_SETTINGS", nullptr, 0) > 0 &&
        GetEnvironmentVariableW(L"EXPLORERPLUS_SYNC_KDF_KIB", buf, 32) > 0) {
        long v = wcstol(buf, nullptr, 10);
        if (v >= 8) p.memKiB = (uint32_t)v;
        p.passes = 1;
    }
    return p;
}

bool argon2_params_ok(const Argon2Params& p) {
    return p.memKiB >= 8 && p.memKiB <= kArgon2MaxKiB && p.passes >= 1 && p.passes <= kArgon2MaxPasses;
}

bool argon2id(std::string_view pass, const uint8_t* salt, size_t saltLen, const Argon2Params& p, Key& out) {
    uint32_t blocks = std::max<uint32_t>(p.memKiB, 8);
    void* work = VirtualAlloc(nullptr, (SIZE_T)blocks * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!work) return false;                             // not enough memory: the caller says so
    crypto_argon2_config cfg{ CRYPTO_ARGON2_ID, blocks, std::max<uint32_t>(p.passes, 1), 1 };
    crypto_argon2_inputs in{ (const uint8_t*)pass.data(), salt, (uint32_t)pass.size(), (uint32_t)saltLen };
    crypto_argon2(out.data(), 32, work, cfg, in, crypto_argon2_no_extras);
    crypto_wipe(work, (size_t)blocks * 1024);
    VirtualFree(work, 0, MEM_RELEASE);
    return true;
}

// ------------------------------------------------------------------ X25519
void x25519_public(const Key& secret, Key& pub) { crypto_x25519_public_key(pub.data(), secret.data()); }
Key x25519_shared(const Key& secret, const Key& peerPublic) {
    Key s;
    crypto_x25519(s.data(), secret.data(), peerPublic.data());
    return s;
}

// ------------------------------------------------------------------ SHA-256 (BCrypt)
static BCRYPT_ALG_HANDLE alg(bool hmac) {
    static BCRYPT_ALG_HANDLE plain = nullptr, keyed = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        BCryptOpenAlgorithmProvider(&plain, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptOpenAlgorithmProvider(&keyed, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    });
    return hmac ? keyed : plain;
}

Hash32 sha256(const void* data, size_t n) {
    Hash32 h{};
    BCryptHash(alg(false), nullptr, 0, (PUCHAR)data, (ULONG)n, h.data(), 32);
    return h;
}

Hash32 hmac_sha256(const void* key, size_t keyLen, const void* data, size_t n) {
    Hash32 h{};
    BCryptHash(alg(true), (PUCHAR)key, (ULONG)keyLen, (PUCHAR)data, (ULONG)n, h.data(), 32);
    return h;
}

Sha256::Sha256() {
    BCRYPT_HASH_HANDLE hh = nullptr;
    BCryptCreateHash(alg(false), &hh, nullptr, 0, nullptr, 0, 0);
    h = hh;
}
Sha256::~Sha256() { if (h) BCryptDestroyHash((BCRYPT_HASH_HANDLE)h); }
void Sha256::update(const void* data, size_t n) { if (n) BCryptHashData((BCRYPT_HASH_HANDLE)h, (PUCHAR)data, (ULONG)n, 0); }
Hash32 Sha256::final() { Hash32 out{}; BCryptFinishHash((BCRYPT_HASH_HANDLE)h, out.data(), 32, 0); return out; }

// ------------------------------------------------------------------ encodings
std::string hex(const void* data, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = ((const uint8_t*)data)[i];
        s += d[c >> 4];
        s += d[c & 15];
    }
    return s;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool unhex(std::string_view s, Bytes& out) {
    out.clear();
    if (s.size() % 2) return false;
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = hexval(s[i]), b = hexval(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back((uint8_t)(a * 16 + b));
    }
    return true;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    std::string s;
    s.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = p[i] << 16;
        if (i + 1 < n) v |= p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        s += kB64[(v >> 18) & 63];
        s += kB64[(v >> 12) & 63];
        s += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
        s += i + 2 < n ? kB64[v & 63] : '=';
    }
    return s;
}

bool unbase64(std::string_view s, Bytes& out) {
    out.clear();
    uint32_t v = 0;
    int bits = 0;
    size_t pad = 0;
    for (char c : s) {
        if (c == '=') { ++pad; continue; }
        if (c == '\r' || c == '\n' || c == ' ') continue;
        if (pad) return false;
        const char* f = std::strchr(kB64, c);
        if (!f || !c) return false;
        v = (v << 6) | (uint32_t)(f - kB64);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(v >> bits)); }
    }
    return pad <= 2;
}

static const char kB32[] = "abcdefghijklmnopqrstuvwxyz234567";

std::string base32(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    std::string s;
    uint32_t v = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        v = (v << 8) | p[i];
        bits += 8;
        while (bits >= 5) { bits -= 5; s += kB32[(v >> bits) & 31]; }
    }
    if (bits) s += kB32[(v << (5 - bits)) & 31];
    return s;
}

bool unbase32(std::string_view s, Bytes& out) {
    out.clear();
    uint32_t v = 0;
    int bits = 0;
    for (char c : s) {
        int d;
        if (c >= 'a' && c <= 'z') d = c - 'a';
        else if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= '2' && c <= '7') d = c - '2' + 26;
        else return false;
        v = (v << 5) | (uint32_t)d;
        bits += 5;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(v >> bits)); }
    }
    // the leftover bits must be zero (one canonical spelling per byte string)
    return bits < 5 && (v & ((1u << bits) - 1)) == 0;
}

// Crockford Base32: no I, L, O, U
static const char kCrock[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

std::string new_code() {
    uint8_t r[kCodeChars];
    random(r, sizeof(r));
    std::string s;
    for (uint8_t c : r) s += kCrock[c & 31];             // 5 uniform bits per character
    wipe(r, sizeof(r));
    return s;
}

std::string normalize_code(std::string_view typed) {
    std::string s;
    for (char c : typed) {
        if (c == ' ' || c == '-' || c == '\t' || c == '_' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        if (!std::strchr(kCrock, c) || !c) return {};
        s += c;
    }
    return s.size() == kCodeChars ? s : std::string();
}

std::string format_code(const std::string& n) {
    std::string s;
    for (size_t i = 0; i < n.size(); ++i) {
        if (i && i % 5 == 0) s += '-';
        s += n[i];
    }
    return s;
}

// ------------------------------------------------------------------ self test
static bool eq_hex(const uint8_t* p, size_t n, const char* expect) { return hex(p, n) == expect; }

bool self_test(std::string* failed) {
    auto fail = [&](const char* what) { if (failed) *failed = what; return false; };
    // SHA-256 / HMAC (FIPS 180-4, RFC 4231 case 2)
    Hash32 s = sha256("abc", 3);
    if (!eq_hex(s.data(), 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")) return fail("sha256");
    const char* msg = "what do ya want for nothing?";
    s = hmac_sha256("Jefe", 4, msg, std::strlen(msg));
    if (!eq_hex(s.data(), 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")) return fail("hmac-sha256");
    // BLAKE2b-512 (RFC 7693 appendix A)
    auto b = blake2b_512("abc", 3);
    if (!eq_hex(b.data(), 64, "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
                              "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923")) return fail("blake2b");
    // X25519 (RFC 7748 6.1)
    Bytes a, bpub;
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a);
    unhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", bpub);
    Key ak, bk, apub;
    std::memcpy(ak.data(), a.data(), 32);
    std::memcpy(bk.data(), bpub.data(), 32);
    x25519_public(ak, apub);
    if (hex(apub.b) != "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a") return fail("x25519 public");
    Key shared = x25519_shared(ak, bk);
    if (hex(shared.b) != "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742") return fail("x25519");
    // XChaCha20-Poly1305 (draft-irtf-cfrg-xchacha-03, A.3.1)
    Key k;
    for (int i = 0; i < 32; ++i) k.b[i] = (uint8_t)(0x80 + i);
    uint8_t nonce[kNonce];
    for (int i = 0; i < 24; ++i) nonce[i] = (uint8_t)(0x40 + i);
    {
        const char* pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
        Bytes ad;
        unhex("50515253c0c1c2c3c4c5c6c7", ad);
        Bytes x = aead_lock(k, nonce, ad.data(), ad.size(), pt, std::strlen(pt));
        if (hex(x.data(), 16) != "bd6d179d3e83d43b9576579493c0e939" ||
            hex(x.data() + x.size() - 16, 16) != "c0875924c1c7987947deafd8780acf49") return fail("xchacha20-poly1305");
    }
    // Argon2id (RFC 9106, 5.3)
    {
        uint8_t pass[32], salt[16], secret[8], adata[12], out[32];
        std::memset(pass, 1, 32); std::memset(salt, 2, 16); std::memset(secret, 3, 8); std::memset(adata, 4, 12);
        std::vector<uint8_t> work(32 * 1024);
        crypto_argon2_config cfg{ CRYPTO_ARGON2_ID, 32, 3, 4 };
        crypto_argon2_inputs in{ pass, salt, 32, 16 };
        crypto_argon2_extras ex{ secret, adata, 8, 12 };
        crypto_argon2(out, 32, work.data(), cfg, in, ex);
        if (!eq_hex(out, 32, "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659")) return fail("argon2id");
    }
    // AEAD: round trip, and a flipped bit fails
    Bytes c = aead_lock(k, nonce, "ad", 2, "hello", 5), p;
    if (!aead_unlock(k, nonce, "ad", 2, c.data(), c.size(), p) || p != Bytes{ 'h', 'e', 'l', 'l', 'o' }) return fail("aead");
    c[0] ^= 1;
    if (aead_unlock(k, nonce, "ad", 2, c.data(), c.size(), p)) return fail("aead forgery");
    return true;
}

} // namespace crypto
