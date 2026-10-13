#pragma once
#include "spectrum.h"
#include <memory>
#include <string>
#include <vector>

// The player's visualizations of the sound playing (but the spectrogram, which the player
// draws itself): a child window drawn by a thread of its own with Direct2D on the GPU
// (Direct3D 11; WARP, Windows' software renderer, without one), a frame per refresh of the
// display. Direct3D, Direct2D and DirectWrite are loaded the first time one is shown.
enum VisStyle {
    VIS_SPECTROGRAM, VIS_BARS, VIS_WAVEFORM, VIS_CIRCLE, VIS_PARTICLES, VIS_RIBBONS, VIS_HORIZON, VIS_TUNNEL,
    VIS_COUNT
};
const wchar_t* vis_name(int style);           // "Mirrored bars", ...

struct VisLook {
    int style = VIS_BARS;
    bool full = false;                          // full screen: on black; else on the window's background
    COLORREF bg = 0, text = 0;
    bool dark = true;                           // (the window's theme; full screen is dark)
    int dpi = 96;
    bool operator==(const VisLook&) const = default;
};

// full screen: what's playing, at the bottom left while the controls show (and a moment
// after the track starts)
struct VisCaption {
    std::wstring title, detail;
    std::shared_ptr<const std::vector<uint32_t>> cover;     // premultiplied BGRA, or none
    int coverW = 0, coverH = 0;
};

class Visualizer {
public:
    Visualizer();
    ~Visualizer();
    Visualizer(const Visualizer&) = delete;
    Visualizer& operator=(const Visualizer&) = delete;

    // the window (hidden), in `parent`; `failMsg` is posted to `notify` if the drawing
    // can't start (no Direct3D / Direct2D)
    bool create(HWND parent, HWND notify, UINT failMsg);
    void destroy();                             // (the thread stops; before the parent goes)
    HWND hwnd() const;
    bool failed() const;
    void place(const RECT* r);                  // shown at r (the parent's client coordinates); null: hidden
    void set_look(const VisLook& look);
    void set_source(const SpectrumTap& tap);    // the analysis of what plays; empty: silence
    void set_clock(LONGLONG pos, bool running); // the playback position now, and whether it moves
    void set_caption(const VisCaption& c);
    void show_caption(bool on);
    void announce();                            // the style's name in the middle, a moment
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
