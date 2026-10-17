# Sync folders: plan

Dropbox / OneDrive / Google Drive style synchronization of local folders with remote
storage, starting with **Amazon S3 (and S3-compatible: MinIO, R2, B2, Wasabi)**, **git**
and **Azure DevOps Git** remotes, built on one internal backend abstraction so further
backends (WebDAV / Nextcloud, OneDrive, Google Drive, Dropbox, Azure Blob, NAS shares...)
are a single new file each.

**Everything that leaves the machine is end-to-end encrypted**: file contents, file and
folder names, modification times (sizes stay visible: format v1 has no padding). The storage only ever
holds ciphertext; there is no unencrypted mode. **One user's devices share portable
keys**: a new PC is unlocked with a 15-character code shown on a PC that's already set
up, or with the 15-character recovery key. Both are short enough to write down, so there
is no passphrase. A removed device is locked out of everything written after its removal.

## Status

Phases 1–6 are built: the engine, encryption and keys, the Folder, S3, git and Azure
DevOps Git backends, the agent (`--sync`, tray icon, `--sync-cli`), and the UI (Sync
folders window with Add / Edit / Remove / Pause / Sync now, Devices and keys with
"Re-encrypt files", badges and a "Sync status" column, status bar, navigation pane nodes,
Sync now / Exclude from sync / Include in sync / Version history... in the context menu,
autostart). The lists learn about changes from the agent by a long poll (`watch`, below).
The stored formats as built are in [sync-format.md](sync-format.md), with where they
differ from this plan (plain fingerprint, no `ring` object, version prefix on names, the
file-swap limitation). Tests: `explorer-plus-sync-tests` (engine, crypto, keys, git with
versions / restore / re-encryption, S3 against an in-process mock, Azure DevOps when its
env variables are set), `tools/sync_test.py` (three devices through the agents, over a
Folder store and Azure DevOps) and `explorer-plus-sync-tests --bench` ("Benchmark" below).

Deferred, out of this plan's scope (decided 2026-10-17): phase 7, each item "later, on its
own". The independent review of the cryptography is in [sync-review.md](sync-review.md) on
its own branch until it merges.

*Recovery key, as built*: the Sync folders window keeps the recovery key it shows (or the
one typed to unlock this PC) in Credential Manager beside the key ring. "Show recovery
key..." in Devices and keys shows it again after a Windows sign-in prompt
(`CredUIPromptForWindowsCredentials`, checked with `LogonUser`), with the date it was kept:
another device may have made a newer one since. Nothing goes through the agent's pipe.

### Benchmark

`explorer-plus-sync-tests --bench [small files] [big file MB]` (10,000 files of 1–8 KB and
one 2 GB file by default) syncs through an encrypted Folder store on the same disk and
counts the store's calls. On the development PC (NVMe, 20 threads), 2 GB + 47 MB:

| | wall | CPU |
|---|---|---|
| copy (`CopyFile`), small files / big file | 0.9 s / 0.8 s | 0.3 s / 0.1 s |
| sync up: hash, encrypt, write | 20.8 s | 9.8 s |
| sync down: read, decrypt, check, write | 18.2 s | 9.8 s |
| a pass with nothing to do | 0.4 s | 0.3 s |
| XChaCha20-Poly1305 alone, 2 GB in memory | 6.1 s | 3.7 s |

The copy is a floor nothing reaches (the big file's copy comes from the cache). Encryption
is about 40 % of the CPU time of a pass; the rest is the BLAKE2b fingerprint (a file is
read once to hash it and once more to encrypt it), the journal, and the per-file flushes
that make a store write durable (a write isn't recorded as synced before it's on disk). Small files
cost about 1 ms each. The Folder store first wrote with 4 threads: on this PC concurrent
small writes from one process were 7–20× slower than one thread (upload 69 s, download
43 s), so it transfers one file at a time now (S3 keeps 4: network round trips dominate
there).

## Ground rules from the codebase

- **No third-party libraries, with one exception: [Monocypher](https://monocypher.org)**
  for the cryptography (two C files, CC0 / BSD-2, audited by Cure53), vendored in
  `third_party/monocypher/` at a pinned release with its checksum and licence; the
  README's "no third-party libraries" line says so. Everything else is built on what
  Windows ships: WinHTTP (HTTPS, proxies), BCrypt (system random, SHA-256 / HMAC for S3's
  SigV4), Credential Manager (`CredWriteW`, DPAPI-protected), ReadDirectoryChangesW,
  IFileOperation (Recycle Bin), and later the Cloud Files API (`cldapi.dll`).
  git is the other external piece: the git backends drive **Git for Windows' `git.exe`**
  (2.31 or later) as a child process; it's a tool the user installs, not something we link.
- **Single self-contained exe, nothing installed.** The sync agent is a mode of the same
  exe (`explorer-plus.exe --sync`), like `--play` and `--notepad`.
- **Pure logic is unit-tested** (`tests/unit_tests.cpp`, ctest): the crypto wrappers and
  file format, the reconciliation planner, paths, ignore rules, SigV4 signing, git output
  parsing, state journal and IPC codec are pure files added to `explorer-plus-tests`.
- **Background work** goes through `worker_submit` / `post_async` in the UI process; the
  agent has its own threads.
- **Tests are isolated by `EXPLORERPLUS_SETTINGS`**: sync configuration, state, the agent's
  pipe name **and the Credential Manager entries** (keys, PATs, S3 secrets) all carry the
  settings folder (or a hash of it), so a test never reads or overwrites the user's real
  keys or talks to their real agent. Autostart (HKCU Run) is the one real-registry write;
  tests never switch it on.
- Flat `src/` layout, `sync_*` / `crypto_*` prefixes for the new files.

## Architecture

```
 Explorer Plus window(s)                      explorer-plus.exe --sync  (agent, one per user)
 ─────────────────────────                    ──────────────────────────────────────────────
 Sync folders dialog  ─┐                      tray icon (status, pause, open, errors)
   (pairs, devices,    │                      KeyManager: user key ring, device key, unlock,
    recovery key)      │   named pipe           pairing, rotation (Credential Manager)
 nav pane sync nodes   ├──────────────────►   SyncAgent ── one SyncEngine per pair
 Status column/badges  │  (sync_ipc codec)        │
 status bar segment    │                          ▼
 context menu items   ─┘                      SyncEngine (backend-agnostic, sees plaintext only)
                                               ├─ LocalScanner   full scan + RDCW recursive watch
                                               ├─ BaseStore      last-synced state, crash-safe journal
                                               ├─ Planner        3-way reconcile (pure, unit-tested)
                                               ├─ Executor       transfer jobs, retries, temp+rename
                                               └─ Backend*
                                                    └─ EncryptingBackend   ◄── always; the engine can't get around it
                                                         └─ RetryingBackend / ThrottledBackend
                                                              ├─ FolderBackend   local / UNC / NAS
                                                              ├─ S3Backend       WinHTTP + SigV4
                                                              ├─ GitBackend      git.exe plumbing, bare mirror
                                                              │    └─ AzureDevOpsBackend   PAT auth, server rules
                                                              └─ FakeBackend     in-memory, fault injection (tests)
```

Why a separate agent process: sync must keep running when every Explorer Plus window is
closed and start at sign-in, which the current "process lives while a window does" model
(`app_window_closed`) can't give. The agent is single-instance (named mutex), starts at
sign-in when the user ticks "Start sync when I sign in", and is also started by Explorer
Plus on launch when pairs exist and it isn't running. Keys live in the agent only; the
Explorer Plus window never holds them.

## The backend abstraction (`sync_backend.h`)

The engine owns all sync decisions; a backend is a dumb, honest object store that reports
versions and refuses writes made against a stale version. Everything backend-specific is
behind `Caps`, so the engine adapts instead of special-casing backends. Backends store
opaque bytes under opaque names: encryption is a layer above them, so a new backend
needs no crypto code at all.

```cpp
namespace sync {

// '/'-separated, relative to the pair's root, NFC-normalized, case preserved
using RelPath = std::wstring;

enum class HashKind : uint8_t { None, Blake2b, GitBlobSha1, Md5, Fingerprint /* keyed, see encryption */ };
struct ContentHash { HashKind kind = HashKind::None; std::array<uint8_t, 32> bytes{}; };

struct RemoteEntry {
    RelPath     path;
    bool        isDir = false;
    uint64_t    size = 0;
    FILETIME    mtime{};        // known after download (it travels encrypted); never compared across machines
    std::string rev;            // opaque version (S3 ETag/VersionId, git blob id): changes iff the stored bytes do
    ContentHash hash;           // when known without a download
};

struct ChangeBatch {
    std::vector<RemoteEntry> upserts;
    std::vector<RelPath>     removals;
    bool        full = false;   // upserts is the complete tree: everything else is gone
    std::string cursor;         // handed back on the next call
};

struct Caps {
    bool changeFeed;        // changes(cursor) is cheaper than a full listing (git diff-tree, Graph delta)
    bool conditionalWrite;  // upload/remove honour expectedRev atomically (S3 If-Match, WebDAV If-Match)
    bool transactional;     // changes become visible together at commit() (git)
    bool serverMove;        // move without re-upload
    bool rangeReads;        // read a byte range (fingerprint peeks; on-demand files, phase 7)
    bool versions;          // older versions can be listed and restored
    bool caseSensitive;
    bool emptyDirs;         // a folder can exist on its own (S3 "dir/" markers yes, git no)
    int  parallelism;       // concurrent transfers it takes (git: 1 writer)
    int  maxNameLength;     // per path component, in stored characters
    int  maxPathLength;     // whole stored path (S3: 1024 bytes of UTF-8)
    uint64_t maxFileSize;
    uint64_t maxCommitBytes;   // transactional: split a pass into several commits above this
    HashKind hash;
    int  pollSeconds;       // how often to ask for changes when nothing pushes them
};

enum class Status { Ok, NotFound, PreconditionFailed /* remote changed under us */,
                    CommitRaced /* transactional: remote moved on, re-plan */,
                    AuthFailed, Offline, Throttled, TooLarge, InvalidName, Unsupported,
                    Rejected /* the server refused by its own rules (branch policy, quota) */,
                    Locked /* no key for this vault yet */, Tampered /* authentication failed */,
                    Cancelled, Fatal };
struct Result { Status st = Status::Ok; std::wstring message; int retryAfterMs = 0; };

struct Progress {                       // transfers report into this; the engine cancels through it
    virtual void bytes(uint64_t done, uint64_t total) = 0;
    virtual bool cancelled() = 0;
};
struct ByteSource { virtual uint64_t size() = 0; virtual bool read(void* buf, size_t n, size_t* got) = 0; virtual bool rewind() = 0; };
struct ByteSink   { virtual bool write(const void* buf, size_t n) = 0; };

class Backend {
public:
    virtual ~Backend() = default;
    virtual Caps   caps() const = 0;
    virtual Result connect(Progress&) = 0;     // credentials valid, bucket / repo reachable
    // "" cursor: the full tree (full = true)
    virtual Result changes(const std::string& cursor, ChangeBatch& out, Progress&) = 0;
    // a cheap "did anything change since this token?" (one small request); default: always yes
    virtual Result changed_since(const std::string& token, bool& changed, std::string& newToken, Progress&);
    virtual Result download(const RemoteEntry& e, ByteSink& out, Progress&) = 0;
    virtual Result download_range(const RemoteEntry& e, uint64_t off, uint64_t len, ByteSink& out, Progress&);  // rangeReads
    // the content hash without downloading the file, when hash wasn't in the listing
    // (default Unsupported: the engine assumes "different" and keeps both, which is safe)
    virtual Result peek_hash(const RemoteEntry& e, ContentHash& out, Progress&);
    // expectedRev: "" = must not exist yet, else the rev the engine last saw
    virtual Result upload(const RelPath& p, ByteSource& in, const FILETIME& mtime,
                          const std::string& expectedRev, RemoteEntry& out, Progress&) = 0;
    virtual Result remove(const RelPath& p, const std::string& expectedRev, Progress&) = 0;
    virtual Result move(const RelPath& from, const RelPath& to, const std::string& expectedRev,
                        RemoteEntry& out, Progress&);           // default Unsupported: engine uploads + removes
    virtual Result make_dir(const RelPath& p, Progress&);       // emptyDirs only
    // transactional backends gather everything between begin and commit; the rest: no-ops
    virtual Result begin(Progress&)                               { return {}; }
    virtual Result commit(const std::wstring& summary, Progress&) { return {}; }
    virtual void   abort()                                         {}
    virtual Result list_versions(const RelPath& p, std::vector<RemoteVersion>& out, Progress&);   // versions
};

// Registry of backend types: the Sync folders dialog builds its form from `fields`,
// so a new backend needs no UI code.
struct ConfigField {
    const wchar_t* key;          // persisted name ("bucket")
    const wchar_t* label;        // "Bucket"
    enum Kind { Text, Secret /* Credential Manager, never in the ini */, Bool, Choice, Url, Folder, Number } kind;
    bool required;
    const wchar_t* def;
    std::vector<std::wstring> choices;
    const wchar_t* help;
};
using BackendConfig = std::vector<std::pair<std::wstring, std::wstring>>;   // secrets resolved on create
struct BackendType {
    const wchar_t* id;           // "s3", "git", "azdo", "folder": persisted, never renamed
    const wchar_t* name;         // "Amazon S3 or compatible"
    std::vector<ConfigField> fields;
    std::wstring (*validate)(const BackendConfig&);                    // empty: fine
    std::unique_ptr<Backend> (*create)(const BackendConfig&);          // the raw store
};
void register_backend(const BackendType& t);       // each backend file registers itself at startup
const BackendType* find_backend(const std::wstring& id);
std::vector<const BackendType*> backend_types();

// The only way the agent gets a backend for a pair: the raw store wrapped in retry /
// throttle and then EncryptingBackend with the pair's vault. Nothing else calls
// BackendType::create (it's private to sync_backend.cpp; tests that exercise raw stores
// use a test-only accessor).
std::unique_ptr<Backend> open_pair_backend(const Pair& pair, KeyManager& keys, Result& why);
}
```

**Decorators** wrap any `Backend` and keep cross-cutting behaviour out of both the engine
and the backends: `EncryptingBackend` (below), `RetryingBackend` (backoff on Offline /
Throttled / 5xx honouring `retryAfterMs`; sources rewind, so a retried upload re-encrypts
the same bytes), `ThrottledBackend` (bandwidth limits). Order: encryption outermost, so
retries and throttling see ciphertext and the engine sees plaintext.

How the planned backends map onto it:

| | rev | change feed | cheap "changed?" | conditional write | transactional | move | versions |
|---|---|---|---|---|---|---|---|
| Folder (local / UNC) | size+mtime+fileId | no (RDCW when local) | head marker | best effort | no | yes | no |
| S3 | ETag (+VersionId) | no: ListObjectsV2 | head marker (`If-None-Match` GET) | If-Match / If-None-Match | no | copy + delete | if bucket versioning |
| git, Azure DevOps Git | blob id | `diff-tree` between commits | `ls-remote` of the branch | via commit (push rejects) | **yes** | yes (same blob) | `git log` |
| WebDAV / Nextcloud (later) | ETag | Nextcloud: sync-token | head marker | If-Match | no | MOVE | Nextcloud: yes |
| OneDrive / Graph (later) | eTag / cTag | delta link | delta link | If-Match | no | yes | yes |
| Google Drive (later) | md5 + version | changes.list pageToken | startPageToken | no (HEAD check) | no | yes | yes |
| Dropbox (later) | rev | list_folder/continue | longpoll | update mode rev | no | yes | yes |

## End-to-end encryption

### Threat model

Protected against anyone with read or write access to the storage (the provider, a leaked
S3 key or PAT, a git server's admins, a stolen NAS disk):

- they can't read file contents, file or folder names, or modification times;
- they can't change a file, truncate it, reorder its chunks or splice two files without the
  download failing as `Tampered` (which never overwrites the local file);
- they can't plant a device or a key: device records and key wraps are authenticated with
  the user's key, and the codes that unlock new devices never touch the storage.

Not protected (documented in the README, shown once at setup):

- **metadata shape**: how many files and folders there are, the folder nesting, each
  file's size rounded up to 64 KiB, and when and how often things change;
- the storage **withholding, deleting or rolling back** individual files to an older
  version it kept (it can't forge new ones). Rollback of a file we've already seen a newer
  version of is detected and reported (the base remembers each file's version counter);
- a **compromised device**: whoever controls a PC that has the keys can read everything.
  Removing the device (below) locks it out of everything written afterwards, and only that;
- **losing every device and the recovery key** means the data is gone for good. Nobody can
  recover it, by design; setup says so in plain words.

### Primitives (all Monocypher except the random source)

| need | Monocypher function | notes |
|---|---|---|
| random | — | `BCryptGenRandom` (system RNG) |
| content and key wraps | XChaCha20-Poly1305 (`crypto_aead_lock` / `unlock`) | 24-byte nonces: random ones are safe |
| hashing, MACs, key derivation | BLAKE2b (`crypto_blake2b_keyed`) | KDF as in libsodium's `crypto_kdf`: keyed BLAKE2b over a context string |
| deterministic name encryption | BLAKE2b + XChaCha20 (`crypto_chacha20_x`) | SIV construction, below |
| short codes → keys | Argon2id (`crypto_argon2`) | 256 MiB, 3 passes (~1 s); parameters stored with each wrap |
| device key exchange | X25519 (`crypto_x25519`) | rotation hand-over |
| constant-time compare, wiping | `crypto_verify32`, `crypto_wipe` | |

`crypto.cpp` is a thin C++ wrapper (spans, `std::array` keys that wipe themselves) and is
the only file that includes `monocypher.h`. The agent runs a known-answer test of each
primitive at start (Monocypher's published vectors) and refuses to sync if one fails.
Speed: Monocypher is portable C, roughly 0.5 GB/s per core for XChaCha20-Poly1305 and
BLAKE2b on current x64. That's above typical upload and download rates, and files are
encrypted in parallel on the transfer threads, so the network and disk stay the limit.
`monocypher.c` and `crypto*.cpp` build with `-O2` like the other number-crunching files.

### Keys

```
User key ring (one per user, the same on all of the user's devices: the portable key)
 │  UMK_g : 256-bit random user master key, generation g (rotated when a device is removed);
 │          each generation keeps the previous one wrapped under it, so older vaults still open
 │
 ├─ wraps → VK_v  : 256-bit random vault key, per remote location ("vault" = one bucket
 │                  prefix, one repo branch, one NAS folder), versioned v1, v2, ...,
 │                  stored on the remote itself, wrapped under UMK_g
 │     KDF(VK_v, context) →
 │        K_name : name encryption (SIV: a MAC key and a cipher key)
 │        K_fp   : content fingerprints
 │        K_head : the vault's change marker
 │        per file: K_file = KDF(VK_v, the file's random 32-byte salt)
 │
 └─ KDF(UMK_g, "device-records") → K_dev : authenticates device records

Device key:   per device X25519 key pair; the public half is registered in each vault.
              Used to hand a new UMK generation to the remaining devices (rotation).
Recovery key: 15 random Crockford Base32 characters (75 bits), `7KQM2-XRPA9-HT4WD`.
              Argon2id(recovery key) wraps UMK_g; the wrap is kept in every vault.
Pairing code: the same shape, new for each pairing, valid 15 minutes.
```

**Why 15 characters are enough.** The codes aren't the key, they unlock a wrap of it.
Each guess costs one Argon2id run of 256 MiB of memory and about a second, and the codes
are random, not chosen by the user. Guessing half of 2^75 at a million guesses a second
(thousands of GPUs, memory-bound) takes about 600 million years, so a passphrase on top
adds nothing. Crockford Base32 has no I, L, O or U, and reading ignores case, spaces and
dashes and treats O as 0 and I / L as 1, so a written-down code reads back reliably. A typo
just fails to unlock and costs a second.

One user key unlocks every vault of that user, so a second PC is unlocked once, not once
per sync folder. Separate vault keys per remote keep a leak contained (a vault key read
from one PC's memory opens that vault, not the others). They also allow sharing one vault
with another person later by wrapping its key for them.

On the device, the key ring and the device's private key are kept in Credential Manager
(`CRED_PERSIST_LOCAL_MACHINE`, DPAPI-protected under the Windows account; never
`ENTERPRISE`, which would roam), with names that include the settings folder hash (test
isolation). Keys in memory sit in `VirtualLock`ed pages, are wiped with `crypto_wipe`, and
are left out of crash minidumps through `MiniDumpWriteDump`'s callback (`crash.cpp`). No
secret goes into logs, perf events or IPC (except the explicit "show recovery key").
Later option: wrap the ring with a TPM-bound key (NCrypt Platform Crypto Provider) or gate
it behind Windows Hello.

### What's stored on the remote

A reserved top-level folder holds the vault's key material; the encrypting layer hides it
from the engine, and encrypted names never start with `.` so nothing collides with it:

- `.ep-vault/vault`: format version, vault ID, `umkId` (a hash that tells which UMK
  generation it needs), the vault key versions each wrapped under that UMK, the current
  version, and the recovery wrap (Argon2id salt and parameters, wrapped UMK). Text in the
  INI format `ini.cpp` already reads, binary fields Base64.
- `.ep-vault/ring`: the older UMK generations, each wrapped under the next one. *(As built:
  not needed. The ring itself carries every generation, in the recovery wrap, pairing
  grants and the rotation hand-overs `.ep-vault/rotate/<device>-g<gen>`.)*
- `.ep-vault/devices/<deviceId>`: the device's public key, its name and when it was added,
  encrypted under a UMK-derived key and MACed with `K_dev`. The storage can't add a device
  (it can't MAC one) or learn device names.
- `.ep-vault/pair/<id>`: a short-lived pairing grant (below), deleted once used or after
  15 minutes.
- `.ep-vault/head`: the change marker, a small encrypted counter every device bumps after a
  pass that changed something. Other devices poll only this object, with a conditional GET
  (`If-None-Match` its ETag → `304 Not Modified`): one tiny request per poll, and the full
  listing only when it moved. (git backends don't need it: `ls-remote` does the same.)

### File format (`crypto_file.cpp`, pure, version 1)

Each file is stored as a header, an encrypted metadata block and encrypted 64 KiB chunks:

```
header (48 bytes, plaintext, authenticated as additional data of every block below)
  "EPSF"  magic                     4
  1       format version            1
  16      chunk size log2           1
  0       reserved                  2
  v       vault key version         4   (which VK_v the file key comes from)
  salt    random per upload         32  (a fresh salt each upload: re-uploading the same
                                         content gives different bytes, no equality leak)
  ...                               4   reserved, zero
meta block (XChaCha20-Poly1305 under KDF(K_file, "meta"), nonce 0; 64 + 16 tag bytes)
  plaintext size 8 | mtime 8 | attributes 4 | version counter 8 | fingerprint 32 | pad
chunks (XChaCha20-Poly1305 under KDF(K_file, "data"), STREAM construction as in age / Tink)
  nonce = chunk index (8 bytes, little-endian) || last-chunk flag (1 byte) || zeros
  every chunk is 64 KiB of plaintext + 16 tag bytes; the last one may be shorter (an empty
  file is one empty last chunk), so truncating or appending is detected
```

- Each upload's file key is new (fresh salt), so counter nonces can't repeat under a key.
- **Fingerprint** = keyed BLAKE2b(K_fp, BLAKE2b hash of the plaintext). The engine already
  hashes local files with BLAKE2b; the keyed form lets it compare a local file with a remote
  one ("both sides changed, but to the same thing?") without a download, while the storage
  can't test the file against content it guesses. `EncryptingBackend::peek_hash` reads it
  with one range read of the first 128 bytes (S3, Folder) or from the blob (git has no
  ranges; peeks happen only in the rare cases that need them). *(As built: the plain
  BLAKE2b of the plaintext. It only exists inside the encrypted meta block, so the storage
  can't test guesses against it either.)*
- Overhead: 128 bytes + 16 per 64 KiB (0.02 %). Plaintext sizes follow from ciphertext
  sizes, so listings report them without downloads.
- Chunks make range reads possible (on-demand files later) and keep memory flat for files
  of any size; a download is written to `transfers\` and only replaces the local file
  after the last chunk authenticated.
- The format is frozen by known-answer tests (a fixed key, salt and plaintext must give the
  same bytes forever) and written up in `docs/sync-format.md` so another client (a phone
  app, a recovery tool) can be written from the document alone.

### Names: each path component encrypted in place

Decided over the alternative (random object IDs plus an encrypted index of the whole tree).
Both hide every real name; the in-place encryption is the simpler and, for syncing, the
faster one:

| | names encrypted in place (chosen) | random IDs + encrypted index |
|---|---|---|
| code | a name function; the store's own listing | an index format, its log and compaction, garbage collection of orphaned objects |
| polling for changes | `head` marker or `ls-remote`: one small request | one small request (the index's ETag) |
| reading the tree | the store's listing (S3: 1,000 names a request) + decrypting names, ~1 µs each | downloading the index: 100,000 files ≈ 10 MB |
| an upload | one object | the object + an index update every device competes for (If-Match retries); parallel uploads queue behind it |
| renaming a big folder | one server-side move per file (S3 copies, git free, no re-upload) | free (index only) |
| git | its tree is the listing, `diff-tree` the change feed | one index blob rewritten on every commit, git's diffs useless |
| damage | a bad object loses that file | a bad or lost index loses the tree |

The only case where the index wins is renaming a folder with very many files, which is
rare; uploads, the common case, would always pay for it.

How a name is encrypted (deterministic, so a plaintext path maps to its stored path without
an index; the SIV construction, built from Monocypher's primitives):

```
tag    = keyed BLAKE2b(K_name.mac, plaintext parent path || 0 || name padded to 16-byte steps)[0..16)
cipher = XChaCha20(K_name.enc, nonce = tag || 8 zero bytes, padded name)
stored = Base32(tag || cipher), lowercase
```

Decryption recomputes the tag over the decrypted name and compares (constant-time), so a
tampered name fails as `Tampered`. The parent path in the tag means the same name in two
folders looks different; padding means name lengths leak only in 16-byte steps; lowercase
Base32 survives case-insensitive stores and every backend's character rules.

- A stored component longer than the backend's `maxNameLength` (a 255-character name
  becomes ~460 characters) is shortened to `<hash>.epn` with the full encrypted name in a
  sidecar object `<hash>.epn.name` (as Cryptomator does); S3 and git rarely need it,
  Folder (NTFS: 255) does.
- Name rules that matter for plaintext (Windows-invalid names, NFC, names differing only in
  case) are checked on the decrypted names, before anything is written locally.

### Unlocking a new device (portability)

When a pair is added, the agent connects and looks for `.ep-vault/vault`:

- **No vault, empty location**: a new vault key is created and wrapped under the user key
  (creating the user key first if this is the user's first vault: first-time setup with the
  recovery key, below).
- **No vault, but files are there**: refused. Those files would be plaintext, and this
  feature never writes or reads plaintext remotely. The user picks an empty location or a
  new prefix / branch / subfolder.
- **A vault this device's key ring opens**: done, nothing to ask.
- **A vault it can't open** (a second PC): "This folder is encrypted by another of your
  devices", with two ways in:
  1. **Code from another device** (the default). On a PC already set up: Sync folders ▸
     Devices ▸ Add a device shows a new code (`K7QM2-2XRPA-9HT4W`) and a 15-minute
     countdown, and writes `.ep-vault/pair/<id>`: the key ring wrapped under
     Argon2id(code). The user types the code on the new PC; it unwraps the ring and deletes
     the grant. The code never touches the storage.
  2. **Recovery key**: opens the recovery wrap in any vault, also when every other device
     is gone.

  Then the new device registers its public key in the vault (a MACed device record) and
  stores the ring locally.

The new device also needs the storage credentials (S3 keys, PAT, git access); those are
typed on each device as for any sync client, and never go into the vault.

### Removing a device, rotating keys

"Remove device" (or "I lost a device") on any unlocked device:

1. creates UMK generation g+1 and a new vault key version in each vault this device
   reaches; new uploads use the new version from then on;
2. writes the new ring generation for every *remaining* device, wrapped to each one's
   public key (X25519, KDF, XChaCha20-Poly1305), and deletes the removed device's record;
3. creates a new recovery key, shows it, and replaces the recovery wraps (the old recovery
   key only opens generation g and older).

Vaults this device doesn't sync are upgraded by the first remaining device that syncs them
(the ring's older generations still open them meanwhile). Files written before the removal
stay readable by the removed device if it can still reach the storage, since it could read
them before. "Re-encrypt everything" (download, encrypt under the new version, upload) is
offered for that case, and the user should also revoke the storage credentials that device
had (S3 key, PAT). Removing a device can't wipe it; that's for Windows' own remote wipe.

*As built* ("Re-encrypt files" in Devices and keys, offered right after Remove device): the
local copies are uploaded again (nothing downloaded) where they still match the last sync,
each with the version + 1 and its last rev as the precondition; files changed meanwhile are
skipped (the next pass sends them under the new key anyway). One commit on git stores; a
commit that loses a race with another device is retried after a pass. Names keep the key
version they were made with. The other devices download every file once more. One that
edited a file before it saw the re-encryption gets no conflict: the stored fingerprint still
matches what it last synced, so its base takes the new rev and its edit goes up. Old
ciphertext stays in git history and in S3 versions: the removed device can read those as
long as it can reach them.

### Effects on the rest of the design

- The engine is unchanged: it sees plaintext paths, sizes and fingerprints. Caps pass
  through the encrypting layer with `hash = Fingerprint`, sizes converted,
  `maxNameLength` handled by shortening, and `serverMove` kept (moves copy ciphertext under
  new names; content isn't bound to its path, so it isn't re-encrypted).
- **git stores every version whole**: ciphertext doesn't delta-compress, so a 20 MB file
  edited 50 times costs 1 GB of repository. The git backends warn when a pair's history
  grows past a limit and suggest S3 for big or busy files. Commit messages and authors are
  neutral by default ("sync", `Explorer Plus <sync@localhost>`) so they leak no device
  names; timestamps still show when syncs happened.
- **No share links**: a presigned S3 URL would hand out ciphertext. Sharing comes back later
  as "share with a person" (wrap a vault key for their public key); not in this plan.
- S3 object metadata stays empty (no plaintext hashes or times in `x-amz-meta-*`).
  Server-side encryption can still be switched on in the bucket as an extra layer.
- An **independent review** of `crypto*.cpp`, the file format and the key protocol (the
  parts built on Monocypher, which is itself audited) is a release gate for the UI phase.

## The sync engine

### State (`sync_state.cpp`)

Per pair, beside the settings file (as `Player\` and `Notepad\` are, so it follows
`EXPLORERPLUS_SETTINGS`): `Sync\pairs.ini` (the pairs: id, local root, backend id, fields,
vault ID, paused, excluded subfolders) and `Sync\<pairId>\` holding:

- `base.dat` + `base.log`: the **base**, the state both sides agreed on at the last sync, per
  path `{isDir, size, mtime, fileId, localHash, remoteRev, fingerprint, versionCounter}`,
  plus the remote cursor and change token. In memory as a hash map; on disk a snapshot plus
  an append-only log of changes (each record checksummed, a torn tail is dropped),
  compacted into a new snapshot written to a temp file and swapped in with `ReplaceFileW`.
  No SQLite needed for a few hundred thousand files. Local only, so it holds plaintext
  paths.
- `transfers\`: partial downloads and resumable upload state (S3 multipart upload id,
  finished parts, and the file salt so a resumed upload keeps encrypting the same bytes).
- Backend secrets in Credential Manager as `ExplorerPlus:<settings hash>:sync:<pairId>:<field>`.

### Local side (`sync_local.cpp`)

- Full scan at start (FindFirstFileExW with `FIND_FIRST_EX_LARGE_FETCH`, `\\?\` long paths)
  and a recursive ReadDirectoryChangesW watch on the root; buffer overflow or a resumed
  machine means a full rescan. Hashing (BLAKE2b) only for files whose size / mtime / fileId
  differ from the base.
- Settle delay: a file still being written (changed in the last ~2 s, or can't be opened
  with share-read) waits.
- Local renames detected by file ID (same `FILE_ID_INFO`, new path).
- Ignored: `desktop.ini`, `Thumbs.db`, `~$*`, `*.tmp`, `.~sync*`, symlinks / junctions
  (not followed), and patterns in an optional `.syncignore` (gitignore-style subset) at the
  root; plus folders the user excluded (selective sync).

### Planner (`sync_plan.cpp`, pure)

`plan(base, localDelta, remoteDelta, caps, options) -> std::vector<Action>`; per path, with
L / R = local / remote compared to the base:

| local | remote | action |
|---|---|---|
| unchanged | unchanged | none |
| changed | unchanged | upload (expectedRev = base rev) |
| unchanged | changed | download |
| changed | changed | same fingerprint: update base only; else **conflict** |
| deleted | unchanged | remove remote (expectedRev) |
| unchanged | deleted | local file to Recycle Bin |
| deleted | changed | download (an edit beats a delete) |
| changed | deleted | upload (an edit beats a delete) |
| deleted | deleted | forget |
| new | new | same fingerprint: base only; else conflict |
| file | folder (or reverse) | conflict: rename the local one |
| renamed (fileId) | unchanged | `move` if `serverMove`, else upload + remove |
| unchanged | renamed (same rev removed + added in one batch) | local `MoveFileExW` |

**Conflict** = keep both: the remote version keeps the name, the local one becomes
`Report (conflicted copy, PC-NAME, 2026-10-17 14-03).docx` and is uploaded as a new file;
the conflict is listed in the UI until dismissed.

Remote names Windows can't hold (`CON`, `a:b`, trailing dot / space, `*?"<>|`), and paths
that differ only in case, are reported and skipped (never renamed on the remote behind the
user's back). Decrypted names are NFC-normalized (macOS clients write NFD).

### Safety rails

- **Root gone is not "everything deleted"**: the root folder carries an ID marker (volume
  serial + root file ID kept in the state); missing root, other volume, or an unplugged
  drive pauses the pair.
- **Mass-delete guard**: a pass that would delete more than 50 files or 25 % of the pair
  on either side pauses and asks ("Restore them" / "Delete them too").
- **First sync of two non-empty sides** merges: no deletes at all, conflicts become copies.
- **Tampered or undecryptable remote files** are never applied: the pair shows the error,
  the local file stays, and the file isn't uploaded over the remote one either until the
  user chooses (the storage may be under attack, or another client is newer than this one).
- **Locked vault** (key rotated by another device, ring not yet received): the pair waits
  as "Locked: unlock with a code from another device" instead of failing per file.
- Downloads go to `transfers\` and replace the target with `ReplaceFileW` / `MoveFileExW`
  only after every chunk authenticated; local deletes go to the Recycle Bin
  (IFileOperation, `FOFX_RECYCLEONDELETE`), never a hard delete.
- Uploads read through a handle that denies writes; a file changed during upload is
  uploaded again later, never committed half-old.
- Times across machines are never compared: decisions go by rev / fingerprint / base.
- Metered connections (`INetworkCostManager`) and battery saver pause transfers (setting).

### Executor (`sync_engine.cpp`)

One pass: local delta + (`changed_since(token)` → `changes(cursor)` only when it moved) →
`plan` → safety checks → `begin()` → transfer jobs (up to `caps.parallelism`, smallest
first so many small files show progress; encryption runs on the transfer threads) →
`commit(summary)` (several commits when a transactional pass exceeds `maxCommitBytes`) →
base, cursor and token written to the journal → `head` bumped when something changed.
`PreconditionFailed` / `CommitRaced` → fetch changes again and re-plan (bounded, then back
off). Passes are triggered by local changes (debounced 2 s), by `pollSeconds`, by "Sync
now", and on network reconnect. Engine time and randomness are injected, so the unit tests
run it against `FakeBackend` deterministically.

## The backends

### Folder (`sync_folder.cpp`)

A local or UNC folder as the store (a NAS share, a USB disk, a second test root). Smallest
possible backend, exercises the whole engine and the encryption layer, needs no network or
accounts. Encrypted like every other store: a NAS or a USB disk is external storage too.

### S3 (`sync_s3.cpp`, `http.cpp`, `sigv4.cpp`)

- `http.cpp`: a WinHTTP wrapper (sessions per host, proxy from the user's settings,
  streaming bodies through `ByteSource` / `ByteSink`, timeouts). Reusable by every later
  HTTP backend.
- `sigv4.cpp` (pure): AWS Signature Version 4 with BCrypt HMAC-SHA256 (S3 requires
  SHA-256; Monocypher has none); unit-tested against AWS's published SigV4 test vectors.
- Fields: endpoint (default `https://s3.<region>.amazonaws.com`), region, bucket, prefix,
  addressing (virtual-host / path-style for MinIO and friends), access key ID, secret
  access key (Secret), storage class.
- Changes: `head` marker polled every 30 s (one conditional GET); ListObjectsV2 with paging
  only when it moved, plus a full listing every few hours in case a client without the
  marker wrote. Folders: `dir/` zero-byte markers for empty folders.
- Upload: PutObject up to 64 MB, multipart above (16 MB parts, resumable: the upload id and
  finished parts are kept in `transfers\`; stale multipart uploads of the prefix aborted).
  Conditional: `If-None-Match: *` for new objects, `If-Match: <etag>` for updates (AWS has
  both); `connect()` probes the server, and where they're missing `conditionalWrite` is
  false and the engine checks with HEAD first (a small race window, documented). Vault
  files are only ever written conditionally; a server without conditional writes gets a
  warning that two devices rotating keys at the same moment could lose one rotation
  (repaired by the next one).
- Move: CopyObject + DeleteObject (≤ 5 GB; above that, multipart copy).
- Range reads: `Range:` GET (fingerprint peeks).
- Versions (bucket versioning on): ListObjectVersions → "Version history...".
- `RequestTimeTooSkewed`: take the server's `Date` and re-sign.

### git (`sync_git.cpp`, `proc.cpp`)

The sync folder is **not** a git work tree (no `.git` inside the user's folder; git couldn't
merge ciphertext anyway). The backend keeps a bare mirror in `Sync\<pairId>\repo.git` and
uses git as an object store with atomic commits, through plumbing:

- Fields: remote URL, branch, subfolder in the repo (optional), git.exe path (default:
  PATH, then `%ProgramFiles%\Git\cmd\git.exe`), commit author (default neutral), max file
  size (default 50 MB: GitHub refuses 100 MB), history size warning (default 2 GB).
- Authentication is git's: Git for Windows' Credential Manager or the user's SSH keys
  (`GIT_TERMINAL_PROMPT=0` so nothing waits on a console that isn't there).
- `changed_since(token)`: `git ls-remote origin refs/heads/<branch>` (token = the tip's
  commit id): one small request, no fetch when nothing moved.
- `changes(cursor)`: `git fetch`, then `diff-tree -r -z --raw <cursor> <tip>` (cursor =
  commit id; `ls-tree -r -z --long <tip>` for a full listing). `changeFeed = true`.
- `download`: one long-running `git cat-file --batch` per pair.
- `begin` / `upload` / `remove` / `move` / `commit`: a private index (`GIT_INDEX_FILE`)
  read from the base commit; `hash-object -w --stdin` for each upload;
  `update-index --index-info` for the batch; `write-tree`; `commit-tree -p <base>`;
  `push origin <commit>:refs/heads/<branch>`. A rejected non-fast-forward push (another
  device pushed) → `CommitRaced` → the engine fetches and re-plans. A pass bigger than
  `maxCommitBytes` (default 1 GB) goes up as several commits, each one consistent.
  `transactional = true`, `parallelism = 1`, `emptyDirs = false`, `serverMove = true`.
- Versions: `git log --format=... -- <stored path>`; restoring is a download of that blob.
- `proc.cpp`: CreateProcessW with pipes and no console window, stdout / stdin streaming,
  environment passed per call, kill on cancel; parsing of `-z` outputs is pure and
  unit-tested.
- No LFS in the first version; files over the limit are reported and skipped.

### Azure DevOps Git (`sync_azdo.cpp`)

Its own entry in the registry ("Azure DevOps Git"), built on the git backend's plumbing.
It adds personal access token sign-in and Azure DevOps' own rules, so the user pastes a
clone URL and a PAT and nothing else. Works with Azure DevOps Services
(`https://dev.azure.com/{org}/{project}/_git/{repo}`) and Azure DevOps Server on premises
(`https://{server}/{collection path}/{project}/_git/{repo}`), e.g. the Škoda sandbox
repository `https://devops.skoda.vwgroup.com/projects/SkodaAuto/sandbox/_git/explorerplus`
used for the integration test.

- Fields: clone URL (validated: has `/_git/`, HTTPS), PAT (Secret: Credential Manager),
  branch (default `explorerplus-sync`, created by the first push), subfolder, the git
  fields' limits.
- **PAT handling**: read from Credential Manager for each git call and handed to git in the
  child's environment only, as an HTTP header scoped to that URL:
  `GIT_CONFIG_COUNT` / `GIT_CONFIG_KEY_n` / `GIT_CONFIG_VALUE_n` setting
  `http.<url>.extraHeader=Authorization: Basic <base64(":" + PAT)>` and
  `credential.helper=` (empty, so Git Credential Manager neither prompts nor stores
  anything), next to the TLS and proxy settings below. Never on the command line (other processes can read command lines), never in
  the mirror's config, never in logs (the logged command lines and git's stderr pass
  through a filter that blanks the header and the token). Needs Git 2.31+ for
  `GIT_CONFIG_COUNT`; `connect()` checks `git --version`.
- **Recommended PAT**: scope *Code (Read & write)* only, for the one organization or
  collection, with an expiry. Azure DevOps PATs expire, so an `AuthFailed` from the server
  says "the personal access token was refused (expired or revoked)", pauses the pair and
  asks for a new token in the tray and the dialog. Nothing else is retried with a bad
  token (repeated failures can get the account throttled).
- **Branch policies**: a push to a branch with policies (required reviewers, builds) is
  refused with `TF402455`; the backend maps it to `Rejected` with "this branch requires
  pull requests: choose a branch without policies (the default `explorerplus-sync`)".
  Other `TF…` refusals (repository size or push size limits, a disabled repository,
  missing permissions) map to `Rejected` with the server's text, not to retries.
- **Corporate network**: git runs with `http.sslBackend=schannel`, so the company's root
  certificates in the Windows store are trusted as in the browser; a proxy set in Windows
  (WinHTTP / IE settings, PAC resolved by `WinHttpGetProxyForUrl`) is passed as
  `http.<url>.proxy`. On-premises servers that sign in with Windows accounts also take
  PATs over Basic; Kerberos / NTLM sign-in isn't used.
- Polling: `ls-remote` every 60 s (one small request); Azure DevOps service hooks could
  push changes, but they need a public endpoint, so no.
- Later option: Azure DevOps' REST API (pushes with `oldObjectId`, items, diffs) would drop
  the git.exe requirement for this backend; it needs JSON with Base64 bodies and has its
  own push size limits, so not in this plan.

## UI (Explorer Plus)

- **"…" ▸ Sync folders...** (`IDM_SYNC_FOLDERS`, next to Media file types in `cmdbar.cpp`):
  a window in the style of `assoc_dialog` with two pages:
  - **Folders**: the pairs (status, last sync, errors) with Add / Edit / Remove / Pause /
    Sync now. Add: pick the local folder, a backend type, a form generated from its
    `ConfigField`s, **Test connection** (through the agent), then the vault step (create,
    or unlock with a code or the recovery key), "Start sync when I sign in".
  - **Devices and keys**: this device and the others (name, added), **Add a device** (shows
    the code and a countdown), **Remove device**, **Recovery key** (shows it after a Windows
    sign-in prompt, `CredUIPromptForWindowsCredentials`; or makes a new one).
- **First-time setup** (the first vault ever): explains in a few lines what end-to-end
  means here (nobody, including us, can recover the files without your key), creates the
  user key, shows the **recovery key** (copy, print, save as a text file) and asks for it
  back once (all 15 characters) before going on.
- **Navigation pane**: each sync root as a node with a cloud icon, beside OneDrive
  (`tree.cpp`), its badge showing the pair's state (a lock when the vault is locked).
- **File list**: a "Sync status" column and a small badge drawn in the list's custom draw
  (`listview.cpp`, `CDDS_ITEMPREPAINT`) for items inside a sync root: up to date, syncing,
  waiting, conflict, error (tampered), excluded. The states come from the agent per shown
  folder and are applied like the overlay pass (`overlay_apply` in `enumerate.cpp`): keyed
  by `Item::key`, after the listing, without blocking it.
- **Status bar**: "Up to date" / "Syncing 3 files (12 MB left)" / "Paused" / "2 conflicts"
  / "Locked" / "Sign-in needed" (an expired PAT).
- **Context menu** (compact menu, items inside a sync root): Sync now, Version history...,
  Exclude from sync / Include, Show conflict.
- **Tray icon** (agent): state icon, pause / resume (1 h / until tomorrow / indefinitely),
  open a sync folder, recent activity, errors and conflicts, Quit.

### IPC (`sync_ipc.cpp`)

A named pipe owned by the agent, `\\.\pipe\ExplorerPlus.Sync.<user SID>.<settings hash>`
(so a test agent and the user's agent never meet), DACL for the current user only.
Length-prefixed UTF-8 messages of `key=value` lines; the codec is pure and unit-tested.
Requests: `status <folder>` (child states), `subscribe`, `pairs`, `pause`, `resume`,
`sync-now`, `reload-config`, `test-connection`, `set-secret` (a new PAT or S3 key, written
to Credential Manager by the agent), `conflicts`, `versions <path>`, `restore <path> <rev>`,
and for keys `keys-status`, `setup-keys`, `unlock {code | recovery}`, `pair-offer` (returns
a code), `devices`, `remove-device`, `recovery-key` (only after the window's Windows sign-in
prompt succeeded), `new-recovery-key`. The agent pushes `changed <folder>` to subscribers
so badges update; Explorer Plus works without an agent (no badges, the dialog offers to
start it).

*As built*: one request and one reply per connection; the requests are `ping`, `quit`,
`reload`, `pairs`, `status folder=`, `sync-now [wait=1]`, `pause`, `resume`,
`confirm-deletes yes=`, `dismiss-conflicts`, `conflicts`, `test-connection`, `set-secret`,
`keys-status`, `setup-keys`, `new-recovery-key`, `unlock mode=code|recovery`, `pair-offer`,
`devices`, `remove-device`, `versions path=`, `restore path= rev=`, `reencrypt pair=`,
`exclude path= on=1|0`, and `watch [folder=] since= wait=` instead of `subscribe`: a long
poll (up to 25 s) answered as soon as the folder's states (or, without a folder, the
pairs') differ from the reply whose `hash` the caller passes; each pane showing a sync
folder keeps one open. `explorer-plus.exe --sync-cli <request> [key=value ...]` sends one and
prints the reply (tests and scripts); `add-pair`, `remove-pair`, `start-agent`,
`forget-keys` and `pairs-file` run without the agent. A secret given as `cfg.pat=env:NAME`
is read from the environment, so tokens stay off command lines.

## Phases

Each phase ends buildable and tested; every green `main` build ships, so **nothing is
user-visible until phase 6** (no menu entry, no agent autostart). Phases 2–5 are exercised
by tests only.

1. **Foundations**: Monocypher vendored (`third_party/monocypher/`, pinned, C added to the
   CMake languages, README note), `crypto.cpp` wrapper + known-answer tests, Crockford
   Base32 codes, `credstore.cpp`, `proc.cpp`, `http.cpp`, sync paths (NFC, Windows-valid
   names, `\\?\`).
2. **Engine**: `sync_backend.h`, registry, `sync_state`, `sync_local`, `sync_plan`,
   `sync_engine`, `FakeBackend` (plaintext, test-only). *Done when* the planner table is
   covered by unit tests and an engine test with injected failures (offline mid-transfer,
   precondition failure, a crash between transfer and journal) converges.
3. **Encryption and keys**: `crypto_file.cpp` (format v1 + known-answer tests),
   `sync_crypt.cpp` (`EncryptingBackend`: names, shortening, content, fingerprints, head
   marker, vault files), `sync_keys.cpp` (key ring, vaults, device records, pairing grants,
   recovery wraps, rotation, Credential Manager), `FolderBackend`, `open_pair_backend`, the
   agent mode `--sync` (headless, pairs from a hand-written `pairs.ini` in tests).
   *Done when*:
   - two simulated devices (two settings folders) syncing through one Folder store stay
     identical through edits, renames, deletes and conflicts (`tools/sync_test.py`);
   - device B unlocks with a pairing code and with the recovery key, typed with dashes,
     spaces, lowercase, O for 0; wrong codes and expired grants fail cleanly;
   - removing device B rotates keys: B can't read files written afterwards, A and C can,
     a vault A doesn't sync is upgraded later by C, and the old recovery key stops working
     for new data;
   - tampering tests: a flipped byte, a truncated file, swapped chunks, a header from
     another file, a renamed stored name, a forged device record, a planted vault on an
     empty location: each is refused as `Tampered` and never written locally;
   - a scan of the store finds no plaintext name, content marker or mtime from the test
     data (the test writes recognisable strings and searches the store's bytes);
   - a benchmark (`tools/perf.py` scenario): 10,000 small files and one 2 GB file through
     the Folder store, CPU time of encryption vs. the copy itself.
4. **S3 backend**: `sigv4.cpp` + tests (AWS vectors), `sync_s3.cpp`. Integration test
   against an S3 endpoint given by environment variables (`EXPLORERPLUS_S3_TEST_ENDPOINT`,
   bucket, keys), skipped when unset; CI can run MinIO.
5. **git and Azure DevOps Git backends**: `sync_git.cpp`, `sync_azdo.cpp`.
   - git: integration tests against a local bare repository (`git init --bare` in the test
     folder, no network; CI has git), including two devices racing pushes, a key rotation
     racing a sync, and a pass split into several commits.
   - Azure DevOps: unit tests of URL validation, `TF…` error mapping and the token filter
     on logs; an integration test against the sandbox repository when
     `EXPLORERPLUS_AZDO_TEST_URL` and `EXPLORERPLUS_AZDO_TEST_PAT` are set (skipped
     otherwise; GitHub's runners can't reach the internal server, so it runs on developer
     machines). Each run uses its own branch `sync-test/<run id>` and deletes it at the
     end; an expired or wrong PAT is checked to pause the pair without retries.
6. **UI**: Sync folders dialog with both pages, first-time setup and recovery key, IPC,
   status column / badges, nav pane nodes, status bar, context menu, tray, autostart,
   README section (with the threat model in plain words). Smoke test additions in
   `tools/smoke_test.py` (a Folder pair in the scratch folder: setup, badges, "Sync now").
   **Gate**: the independent review of the crypto code, format and key protocol is done and
   its findings fixed; format v1 is frozen.
7. **Later, each on its own**:
   - *Files On-Demand* via the Cloud Files API (`CfRegisterSyncRoot`, placeholders,
     hydration through `download_range`: the chunked format decrypts any range). `cldapi.dll`
     loaded at run time; the few declarations in a compat header (like `pkeys_compat.h`).
     Needs a spike: registering the root in Explorer's navigation pane may need package
     identity for an unpackaged exe.
   - Size padding beyond 64 KiB steps (hides sizes better, costs space), and a signed
     per-vault manifest to detect withheld or rolled-back files across the whole tree.
   - TPM / Windows Hello protection of the local key ring.
   - Azure DevOps over its REST API (no git.exe needed).
   - Sharing a vault with another person (their public key), more backends (WebDAV /
     Nextcloud, OneDrive / SharePoint, Google Drive, Dropbox, Azure Blob).

## New files (CMake)

`third_party/monocypher/` (`monocypher.c`, `monocypher.h`, licence), `src/crypto,
crypto_file, credstore, proc, http, sigv4, sync_backend.h, sync_paths, sync_ignore,
sync_state, sync_local, sync_plan, sync_engine, sync_crypt, sync_keys, sync_folder,
sync_s3, sync_git, sync_azdo, sync_ipc, sync_agent, sync_ui` (+ `.h`),
`docs/sync-format.md`, `tests/` additions, `tools/sync_test.py`; `project(... LANGUAGES C
CXX RC)`; libraries `winhttp bcrypt advapi32` (Credential Manager) `credui`
(re-authentication prompt). The pure ones (`crypto*` with Monocypher, `sigv4`,
`sync_paths`, `sync_ignore`, `sync_state`, `sync_plan`, `sync_ipc`, git output parsing,
Azure DevOps URL and error mapping) and `sync_engine` + `sync_crypt` + `FakeBackend` (the
engine takes its clock and file system access through interfaces) also go into
`explorer-plus-tests`.

## Decided

- **Names**: each path component encrypted in place (simpler, faster for uploads and
  listing; see the comparison above).
- **Unlocking**: a 15-character pairing code from another device, or the 15-character
  recovery key; no passphrase (both are short enough to write down, and Argon2id makes them
  strong enough).
- **Cryptography**: Monocypher, vendored.
- **Backends**: Folder, S3, git, Azure DevOps Git.

## Decisions to confirm

1. **Agent process** (`--sync`, runs without windows, tray icon, optional autostart):
   recommended. Alternative: sync only while Explorer Plus is open.
2. **git as an encrypted object store** with a hidden bare mirror (with encryption the only
   sensible option: git can't diff or merge ciphertext), accepting that every version of a
   file is stored whole.
3. **Requiring Git for Windows 2.31+** for the git and Azure DevOps backends.
4. **Full copies first, Files On-Demand later** (phase 7).
