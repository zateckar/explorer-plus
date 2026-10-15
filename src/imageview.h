#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The player's pictures: which files are pictures, and their decoding (WIC) on a thread of
// its own, fitted to a box (the monitor), turned as the camera says (EXIF orientation),
// the next one decoded ahead.

// a picture Windows can decode (its WIC decoders' file types; HEIC, WebP, camera raw files
// with their codecs installed)
bool image_file(const std::wstring& path);

struct ImageNote {
    unsigned id = 0;
    std::wstring path;
    std::shared_ptr<const std::vector<uint32_t>> pixels;     // premultiplied BGRA, w x h; null: couldn't
    int w = 0, h = 0;                // as decoded (fits the box asked for)
    int fullW = 0, fullH = 0;        // the picture's own size, turned upright
};

// the whole picture at its own size, upright (COM initialised on the calling thread)
bool image_decode_full(const std::wstring& path, ImageNote& out);

// decodes `path` (fitting maxW x maxH; never enlarged) and posts `msg` with lParam an
// ImageNote* (the window deletes it); then decodes `next` ahead. A newer request replaces
// one not started yet.
void image_request(HWND notify, UINT msg, unsigned id, const std::wstring& path, int maxW, int maxH,
                   const std::wstring& next);
