#pragma once
#include <windows.h>
#include <string>

// The check value Windows keeps beside a file type's default app (UserChoice under the type's
// key in HKCU\...\FileExts: ProgId, Hash). Windows ignores a choice whose hash doesn't match: the hash
// is worked out from the type, the user's SID, the ProgID and the minute the key was last
// written. Pure computation, unit-tested (tests/).

// `ext` ".mp3", `sid` "S-1-5-21-...", `written` the UserChoice key's last write time (UTC)
std::wstring user_choice_hash(const std::wstring& ext, const std::wstring& sid, const std::wstring& progId,
                              const FILETIME& written);
