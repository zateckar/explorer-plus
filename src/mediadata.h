#pragma once
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

// The media player's pure helpers (unit tested): which picture of a folder is the album
// cover, the spectrogram's arithmetic, names of audio formats, subtitle files read, and the
// OpusHead Windows' Opus decoder wants made from an MP4 file's 'dOps' box.

// --- album cover ---
struct ImageFile {
    std::wstring name;              // file name (no folder)
    unsigned long long size = 0;    // bytes
};
bool is_image_name(const std::wstring& name);      // .jpg .jpeg .jfif .png .bmp .gif .webp .tif .tiff
// cover.* / folder.* / front.* / album.* / AlbumArt*Large.* / AlbumArt*.* in that order (the
// bigger file of two equally named), else the biggest picture; -1: none
int pick_cover(const std::vector<ImageFile>& files);

// --- spectrogram ---
// Columns of `bands()` intensities (0..255, low frequencies first) from windows of `size()`
// mono samples: a Hann window, an FFT, log-spaced bands from `fmin` to `fmax`, -80..0 dBFS
class SpectrumMath {
public:
    SpectrumMath(int fftSize, int sampleRate, int bands, double fmin = 30, double fmax = 18000);
    int size() const { return m_n; }
    int bands() const { return (int)m_lo.size(); }
    void column(const float* samples, uint8_t* out);     // `size()` samples -> `bands()` values
    double band_freq(int band) const;                     // its centre frequency (Hz)
private:
    int m_n, m_rate;
    std::vector<float> m_window;
    std::vector<std::complex<float>> m_buf, m_twiddle;
    std::vector<int> m_rev;
    std::vector<float> m_mag;
    std::vector<double> m_lo, m_hi;                     // each band's edges, in FFT bins
};

// --- visualizations ---
// The newest samples of a stream by their number (at the stream's rate), for the waveform
// visualizations; samples not held (a gap, before the start, past the end) read as silence
class SampleRing {
public:
    explicit SampleRing(size_t capacity = 0) { reset(capacity); }
    void reset(size_t capacity);
    void put(long long first, const float* s, size_t n);    // samples first .. first + n - 1
    // the n samples before `end` (end - n .. end - 1) into `out`; how many of them were held
    size_t get(long long end, float* out, size_t n) const;
    long long end() const { return m_end; }
private:
    std::vector<float> m_buf;
    long long m_end = 0;                            // one past the newest sample
    size_t m_held = 0;                              // how many before m_end are held
};
// a band's intensity (0..255 as SpectrumMath makes it, -80..0 dB) as a visualization's
// level, 0..1: -62 dB and less nothing, -12 dB and more all
float vis_level(float v255);
// `n` bars from bands `from` .. `to` - 1 (log-spaced, low first), spread evenly over them:
// each the loudest band of its share, or (more bars than bands) between two in proportion
void vis_bars(const float* bands, int from, int to, float* bars, int n);
// how loud (dB, RMS) the last `tail` of `n` samples are in a band: below `hz` (high false)
// or above it, through a second-order filter the samples before warm up; -120: silence
float band_db(const float* s, size_t n, size_t tail, unsigned rate, float hz, bool high);
// Beats in a loudness (dB, e.g. the bass's) given frame by frame: a quick rise well over
// its recent average, after it fell back from the last one (or a while went by)
class BeatDetector {
public:
    bool update(float db, float dt);                // dt: seconds since the last; true: a beat now
    float level() const { return m_level; }         // 0..1: within 36 dB under the recent peak
    float punch() const { return m_punch; }         // 0..1: how far over its average it is now
private:
    float m_peak = -120, m_level = 0, m_avg = 0, m_dev = 0.05f, m_floor = 0, m_punch = 0;
    float m_cool = 0, m_since = 10;
    bool m_armed = true, m_started = false;
};

// --- loudness ---
// A track's loudness (LUFS, ITU-R BS.1770: K-weighting, 400 ms blocks every 100 ms,
// gated at -70 LUFS and 10 LU under the mean), from all of it or from excerpts: `cut()`
// between two (no block spans the jump). Channels weigh the same (no surround weights).
class LoudnessMeter {
public:
    LoudnessMeter(unsigned rate, unsigned channels);
    void add(const float* interleaved, size_t frames);
    void cut();
    double lufs() const;                             // -HUGE_VAL: nothing above the gates
private:
    struct Biquad { double b0, b1, b2, a1, a2; };
    Biquad m_shelf{}, m_highpass{};
    std::vector<double> m_state;                    // per channel: 2 filters x 2 delays
    unsigned m_channels;
    size_t m_subLen;                                // frames per 100 ms
    size_t m_subFill = 0;
    double m_subSum = 0;                            // of the 100 ms being filled
    std::vector<double> m_recent;                   // the last (up to 3) sub-blocks since a cut
    std::vector<double> m_blocks;                   // mean squares of the 400 ms blocks
};
// the gain (dB) bringing a track of `lufs` to `target`, within [-20, +12]; 0: not known
double loudness_gain_db(double lufs, double target = -14.0);

// --- formats ---
// an audio stream's format by its Media Foundation subtype's first field (WAVE_FORMAT_* or
// FOURCC): "MP3", "AAC", "FLAC", "Opus", ...; empty when not known
std::wstring audio_format_name(unsigned long subtype);
std::wstring fmt_sample_rate(unsigned hz);           // "44.1 kHz", "48 kHz"
std::wstring fmt_channels(unsigned n);               // "Mono", "Stereo", "5.1", "3 channels"
std::wstring fmt_bitrate(unsigned bps);              // "320 kbps"
std::wstring fmt_file_size(unsigned long long b);    // "4.2 MB"

// --- subtitles ---
struct SubtitleCue {
    long long start = 0, end = 0;   // 100-ns units
    std::wstring text;              // its lines joined by '\n', markup taken out
};
// SubRip (.srt), WebVTT (.vtt) and SubStation (.ssa / .ass: its Dialogue lines' text) by
// their content; sorted by start
std::vector<SubtitleCue> subtitles_parse(const std::wstring& text);
// the text shown at `t`: every cue on then, in order ("" none)
std::wstring subtitle_at(const std::vector<SubtitleCue>& cues, long long t);
bool subtitle_file(const std::wstring& name);        // .srt .vtt .ass .ssa
// `candidate` (a file name in the video's folder) is a subtitle of `video` (a file name):
// "Film.srt", "Film.en.srt", "Film.English.forced.vtt" for "Film.mkv". `label`: what tells
// it apart ("en", "English.forced"; empty for "Film.srt")
bool subtitle_for(const std::wstring& video, const std::wstring& candidate, std::wstring* label = nullptr);

// Opus in MP4: the 19-byte OpusHead (RFC 7845) from the sample description holding the
// 'dOps' box (big-endian there, little-endian in OpusHead); false: no usable box (only
// channel mapping family 0, mono / stereo, is converted)
bool opus_head_from_dops(const uint8_t* desc, size_t size, uint8_t head[19]);
