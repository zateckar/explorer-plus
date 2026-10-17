# Sync folders: stored formats (version 1)

What Explorer Plus writes to a sync storage location, and what it keeps on the PC. Written
so another client (a recovery tool, a phone app) can be built from this document alone.
The design and the reasons behind it are in [sync-plan.md](sync-plan.md). Format version 1
is frozen by known-answer tests (`tests/sync_tests.cpp`, `FORMAT_V1_KAT`).

Integers are little-endian. `||` is concatenation. All cryptography is Monocypher's,
except the random source (`BCryptGenRandom`).

## Primitives

| name | definition |
|---|---|
| `KDF(K, ctx, salt)` | keyed BLAKE2b-256 with key `K` over `"ep-kdf" \|\| len(ctx) (1 byte) \|\| ctx \|\| salt` (salt optional) |
| `AEAD(K, nonce, ad, m)` | XChaCha20-Poly1305: `cipher \|\| tag (16)` |
| `SEAL(K, ad, m)` | `nonce (24, random) \|\| AEAD(K, nonce, ad, m)`; `ad` is an ASCII string |
| `Argon2id(code, salt, mem, passes)` | Argon2id, 1 lane, 32-byte output; `code` is the normalized code (below) |
| `umk_id(UMK)` | the first 8 bytes of `KDF(UMK, "ep umk id")` as 16 lowercase hex digits |
| `base32` | RFC 4648 alphabet, lowercase, no padding |

**Codes** (recovery key, pairing code): 15 characters of Crockford Base32
(`0123456789ABCDEFGHJKMNPQRSTVWXYZ`), shown as `XXXXX-XXXXX-XXXXX`. Before use they are
normalized: upper case, spaces and dashes removed, `O` → `0`, `I` and `L` → `1`; anything
else that isn't 15 alphabet characters is refused. Default Argon2id cost: 256 MiB, 3
passes (stored with each use, so it can change later).

## Keys

- **UMK** (user master key): 32 random bytes per *generation* `g` (1, 2, ...). A new
  generation is made when a device is removed.
- **Ring**: every UMK generation the user has, plus the recovery key's Argon2id salt,
  parameters and result (`recKek`), plus the IDs of removed devices:
  ```
  "EPR1" | n (u32) | n × (gen (u32) | UMK (32)) | recSalt (16) | recMem KiB (u32) | recPasses (u32)
         | recKek (32) | m (u32) | m × deviceId (16 ASCII hex)
  ```
  The current generation is the **last** entry (not necessarily the highest number when
  two rings were merged; see the code's `merge`).
- **Vault key** `VK_v`: 32 random bytes per version `v` (1, 2, ...), one set per storage
  location ("vault"). New uploads and names use the current version.
- **Device key**: an X25519 key pair per device; `deviceId` = the first 8 bytes of
  `BLAKE2b(public key)` as 16 hex digits.

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
EPV1
umk=<umk_id of the generation the payload is sealed under>
gen=<that generation's number>
data=<base64 SEAL(KDF(UMK_gen, "ep vault"), "ep vault", payload)>
rsalt=<base64 of the recovery key's 16-byte Argon2id salt>
rmem=<Argon2id memory, KiB>
rpasses=<Argon2id passes>
recovery=<base64 SEAL(Argon2id(recovery key, rsalt, rmem, rpasses), "ep recovery", ring)>
```

`payload` = `"EPVP" | vault id (16) | current version (u32) | n (u32) | n × (version (u32) | VK (32))`.

The recovery wrap holds the whole ring, so the recovery key alone opens every generation.
A vault whose `umk` isn't in a device's ring is *locked* for that device.

### `.ep-vault/devices/<deviceId>`

```
"EPD1" | gen (u32) | umk_id (16 ASCII) | SEAL(KDF(UMK, "ep devices"), "ep device " + deviceId,
                                               public key (32) | added (u64 FILETIME) | name (UTF-8))
```

The reader checks that `BLAKE2b(public key)` gives the file's `deviceId`. Only holders of
the UMK can write a record; the storage learns nothing about the devices.

### `.ep-vault/pair/<16 hex>` (Add a device)

```
"EPP1" | salt (16) | mem KiB (u32) | passes (u32) | expiry (u64 FILETIME)
       | SEAL(Argon2id(code, salt, mem, passes), "ep pair " + decimal(expiry), ring)
```

Valid for 15 minutes; the redeeming device deletes it, and expired grants are deleted by
the next device that makes one. The code itself is never stored. A reader only tries
grants whose Argon2id parameters are within 8 KiB–1 GiB and 1–10 passes (the same bounds
apply to the recovery wrap) and whose expiry is at most 30 minutes ahead. It tries 8
grants at most.

### `.ep-vault/rotate/<deviceId>-g<gen>` (a device was removed)

```
"EPX1" | umk_id of the previous generation (16 ASCII) | ephemeral X25519 public key (32)
       | SEAL(KDF(UMK_previous, "ep rotate", X25519(eph, device public key)), "ep rotate " + deviceId, new ring)
```

One per remaining device: it needs both its device key and the previous UMK to open it.
The device takes it into its ring and deletes it. The removed device gets none, and its
`devices/` record is deleted. A device only takes a hand-over that continues its current
generation: one sealed under it, or one whose ring contains it. It deletes others that
open. "New recovery key" also makes a new generation and hands it over the same way.

### `.ep-vault/head`

16 random bytes, rewritten after every pass that changed something. Stores without a change
feed (S3, Folder) poll its revision (ETag / time) instead of listing everything. Git
backends don't use it (`ls-remote` does the same).

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
  `EPN1`, then `id=` (vault ID, 32 hex), `gen=` (highest UMK generation) and `version=`
  (highest vault key version). A vault with another ID, an older generation or fewer key
  versions, or no vault at all, is refused as tampered (rolled back or swapped).

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
- **Known limitation, file swaps**: a file's content isn't bound to its path. A storage
  that swaps two stored files (or serves an old version of one) isn't detected; each file
  still authenticates on its own. A signed per-vault manifest (plan, phase 7) would close
  this.
- **Recovery key**: it can't be shown again (only its Argon2id result is kept), so
  "Devices and keys" offers *New recovery key* instead of a Windows sign-in prompt that
  shows the old one.
