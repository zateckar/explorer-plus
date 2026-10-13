#include "spectrum.h"
#include "media.h"
#include "mediadata.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <deque>
#include <mutex>
#include <thread>

struct SpectrumFeed::Shared {
    std::mutex mx;
    struct Column { LONGLONG time; uint8_t v[kBands]; };
    std::deque<Column> columns;             // from a moment before playback on, by time
    LONGLONG taken = LLONG_MIN;             // the newest column the window has taken
    std::atomic<bool> cancel{ false };
    std::atomic<LONGLONG> pos{ 0 };         // where playback is (the window says)
    std::vector<double> freqs;              // band centres, once the rate is known
    unsigned rate = 0;
    SampleRing ring;                        // the mono samples, the newest few seconds
};

static const LONGLONG kAhead = 20000000;    // the thread stays no more than 2 s ahead
static const LONGLONG kKept = 10000000;     // columns kept 1 s behind playback (the taps)

static void run(std::shared_ptr<SpectrumFeed::Shared> s, std::wstring path, LONGLONG from, int cps) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        AudioDecoder dec;
        if (dec.open(path, from)) {
            const unsigned ch = dec.channels(), rate = dec.rate();
            const int n = rate > 32000 ? 4096 : 2048;
            SpectrumMath math(n, (int)rate, SpectrumFeed::kBands, SpectrumFeed::kMinHz, SpectrumFeed::kMaxHz);
            {
                std::lock_guard<std::mutex> lk(s->mx);
                for (int b = 0; b < SpectrumFeed::kBands; ++b) s->freqs.push_back(math.band_freq(b));
                s->rate = rate;
                s->ring.reset((size_t)rate * 4);    // (2 s ahead of playback, and a window behind it)
            }
            std::vector<float> block, mono;         // mono: the samples from `monoStart` on
            long long monoStart = -1;               // its first sample's index (in `rate` units)
            double next = -1;                       // the next column's first sample
            const double hop = (double)rate / cps;
            LONGLONG ts = 0;
            while (!s->cancel && (block.clear(), dec.read(block, &ts))) {
                long long first = (long long)((double)ts * rate / 10000000.0 + 0.5);
                if (monoStart < 0 || first > monoStart + (long long)mono.size() + (long long)rate / 10) {
                    mono.clear();                   // (the start, or a gap: from here)
                    monoStart = first;
                    next = (double)first;
                }
                size_t added = mono.size();
                long long at = monoStart + (long long)mono.size();
                for (size_t i = 0; i + ch <= block.size(); i += ch) {
                    float m = 0;
                    for (unsigned c = 0; c < ch; ++c) m += block[i + c];
                    mono.push_back(m / (float)ch);
                }
                {
                    std::lock_guard<std::mutex> lk(s->mx);
                    s->ring.put(at, mono.data() + added, mono.size() - added);
                }
                // the columns whose window is decoded: each centred on its time
                while (!s->cancel && next + n <= (double)(monoStart + (long long)mono.size())) {
                    SpectrumFeed::Shared::Column col;
                    size_t off = (size_t)((long long)next - monoStart);
                    math.column(mono.data() + off, col.v);
                    col.time = (LONGLONG)((next + n / 2.0) * 10000000.0 / rate);
                    next += hop;
                    {
                        std::lock_guard<std::mutex> lk(s->mx);
                        s->columns.push_back(col);
                    }
                    // not too far ahead of playback (a paused track waits here)
                    while (!s->cancel && col.time > s->pos.load() + kAhead) Sleep(20);
                }
                size_t used = (size_t)std::max(0LL, (long long)next - monoStart);
                if (used > mono.size()) used = mono.size();
                if (used > (size_t)rate) {          // (drop what no window needs any more)
                    mono.erase(mono.begin(), mono.begin() + (long)used);
                    monoStart += (long long)used;
                }
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

void SpectrumFeed::start(const std::wstring& path, LONGLONG from, int columnsPerSecond) {
    stop();
    m_s = std::make_shared<Shared>();
    m_s->pos = from;
    std::thread(run, m_s, path, from, columnsPerSecond > 0 ? columnsPerSecond : 50).detach();
}

void SpectrumFeed::stop() {
    if (m_s) m_s->cancel = true;            // (the thread lets go of its own reference)
    m_s.reset();
}

int SpectrumFeed::take(LONGLONG pos, std::vector<uint8_t>& out) {
    if (!m_s) return 0;
    m_s->pos = pos;
    std::lock_guard<std::mutex> lk(m_s->mx);
    auto& cols = m_s->columns;
    while (!cols.empty() && cols.front().time < pos - kKept) cols.pop_front();
    int n = 0;
    for (auto& c : cols) {
        if (c.time > pos) break;
        if (c.time <= m_s->taken) continue;
        out.insert(out.end(), c.v, c.v + kBands);
        m_s->taken = c.time;
        ++n;
    }
    return n;
}

double SpectrumFeed::band_freq(int band) const {
    if (!m_s) return 0;
    std::lock_guard<std::mutex> lk(m_s->mx);
    return band >= 0 && band < (int)m_s->freqs.size() ? m_s->freqs[band] : 0;
}

SpectrumTap SpectrumFeed::tap() const {
    SpectrumTap t;
    t.m_s = m_s;
    return t;
}

bool SpectrumTap::bands(LONGLONG t, float out[SpectrumFeed::kBands]) const {
    if (!m_s) return false;
    const int B = SpectrumFeed::kBands;
    const LONGLONG kNear = 1000000;         // (a column further than 100 ms away isn't "now")
    std::lock_guard<std::mutex> lk(m_s->mx);
    auto& cols = m_s->columns;
    auto it = std::upper_bound(cols.begin(), cols.end(), t, [](LONGLONG v, const auto& c) { return v < c.time; });
    const SpectrumFeed::Shared::Column* a = it != cols.begin() ? &*(it - 1) : nullptr;     // at or before t
    const SpectrumFeed::Shared::Column* b = it != cols.end() ? &*it : nullptr;             // after it
    if (a && b && b->time - a->time <= 2 * kNear) {
        float f = (float)(t - a->time) / (float)(b->time - a->time);
        for (int i = 0; i < B; ++i) out[i] = a->v[i] + (b->v[i] - a->v[i]) * f;
        return true;
    }
    const SpectrumFeed::Shared::Column* c = a && t - a->time <= kNear ? a : b && b->time - t <= kNear ? b : nullptr;
    if (!c) return false;
    for (int i = 0; i < B; ++i) out[i] = c->v[i];
    return true;
}

size_t SpectrumTap::samples(LONGLONG t, float* out, size_t n) const {
    if (!m_s) { std::fill(out, out + n, 0.0f); return 0; }
    std::lock_guard<std::mutex> lk(m_s->mx);
    if (!m_s->rate) { std::fill(out, out + n, 0.0f); return 0; }
    long long end = (long long)((double)t * m_s->rate / 10000000.0);
    return m_s->ring.get(end, out, n);
}

unsigned SpectrumTap::rate() const {
    if (!m_s) return 0;
    std::lock_guard<std::mutex> lk(m_s->mx);
    return m_s->rate;
}
