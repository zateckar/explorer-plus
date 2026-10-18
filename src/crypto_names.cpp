#include "crypto_names.h"
#include <cstring>

namespace cnames {

NameKeys::NameKeys(const crypto::Key& vk) {
    crypto::Key nk = crypto::kdf(vk, "ep names");
    mac = crypto::kdf(nk, "mac");
    enc = crypto::kdf(nk, "enc");
}

static crypto::Bytes pad(const std::string& name) {
    crypto::Bytes p(name.begin(), name.end());
    p.push_back(0x80);
    while (p.size() % 16) p.push_back(0);
    return p;
}

static void tag_of(const NameKeys& k, const std::string& parent, const crypto::Bytes& padded, uint8_t tag[16]) {
    crypto::Blake2b b(k.mac);
    b.update(parent.data(), parent.size());
    uint8_t zero = 0;
    b.update(&zero, 1);
    b.update(padded.data(), padded.size());
    crypto::Hash32 h = b.final();
    std::memcpy(tag, h.data(), 16);
}

std::string encrypt(const NameKeys& k, uint16_t version, const std::string& parent, const std::string& name) {
    crypto::Bytes p = pad(name);
    crypto::Bytes out(2 + 16 + p.size());
    out[0] = (uint8_t)version;
    out[1] = (uint8_t)(version >> 8);
    tag_of(k, parent, p, out.data() + 2);
    uint8_t nonce[crypto::kNonce] = {};
    std::memcpy(nonce, out.data() + 2, 16);
    crypto::xchacha20(k.enc, nonce, p.data(), out.data() + 18, p.size());
    crypto::wipe(p.data(), p.size());
    return crypto::base32(out.data(), out.size());
}

bool stored_version(const std::string& stored, uint16_t& version) {
    crypto::Bytes raw;
    if (stored.size() < 8 || !crypto::unbase32(stored.substr(0, 8), raw) || raw.size() < 2) return false;   // 8 characters: 5 whole bytes
    version = (uint16_t)(raw[0] | (raw[1] << 8));
    return true;
}

bool decrypt(const NameKeys& k, const std::string& parent, const std::string& stored, std::string& name) {
    crypto::Bytes raw;
    if (!crypto::unbase32(stored, raw) || raw.size() < 34 || (raw.size() - 18) % 16) return false;
    uint8_t nonce[crypto::kNonce] = {};
    std::memcpy(nonce, raw.data() + 2, 16);
    crypto::Bytes p(raw.size() - 18);
    crypto::xchacha20(k.enc, nonce, raw.data() + 18, p.data(), p.size());
    uint8_t tag[16];
    tag_of(k, parent, p, tag);
    if (!crypto::equal(tag, raw.data() + 2, 16)) return false;
    size_t end = p.size();
    while (end && p[end - 1] == 0) --end;
    if (!end || p[end - 1] != 0x80) return false;
    name.assign((const char*)p.data(), end - 1);
    return !name.empty();
}

std::string shortened(const std::string& stored) {
    crypto::Hash32 h = crypto::blake2b(stored.data(), stored.size());
    return crypto::base32(h.data(), 20) + kShortExt;
}

} // namespace cnames
