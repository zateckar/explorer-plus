#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// The picture editor: a window of its own with a picture (a file, or a screenshot not saved
// yet) and rectangles, ellipses and text drawn over it. Those stay objects - picked, moved,
// resized, recoloured, removed, undone - while the window is open; Save writes the picture
// with them drawn in, Copy puts it on the clipboard.

void imageedit_open_file(const std::wstring& path);
// a screenshot: `pixels` BGRA (opaque), w x h; `name` its title until it is saved
void imageedit_open_pixels(std::vector<uint32_t> pixels, int w, int h, const std::wstring& name);

bool imageedit_any();                    // an editor window is open
bool imageedit_owns(HWND h);             // h is an editor or one of its windows (its keys are its own)
