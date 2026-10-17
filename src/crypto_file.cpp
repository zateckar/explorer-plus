#include "crypto_file.h"
#include <cstring>

namespace cfile {

using crypto::Key;

static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t* p) { uint32_t v = 0; for (int i = 3; i >= 0; --i) v = (v << 8) | p[i]; return v; }
static uint64_t get64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; }

static uint64_t chunks_for(uint64_t plain) { return plain == 0 ? 1 : (plain + kChunk - 1) / kChunk; }

uint64_t cipher_size(uint64_t plain) { return kPrefix + plain + chunks_for(plain) * crypto::kTag; }

bool plain_size(uint64_t cipher, uint64_t& plain) {
    if (cipher < kPrefix + crypto::kTag) return false;
    uint64_t c = cipher - kPrefix;
    uint64_t n = (c + kChunk + crypto::kTag - 1) / (kChunk + crypto::kTag);
    if (c < n * crypto::kTag) return false;
    plain = c - n * crypto::kTag;
    return cipher_size(plain) == cipher;
}

static void data_nonce(uint64_t index, bool last, uint8_t nonce[crypto::kNonce]) {
    std::memset(nonce, 0, crypto::kNonce);
    put64(nonce, index);
    nonce[8] = last ? 1 : 0;
}

static void file_keys(const Key& vk, const uint8_t* salt, Key& meta, Key& data) {
    Key fk = crypto::kdf(vk, "ep file", salt, 32);
    meta = crypto::kdf(fk, "meta");
    data = crypto::kdf(fk, "data");
}

static void meta_encode(const Meta& m, uint8_t out[kMetaPlain]) {
    std::memset(out, 0, kMetaPlain);
    put64(out, m.size);
    put64(out + 8, ((uint64_t)m.mtime.dwHighDateTime << 32) | m.mtime.dwLowDateTime);
    put32(out + 16, m.attributes);
    put64(out + 20, m.version);
    std::memcpy(out + 28, m.fingerprint.data(), 32);
}

static void meta_decode(const uint8_t in[kMetaPlain], Meta& m) {
    m.size = get64(in);
    uint64_t t = get64(in + 8);
    m.mtime.dwLowDateTime = (DWORD)t;
    m.mtime.dwHighDateTime = (DWORD)(t >> 32);
    m.attributes = get32(in + 16);
    m.version = get64(in + 20);
    std::memcpy(m.fingerprint.data(), in + 28, 32);
}

// ------------------------------------------------------------------ encrypt
EncryptingSource::EncryptingSource(sync::ByteSource& plain, const Key& vk, uint32_t vkVersion, const Meta& meta,
                                   const uint8_t* fixedSalt)
    : m_plain(plain), m_vk(vk), m_vkVersion(vkVersion), m_meta(meta), m_fixedSalt(fixedSalt != nullptr) {
    m_plainSize = meta.size;
    m_chunks = chunks_for(m_plainSize);
    m_total = cipher_size(m_plainSize);
    if (fixedSalt) std::memcpy(m_head + 12, fixedSalt, 32);
    start();
}

// the header and meta block of one pass: a new salt each time (unless fixed), so a retried
// upload of a file that changed meanwhile never reuses a (key, nonce) pair
void EncryptingSource::start() {
    uint8_t* h = m_head;
    std::memcpy(h, "EPSF", 4);
    h[4] = (uint8_t)kFormat;
    h[5] = 16;                                            // chunk size log2
    h[6] = h[7] = 0;
    put32(h + 8, m_vkVersion);
    if (!m_fixedSalt) crypto::random(h + 12, 32);
    std::memset(h + 44, 0, 4);
    Key mk;
    file_keys(m_vk, h + 12, mk, m_data);
    uint8_t* mb = h + kHeader;
    meta_encode(m_meta, mb);
    uint8_t nonce[crypto::kNonce] = {};
    crypto::aead_lock_into(mk, nonce, h, kHeader, mb, kMetaPlain, mb + kMetaPlain);
    m_buf.assign(m_head, m_head + kPrefix);
}

EncryptingSource::~EncryptingSource() { crypto::wipe(m_buf.data(), m_buf.size()); }

bool EncryptingSource::rewind() {
    if (!m_plain.rewind()) return false;
    m_next = 0;
    m_emitted = 0;
    m_bufPos = 0;
    m_failed = false;
    start();
    return true;
}

bool EncryptingSource::fill() {
    if (m_next >= m_chunks) return true;
    uint64_t want = std::min<uint64_t>(kChunk, m_plainSize - std::min(m_plainSize, m_next * kChunk));
    m_buf.assign(want + crypto::kTag, 0);
    m_bufPos = 0;
    size_t have = 0;
    while (have < want) {
        size_t got = 0;
        if (!m_plain.read(m_buf.data() + have, (size_t)want - have, &got)) return false;
        if (!got) { m_failed = true; return false; }      // shorter than it said
        have += got;
    }
    bool last = m_next + 1 == m_chunks;
    if (last) {                                           // and not longer either
        uint8_t probe;
        size_t got = 0;
        if (m_plain.read(&probe, 1, &got) && got) { m_failed = true; return false; }
    }
    uint8_t nonce[crypto::kNonce];
    data_nonce(m_next, last, nonce);
    crypto::aead_lock_into(m_data, nonce, m_head, kHeader, m_buf.data(), (size_t)want, m_buf.data() + want);
    ++m_next;
    return true;
}

bool EncryptingSource::read(void* buf, size_t n, size_t* got) {
    *got = 0;
    uint8_t* out = (uint8_t*)buf;
    while (n) {
        if (m_bufPos >= m_buf.size()) {
            if (m_next >= m_chunks) break;
            if (!fill()) return false;
        }
        size_t k = std::min(n, m_buf.size() - m_bufPos);
        std::memcpy(out, m_buf.data() + m_bufPos, k);
        m_bufPos += k;
        out += k;
        n -= k;
        *got += k;
    }
    m_emitted += *got;
    return true;
}

// ------------------------------------------------------------------ decrypt
DecryptingSink::DecryptingSink(sync::ByteSink& out, KeyLookup keys) : m_out(out), m_keys(std::move(keys)) {}
DecryptingSink::~DecryptingSink() { crypto::wipe(m_buf.data(), m_buf.size()); }

static bool parse_head(const uint8_t* h, const KeyLookup& keys, Key& mk, Key& dk, std::wstring& why) {
    if (std::memcmp(h, "EPSF", 4) != 0) { why = L"not an encrypted sync file"; return false; }
    if (h[4] != kFormat || h[5] != 16) { why = L"written by a newer version"; return false; }
    Key vk;
    if (!keys(get32(h + 8), vk)) { why = L"encrypted with a key this device doesn't have"; return false; }
    file_keys(vk, h + 12, mk, dk);
    return true;
}

static bool open_meta(const uint8_t* h, const Key& mk, Meta& m) {
    uint8_t mb[kMeta];
    std::memcpy(mb, h + kHeader, kMeta);
    uint8_t nonce[crypto::kNonce] = {};
    if (!crypto::aead_unlock_into(mk, nonce, h, kHeader, mb, kMetaPlain, mb + kMetaPlain)) return false;
    meta_decode(mb, m);
    crypto::wipe(mb, sizeof(mb));
    return true;
}

bool DecryptingSink::write(const void* buf, size_t n) {
    if (m_bad) return false;
    m_buf.insert(m_buf.end(), (const uint8_t*)buf, (const uint8_t*)buf + n);
    if (!m_haveMeta) {
        if (m_buf.size() < kPrefix) return true;
        Key mk;
        if (!parse_head(m_buf.data(), m_keys, mk, m_data, m_why)) { m_bad = true; return false; }
        if (!open_meta(m_buf.data(), mk, m_meta)) { m_why = L"its header doesn't authenticate"; m_bad = true; return false; }
        std::memcpy(m_head, m_buf.data(), kHeader);
        m_buf.erase(m_buf.begin(), m_buf.begin() + kPrefix);
        m_haveHead = m_haveMeta = true;
    }
    // a chunk is known not to be the last once more bytes follow it
    while (m_buf.size() > kChunk + crypto::kTag)
        if (!chunk(false)) return false;
    return !m_outStopped;
}

bool DecryptingSink::chunk(bool last) {
    size_t n = last ? m_buf.size() : kChunk + crypto::kTag;
    if (n < crypto::kTag) { m_why = L"it was cut short"; m_bad = true; return false; }
    size_t plain = n - crypto::kTag;
    uint8_t nonce[crypto::kNonce];
    data_nonce(m_index, last, nonce);
    if (!crypto::aead_unlock_into(m_data, nonce, m_head, kHeader, m_buf.data(), plain, m_buf.data() + plain)) {
        m_why = last ? L"it was cut short or changed" : L"its content was changed";
        m_bad = true;
        return false;
    }
    if (!m_out.write(m_buf.data(), plain)) m_outStopped = true;
    m_written += plain;
    crypto::wipe(m_buf.data(), plain);
    m_buf.erase(m_buf.begin(), m_buf.begin() + n);
    ++m_index;
    return true;
}

sync::Result DecryptingSink::finish() {
    if (!m_bad && !m_haveMeta) { m_why = L"it was cut short"; m_bad = true; }
    if (!m_bad && !m_outStopped) chunk(true);
    if (m_outStopped) return sync::Result::fail(sync::Status::Cancelled, L"stopped");
    if (!m_bad && m_written != m_meta.size) { m_why = L"its size doesn't match"; m_bad = true; }
    if (m_bad) return sync::Result::fail(sync::Status::Tampered, L"The stored file can't be decrypted: " + m_why + L".");
    return {};
}

sync::Result read_meta(const uint8_t* prefix, size_t n, const KeyLookup& keys, Meta& out) {
    if (n < kPrefix) return sync::Result::fail(sync::Status::Tampered, L"The stored file is too short.");
    Key mk, dk;
    std::wstring why;
    if (!parse_head(prefix, keys, mk, dk, why)) return sync::Result::fail(sync::Status::Tampered, why);
    if (!open_meta(prefix, mk, out)) return sync::Result::fail(sync::Status::Tampered, L"The stored file's header doesn't authenticate.");
    return {};
}

} // namespace cfile
