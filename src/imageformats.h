#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pictures Windows has no decoder for, decoded here (pure, unit tested): Truevision TGA,
// QOI, Radiance HDR (tone mapped), Photoshop's PSD / PSB composite image, ZSoft PCX. Also
// what animations need: an APNG's frames as PNGs of their own (for WIC to decode), the
// frames of a GIF or APNG put together as the canvas shows them, and an SVG's size.

struct RawImage {
    int w = 0, h = 0;
    std::vector<uint32_t> px;       // premultiplied BGRA, top-down, w x h
};

// by the file's extension (".tga", ".qoi", ".hdr", ".pic", ".psd", ".psb", ".pcx"):
// decoded here
bool own_image_ext(const std::wstring& ext);
// `ext` as own_image_ext takes it; false: not that format, or damaged
bool own_image_decode(const std::wstring& ext, const uint8_t* data, size_t size, RawImage& out);
bool tga_decode(const uint8_t* data, size_t size, RawImage& out);
bool qoi_decode(const uint8_t* data, size_t size, RawImage& out);
bool hdr_decode(const uint8_t* data, size_t size, RawImage& out);
bool psd_decode(const uint8_t* data, size_t size, RawImage& out);
bool pcx_decode(const uint8_t* data, size_t size, RawImage& out);

// --- animations ---
// a frame as the file has it: its place on the canvas, how it's drawn, what's done with
// it before the next one
struct AnimFrame {
    int x = 0, y = 0, w = 0, h = 0;
    const uint32_t* px = nullptr;   // premultiplied BGRA, w x h
    enum Dispose { Keep, Clear, Restore } dispose = Keep;     // after it: left, cleared, the canvas before it back
    bool over = true;               // drawn over the canvas (else its pixels replace it)
};
// the canvas as each frame leaves it, starting see-through
class AnimCanvas {
public:
    AnimCanvas(int w, int h);
    const std::vector<uint32_t>& add(const AnimFrame& f);     // the canvas with `f` drawn
    int w() const { return m_w; }
    int h() const { return m_h; }
private:
    int m_w, m_h;
    std::vector<uint32_t> m_px, m_saved;
    AnimFrame m_last;               // (its pixels not kept: only its place and disposal)
    bool m_any = false;
};

// An APNG's frames, each as a PNG of its own (the header, the chunks before the image
// data, the frame's data); false: a still PNG (no acTL), or damaged
struct ApngFrame {
    int x = 0, y = 0, w = 0, h = 0;
    int delayMs = 0;
    AnimFrame::Dispose dispose = AnimFrame::Keep;
    bool over = false;
    std::vector<uint8_t> png;
};
bool apng_split(const uint8_t* data, size_t size, int& canvasW, int& canvasH, std::vector<ApngFrame>& frames);
// how long a frame shows (ms): what the file says, but browsers' 100 ms for 0..10 ms
int anim_delay(int ms);

// --- SVG ---
// the size an SVG asks for (CSS pixels) from its root element's width, height and viewBox;
// 300 x 150 when it says nothing (as browsers). false: no <svg> element
bool svg_size(const std::string& text, double& w, double& h);
