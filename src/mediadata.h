#pragma once
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

// The media player's pure helpers (unit tested): which picture of a folder is the album
// cover, the spectrogram's arithmetic, names of audio formats, and the OpusHead Windows'
// Opus decoder wants made from an MP4 file's 'dOps' box.

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

// --- formats ---
// an audio stream's format by its Media Foundation subtype's first field (WAVE_FORMAT_* or
// FOURCC): "MP3", "AAC", "FLAC", "Opus", ...; empty when not known
std::wstring audio_format_name(unsigned long subtype);
std::wstring fmt_sample_rate(unsigned hz);           // "44.1 kHz", "48 kHz"
std::wstring fmt_channels(unsigned n);               // "Mono", "Stereo", "5.1", "3 channels"
std::wstring fmt_bitrate(unsigned bps);              // "320 kbps"
std::wstring fmt_file_size(unsigned long long b);    // "4.2 MB"

// Opus in MP4: the 19-byte OpusHead (RFC 7845) from the sample description holding the
// 'dOps' box (big-endian there, little-endian in OpusHead); false: no usable box (only
// channel mapping family 0, mono / stereo, is converted)
bool opus_head_from_dops(const uint8_t* desc, size_t size, uint8_t head[19]);
