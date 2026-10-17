// Unit tests of the sync feature: cryptography (against published vectors), the stored
// file format, name encryption, and (as they land) the planner, state journal, engine and
// stores. Run: ctest --test-dir build (or build\explorer-plus-sync-tests.exe)
#include "crypto.h"
#include "crypto_file.h"
#include "crypto_names.h"
#include "../third_party/monocypher/monocypher.h"
#include "sync_config.h"
#include "sync_ipc.h"
#include "sync_tests_common.h"
#include <cstdio>
#include <cstring>

int t_failed = 0, t_run = 0;

void t_check(bool ok, const char* what, const char* file, int line) {
    ++t_run;
    if (ok) return;
    ++t_failed;
    std::printf("FAIL %s:%d: %s\n", file, line, what);
    std::fflush(stdout);
}

static crypto::Bytes H(const char* s) { crypto::Bytes b; crypto::unhex(s, b); return b; }

// ------------------------------------------------------------------ primitives
static void test_vectors() {
    std::string why;
    CHECK(crypto::self_test(&why));
    if (!why.empty()) std::printf("  self test: %s\n", why.c_str());

    // BLAKE2b-512 of "" (RFC 7693 / reference implementation)
    auto e = crypto::blake2b_512("", 0);
    CHECK(crypto::hex(e) == "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419"
                            "d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce");

    // XChaCha20-Poly1305 (draft-irtf-cfrg-xchacha-03, A.3.1)
    const char* pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    crypto::Key k;
    for (int i = 0; i < 32; ++i) k.b[i] = (uint8_t)(0x80 + i);
    uint8_t nonce[24];
    for (int i = 0; i < 24; ++i) nonce[i] = (uint8_t)(0x40 + i);
    crypto::Bytes ad = H("50515253c0c1c2c3c4c5c6c7");
    crypto::Bytes c = crypto::aead_lock(k, nonce, ad.data(), ad.size(), pt, std::strlen(pt));
    CHECK(crypto::hex(c.data(), 16) == "bd6d179d3e83d43b9576579493c0e939");
    CHECK(crypto::hex(c.data() + c.size() - 16, 16) == "c0875924c1c7987947deafd8780acf49");

    // Argon2id (RFC 9106, 5.3): 32 KiB, 3 passes, 4 lanes, secret and associated data
    {
        uint8_t pass[32], salt[16], secret[8], adata[12], out[32];
        std::memset(pass, 1, 32); std::memset(salt, 2, 16); std::memset(secret, 3, 8); std::memset(adata, 4, 12);
        std::vector<uint8_t> work(32 * 1024);
        crypto_argon2_config cfg{ CRYPTO_ARGON2_ID, 32, 3, 4 };
        crypto_argon2_inputs in{ pass, salt, 32, 16 };
        crypto_argon2_extras ex{ secret, adata, 8, 12 };
        crypto_argon2(out, 32, work.data(), cfg, in, ex);
        CHECK(crypto::hex(out, 32) == "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659");
    }
    // our wrapper: deterministic, salt-dependent
    {
        uint8_t s1[16] = { 1 }, s2[16] = { 2 };
        crypto::Argon2Params p{ 64, 1 };
        crypto::Key a, b, c;
        CHECK(crypto::argon2id("ABCDE", s1, 16, p, a) && crypto::argon2id("ABCDE", s1, 16, p, b) &&
              crypto::argon2id("ABCDE", s2, 16, p, c));
        CHECK(a == b && !(a == c));
        // parameters from the storage: a planted wrap can't ask for terabytes or hours
        CHECK(crypto::argon2_params_ok(crypto::Argon2Params{}));
        CHECK(!crypto::argon2_params_ok(crypto::Argon2Params{ 0xFFFFFFFFu, 3 }));
        CHECK(!crypto::argon2_params_ok(crypto::Argon2Params{ 256 * 1024, 1000 }));
        CHECK(!crypto::argon2_params_ok(crypto::Argon2Params{ 4, 1 }));
        // the test-only cost needs the test's own settings file
        wchar_t keep[MAX_PATH] = {};
        GetEnvironmentVariableW(L"EXPLORERPLUS_SETTINGS", keep, MAX_PATH);
        SetEnvironmentVariableW(L"EXPLORERPLUS_SYNC_KDF_KIB", L"64");
        CHECK(crypto::argon2_default().memKiB == 64);
        SetEnvironmentVariableW(L"EXPLORERPLUS_SETTINGS", nullptr);
        CHECK(crypto::argon2_default().memKiB == 256 * 1024 && crypto::argon2_default().passes == 3);
        SetEnvironmentVariableW(L"EXPLORERPLUS_SETTINGS", keep);
        SetEnvironmentVariableW(L"EXPLORERPLUS_SYNC_KDF_KIB", nullptr);
    }
    // SigV4 signing key (AWS docs, "Examples of how to derive a signing key")
    {
        std::string secret = "AWS4wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
        auto d = crypto::hmac_sha256(secret.data(), secret.size(), "20120215", 8);
        auto r = crypto::hmac_sha256(d.data(), 32, "us-east-1", 9);
        auto s = crypto::hmac_sha256(r.data(), 32, "iam", 3);
        auto g = crypto::hmac_sha256(s.data(), 32, "aws4_request", 12);
        CHECK(crypto::hex(g) == "f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d");
    }
    // streaming SHA-256 = one shot
    {
        crypto::Sha256 s;
        s.update("ab", 2);
        s.update("c", 1);
        CHECK(crypto::hex(s.final()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
}

static void test_encodings() {
    CHECK(crypto::base64("foobar", 6) == "Zm9vYmFy");
    CHECK(crypto::base64("fo", 2) == "Zm8=");
    crypto::Bytes b;
    CHECK(crypto::unbase64("Zm8=", b) && b == crypto::Bytes({ 'f', 'o' }));
    CHECK(crypto::base32("foobar", 6) == "mzxw6ytboi");           // RFC 4648 (lowercase, unpadded)
    CHECK(crypto::unbase32("mzxw6ytboi", b) && std::string(b.begin(), b.end()) == "foobar");
    CHECK(!crypto::unbase32("mzxw6ytboj", b));                    // non-zero leftover bits
    CHECK(!crypto::unbase32("MZ!", b));
    // codes
    std::string c = crypto::new_code();
    CHECK(c.size() == 15);
    CHECK(crypto::normalize_code(c) == c);
    CHECK(crypto::format_code(c).size() == 17 && crypto::format_code(c)[5] == '-');
    CHECK(crypto::normalize_code("7kqm2-xrpa9-ht4wd") == "7KQM2XRPA9HT4WD");
    CHECK(crypto::normalize_code(" 7KQM2 XRPA9 HT4WD ") == "7KQM2XRPA9HT4WD");
    CHECK(crypto::normalize_code("oIL00-00000-00000") == "01100000000000" "0");
    CHECK(crypto::normalize_code("7KQM2-XRPA9-HT4W").empty());     // 14 characters
    CHECK(crypto::normalize_code("7KQM2-XRPA9-HT4WU").empty());    // U isn't Crockford
    // two codes differ (75 random bits)
    CHECK(crypto::new_code() != crypto::new_code());
}

// ------------------------------------------------------------------ file format
struct VecSource : sync::ByteSource {
    std::vector<uint8_t> d; size_t pos = 0;
    uint64_t size() override { return d.size(); }
    bool read(void* b, size_t n, size_t* got) override {
        size_t k = std::min(n, d.size() - pos); std::memcpy(b, d.data() + pos, k); pos += k; *got = k; return true; }
    bool rewind() override { pos = 0; return true; }
};
struct VecSink : sync::ByteSink {
    std::vector<uint8_t> d;
    bool write(const void* b, size_t n) override { d.insert(d.end(), (const uint8_t*)b, (const uint8_t*)b + n); return true; }
};

static std::vector<uint8_t> encrypt_all(const std::vector<uint8_t>& plain, const crypto::Key& vk, uint32_t ver, const uint8_t* salt = nullptr) {
    VecSource src; src.d = plain;
    cfile::Meta m; m.size = plain.size(); m.version = 7; m.attributes = 32;
    m.mtime.dwLowDateTime = 1234; m.mtime.dwHighDateTime = 5678;
    m.fingerprint[0] = 0xAB;
    cfile::EncryptingSource e(src, vk, ver, m, salt);
    std::vector<uint8_t> out;
    uint8_t buf[7777];
    for (;;) { size_t got = 0; if (!e.read(buf, sizeof(buf), &got) || !got) break; out.insert(out.end(), buf, buf + got); }
    CHECK(out.size() == e.size());
    return out;
}

static sync::Result decrypt_all(const std::vector<uint8_t>& c, const crypto::Key& vk, std::vector<uint8_t>& plain, cfile::Meta* meta = nullptr,
                                size_t step = 5000) {
    VecSink sink;
    cfile::DecryptingSink d(sink, [&](uint32_t v, crypto::Key& k) { if (v != 3) return false; k = vk; return true; });
    for (size_t i = 0; i < c.size(); i += step)
        if (!d.write(c.data() + i, std::min(step, c.size() - i))) break;
    sync::Result r = d.finish();
    plain = sink.d;
    if (meta) *meta = d.meta();
    return r;
}

static void test_file_format() {
    crypto::Key vk = crypto::random_key();
    for (size_t n : { (size_t)0, (size_t)1, (size_t)65535, (size_t)65536, (size_t)65537, (size_t)200000 }) {
        std::vector<uint8_t> p(n);
        for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)(i * 7 + 3);
        auto c = encrypt_all(p, vk, 3);
        CHECK(c.size() == cfile::cipher_size(n));
        uint64_t back = 0;
        CHECK(cfile::plain_size(c.size(), back) && back == n);
        std::vector<uint8_t> q;
        cfile::Meta m;
        CHECK(decrypt_all(c, vk, q, &m).ok());
        CHECK(q == p);
        CHECK(m.size == n && m.version == 7 && m.attributes == 32 && m.mtime.dwHighDateTime == 5678 && m.fingerprint[0] == 0xAB);
        // byte-by-byte delivery gives the same
        if (n < 70000) { CHECK(decrypt_all(c, vk, q, nullptr, 1).ok() && q == p); }
        cfile::Meta pm;
        CHECK(cfile::read_meta(c.data(), c.size(), [&](uint32_t, crypto::Key& k) { k = vk; return true; }, pm).ok() && pm.size == n);
    }
    // the same content twice: different bytes (fresh salt)
    std::vector<uint8_t> p(1000, 'x');
    CHECK(encrypt_all(p, vk, 3) != encrypt_all(p, vk, 3));
    // a retried upload (rewind) of a file that changed meanwhile: a new salt, so no
    // (key, nonce) pair encrypts two different plaintexts; a fixed salt (tests) repeats
    {
        auto pass = [](cfile::EncryptingSource& e) {
            std::vector<uint8_t> out(e.size());
            size_t got = 0;
            CHECK(e.read(out.data(), out.size(), &got) && got == out.size());
            return out;
        };
        VecSource src;
        src.d.assign(70000, 'a');
        cfile::Meta m;
        m.size = src.d.size();
        cfile::EncryptingSource e(src, vk, 3, m);
        auto first = pass(e);
        src.d.assign(70000, 'b');                                  // same size, new content
        CHECK(e.rewind());
        auto second = pass(e);
        CHECK(!std::equal(first.begin() + 12, first.begin() + 44, second.begin() + 12));   // the salt
        std::vector<uint8_t> q;
        CHECK(decrypt_all(first, vk, q).ok() && q == std::vector<uint8_t>(70000, 'a'));
        CHECK(decrypt_all(second, vk, q).ok() && q == std::vector<uint8_t>(70000, 'b'));
        uint8_t salt[32] = { 9 };
        src.rewind();
        cfile::EncryptingSource f(src, vk, 3, m, salt);
        auto a = pass(f);
        CHECK(f.rewind());
        CHECK(pass(f) == a);
    }
    // known answer: fixed key + salt gives fixed bytes forever (format v1 is frozen)
    {
        crypto::Key fk;
        for (int i = 0; i < 32; ++i) fk.b[i] = (uint8_t)i;
        uint8_t salt[32];
        for (int i = 0; i < 32; ++i) salt[i] = (uint8_t)(0xA0 + i);
        std::vector<uint8_t> hello = { 'h', 'e', 'l', 'l', 'o' };
        auto c = encrypt_all(hello, fk, 3, salt);
        auto h = crypto::blake2b(c.data(), c.size());
        std::printf("  format v1 known answer: %s\n", crypto::hex(h).c_str());
        CHECK(crypto::hex(h) == FORMAT_V1_KAT);
    }
    // tampering
    std::vector<uint8_t> big(150000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = (uint8_t)i;
    auto c = encrypt_all(big, vk, 3);
    std::vector<uint8_t> q;
    auto t = c; t[cfile::kPrefix + 100] ^= 1;
    CHECK(decrypt_all(t, vk, q).st == sync::Status::Tampered);
    t = c; t.resize(t.size() - 1);
    CHECK(decrypt_all(t, vk, q).st == sync::Status::Tampered);
    t = c; t.resize(cfile::kPrefix + 65536 + 16);                 // cut at a chunk boundary
    CHECK(decrypt_all(t, vk, q).st == sync::Status::Tampered);
    t = c; t.push_back(0);
    CHECK(decrypt_all(t, vk, q).st == sync::Status::Tampered);
    t = c; t[20] ^= 1;                                              // the salt (header)
    CHECK(decrypt_all(t, vk, q).st == sync::Status::Tampered);
    t = c;                                                          // swapped chunks
    std::swap_ranges(t.begin() + cfile::kPrefix, t.begin() + cfile::kPrefix + 65552, t.begin() + cfile::kPrefix + 65552);
    CHECK(decrypt_all(t, vk, q).st == sync::Status::Tampered);
    // another file's header on this file's chunks
    auto other = encrypt_all(big, vk, 3);
    t = c; std::copy(other.begin(), other.begin() + cfile::kPrefix, t.begin());
    CHECK(decrypt_all(t, vk, q).st == sync::Status::Tampered);
    // wrong key / unknown version
    CHECK(decrypt_all(c, crypto::random_key(), q).st == sync::Status::Tampered);
    auto v4 = encrypt_all(big, vk, 4);
    CHECK(decrypt_all(v4, vk, q).st == sync::Status::Tampered);
    // a nothing-written guarantee: no plaintext comes out of a chunk that fails
    t = c; t[cfile::kPrefix + 5] ^= 1;
    decrypt_all(t, vk, q);
    CHECK(q.empty());
}

static void test_names() {
    crypto::Key vk = crypto::random_key();
    cnames::NameKeys k(vk);
    std::string s = cnames::encrypt(k, 1, "", "Report.docx");
    CHECK(s.find_first_not_of("abcdefghijklmnopqrstuvwxyz234567") == std::string::npos);
    CHECK(s == cnames::encrypt(k, 1, "", "Report.docx"));             // deterministic
    CHECK(s != cnames::encrypt(k, 1, "Other", "Report.docx"));         // the parent counts
    std::string n;
    CHECK(cnames::decrypt(k, "", s, n) && n == "Report.docx");
    uint16_t ver = 0;
    CHECK(cnames::stored_version(cnames::encrypt(k, 513, "", "x"), ver) && ver == 513);
    CHECK(!cnames::decrypt(k, "Other", s, n));                      // moved into another folder
    CHECK(!cnames::decrypt(cnames::NameKeys(crypto::random_key()), "", s, n));
    std::string t = s; t[3] = t[3] == 'a' ? 'b' : 'a';
    CHECK(!cnames::decrypt(k, "", t, n));
    // lengths leak in 16-byte steps only
    CHECK(cnames::encrypt(k, 1, "", "a").size() == cnames::encrypt(k, 1, "", "abcdefghijklmno").size());
    CHECK(cnames::encrypt(k, 1, "", "a").size() < cnames::encrypt(k, 1, "", "abcdefghijklmnop").size());
    // unicode
    std::string u = "\xC5\xA0koda \xF0\x9F\x9A\x97.txt";
    CHECK(cnames::decrypt(k, "a/b", cnames::encrypt(k, 1, "a/b", u), n) && n == u);
    // shortening
    std::string sh = cnames::shortened(s);
    CHECK(cnames::is_short(sh) && !cnames::is_sidecar(sh) && cnames::is_sidecar(sh + ".name"));
}

// ------------------------------------------------------------------ agent plumbing
static void test_ipc_and_config() {
    // the message codec: any byte in keys and values, repeated keys kept in order
    syncipc::Msg m = { { "cmd", "status" }, { "folder", "C:\\a=b\\c\n\r\\e" }, { "item", "x\ty" }, { "item", "" }, { "k=ey", "v" } };
    syncipc::Msg back;
    CHECK(syncipc::decode(syncipc::encode(m), back) && back == m);
    CHECK(syncipc::get(back, "folder") == "C:\\a=b\\c\n\r\\e" && syncipc::all(back, "item").size() == 2);
    CHECK(syncipc::get(back, "missing", "d") == "d");
    CHECK(!syncipc::decode("no equals sign\n", back));

    // pairs.ini
    std::vector<syncconf::Pair> ps(2);
    ps[0].id = "0011aabb";
    ps[0].name = L"Docs \x0160koda";
    ps[0].root = L"D:\\Sync\\Docs";
    ps[0].type = L"azdo";
    ps[0].cfg = { { L"url", L"https://dev.azure.com/o/p/_git/r" }, { L"branch", L"explorerplus-sync" } };
    ps[0].create = true;
    ps[0].excluded = { L"Big stuff", L"a/b c" };
    ps[1].id = "ccdd0099";
    ps[1].name = L"Docs Old";
    ps[1].root = L"D:\\Sync\\Docs Old";
    ps[1].type = L"folder";
    ps[1].paused = true;
    auto again = syncconf::parse(syncconf::serialize(ps));
    CHECK(again.size() == 2);
    if (again.size() == 2) {
        CHECK(again[0].id == ps[0].id && again[0].name == ps[0].name && again[0].root == ps[0].root && again[0].type == ps[0].type);
        CHECK(again[0].cfg == ps[0].cfg && again[0].create && !again[0].paused && again[0].excluded == ps[0].excluded);
        CHECK(again[1].paused && !again[1].create && again[1].root == ps[1].root);
    }
    // a path's pair: the folder itself or below it, not a sibling with the same prefix
    sync::RelPath rel;
    CHECK(syncconf::pair_of(ps, L"D:\\Sync\\Docs", &rel) == &ps[0] && rel.empty());
    CHECK(syncconf::pair_of(ps, L"d:\\sync\\docs\\Sub\\x.txt", &rel) == &ps[0] && rel == L"Sub/x.txt");
    CHECK(syncconf::pair_of(ps, L"D:\\Sync\\Docs Old\\y", &rel) == &ps[1] && rel == L"y");
    CHECK(syncconf::pair_of(ps, L"D:\\Sync\\Docs2") == nullptr);
    CHECK(syncconf::pair_of(ps, L"D:\\Sync") == nullptr);

    // a request and its reply over the pipe (this test run's own settings namespace)
    syncipc::Server srv;
    srv.start([](const syncipc::Msg& q) {
        syncipc::Msg r = syncipc::ok();
        r.push_back({ "echo", syncipc::get(q, "x") });
        return r;
    });
    syncipc::Msg reply;
    bool ok = false;
    for (int i = 0; i < 50 && !ok; ++i) {
        ok = syncipc::call({ { "cmd", "ping" }, { "x", std::string(100000, 'z') } }, reply, 5000);
        if (!ok) Sleep(50);
    }
    CHECK(ok && syncipc::get(reply, "ok") == "1" && syncipc::get(reply, "echo").size() == 100000);
    srv.stop();
    CHECK(!syncipc::call(syncipc::request("ping"), reply, 300));
}

int run_engine_tests();

int wmain() {
    // a settings folder of its own: the pipe, Credential Manager names (tests never meet a
    // real agent or the user's keys)
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring settingsDir = std::wstring(tmp) + L"ep-sync-tests-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(settingsDir.c_str(), nullptr);
    SetEnvironmentVariableW(L"EXPLORERPLUS_SETTINGS", (settingsDir + L"\\settings.ini").c_str());
    test_ipc_and_config();
    test_vectors();
    test_encodings();
    test_file_format();
    test_names();
    run_engine_tests();
    RemoveDirectoryW((settingsDir + L"\\Sync").c_str());
    RemoveDirectoryW(settingsDir.c_str());
    std::printf("%d checks, %d failed\n", t_run, t_failed);
    return t_failed ? 1 : 0;
}
