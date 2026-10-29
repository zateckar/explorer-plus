#pragma once
#include <windows.h>
#include <string>

// File types: the ones Explorer Plus is registered to open (for this user, no administrator
// needed: a ProgID per type running "explorer-plus.exe --play" for media, "--notepad" for
// text, listed under the type's Open with and in Windows' Default apps), made their default
// app on Apply (the choice written with the hash Windows checks it by, userchoice.cpp); a type
// Windows won't let go of is left to Settings.

void assoc_dialog(HWND owner);       // "..." > File types: modal
// at start: a registration an exe made that has moved since points to this one
void assoc_follow_exe();

// What opens a file by its type when that is Explorer Plus itself (its ProgID made the type's
// default here): the Notepad or the player. The file list opens those in this process, not
// through the shell, which starts another explorer-plus.exe just to hand the file over.
enum class OwnApp { None, Notepad, Player };
OwnApp assoc_own_app(const std::wstring& path);

// Folders: Explorer Plus opens them in File Explorer's place, from anywhere in Windows (the
// desktop, This PC, Win+E, "Show in folder"), for this user, no administrator needed. File
// Explorer stays the shell and keeps what isn't a folder on disk (Control Panel, the Recycle
// Bin, This PC itself). Turned off, nothing of it is left in the registry.
bool assoc_folders_on();
void assoc_folders_toggle(HWND owner);   // "..." > Default file manager (asked first when turning it on)
