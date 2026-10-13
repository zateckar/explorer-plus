#include "mediadata.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

static std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// ------------------------------------------------------------------
// album cover
// ------------------------------------------------------------------
bool is_image_name(const std::wstring& name) {
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = lower(name.substr(dot + 1));
    for (const wchar_t* e : { L"jpg", L"jpeg", L"jfif", L"png", L"bmp", L"gif", L"webp", L"tif", L"tiff" })
        if (ext == e) return true;
    return false;
}

static int cover_rank(const std::wstring& name) {
    std::wstring s = lower(name);
    size_t dot = s.rfind(L'.');
    std::wstring stem = dot == std::wstring::npos ? s : s.substr(0, dot);
    if (stem == L"cover") return 0;
    if (stem == L"folder") return 1;
    if (stem == L"front") return 2;
    if (stem == L"album") return 3;
    if (stem.rfind(L"albumart", 0) == 0) return stem.find(L"large") != std::wstring::npos ? 4 : 5;
    return 6;                                   // any picture: the biggest
}

int pick_cover(const std::vector<ImageFile>& files) {
    int best = -1, bestRank = 99;
    unsigned long long bestSize = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        if (!is_image_name(files[i].name)) continue;
        int r = cover_rank(files[i].name);
        if (r < bestRank || (r == bestRank && files[i].size > bestSize)) {
            best = (int)i;
            bestRank = r;
            bestSize = files[i].size;
        }
    }
    return best;
}

// ------------------------------------------------------------------
// spectrogram
// ------------------------------------------------------------------
SpectrumMath::SpectrumMath(int fftSize, int sampleRate, int bands, double fmin, double fmax)
    : m_n(fftSize), m_rate(sampleRate > 0 ? sampleRate : 44100) {
    const double pi = 3.14159265358979323846;
    m_window.resize(m_n);
    for (int i = 0; i < m_n; ++i) m_window[i] = (float)(0.5 - 0.5 * std::cos(2 * pi * i / (m_n - 1)));
    m_buf.resize(m_n);
    m_mag.resize(m_n / 2 + 1);
    m_twiddle.resize(m_n / 2);
    for (int i = 0; i < m_n / 2; ++i) m_twiddle[i] = std::polar(1.0f, (float)(-2 * pi * i / m_n));
    int bits = 0;
    while ((1 << bits) < m_n) ++bits;
    m_rev.resize(m_n);
    for (int i = 0; i < m_n; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        m_rev[i] = r;
    }
    fmax = std::min(fmax, m_rate * 0.48);
    if (fmin >= fmax) fmin = fmax / 100;
    double hzPerBin = (double)m_rate / m_n;
    m_lo.resize(bands);
    m_hi.resize(bands);
    for (int b = 0; b < bands; ++b) {
        m_lo[b] = fmin * std::pow(fmax / fmin, (double)b / bands) / hzPerBin;
        m_hi[b] = fmin * std::pow(fmax / fmin, (double)(b + 1) / bands) / hzPerBin;
    }
}

double SpectrumMath::band_freq(int band) const {
    return std::sqrt(m_lo[band] * m_hi[band]) * m_rate / m_n;
}

void SpectrumMath::column(const float* samples, uint8_t* out) {
    for (int i = 0; i < m_n; ++i) m_buf[m_rev[i]] = { samples[i] * m_window[i], 0.0f };
    for (int len = 2; len <= m_n; len <<= 1) {
        int step = m_n / len;
        for (int i = 0; i < m_n; i += len)
            for (int k = 0; k < len / 2; ++k) {
                std::complex<float> t = m_twiddle[k * step] * m_buf[i + k + len / 2];
                m_buf[i + k + len / 2] = m_buf[i + k] - t;
                m_buf[i + k] += t;
            }
    }
    // a full-scale sine reads 0 dB: the Hann window halves the amplitude, the FFT sums n/2
    const float scale = 4.0f / m_n;
    for (int i = 0; i <= m_n / 2; ++i) m_mag[i] = std::abs(m_buf[i]) * scale;
    int last = m_n / 2;
    for (size_t b = 0; b < m_lo.size(); ++b) {
        double lo = m_lo[b], hi = m_hi[b];
        float v = 0;
        if (hi - lo < 1.0) {            // narrower than a bin: the spectrum at its centre
            double c = (lo + hi) / 2;
            int i = std::min((int)c, last - 1);
            double f = c - i;
            v = (float)(m_mag[i] * (1 - f) + m_mag[i + 1] * f);
        } else {
            for (int i = std::max(0, (int)std::ceil(lo)); i <= std::min(last, (int)hi); ++i) v = std::max(v, m_mag[i]);
        }
        double db = 20 * std::log10(std::max(v, 1e-9f));
        double t = (db + 80) / 80;
        out[b] = (uint8_t)std::lround(std::clamp(t, 0.0, 1.0) * 255);
    }
}

// ------------------------------------------------------------------
// loudness
// ------------------------------------------------------------------
LoudnessMeter::LoudnessMeter(unsigned rate, unsigned channels) : m_channels(channels ? channels : 1) {
    const double pi = 3.14159265358979323846;
    double fs = rate ? rate : 48000;
    // BS.1770's two filters, designed for this rate (the standard lists 48 kHz only)
    {   // the head's high shelf, +4 dB above ~1.7 kHz
        double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        double K = std::tan(pi * f0 / fs), Vh = std::pow(10.0, G / 20), Vb = std::pow(Vh, 0.4996667741545416);
        double a0 = 1 + K / Q + K * K;
        m_shelf = { (Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                    2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0 };
    }
    {   // the RLB high pass at ~38 Hz
        double f0 = 38.13547087602444, Q = 0.5003270373238773;
        double K = std::tan(pi * f0 / fs), a0 = 1 + K / Q + K * K;
        m_highpass = { 1, -2, 1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0 };
    }
    m_state.assign((size_t)m_channels * 4, 0.0);
    m_subLen = std::max<size_t>(1, (size_t)(fs / 10));
}

void LoudnessMeter::cut() {
    std::fill(m_state.begin(), m_state.end(), 0.0);
    m_subFill = 0;
    m_subSum = 0;
    m_recent.clear();
}

void LoudnessMeter::add(const float* x, size_t frames) {
    for (size_t f = 0; f < frames; ++f) {
        double sum = 0;
        for (unsigned c = 0; c < m_channels; ++c) {
            double* s = &m_state[(size_t)c * 4];
            double in = x[f * m_channels + c];
            // (transposed direct form II, one filter after the other)
            double y = m_shelf.b0 * in + s[0];
            s[0] = m_shelf.b1 * in - m_shelf.a1 * y + s[1];
            s[1] = m_shelf.b2 * in - m_shelf.a2 * y;
            double z = m_highpass.b0 * y + s[2];
            s[2] = m_highpass.b1 * y - m_highpass.a1 * z + s[3];
            s[3] = m_highpass.b2 * y - m_highpass.a2 * z;
            sum += z * z;
        }
        m_subSum += sum;
        if (++m_subFill < m_subLen) continue;
        double sub = m_subSum / (double)m_subLen;
        m_subFill = 0;
        m_subSum = 0;
        if (m_recent.size() == 3) {            // a full 400 ms: these three and this one
            m_blocks.push_back((m_recent[0] + m_recent[1] + m_recent[2] + sub) / 4);
            m_recent.erase(m_recent.begin());
        }
        m_recent.push_back(sub);
    }
}

double LoudnessMeter::lufs() const {
    auto level = [](double ms) { return -0.691 + 10 * std::log10(ms); };
    double sum = 0;
    size_t n = 0;
    for (double b : m_blocks)
        if (b > 0 && level(b) > -70) { sum += b; ++n; }
    if (!n) return -HUGE_VAL;
    double gate = level(sum / (double)n) - 10;
    sum = 0;
    n = 0;
    for (double b : m_blocks)
        if (b > 0 && level(b) > -70 && level(b) > gate) { sum += b; ++n; }
    return n ? level(sum / (double)n) : -HUGE_VAL;
}

double loudness_gain_db(double lufs, double target) {
    if (!std::isfinite(lufs)) return 0;
    return std::clamp(target - lufs, -20.0, 12.0);
}

// ------------------------------------------------------------------
// formats
// ------------------------------------------------------------------
std::wstring audio_format_name(unsigned long subtype) {
    switch (subtype) {
        case 0x0001: return L"PCM";
        case 0x0003: return L"PCM (float)";
        case 0x0050: return L"MPEG Audio";
        case 0x0055: return L"MP3";
        case 0x00FF: case 0x1600: case 0x1610: return L"AAC";
        case 0x0160: case 0x0161: return L"WMA";
        case 0x0162: return L"WMA Pro";
        case 0x0163: return L"WMA Lossless";
        case 0x2000: return L"Dolby Digital";
        case 0x2001: return L"DTS";
        case 0xA7FB87AF: return L"Dolby Digital Plus";
        case 0x704F: case 0x4F707573: return L"Opus";
        case 0xF1AC: return L"FLAC";
        case 0x6C61: case 0x63616C61: return L"ALAC";
        case 0x8D2FD10B: return L"Vorbis";
        case 0x7361: case 0x73616D72: return L"AMR";
    }
    return {};
}

std::wstring fmt_sample_rate(unsigned hz) {
    if (!hz) return {};
    wchar_t b[32];
    if (hz % 1000 == 0) swprintf(b, 32, L"%u kHz", hz / 1000);
    else swprintf(b, 32, L"%.1f kHz", hz / 1000.0);
    return b;
}

std::wstring fmt_channels(unsigned n) {
    switch (n) {
        case 0: return {};
        case 1: return L"Mono";
        case 2: return L"Stereo";
        case 6: return L"5.1";
        case 8: return L"7.1";
    }
    return std::to_wstring(n) + L" channels";
}

std::wstring fmt_bitrate(unsigned bps) {
    if (!bps) return {};
    return std::to_wstring((bps + 500) / 1000) + L" kbps";
}

std::wstring fmt_file_size(unsigned long long b) {
    wchar_t s[32];
    if (b < 1024) swprintf(s, 32, L"%llu bytes", b);
    else if (b < 1024 * 1024) swprintf(s, 32, L"%.0f KB", b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) swprintf(s, 32, L"%.1f MB", b / (1024.0 * 1024));
    else swprintf(s, 32, L"%.2f GB", b / (1024.0 * 1024 * 1024));
    return s;
}

bool opus_head_from_dops(const uint8_t* desc, size_t size, uint8_t head[19]) {
    for (size_t i = 4; i + 4 + 11 <= size; ++i) {
        if (std::memcmp(desc + i, "dOps", 4) != 0) continue;
        const uint8_t* d = desc + i + 4;
        if (d[0] != 0 || d[10] != 0 || d[1] < 1 || d[1] > 2) return false;
        const uint8_t h[19] = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, d[1],
                                d[3], d[2],                 // pre-skip
                                d[7], d[6], d[5], d[4],     // input sample rate
                                d[9], d[8],                 // output gain
                                0 };                        // channel mapping family
        std::memcpy(head, h, sizeof(h));
        return true;
    }
    return false;
}
