#pragma once
#include "shell_utils.h"
#include <cstdint>
#include <memory>
#include <vector>

class SpectrumTap;

// The spectrogram's analysis: a thread of its own decodes the playing file apart from
// playback (AudioDecoder) and turns it into columns of band intensities, a fixed number
// per second, a little ahead of where playback is (no further: a paused track holds it).
// The window takes the columns as playback reaches them; the visualizations read them,
// and the samples, through a tap.
class SpectrumFeed {
public:
    static const int kBands = 160;          // per column, low frequencies first, 0..255
    static constexpr double kMinHz = 40, kMaxHz = 18000;    // the bands' span (log spaced)

    SpectrumFeed() = default;
    ~SpectrumFeed() { stop(); }
    SpectrumFeed(const SpectrumFeed&) = delete;
    SpectrumFeed& operator=(const SpectrumFeed&) = delete;

    // (re)starts at `from` (100-ns units); the previous analysis is dropped
    void start(const std::wstring& path, LONGLONG from, int columnsPerSecond);
    void stop();
    bool active() const { return (bool)m_s; }
    // the columns up to playback position `pos` not taken before, appended to `out`
    // (kBands bytes each); their number. Also tells the thread where playback is.
    int take(LONGLONG pos, std::vector<uint8_t>& out);
    double band_freq(int band) const;        // centre frequency (Hz) of a band; 0: not known yet
    SpectrumTap tap() const;                 // (of this analysis: a new start needs a new tap)

    struct Shared;                           // (spectrum.cpp's: the thread's and the window's)
private:
    std::shared_ptr<Shared> m_s;
};

// An analysis as the visualizations read it, from any thread: the bands at a playback time
// (between two columns: in proportion) and the samples up to it. Empty: nothing playing.
class SpectrumTap {
public:
    SpectrumTap() = default;
    explicit operator bool() const { return (bool)m_s; }
    bool operator==(const SpectrumTap& o) const { return m_s == o.m_s; }
    bool bands(LONGLONG t, float out[SpectrumFeed::kBands]) const;    // 0..255; false: none near t
    // the n mono samples up to t (-1..1); how many of them are known (the others: 0)
    size_t samples(LONGLONG t, float* out, size_t n) const;
    unsigned rate() const;                   // the samples' (0: not known yet)
private:
    friend class SpectrumFeed;
    std::shared_ptr<SpectrumFeed::Shared> m_s;
};
