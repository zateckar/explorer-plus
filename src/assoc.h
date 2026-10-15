#pragma once
#include <windows.h>

// Media file types: the ones Explorer Plus is registered to open (for this user, no
// administrator needed: a ProgID per type running "explorer-plus.exe --play", listed under
// the type's Open with and in Windows' Default apps), and Windows' own ways to make it
// their default app - only the user may choose that (Windows guards the choice).

void assoc_dialog(HWND owner);       // "..." > Media file types: modal
// at start: a registration an exe made that has moved since points to this one
void assoc_follow_exe();
