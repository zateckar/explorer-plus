#include "spectrum.h"
#include "media.h"
#include "mediadata.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

struct SpectrumFeed::Shared {
    std::mutex mx;
    struct Column { LONGLONG time; uint8_t v[kBands]; };
    std::deque<Column> columns;             // made, not taken yet
    std::atomic<bool> cancel{ false };
    std::atomic<LONGLONG> pos{ 0 };         // where playback is (the window says)
    std::vector<double> freqs;              // band centres, once the rate is known
};

static const LONGLONG kAhead = 20000000;    // the thread stays no more than 2 s ahead

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
                for (size_t i = 0; i + ch <= block.size(); i += ch) {
                    float m = 0;
                    for (unsigned c = 0; c < ch; ++c) m += block[i + c];
                    mono.push_back(m / (float)ch);
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
    int n = 0;
    while (!m_s->columns.empty() && m_s->columns.front().time <= pos) {
        out.insert(out.end(), m_s->columns.front().v, m_s->columns.front().v + kBands);
        m_s->columns.pop_front();
        ++n;
    }
    return n;
}

double SpectrumFeed::band_freq(int band) const {
    if (!m_s) return 0;
    std::lock_guard<std::mutex> lk(m_s->mx);
    return band >= 0 && band < (int)m_s->freqs.size() ? m_s->freqs[band] : 0;
}
