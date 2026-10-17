#pragma once
#include "crypto.h"
#include "sync_backend.h"
#include <functional>

// The stored form of a synced file (format version 1, docs/sync-format.md):
//   header  48 bytes, plaintext: "EPSF", version 1, chunk size log2 16, vault key
//           version, a random 32-byte salt; authenticated as additional data below
//   meta    64 bytes + tag: plaintext size, mtime, attributes, version counter, fingerprint
//   chunks  64 KiB of plaintext + 16-byte tag each, XChaCha20-Poly1305 with
//           nonce = chunk index || last-chunk flag (STREAM); an empty file is one empty
//           last chunk, so truncation and appending are detected
// The file key is KDF(vault key, "ep file", salt): new for every upload.
namespace cfile {

constexpr size_t kHeader = 48, kMetaPlain = 64, kMeta = kMetaPlain + crypto::kTag, kPrefix = kHeader + kMeta;
constexpr size_t kChunk = 64 * 1024;
constexpr uint32_t kFormat = 1;

struct Meta {
    uint64_t size = 0;
    FILETIME mtime{};
    uint32_t attributes = 0;
    uint64_t version = 0;
    crypto::Hash32 fingerprint{};
};

uint64_t cipher_size(uint64_t plain);
bool plain_size(uint64_t cipher, uint64_t& plain);      // false: not a size this format makes

// the vault key of a version (false: unknown version)
using KeyLookup = std::function<bool(uint32_t version, crypto::Key& vk)>;

// Encrypts `plain` (its size() bytes) as it is read. rewind() (a retried upload) starts
// again under a new salt, so a file that changed between attempts never reuses a nonce
// under the same key; with fixedSalt (tests) every pass gives the same bytes.
class EncryptingSource : public sync::ByteSource {
public:
    EncryptingSource(sync::ByteSource& plain, const crypto::Key& vk, uint32_t vkVersion, const Meta& meta,
                     const uint8_t* fixedSalt = nullptr);            // fixedSalt: tests (known answers)
    ~EncryptingSource() override;
    uint64_t size() override { return m_total; }
    bool read(void* buf, size_t n, size_t* got) override;
    bool rewind() override;
    bool failed() const { return m_failed; }                         // the plaintext didn't have the size it claimed
private:
    void start();
    bool fill();
    sync::ByteSource& m_plain;
    crypto::Key m_vk, m_data;
    uint32_t m_vkVersion;
    Meta m_meta;
    bool m_fixedSalt;
    uint8_t m_head[kPrefix];
    uint64_t m_plainSize, m_total, m_chunks, m_next = 0;   // chunks produced so far
    std::vector<uint8_t> m_buf;                             // produced, not yet read
    size_t m_bufPos = 0;
    uint64_t m_emitted = 0;
    bool m_failed = false;
};

// Decrypts as the stored bytes arrive and writes plaintext to `out`; finish() at the end.
// Nothing is written for a chunk that doesn't authenticate.
class DecryptingSink : public sync::ByteSink {
public:
    DecryptingSink(sync::ByteSink& out, KeyLookup keys);
    ~DecryptingSink() override;
    bool write(const void* buf, size_t n) override;
    sync::Result finish();                                  // Tampered unless everything authenticated
    const Meta& meta() const { return m_meta; }
    bool has_meta() const { return m_haveMeta; }
private:
    bool chunk(bool last);
    sync::ByteSink& m_out;
    KeyLookup m_keys;
    std::vector<uint8_t> m_buf;
    uint8_t m_head[kHeader]{};
    crypto::Key m_data;
    Meta m_meta;
    bool m_haveHead = false, m_haveMeta = false, m_bad = false, m_outStopped = false;
    uint64_t m_index = 0, m_written = 0;
    std::wstring m_why;
};

// the meta block from the first kPrefix stored bytes (fingerprint peeks)
sync::Result read_meta(const uint8_t* prefix, size_t n, const KeyLookup& keys, Meta& out);

} // namespace cfile
