#pragma once
// shared by the sync test files
#include "sync_crypt.h"
extern int t_failed, t_run;
void t_check(bool ok, const char* what, const char* file, int line);
#define CHECK(cond) t_check((cond), #cond, __FILE__, __LINE__)

// the vault reopen a pair's store gets (open_pair_backend), for tests that open vaults themselves
inline sync::VaultReopen reopen_with(synckeys::KeyStore& ks) {
    return [&ks](sync::Backend& b, synckeys::Vault& v) { sync::NoProgress np; return synckeys::open_vault(b, ks, false, v, np); };
}

// BLAKE2b-256 of the format v1 encryption of "hello" under key 00..1f, salt a0..bf,
// vault key version 3, meta of encrypt_all(): format v1 is frozen, this never changes
#define FORMAT_V1_KAT "40ebae9cd12110bb4e4ffe022328b5947171f10e1314d15cc5e30c14a8160cd6"
