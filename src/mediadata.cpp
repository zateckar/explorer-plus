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
// visualizations
// ------------------------------------------------------------------
void SampleRing::reset(size_t capacity) {
    m_buf.assign(capacity, 0.0f);
    m_end = 0;
    m_held = 0;
}

void SampleRing::put(long long first, const float* s, size_t n) {
    const size_t cap = m_buf.size();
    if (!cap || !n) return;
    long long last = first + (long long)n;
    if (first < 0) {                                        // (before the start: not kept)
        if (last <= 0) return;
        s += -first;
        n = (size_t)last;
        first = 0;
    }
    if (last <= m_end - (long long)m_held) return;          // (older than all held)
    if (first < m_end - (long long)m_held || first > m_end + (long long)cap) {
        m_end = first;                                      // (a jump: from here on)
        m_held = 0;
    }
    while (m_end < first) {                                 // a gap: silence
        m_buf[(size_t)(m_end % (long long)cap)] = 0.0f;
        ++m_end;
        m_held = std::min(cap, m_held + 1);
    }
    for (long long i = std::max(first, m_end - (long long)m_held); i < last; ++i)
        m_buf[(size_t)(i % (long long)cap)] = s[i - first];
    if (last > m_end) {
        m_held = std::min(cap, m_held + (size_t)(last - m_end));
        m_end = last;
    }
}

size_t SampleRing::get(long long end, float* out, size_t n) const {
    const long long cap = (long long)m_buf.size(), oldest = m_end - (long long)m_held;
    size_t got = 0;
    for (size_t k = 0; k < n; ++k) {
        long long i = end - (long long)n + (long long)k;
        if (i >= oldest && i < m_end && i >= 0) { out[k] = m_buf[(size_t)(i % cap)]; ++got; }
        else out[k] = 0.0f;
    }
    return got;
}

float vis_level(float v255) {
    double db = v255 / 255.0 * 80 - 80;
    return (float)std::clamp((db + 62) / 50, 0.0, 1.0);
}

void vis_bars(const float* bands, int from, int to, float* bars, int n) {
    if (n <= 0) return;
    double span = (double)(to - from) / n;
    for (int k = 0; k < n; ++k) {
        double lo = from + span * k, hi = lo + span;
        float v = 0;
        if (span < 1.0) {                                   // (narrower than a band: between two)
            double c = (lo + hi) / 2 - 0.5;
            int i = std::clamp((int)std::floor(c), from, to - 1);
            int j = std::min(i + 1, to - 1);
            double f = std::clamp(c - i, 0.0, 1.0);
            v = (float)(bands[i] * (1 - f) + bands[j] * f);
        } else {
            for (int i = (int)lo; i < std::min(to, (int)std::ceil(hi)); ++i) v = std::max(v, bands[i]);
        }
        bars[k] = v;
    }
}

float band_db(const float* s, size_t n, size_t tail, unsigned rate, float hz, bool high) {
    if (!n || !rate) return -120;
    tail = std::clamp<size_t>(tail, 1, n);
    // (RBJ's cookbook: Butterworth, Q 0.707)
    const double pi = 3.14159265358979323846, w = 2 * pi * std::min<double>(hz, rate * 0.45) / rate;
    const double cw = std::cos(w), al = std::sin(w) / (2 * 0.70710678), a0 = 1 + al;
    const double b0 = (high ? (1 + cw) / 2 : (1 - cw) / 2) / a0, b1 = (high ? -(1 + cw) : 1 - cw) / a0, b2 = b0;
    const double a1 = -2 * cw / a0, a2 = (1 - al) / a0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0, sum = 0;
    for (size_t i = 0; i < n; ++i) {
        double x = s[i], y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        if (i >= n - tail) sum += y * y;
    }
    double ms = sum / (double)tail;
    return ms > 1e-12 ? (float)(10 * std::log10(ms)) : -120.f;
}

bool BeatDetector::update(float db, float dt) {
    dt = std::clamp(dt, 0.001f, 0.1f);
    // the level: the loudness under the peak of late (a quiet track as lively as a loud one)
    m_peak = std::max(db, m_peak - dt * 4);
    m_level = std::clamp((db - m_peak + 36) / 36, 0.f, 1.f);
    float lvl = m_level;
    if (!m_started) { m_started = true; m_avg = m_floor = lvl; }
    float thr = std::max(0.06f, 1.5f * m_dev);
    float rise = lvl - m_floor;                     // (over the lowest of the last ~50 ms)
    m_cool -= dt;
    m_since += dt;
    if (lvl < m_avg + thr * 0.5f || m_since > 0.4f) m_armed = true;
    bool beat = m_armed && m_cool <= 0 && lvl > m_avg + thr && rise > thr * 0.6f;
    if (beat) {
        m_armed = false;
        m_cool = 0.12f;
        m_since = 0;
    }
    m_punch = std::clamp((lvl - m_avg) / (thr * 2.5f), 0.f, 1.f);
    m_avg += (lvl - m_avg) * (1 - std::exp(-dt / 0.6f));
    m_dev += (std::fabs(lvl - m_avg) - m_dev) * (1 - std::exp(-dt / 1.5f));
    m_floor = std::min(lvl, m_floor + dt * 3);      // (follows down at once, up slowly: a slow swell isn't a beat)
    return beat;
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
        case 0x4D494449: return L"MIDI";                // (media.cpp's own labels, MCI's devices)
        case 0x43444441: return L"CD audio";
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

// ------------------------------------------------------------------
// subtitles
// ------------------------------------------------------------------
static std::wstring trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n\xFEFF");
    if (a == std::wstring::npos) return {};
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::vector<std::wstring> split_lines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    size_t i = 0;
    while (i <= text.size()) {
        size_t e = text.find(L'\n', i);
        if (e == std::wstring::npos) e = text.size();
        std::wstring l = text.substr(i, e - i);
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        lines.push_back(std::move(l));
        i = e + 1;
    }
    return lines;
}

// "01:02:03,456", "02:03.456", "1:02:03.45" (hundredths) -> 100-ns units; -1: not a time
static long long sub_time(const std::wstring& s) {
    std::wstring t = trim(s);
    long long parts[3] = {}, frac = 0;
    int n = 0, fracDigits = 0, digits = 0;
    bool inFrac = false;
    for (wchar_t c : t) {
        if (c >= L'0' && c <= L'9') {
            if (inFrac) {
                if (fracDigits < 7) { frac = frac * 10 + (c - L'0'); ++fracDigits; }
            } else {
                if (digits >= 9) return -1;         // (no time has more: and no overflow)
                parts[n] = parts[n] * 10 + (c - L'0');
                ++digits;
            }
        } else if (c == L':' && !inFrac && digits && n < 2) {
            ++n;
            digits = 0;
        } else if ((c == L',' || c == L'.') && !inFrac && digits) {
            inFrac = true;
        } else {
            return -1;
        }
    }
    if (!digits || n < 1) return -1;
    long long sec = n == 2 ? parts[0] * 3600 + parts[1] * 60 + parts[2] : parts[0] * 60 + parts[1];
    for (int i = fracDigits; i < 7; ++i) frac *= 10;
    return sec * 10000000 + frac;
}

static void put_entity(std::wstring& out, const std::wstring& name) {
    if (name == L"amp") out += L'&';
    else if (name == L"lt") out += L'<';
    else if (name == L"gt") out += L'>';
    else if (name == L"quot") out += L'"';
    else if (name == L"apos") out += L'\'';
    else if (name == L"nbsp") out += L' ';
    else if (name.size() > 1 && name[0] == L'#') {
        unsigned long v = name[1] == L'x' || name[1] == L'X' ? wcstoul(name.c_str() + 2, nullptr, 16)
                                                            : wcstoul(name.c_str() + 1, nullptr, 10);
        if (v && v < 0x10000) out += (wchar_t)v;
    } else {
        out += L'&' + name + L';';
    }
}

// a SubRip / WebVTT cue's line: <i>, <font ...>, <c.yellow>, {\an8} taken out, entities read
static std::wstring srt_clean(const std::wstring& s) {
    std::wstring out;
    // (once a closing '>' or '}' isn't found, none is further on: not looked for again)
    bool noGt = false, noBrace = false;
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'<' && !noGt) {
            size_t e = s.find(L'>', i);
            if (e != std::wstring::npos) { i = e; continue; }
            noGt = true;
        } else if (c == L'{' && !noBrace && i + 1 < s.size() && s[i + 1] == L'\\') {
            size_t e = s.find(L'}', i);
            if (e != std::wstring::npos) { i = e; continue; }
            noBrace = true;
        } else if (c == L'&') {
            size_t e = s.substr(i, 9).find(L';');               // (looked for close by only)
            if (e != std::wstring::npos) e += i;
            if (e != std::wstring::npos) {
                put_entity(out, s.substr(i + 1, e - i - 1));
                i = e;
                continue;
            }
        }
        out += c;
    }
    return out;
}

// a SubStation line's text: {override blocks} and drawings ({\p1} .. {\p0}) taken out, \N
// and \n line breaks, \h a space
static std::wstring ass_clean(const std::wstring& s) {
    std::wstring out;
    bool drawing = false, noBrace = false;
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'{' && !noBrace) {
            size_t e = s.find(L'}', i);
            if (e == std::wstring::npos) noBrace = true;      // (none further on either)
            else {
                std::wstring block = s.substr(i, e - i);
                for (size_t p = block.find(L"\\p"); p != std::wstring::npos; p = block.find(L"\\p", p + 2))
                    if (p + 2 < block.size() && iswdigit(block[p + 2])) drawing = block[p + 2] != L'0';
                i = e;
                continue;
            }
        }
        if (drawing) continue;
        if (c == L'\\' && i + 1 < s.size()) {
            wchar_t d = s[i + 1];
            if (d == L'N' || d == L'n') { out += L'\n'; ++i; continue; }
            if (d == L'h') { out += L' '; ++i; continue; }
        }
        out += c;
    }
    return out;
}

static void add_cue(std::vector<SubtitleCue>& cues, long long start, long long end, std::wstring text) {
    // no empty lines, no blanks around them
    std::wstring joined;
    for (auto& l : split_lines(text)) {
        std::wstring t = trim(l);
        if (t.empty()) continue;
        if (!joined.empty()) joined += L'\n';
        joined += t;
    }
    if (joined.empty() || end <= start) return;
    cues.push_back({ start, end, std::move(joined) });
}

static void parse_ass(const std::vector<std::wstring>& lines, std::vector<SubtitleCue>& cues) {
    int fStart = 1, fEnd = 2, fields = 10;                 // the usual ASS Format
    bool events = false;
    for (auto& raw : lines) {
        std::wstring l = trim(raw);
        if (!l.empty() && l[0] == L'[') { events = lower(l) == L"[events]"; continue; }
        size_t colon = l.find(L':');
        if (colon == std::wstring::npos) continue;
        std::wstring key = lower(trim(l.substr(0, colon)));
        std::wstring rest = l.substr(colon + 1);
        if (key == L"format" && events) {
            int i = 0;
            fStart = fEnd = -1;
            size_t a = 0;
            while (true) {
                size_t e = rest.find(L',', a);
                std::wstring f = lower(trim(rest.substr(a, e == std::wstring::npos ? e : e - a)));
                if (f == L"start") fStart = i;
                else if (f == L"end") fEnd = i;
                ++i;
                if (e == std::wstring::npos) break;
                a = e + 1;
            }
            fields = i;
            if (fStart < 0 || fEnd < 0) { fStart = 1; fEnd = 2; fields = 10; }
        } else if (key == L"dialogue") {
            // the text is the last field and may hold commas itself
            std::vector<std::wstring> f;
            size_t a = 0;
            while ((int)f.size() < fields - 1) {
                size_t e = rest.find(L',', a);
                if (e == std::wstring::npos) break;
                f.push_back(rest.substr(a, e - a));
                a = e + 1;
            }
            if ((int)f.size() < fields - 1 || fStart >= (int)f.size() || fEnd >= (int)f.size()) continue;
            long long s = sub_time(f[fStart]), e = sub_time(f[fEnd]);
            if (s < 0 || e < 0) continue;
            add_cue(cues, s, e, ass_clean(rest.substr(a)));
        }
    }
}

static void parse_srt(const std::vector<std::wstring>& lines, std::vector<SubtitleCue>& cues) {
    for (size_t i = 0; i < lines.size(); ++i) {
        size_t arrow = lines[i].find(L"-->");
        if (arrow == std::wstring::npos) continue;
        long long s = sub_time(lines[i].substr(0, arrow));
        // WebVTT puts the cue's settings after the end time
        std::wstring after = trim(lines[i].substr(arrow + 3));
        size_t sp = after.find_first_of(L" \t");
        long long e = sub_time(sp == std::wstring::npos ? after : after.substr(0, sp));
        std::wstring text;
        size_t j = i + 1;
        for (; j < lines.size() && !trim(lines[j]).empty(); ++j) {
            if (lines[j].find(L"-->") != std::wstring::npos) break;    // a cue with no text
            text += srt_clean(lines[j]) + L'\n';
        }
        if (s >= 0 && e >= 0) add_cue(cues, s, e, text);
        i = j - 1;
    }
}

std::vector<SubtitleCue> subtitles_parse(const std::wstring& text) {
    std::vector<SubtitleCue> cues;
    std::vector<std::wstring> lines = split_lines(text);
    bool ass = false;
    for (auto& l : lines) {
        std::wstring t = lower(trim(l));
        if (t == L"[script info]" || t == L"[events]" || t.rfind(L"dialogue:", 0) == 0) { ass = true; break; }
        if (t.find(L"-->") != std::wstring::npos) break;
    }
    if (ass) parse_ass(lines, cues);
    else parse_srt(lines, cues);
    std::stable_sort(cues.begin(), cues.end(), [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });
    return cues;
}

std::wstring subtitle_at(const std::vector<SubtitleCue>& cues, long long t) {
    std::wstring out;
    for (auto& c : cues) {
        if (c.start > t) break;
        if (t >= c.end) continue;
        if (!out.empty()) out += L'\n';
        out += c.text;
    }
    return out;
}

static std::wstring sub_ext(const std::wstring& name) {
    size_t dot = name.rfind(L'.');
    return dot == std::wstring::npos ? std::wstring() : lower(name.substr(dot + 1));
}

bool subtitle_file(const std::wstring& name) {
    std::wstring ext = sub_ext(name);
    return ext == L"srt" || ext == L"vtt" || ext == L"ass" || ext == L"ssa";
}

bool subtitle_for(const std::wstring& video, const std::wstring& candidate, std::wstring* label) {
    if (!subtitle_file(candidate)) return false;
    size_t vd = video.rfind(L'.'), cd = candidate.rfind(L'.');
    std::wstring stem = lower(vd == std::wstring::npos ? video : video.substr(0, vd));
    std::wstring cstem = candidate.substr(0, cd);
    std::wstring cl = lower(cstem);
    std::wstring rest;
    if (cl == stem || cl == lower(video)) {
        rest.clear();                                       // Film.srt, Film.mkv.srt
    } else if (cl.size() > stem.size() + 1 && cl.compare(0, stem.size(), stem) == 0 && cl[stem.size()] == L'.') {
        rest = cstem.substr(stem.size() + 1);               // Film.en.srt
        if (vd != std::wstring::npos) {                     // Film.mkv.en.srt
            std::wstring vext = lower(video.substr(vd + 1)) + L'.';
            if (lower(rest).rfind(vext, 0) == 0) rest = rest.substr(vext.size());
        }
    } else {
        return false;
    }
    if (label) *label = rest;
    return true;
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
