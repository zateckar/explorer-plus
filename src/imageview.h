#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The player's pictures: which files are pictures, and their decoding on a thread of its
// own, fitted to a box (the monitor), turned as the camera says (EXIF orientation), the
// next one decoded ahead. WIC decodes what Windows has codecs for (with GIF and APNG
// animations, TIFF pages, an icon's biggest size); SVG is drawn by Direct2D, EMF / WMF by
// GDI+, and TGA, QOI, Radiance HDR, PSD and PCX by imageformats.cpp.

// a picture shown here (WIC's file types: HEIC, WebP, camera raw files with their codecs
// installed; and the ones above)
bool image_file(const std::wstring& path);

struct ImageNote {
    unsigned id = 0;
    std::wstring path;
    std::shared_ptr<const std::vector<uint32_t>> pixels;     // premultiplied BGRA, w x h; null: couldn't
    int w = 0, h = 0;                // as decoded (fits the box asked for)
    int fullW = 0, fullH = 0;        // the picture's own size, turned upright (a drawing: the size it asks for)
    bool drawing = false;            // SVG, EMF, WMF: drawn sharp at any size asked for
    // an animation (GIF, APNG): every frame as the canvas shows it (w x h; `pixels` is the
    // first) and how long each stays (ms); empty: a still picture
    std::vector<std::shared_ptr<const std::vector<uint32_t>>> frames;
    std::vector<int> delays;
    int page = 0, pages = 1;         // a TIFF's pages: the one decoded, of how many
};

// the whole picture at its own size, upright (COM initialised on the calling thread); an
// animation's first frame, a document's first page
bool image_decode_full(const std::wstring& path, ImageNote& out);

// decodes `path` (fitting maxW x maxH; never enlarged, but for drawings: SVG, EMF, WMF, up
// to `enlarge` times their size) and posts `msg` with lParam an ImageNote* (the window
// deletes it); then decodes `next` ahead. `page`: a TIFF's page. A newer request replaces
// one not started yet.
void image_request(HWND notify, UINT msg, unsigned id, const std::wstring& path, int maxW, int maxH,
                   const std::wstring& next, int page = 0, double enlarge = 4.0);
