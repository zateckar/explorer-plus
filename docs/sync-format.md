# Sync folders: stored formats (files and names version 1, key objects version 2)

What Explorer Plus writes to a sync storage location, and what it keeps on the PC. Written
so another client (a recovery tool, a phone app) can be built from this document alone.
The design and the reasons behind it are in [sync-plan.md](sync-plan.md). Format version 1
is frozen by known-answer tests (`tests/sync_tests.cpp`, `FORMAT_V1_KAT`). The key objects
(ring, vault, device records, pairing grants, hand-overs) are at version 2: their magics end
in `2`, and version 1 objects are refused (they had no members and no signatures; see
[sync-review.md](sync-review.md), S1).

Integers are little-endian. `||` is concatenation. All cryptography is Monocypher's,
except the random source (`BCryptGenRandom`).

## Primitives

| name | definition |
|---|---|
| `KDF(K, ctx, salt)` | keyed BLAKE2b-256 with key `K` over `"ep-kdf" \|\| len(ctx) (1 byte) \|\| ctx \|\| salt` (salt optional) |
| `AEAD(K, nonce, ad, m)` | XChaCha20-Poly1305: `cipher \|\| tag (16)` |
| `SEAL(K, ad, m)` | `nonce (24, random) \|\| AEAD(K, nonce, ad, m)`; `ad` is an ASCII string |
| `Argon2id(code, salt, mem, passes)` | Argon2id, 1 lane, 32-byte output; `code` is a normalized code (below) or the sync password's UTF-8 bytes as typed |
| `umk_id(UMK)` | the first 8 bytes of `KDF(UMK, "ep umk id")` as 16 lowercase hex digits |
| `base32` | RFC 4648 alphabet, lowercase, no padding |
| `SIGN(dev, ctx, m)` | Monocypher EdDSA (Curve25519 with BLAKE2b, not RFC 8032's SHA-512) over `ctx \|\| 0x00 \|\| m`, with the device's signing key; 64 bytes |

**Sync password**: the user's, 12 characters or more with lower and upper case letters and
digits (a reader enforces nothing: the rules are for the device that sets it). Rings and
vaults from before it have a **recovery key** in its place, a code as below; a reader
tries what is typed as given, then normalized as a code.

**Codes** (pairing code; the recovery key of older rings): 15 characters of Crockford Base32
(`0123456789ABCDEFGHJKMNPQRSTVWXYZ`), shown as `XXXXX-XXXXX-XXXXX`. Before use they are
normalized: upper case, spaces and dashes removed, `O` → `0`, `I` and `L` → `1`; anything
else that isn't 15 alphabet characters is refused. Default Argon2id cost: 256 MiB, 3
passes (stored with each use, so it can change later).

## Keys

- **UMK** (user master key): 32 random bytes per *generation* `g` (1, 2, ...). A new
  generation is made when a device is removed.
- **Ring**: every UMK generation the user has, plus the sync password's Argon2id salt,
  parameters and result (`recKek`), the IDs of removed devices, and the **members** (the
  user's devices, with their public keys):
  ```
  "EPR2" | n (u32) | n × (gen (u32) | UMK (32)) | recSalt (16) | recMem KiB (u32) | recPasses (u32)
         | recKek (32) | r (u32) | r × deviceId (16 ASCII hex)
         | m (u32) | m × (deviceId (16 ASCII hex) | X25519 public (32) | EdDSA public (32))
  ```
  The current generation is the **last** entry. A reader refuses a member whose ID isn't
  its X25519 key's. A ring that comes from another device (pairing grant, recovery wrap,
  hand-over) replaces the device's current generation, password wrap key, members and removed
  list; the device keeps its other generations as older ones, and adds itself as a member
  unless the ring lists it as removed (then, on pairing or recovery, it makes a new device
  key and comes back as a new device).
- **Vault key** `VK_v`: 32 random bytes per version `v` (1, 2, ...), one set per storage
  location ("vault"). New uploads and names use the current version.
- **Device key**: an X25519 key pair per device; `deviceId` = the first 8 bytes of
  `BLAKE2b(public key)` as 16 hex digits. Its **signing key** is the EdDSA key pair from
  the seed `KDF(X25519 secret, "ep sign")`.
- **Members**: a device signs the vault objects, device records and hand-overs it writes;
  the others believe them only from a member of their ring that isn't removed. A device
  becomes a member by pairing or recovery (it adds itself), or when a member reads its
  device record sealed under the member's **current** generation: a removed device never
  has that one, so it can't bring devices of its own in.

Keys derived from a vault key:

```
K_file  = KDF(VK_v, "ep file", salt)         per stored file (salt below)
K_meta  = KDF(K_file, "meta")
K_data  = KDF(K_file, "data")
K_names = KDF(VK_v, "ep names");  K_mac = KDF(K_names, "mac");  K_enc = KDF(K_names, "enc")
```

## On the storage

Everything lives under the location's root (an S3 prefix, a git branch and subfolder, a
folder). The reserved folder `.ep-vault/` holds key material; every other object is an
encrypted file or folder under an encrypted name.

### `.ep-vault/vault`

UTF-8 text, one `key=value` per line after the first:

```
EPV2
umk=<umk_id of the generation the payload is sealed under>
gen=<that generation's number>
data=<base64 SEAL(KDF(UMK_gen, "ep vault"), "ep vault", payload)>
rsalt=<base64 of the sync password's 16-byte Argon2id salt>
rmem=<Argon2id memory, KiB>
rpasses=<Argon2id passes>
recovery=<base64 SEAL(Argon2id(sync password, rsalt, rmem, rpasses), "ep recovery", ring)>
manifest=1                      (the vault keeps a manifest; absent in vaults from before them)
signer=<deviceId of the device that wrote it>
sig=<base64 SIGN(signer, "ep vault object", every byte of the object before this line)>
```

`payload` = `"EPVP" | vault id (16) | current version (u32) | n (u32) | n × (version (u32) | VK (32))`.

The password (`recovery`) wrap holds the whole ring, so the sync password alone opens every generation.
A vault whose `umk` isn't in a device's ring is *locked* for that device. A vault whose
signer is removed, isn't a member, or whose signature doesn't check is refused as tampered,
except (removed signer) when it is byte for byte the object this device took before (the
pin's `hash`): what the removed device wrote while it was a member. Opening it then moves
it on, signed by the opening device.

### `.ep-vault/devices/<deviceId>`

```
body = "EPD2" | gen (u32) | umk_id (16 ASCII) | SEAL(KDF(UMK, "ep devices"), "ep device " + deviceId,
                        X25519 public (32) | EdDSA public (32) | added (u64 FILETIME) | name (UTF-8))
record = body | SIGN(the device itself, "ep device record", body)
```

The reader checks that `BLAKE2b(X25519 public)` gives the file's `deviceId` and that the
signature checks with the EdDSA key inside. Only holders of the UMK can write a record; the
storage learns nothing about the devices. A record makes its device a member only when it
is sealed under the reader's current generation (above); "Devices" lists members only.

### `.ep-vault/pair/<16 hex>` (Add a device)

```
"EPP2" | salt (16) | mem KiB (u32) | passes (u32) | expiry (u64 FILETIME)
       | SEAL(Argon2id(code, salt, mem, passes), "ep pair " + decimal(expiry), ring)
```

Valid for 15 minutes; the redeeming device deletes it, and expired grants are deleted by
the next device that makes one. The code itself is never stored. A reader only tries
grants whose Argon2id parameters are within 8 KiB–1 GiB and 1–10 passes (the same bounds
apply to the recovery wrap) and whose expiry is at most 30 minutes ahead. It tries 8
grants at most.

### `.ep-vault/rotate/<deviceId>-g<gen>` (a device was removed)

```
body = "EPX2" | umk_id of the previous generation (16 ASCII) | sender deviceId (16 ASCII)
       | ephemeral X25519 public key (32)
       | SEAL(KDF(UMK_previous, "ep rotate", X25519(eph, receiver public key)),
              "ep rotate " + sender deviceId + " " + receiver deviceId, new ring)
object = body | SIGN(sender, "ep rotate", body)
```

One per remaining member that has a record in this vault: it needs both its device key
and the previous UMK to open it. The removed device gets none, and its `devices/` record is
deleted; records of devices that aren't members get none either. A device takes a
hand-over only when its sender is a member of its ring, not removed, and the signature
checks; the new ring lists both the receiver and the sender as members (not removed); and
it continues the receiver's current generation (sealed under it, or its ring contains it).
It deletes the hand-over once read, and keeps one whose sender or generation it doesn't
know yet. "Change password" also makes a new generation and hands it over the same way.

### `.ep-vault/head`

16 random bytes, rewritten after every pass that changed something. Stores without a change
feed (S3, Folder) poll its revision (ETag / time) instead of listing everything. Git
backends don't use it (`ls-remote` does the same).

### `.ep-vault/manifest` and `.ep-vault/m/<32 hex>` (the manifest)

Every file of the vault, signed by the member device that wrote the list. A device takes
the storage as the manifest says, so the storage can't withhold, roll back, replace or
plant files unnoticed, across the whole tree (`sync_manifest.cpp`).

```
root  = "EPM1" | key version v (u32) | signer deviceId (16 ASCII) | n (u32)
        | SEAL(KDF(VK_v, "ep manifest"), the 24 bytes before n, body) (n bytes)
        | SIGN(signer, "ep manifest", every byte before this)
body  = vault id (16) | seq (u64) | written (u64 FILETIME) | placement version p (u32)
        | count (u32, a power of two) | count × BLAKE2b-256 of shard i
shard = "EPMS" | key version w (u32) | index i (u32)
        | SEAL(KDF(VK_w, "ep manifest"), "EPMS" | w | i | vault id, entries)
entries = n (u32) | n × (path length (u16) | plaintext path, '/'-separated UTF-8
        | size (u64) | version counter (u64) | BLAKE2b-256 of the plaintext (32)
        | revision length (u16) | the stored object's revision)
```

A shard is stored under the first 16 bytes of its BLAKE2b-256, in hex. A path is in shard
`BLAKE2b-256 keyed with KDF(VK_p, "ep manifest shard") over the path, first 4 bytes (LE)
mod count`; about 1,000 files a shard. A new manifest keeps the bytes (and name) of every
shard whose files didn't change, keeps the placement version while the shard count stays,
and deletes the shards the old root named and the new one doesn't. `seq` grows by one with
each manifest; the hash is zero for files taken in from a listing (below).

**Reading.** The root's signer must be a member of the reader's ring, not removed, and the
signature must check, except when the root is byte for byte the one this device took
before (`manifest.pin`): what a device since removed wrote while it was a member. A root
with a lower `seq` than the pin is rolled back. Each shard must hash to the root's entry,
open, and hold only paths that belong in it. Then the plaintext listing is what the manifest
lists, at its sizes and revisions:

| the storage shows | the device sees | reported as |
|---|---|---|
| the listed revision | the file | — |
| no file (withheld, deleted) | the file at the listed revision: not deleted here | "the storage doesn't have it" |
| another revision (rolled back, replaced) | the file at the listed revision: not downloaded | "isn't the version the manifest lists" |
| a file the manifest doesn't list (planted, brought back) | nothing | "the manifest doesn't list it" |

A disagreement is reported once it has stood 15 minutes (`EXPLORERPLUS_SYNC_MANIFEST_GRACE`,
seconds): on stores without transactions another device's files are on the storage a moment
before its manifest. A download of the listed revision must hash to the listed BLAKE2b
(Folder revisions can be put back; contents can't); within the grace period a mismatch is a
quiet retry, after it tampered. The next upload of such a file goes over what the storage
has, which repairs it. Versions restored from the history aren't in the manifest and aren't
checked against it.

**Writing.** Every commit with uploads, deletions or moves writes the next manifest: this
device's changes over the manifest it read, a change replacing an entry only when that
entry's version counter isn't higher (another device's later write wins).
- Transactional stores (git, Azure DevOps): the new shards, the root (conditional on the
  root that was read) and the removal of the old shards go into the same commit as the files.
- Other stores: after the files, a conditional write of the root; when another device wrote
  one meanwhile, this device reads it and applies its changes again (up to 6 times). Until a
  manifest with them is written, the changes stay in `manifest.own` (On the PC), are counted
  as listed, and are written at the next pass.

**No manifest.** If this device has seen one (pin), or the vault object says `manifest=1`
and there are files, the vault is tampered. A vault from before manifests (no `manifest=1`):
the first device with them takes the files there as the start (hash unknown, version 0),
writes manifest 1 and adds `manifest=1` to the vault object (signed again, only if it is
still the object it opened). New vaults have `manifest=1` from the start.

**Removed devices.** When a vault moves to a new key version (a device removed, a new
sync password), the device that moves it writes the root again under the new version, signed
by itself, with the same shards and `seq + 1`. It accepts the old root's writer even if it
was just removed (only the seal is checked then). What the removed device signs afterwards
is refused.

### Names

Each path component is encrypted on its own, under the vault key version current when it
was made:

```
padded = UTF-8 name || 0x80 || zeros up to a multiple of 16 bytes
tag    = BLAKE2b-256 keyed with K_mac over (plaintext parent path, '/'-separated UTF-8 || 0x00 || padded), first 16 bytes
cipher = XChaCha20(K_enc, nonce = tag || 8 zero bytes, padded)
stored = base32(version (u16) || tag || cipher)
```

Decryption reads the version from the first 8 base32 characters (5 bytes), decrypts, and
recomputes the tag (constant-time compare): a changed or moved name fails. The same name
in two folders gives two different stored names.

A stored component longer than the store allows (Folder: 255 characters) is written as
`base32(BLAKE2b-256(stored)[0..20)) + ".epn"`, with the full stored name as the content of
a plain sidecar object beside it, `<that>.epn.name`. A reader checks that the sidecar's
content gives the short name.

### Files

```
header (48 bytes, plaintext; the additional data of every AEAD below)
  0   "EPSF"
  4   format version, 1
  5   chunk size log2, 16 (64 KiB)
  6   0, 0
  8   vault key version (u32)
  12  salt (32 random bytes, new for every upload)
  44  0 (4 bytes)
meta (64 + 16 bytes): AEAD(K_meta, nonce = 24 zero bytes, header, m)
  m = size (u64) | mtime (u64 FILETIME) | attributes (u32) | version counter (u64)
      | fingerprint (32) | 4 zero bytes
chunks: AEAD(K_data, nonce_i, header, plaintext chunk i)
  nonce_i = i (u64) || last (1 byte: 1 for the final chunk, else 0) || 15 zero bytes
  every chunk holds 64 KiB of plaintext except the last, which may be shorter; an empty file
  is one empty last chunk
```

The stored size is `128 + plaintext + 16 × chunks`, so plaintext sizes are known from
listings. Truncation, appending, reordering or splicing chunks of another file fail to
authenticate. The **fingerprint** is the plain BLAKE2b-256 of the plaintext (see
"Differences from the plan").

### Git storage

The same objects as files in a branch (default `explorerplus-sync`), optionally under a
subfolder. Commits carry a neutral author `Explorer Plus <sync@localhost>` and the message
`sync`; a pass larger than 1 GB is split over several commits.

## On the PC

- **Credential Manager** (generic credentials, `CRED_PERSIST_LOCAL_MACHINE`, names include
  a hash of the settings folder): `sync.ring` (the ring above), `sync.device` (`"EPK1" |
  X25519 secret (32) | device name`), and each pair's secrets (`pair.<id>.<field>`: a PAT,
  an S3 secret key).
- **`Sync\pairs.ini`** beside `settings.ini`: the pairs (`[pair.<id>]` with `name`, `root`,
  `type`, `paused`, `create`, `exclude=` lines and `cfg.<field>=` lines; never secrets).
- **`Sync\pairs\<id>\`**: the pair's state journal, transfers in progress, and for git
  storage the bare mirror `repo.git`.
- **`Sync\pairs\<id>\vault.pin`**: what this device saw of the pair's vault, as text:
  `EPN1`, then `id=` (vault ID, 32 hex), `gen=` (highest UMK generation), `version=`
  (highest vault key version) and `hash=` (BLAKE2b-256 of the vault object last taken, hex).
  A vault with another ID, an older generation or fewer key versions, or no vault at all,
  is refused as tampered (rolled back or swapped).
- **`Sync\pairs\<id>\retired.dat`**: the note of **retired key versions**. When the
  vault's current key version moves on (a device removed, a new sync password), the device
  lists the storage and notes what is there, so that what a removed device writes later
  with the keys it still has is refused:
  ```
  "EPS1" | below (u32) | f (u32) | f × (key (16) | lowest content version (u32))
         | d (u32) | d × key (16)
  file key   = BLAKE2b-256(UTF-8 stored path || 0x00 || revision)[0..16)
  folder key = BLAKE2b-256(UTF-8 stored path || 0x01)[0..16)
  ```
  Versions below `below` are retired. A file whose content is under a retired version
  opens only if its stored path and current revision are noted and its content version is
  at least the noted one (files noted at the first sight: 0; files written since the
  previous note: that note's `below`). A folder named under a retired version is listed only
  if it is noted. A file that may hold retired content is uploaded again under the
  current version rather than moved. The first time a device sees a vault with retired
  versions it notes everything there (trust on first use).
- **`Sync\pairs\<id>\manifest.pin`**: the manifest this device last took or wrote, as text:
  `EPMP`, then `seq=` and `hash=` (BLAKE2b-256 of the root, hex). A root with a lower `seq`,
  or no root at all, is refused as tampered.
- **`Sync\pairs\<id>\manifest.own`** (stores without transactions): this device's writes not
  in a manifest yet. Records `kind (1 written, 2 removed) | entry (as in a shard) | first 8
  bytes of BLAKE2b-256 of the record`, appended and flushed after each upload, deletion or
  move; a torn last record is dropped. Deleted once a manifest with them is written.

## Differences from the plan

- **Fingerprint**: the plan has a keyed fingerprint `BLAKE2b(K_fp, hash)`. It's the plain
  BLAKE2b of the plaintext instead. It sits only inside the encrypted meta block, so the
  storage never sees it; the keyed form protected nothing more.
- **No `.ep-vault/ring` object**: older generations travel inside the ring itself (in the
  recovery wrap, pairing grants and rotation hand-overs), so a separate chain of wrapped
  generations isn't needed.
- **Version prefix on names**: stored names start with the vault key version (2 bytes), so
  names made after a rotation use the new key and a removed device can't read them.
- **Sizes**: the plan's intro promises sizes "to the nearest 64 KiB"; format v1 has no size
  padding, so a stored file's size gives its exact plaintext size. Padding is a later
  option (plan, phase 7).
- **Known limitation, retired keys**: the note is per device and made when the device
  learns of the new key version; see sync-review.md, S2, for what stays open until the
  signed manifest.
- **Known limitation, file swaps**: a file's content isn't bound to its path. A storage
  that swaps two stored files (or serves an old version of one) isn't detected; each file
  still authenticates on its own. A signed per-vault manifest (plan, phase 7) would close
  this.
- **Sync password** in place of the recovery key (2026-10-18): the stored fields keep their
  names (`rsalt`, `recovery`, `recKek`); only what goes into Argon2id changed. The
  password is kept nowhere, so it can't be shown again; "Devices and keys" offers *Change
  password...* (the current one first).
