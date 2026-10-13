#pragma once
#include "shell_utils.h"
#include <cstdint>
#include <memory>
#include <vector>

// The spectrogram's analysis: a thread of its own decodes the playing file apart from
// playback (AudioDecoder) and turns it into columns of band intensities, a fixed number
// per second, a little ahead of where playback is (no further: a paused track holds it).
// The window takes the columns as playback reaches them.
class SpectrumFeed {
public:
    static const int kBands = 160;          // per column, low frequencies first, 0..255

    SpectrumFeed() = default;
    ~SpectrumFeed() { stop(); }
    SpectrumFeed(const SpectrumFeed&) = delete;
    SpectrumFeed& operator=(const SpectrumFeed&) = delete;

    // (re)starts at `from` (100-ns units); the previous analysis is dropped
    void start(const std::wstring& path, LONGLONG from, int columnsPerSecond);
    void stop();
    bool active() const { return (bool)m_s; }
    // the columns up to playback position `pos`, appended to `out` (kBands bytes each);
    // their number. Also tells the thread where playback is.
    int take(LONGLONG pos, std::vector<uint8_t>& out);
    double band_freq(int band) const;        // centre frequency (Hz) of a band; 0: not known yet

    struct Shared;                           // (spectrum.cpp's: the thread's and the window's)
private:
    std::shared_ptr<Shared> m_s;
};
