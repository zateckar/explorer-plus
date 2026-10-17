#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The sync feature's cryptography: a thin layer over Monocypher (third_party/monocypher,
// the only file that includes monocypher.h) plus the system random source. Pure apart
// from crypto_random; unit-tested (tests/sync_tests.cpp) against published vectors.
namespace crypto {

using Bytes = std::vector<uint8_t>;

// a 256-bit key that wipes itself
struct Key {
    std::array<uint8_t, 32> b{};
    Key() = default;
    Key(const Key& o) : b(o.b) {}
    Key& operator=(const Key& o) { b = o.b; return *this; }
    ~Key();
    const uint8_t* data() const { return b.data(); }
    uint8_t* data() { return b.data(); }
    bool operator==(const Key& o) const;
};
using Hash32 = std::array<uint8_t, 32>;

void wipe(void* p, size_t n);
void random(void* out, size_t n);          // BCryptGenRandom (the system RNG)
Key  random_key();
bool equal(const uint8_t* a, const uint8_t* b, size_t n);   // constant time

// BLAKE2b
Hash32 blake2b(const void* data, size_t n);
Hash32 blake2b_keyed(const Key& k, const void* data, size_t n);
std::array<uint8_t, 64> blake2b_512(const void* data, size_t n);
struct Blake2b {                           // streaming, 32-byte output (optionally keyed)
    Blake2b();
    explicit Blake2b(const Key& k);
    ~Blake2b();
    void update(const void* data, size_t n);
    Hash32 final();
private:
    alignas(8) uint8_t ctx[256];
};

// key derivation as libsodium's crypto_kdf: keyed BLAKE2b over a context string and an
// optional salt (subkeys of one key with different contexts are independent)
Key kdf(const Key& k, std::string_view context, const void* salt = nullptr, size_t saltLen = 0);

// XChaCha20-Poly1305: cipher = plain || 16-byte tag
constexpr size_t kTag = 16, kNonce = 24;
Bytes aead_lock(const Key& k, const uint8_t nonce[kNonce], const void* ad, size_t adLen, const void* plain, size_t n);
bool  aead_unlock(const Key& k, const uint8_t nonce[kNonce], const void* ad, size_t adLen,
                  const void* cipher, size_t n, Bytes& plain);     // false: forged / wrong key
// in place (buf holds n plaintext bytes, then the tag is written after them)
void  aead_lock_into(const Key& k, const uint8_t nonce[kNonce], const void* ad, size_t adLen, uint8_t* buf, size_t n, uint8_t tag[kTag]);
bool  aead_unlock_into(const Key& k, const uint8_t nonce[kNonce], const void* ad, size_t adLen, uint8_t* buf, size_t n, const uint8_t tag[kTag]);
// self-describing wrap: random nonce || cipher || tag
Bytes seal(const Key& k, std::string_view ad, const void* plain, size_t n);
bool  open(const Key& k, std::string_view ad, const Bytes& sealed, Bytes& plain);

// XChaCha20 keystream (the name encryption's cipher)
void xchacha20(const Key& k, const uint8_t nonce[kNonce], const uint8_t* in, uint8_t* out, size_t n);

// Argon2id (memKiB >= 8): the slow key from a short code
struct Argon2Params { uint32_t memKiB = 256 * 1024, passes = 3; };
Key argon2id(std::string_view pass, const uint8_t* salt, size_t saltLen, const Argon2Params& p);
Argon2Params argon2_default();             // 256 MiB / 3 passes (EXPLORERPLUS_SYNC_KDF_KIB: tests only)

// X25519
void x25519_public(const Key& secret, Key& pub);
Key  x25519_shared(const Key& secret, const Key& peerPublic);   // raw shared secret: hash it (kdf) before use

// SHA-256 / HMAC-SHA256 (BCrypt: AWS signatures need them; Monocypher has neither)
Hash32 sha256(const void* data, size_t n);
Hash32 hmac_sha256(const void* key, size_t keyLen, const void* data, size_t n);
struct Sha256 {
    Sha256();
    ~Sha256();
    void update(const void* data, size_t n);
    Hash32 final();
private:
    void* h = nullptr;
};

// encodings
std::string hex(const void* data, size_t n);
template <class A> std::string hex(const A& a) { return hex(a.data(), a.size()); }
bool unhex(std::string_view s, Bytes& out);
std::string base64(const void* data, size_t n);
inline std::string base64(const Bytes& b) { return base64(b.data(), b.size()); }
bool unbase64(std::string_view s, Bytes& out);
// RFC 4648 base32, lowercase, no padding (stored names: safe on any store, case-insensitive ones too)
std::string base32(const void* data, size_t n);
bool unbase32(std::string_view s, Bytes& out);

// Codes the user writes down: 15 Crockford Base32 characters (75 bits) as XXXXX-XXXXX-XXXXX
constexpr size_t kCodeChars = 15;
std::string new_code();
// tidy what the user typed: case, blanks, dashes ignored; O -> 0, I / L -> 1; "" when it
// isn't 15 valid characters
std::string normalize_code(std::string_view typed);
std::string format_code(const std::string& normalized);    // with the dashes

// known-answer self test of every primitive (the agent runs it at start)
bool self_test(std::string* failed = nullptr);

} // namespace crypto
