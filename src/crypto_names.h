#pragma once
#include "crypto.h"
#include <string>

// Path component encryption (docs/sync-plan.md, "Names"): deterministic and authenticated,
// the SIV construction built from Monocypher's primitives:
//   padded = UTF-8 name || 0x80 || zeros to a multiple of 16
//   tag    = keyed BLAKE2b(mac key, parent path (UTF-8) || 0 || padded)[0..16)
//   cipher = XChaCha20(enc key, nonce = tag || 8 zero bytes, padded)
//   stored = base32(vault key version (2 bytes, LE) || tag || cipher), lowercase
// The plaintext parent path is in the tag: the same name in two folders looks different.
// The version picks the vault key: names made after a key rotation use the new key, so a
// removed device can't read them either.
namespace cnames {

struct NameKeys {
    crypto::Key mac, enc;
    explicit NameKeys(const crypto::Key& vk);                 // derived from the vault key
};

std::string encrypt(const NameKeys& k, uint16_t version, const std::string& parentUtf8, const std::string& nameUtf8);
bool stored_version(const std::string& stored, uint16_t& version);   // false: not a stored name
// false: not a name these keys made for this parent (tampered, or another vault's)
bool decrypt(const NameKeys& k, const std::string& parentUtf8, const std::string& stored, std::string& nameUtf8);

// stored names longer than a store allows are kept as "<hash>.epn" with the full stored
// name in a sidecar object "<hash>.epn.name" beside it
std::string shortened(const std::string& stored);
inline bool is_short(const std::string& n) { return n.size() > 4 && n.compare(n.size() - 4, 4, ".epn") == 0; }
inline bool is_sidecar(const std::string& n) { return n.size() > 9 && n.compare(n.size() - 9, 9, ".epn.name") == 0; }

} // namespace cnames
