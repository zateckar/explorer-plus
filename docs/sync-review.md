# Sync folders: security review of the cryptography (format v1)

An adversarial review of the end-to-end encryption of Sync folders, as built in commit
16f3a39, against the threat model in [sync-plan.md](sync-plan.md) ("End-to-end encryption")
and the stored formats in [sync-format.md](sync-format.md). It is the review that phase 6
of the plan makes a release gate.

**Scope:** `crypto.cpp` (Monocypher wrapper, KDF, AEAD, Argon2id, codes), `crypto_file.cpp`
(format v1, STREAM chunks), `crypto_names.cpp` (SIV names, shortening), `sync_keys.cpp`
(ring, vault `EPV1`, device records `EPD1`, pairing grants `EPP1`, rotation hand-overs
`EPX1`, recovery wrap, device removal), `sync_crypt.cpp` (the encrypting backend),
`credstore.cpp` and the agent's pipe (`sync_ipc.cpp`, its handler in `sync_agent.cpp`).
Monocypher itself is out of scope (it has its own audit).

**Attackers considered:**
- **the storage**: anyone who can read and write the storage location (the provider, a
  leaked S3 key or PAT, a git server's admins). It keeps old copies: git history, S3
  versions, or simply copies it made earlier;
- **a removed device** that still has the storage credentials it had. The plan only says the
  user "should also revoke" them;
- **the holder of a replaced recovery key**: the user made a new recovery key because the
  old one leaked;
- **another local process**, of the same user or of another account.

Format v1's bytes are unchanged; `FORMAT_V1_KAT` still passes.

## Summary

| # | severity | finding | status |
|---|---|---|---|
| S1 | **critical** | A removed device that can still write to the storage gets the next ring back (forged hand-over, planted device record, rolled-back vault) | narrowed; design fix needed |
| S2 | high | A removed device can still write files and names that the remaining devices accept | open; documented |
| S3 | high | Retried uploads re-encrypted under the same salt: nonce reuse when the file changed between attempts | **fixed** |
| S4 | high | "New recovery key" left the old key able to open everything, including what is written later | **fixed** |
| S5 | medium | The vault object can be rolled back, swapped for another vault of the user's, or deleted | **fixed** (pin) |
| S6 | medium | Running devices kept encrypting under the superseded key after a rotation | **fixed** |
| S7 | low | Shortened names weren't bound to their sidecar: two long names could be swapped | **fixed** |
| S8 | low | Argon2id parameters from the storage were barely bounded, and running out of memory aborted the agent | **fixed** |
| S9 | low | `EXPLORERPLUS_SYNC_KDF_KIB` weakened real wraps in release builds | **fixed** |
| S10 | low | The pipe fell back to the default DACL when its own couldn't be built | **fixed** |
| S11 | low | Re-registration trusted the unauthenticated generation number of a device record | **fixed** |
| I1–I9 | info | Claims in the docs that the code doesn't meet, and smaller notes | docs corrected where noted |

## Findings

### S1 (critical): removal doesn't hold against a removed device that can still write to the storage

**Root cause.** Group membership and key hand-overs are authenticated only with UMK
generations. The party being excluded (a removed device, or anyone holding an old ring)
knows those generations. Three related attacks follow:

**(a) Forged hand-over.** A removed device D knows UMK_g. Every remaining device B
still has UMK_g in its ring, and D can read B's public key from B's device record, which
is sealed under UMK_g. D builds a ring of its own: B's generations plus `gen = 99` with a
UMK and a `recKek` that D chose. It seals that ring as `.ep-vault/rotate/<B>-g99`:
`EPX1 | umk_id(UMK_g) | eph | SEAL(KDF(UMK_g, "ep rotate", X25519(eph, B.pub)), ...)`.
Before this review, `take_rotations` took any hand-over it could open whose generation
was higher, so B's *current* UMK became D's. From then on B seals:
- every vault it upgrades (new vault key versions included) under D's UMK;
- its hand-overs to the other devices (which take them too, so the forgery spreads);
- its recovery wraps under D's `recKek`.

D reads everything written from then on, and the user sees nothing wrong. The test
`test_removal_hardening` builds such a forgery from the format description alone.

**(b) Planted device record in a vault not yet upgraded.** The plan upgrades vaults
lazily: "the first remaining device that syncs them". Until then such a vault is sealed
under UMK_g. D writes `.ep-vault/devices/<F>`, sealed under UMK_g, for a key pair F it
controls. When a remaining device upgrades the vault, `hand_over` seals the new ring
under UMK_g, which D knows, to F. That hands over every UMK and the new recovery `recKek`.

**(c) Rolled-back vault.** In a vault that has already been upgraded, D (or the storage)
puts back an old copy of the vault object sealed under UMK_g. Any remaining device that
opens it treats it as "behind the ring" and upgrades it again, which runs (b).

**The same goes for the holder of a replaced recovery key.** The old recovery wrap in
an old copy of any vault opens the ring of that time.

**Done here:**
- (c) is closed for every device that has seen the newer vault. The vault pin (S5) refuses
  an older generation.
- (a) is narrowed. `take_rotations` takes a hand-over only if it *continues this device's
  current generation*: sealed under it, or carrying it. Once B has taken its legitimate
  hand-over (generation g+1), D can't seal anything that B accepts. Hand-overs that open
  but don't continue are deleted.

**Still open:**
- (a) in the window before B takes its legitimate hand-over. An attacker with write
  access can delete A's hand-over and plant its own, so it controls that window.
- (b) in vaults that are upgraded lazily.

No local fix can close these. When B is still on generation g, D and the remover A look
exactly the same to B: both are former members with the same keys.

**Recommendation (a format addition, not a v1 change):**
- Carry the member list (device ID → public key) inside the ring, which only devices that
  aren't removed receive.
- Have the sender sign hand-overs and device records with its device key (static-static
  X25519 into the KDF, or an Ed25519 signature).
- Accept a hand-over only from a member of the receiver's own ring, and only one that keeps
  the receiver's generations.
- On a conflict (two different rings for the same generation), stop and ask the user.

`Ring::parse` ignores trailing bytes, so a member section after `removed` stays readable
by v1 code.

Until then:
- In "Remove device", make revoking the removed device's storage credentials the *first*
  step, not an afterthought.
- Say plainly that removal protects nothing against a device that can still write to the
  storage.

### S2 (high): a removed device can still write accepted content

Vault key versions from before a removal must stay readable, because old files use
them. Nothing marks them as superseded for *writing*. A removed device that can still
write to the storage can do both of these under VK_old:
- upload new files;
- overwrite existing ones.

The names authenticate too. The remaining devices download them as ordinary remote
edits, and the planner even prefers an edit over a delete. The plan promises only
confidentiality for what is written after a removal, but users will assume integrity.

**Recommendation:**
- In the vault payload, record the generation at which each key version was superseded.
- Once every remaining device has moved on (they re-register under the new generation),
  refuse *new* revisions written under superseded versions.
- Alternatively, the signed per-vault manifest from phase 7.
- "Re-encrypt everything" followed by dropping old versions from the vault closes it fully.

### S3 (high, fixed): nonce reuse on retried uploads

`EncryptingSource::rewind()` kept the salt, so the same `K_data` and the same counter
nonces were used again. The retry layer (`Retrying::upload`) rewinds after a transient
failure and reads the local file again from the start. If the file changed between
attempts and kept its size, two different plaintexts were encrypted under the same
(key, nonce) pairs. The storage receives the bytes of the failed attempt too: it is the
server. XChaCha20 then leaks the XOR of the two plaintexts. Poly1305 with a reused
one-time key lets the storage forge that chunk under that file's key.

**Fix:** every pass draws a new salt and derives new file keys. With `fixedSalt` (the
known-answer tests) passes still repeat. Test: `test_file_format`, "a retried upload".

### S4 (high, fixed): "New recovery key" didn't take the old key's access away

The recovery wrap holds the whole ring, including the *current* UMK. "New recovery key"
only replaced the `recKek`, and the storage keeps old copies of the vault object (git
history, S3 versions, copies). So whoever had the old code could:
1. open an old wrap;
2. get the current UMK from it;
3. open the current vault object with that UMK;
4. read every file, including ones written after the key was replaced.

**Fix:** `new_recovery_key` also starts a new UMK generation. Each vault the agent
refreshes moves to it, with a new vault key version and hand-overs for the other
devices, and the runners reopen. Files written before the new key stay readable with the
old one, as with a removal, and S1 still applies to the old key's holder.
Test: `test_removal_hardening`, the last part.

### S5 (medium, fixed): the vault object wasn't pinned

The vault payload is authenticated (`"ep vault"`) but bound neither to its location nor
to time. The storage could:
- serve an **old copy**. A device then "upgraded" it with a new vault key version whose
  *number* already existed with another key, so files under the real version failed as
  `Tampered`. It also sealed hand-overs under the old generation (S1 b, c);
- put **another of the user's vaults** in its place (another prefix or branch on the same
  storage, sealed under the same UMK). The pair then synced against that vault's files;
- **delete** it. With `create` still set, a new vault would be made.

**Fix:** `synckeys::VaultPin` (vault ID, highest generation, highest key version) in
`Sync\pairs\<id>\vault.pin`. The agent checks it on every open of the pair's vault
(`open_pair_backend` and the encrypting layer's reopen) and moves it forward. It is
trusted on first use. A different ID, an older generation, fewer key versions, or a
missing vault object is refused as `Tampered`.

Residual: the key operations the agent runs on a raw store don't check the pin. They
are "Unlock", "Add a device", "Remove device" and the recovery refresh. Of these, "Remove
device" opens the vault and upgrades it if it is behind.
Test: `test_removal_hardening`.

### S6 (medium, fixed): running devices kept the superseded key

The encrypting layer read the vault again only when it met a key version it didn't know.
After A removed B, a running C kept encrypting uploads and new names under the old
version, which B can read. That lasted until C happened to download something written
under the new version, or restarted.

**Fix:** each listing compares the vault object's revision with the last one seen and
reads the vault again before anything is written. That takes C's hand-over and switches
to the new version. Test: `test_removal_hardening` checks the header of C's next upload.

### S7 (low, fixed): shortened names weren't bound to their sidecar

`<hash>.epn` took its full name from `<hash>.epn.name` without checking that the hash
matches. The storage could swap the sidecars of two long-named siblings. Both names
authenticate in that folder, so the two files traded names; that is one more form of the
known "file swap" limitation. **Fix:** the short name must equal `shortened(sidecar)`.
Test: `test_long_names`.

### S8 (low, fixed): untrusted Argon2id parameters

Pairing grants and recovery wraps carry their own Argon2id parameters, and the code
accepted up to 4 GiB and 16 passes. When `VirtualAlloc` failed, `argon2id()` called
`abort()`. A grant the storage planted could therefore:
- crash the agent on every "Unlock";
- tie it up for minutes per grant, with any number of grants.

**Fix:**
- At most 1 GiB and 10 passes.
- Running out of memory is an error, not an abort.
- At most 8 grants per try.
- Grants that expire further than two lifetimes ahead are skipped.

### S9 (low, fixed): the test-only Argon2id cost applied anywhere

`EXPLORERPLUS_SYNC_KDF_KIB` was honoured in release builds. A stray variable made real
recovery wraps and pairing grants at 8 KiB and 1 pass, and those parameters are stored
for the storage to see. With 75-bit codes that isn't an offline break, but the margin the
plan relies on was gone silently. **Fix:** it only applies together with
`EXPLORERPLUS_SETTINGS` (a test's own settings file).

### S10 (low, fixed): the pipe's fallback DACL

If the user's SID couldn't be read, the SDDL string was invalid and the pipe was created
with the *default* DACL, which gives read access to every local account. **Fix:** without
its descriptor the agent serves no pipe.

### S11 (low, fixed): re-registration trusted a plain number

`register_device` decided whether its record was current from the record's plain
generation field, which nothing authenticates. The storage could set it so that a device
never re-sealed its record under the current generation, leaving it readable and
replaceable with the old one (S1). **Fix:** it compares the record's UMK ID with the
current generation's.

## Information, and claims the code didn't meet

- **I1, keys in memory.** The plan says keys sit in `VirtualLock`ed pages and are left out
  of crash dumps through `MiniDumpWriteDump`'s callback. Neither is implemented.
  `crash.cpp` writes `MiniDumpWithIndirectlyReferencedMemory` without a callback, so a
  dump of the agent can hold UMKs, vault keys and the device secret. Dumps stay on the PC,
  but people attach them to bug reports. Recommendation: don't write dumps in the `--sync`
  process, or exclude its heap. The plan now says "not yet".
- **I2, `head`.** The plan calls it "a small encrypted counter". It is 16 random bytes and
  isn't authenticated. That's harmless, because the storage can withhold changes anyway.
  The plan is corrected.
- **I3, start-up self test.** The plan promises a known-answer test of each primitive.
  XChaCha20-Poly1305 had only a round trip and Argon2id had nothing; both now check their
  published vectors (fixed). Keyed BLAKE2b is covered only by the unit tests.
- **I4, what goes over the pipe.** The plan says no secret crosses IPC except "show
  recovery key". In fact the pipe carries:
  - pairing codes (`pair-offer`);
  - the code typed to unlock;
  - new recovery keys.

  Any process of the same user can ask for `pair-offer` and redeem the grant, which gets
  it the whole ring. It can also ask for `remove-device`. That is the same trust as
  `CredRead` for that user (the DACL keeps other accounts out, and Low integrity and
  AppContainer processes can't write to the pipe). It becomes the bypass if the ring is
  ever gated behind Windows Hello or the TPM (the plan's "later option"). Any process of
  the same user can also create the pipe name first and catch typed codes. The plan is
  corrected.
- **I5, ring capacity.** `Ring::parse` accepts at most 64 generations, and Credential
  Manager holds 2560 bytes. Each removal adds about 52 bytes and each new recovery key 36,
  so after about 45 such operations the ring can't be saved. `save_ring` now refuses what
  `parse` can't read back (before, a 65th generation would have been saved and then lost
  on the next load). Pruning is needed eventually.
- **I6, unbound fields of the vault object.** `gen`, `umk`, and the recovery wrap with its
  salt and parameters aren't bound to the sealed payload. The storage can paste an older
  `recovery=` line into the current vault. The new recovery key then fails on that vault
  (denial of service); the old key gets only the old ring, which an old copy gave it anyway.
- **I7, names.** The version prefix isn't in the SIV tag, which is harmless: another
  version means another key, and the tag fails. The encrypting layer's `sane_name` lets
  `\` and `:` through. The engine's `windows_valid_path` refuses them before anything is
  written locally (`sync_engine.cpp`), so there is no path traversal. Only key holders can
  make names, and that includes a removed device (S2).
- **I8, memory hygiene.** Plaintext buffers on the transfer path, codes held in
  `std::string` and copies of the ring aren't wiped. That fits the threat model (a
  compromised device is out of scope) but is noted for I1.
- **I9, known limitations, confirmed.**
  - Content isn't bound to its path: the storage can swap files or serve an old version
    of a file when this device hasn't seen a newer one.
  - Sizes are exact.
  - The fingerprint is the plain BLAKE2b, inside the encrypted meta block.
  - None of these is affected by the fixes above.

## Checked and found sound

- **KDF domain separation.** The contexts are all different: `ep file`, `meta`, `data`,
  `ep names`, `mac`, `enc`, `ep umk id`, `ep vault`, `ep devices`, `ep rotate`.
  - The length prefix rules out context/salt ambiguity.
  - Sub-keys come from different parent keys: `K_file` per salt, `K_names` per vault key.
  - No subkey of one use equals one of another.
- **AEAD use.** Every seal has its own key *and* its own additional data:
  - `ep vault`, `ep recovery`;
  - `ep device <id>`, `ep pair <expiry>`, `ep rotate <id>`.

  One kind of object can't be passed off as another. Nonces are random 24-byte values for
  wraps. File keys are new per upload (per pass, after S3), so the zero meta nonce and the
  counter chunk nonces never repeat under a key.
- **File format.**
  - The header is the additional data of every block, and the reserved bytes are covered too.
  - The last-chunk flag catches truncation and appending.
  - The chunk index catches reordering.
  - Per-file keys catch splicing.
  - The size is checked against meta.
  - The plaintext hash is checked against the fingerprint.
  - Nothing from an unauthenticated chunk reaches the sink.
- **Names.**
  - It is a deterministic SIV: the tag is a keyed BLAKE2b over the parent path and the
    padded name, and the tag is compared in constant time.
  - Padding is ISO 7816-4 and checked.
  - Base32 decoding is canonical.
- **Pairing grants.**
  - The code never reaches the storage.
  - The expiry is in the additional data, so it can't be extended.
  - Grants the storage plants can't be opened with the user's code.
  - Offline guessing costs a 256 MiB Argon2id per guess over 75 bits.
  - A replayed grant still needs the code.
- **Device records.** The storage can't forge them: it would need the UMK. The ID is
  bound to the public key, and removed IDs are dropped from hand-overs.
- **Random numbers.**
  - `BCryptGenRandom` aborts on failure.
  - Codes take 5 uniform bits per character.
  - X25519 results are always hashed with a UMK before use, so a low-order ephemeral
    point in a hand-over only helps someone who already has that UMK.
- **Timing.** Secret comparisons use `crypto_verify32`, `crypto::equal` and Monocypher's
  AEAD check. Other comparisons (UMK IDs, device IDs, revisions) are on public data.
- **Credential Manager.** It uses `CRED_PERSIST_LOCAL_MACHINE` (DPAPI, never
  `ENTERPRISE`), names carry the settings hash, and blobs are wiped after use.

## Tests added

- `tests/sync_tests.cpp`:
  - a retried upload gets a new salt and still decrypts;
  - a fixed salt still repeats;
  - Argon2id parameter bounds;
  - the test-only cost is gated.
- `tests/sync_crypt_tests.cpp`:
  - `test_long_names`: swapped sidecars are refused;
  - `test_removal_hardening`:
    - a running device's first upload after a removal is under the new key version;
    - the vault pin refuses a rolled-back, swapped or missing vault;
    - a hand-over forged under an old generation is taken by a device still on it (the
      remaining window of S1) and refused by one that moved on;
    - after "New recovery key", the ring from an old copy of the vault no longer opens it,
      and the other devices follow.
