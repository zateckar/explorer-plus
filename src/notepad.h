#pragma once
#include "appstate.h"

// The Notepad: a window of its own with text files in tabs (new ones, Untitled, too). Its tabs
// are kept as they are, unsaved text included, while it is closed: it opens again with them.
// One per desktop session: a process without one hands the files to the process that has it.

// the window (its tabs as left), with `files` opened in tabs
void notepad_open(const std::vector<std::wstring>& files);
// explorer-plus.exe --notepad [file]: to a Notepad already running, else to one of this
// process. True: this process has the window (it ends when the window closes)
bool notepad_open_file(const std::wstring& path);

HWND notepad_window();                      // this process's Notepad, or nullptr
bool notepad_owns(HWND h);                  // h is the Notepad or one of its windows
bool notepad_translate(MSG* m);             // message loop: the Notepad's keys (true: handled)
