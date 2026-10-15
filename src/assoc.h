#pragma once
#include <windows.h>

// Media file types: the ones Explorer Plus is registered to open (for this user, no
// administrator needed: a ProgID per type running "explorer-plus.exe --play", listed under
// the type's Open with and in Windows' Default apps), made their default app on Apply (the
// choice written with the hash Windows checks it by, userchoice.cpp); a type Windows won't
// let go of is left to Settings.

void assoc_dialog(HWND owner);       // "..." > Media file types: modal
// at start: a registration an exe made that has moved since points to this one
void assoc_follow_exe();
