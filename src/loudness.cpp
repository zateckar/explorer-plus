#include "loudness.h"
#include "media.h"
#include "mediadata.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {

double measure(const std::wstring& path) {
    AudioDecoder dec;
    if (!dec.open(path, 0)) return NAN;
    const unsigned ch = dec.channels(), rate = dec.rate();
    LoudnessMeter meter(rate, ch);
    std::vector<float> buf;
    LONGLONG ts = 0;
    auto take = [&](size_t frames) {          // the next `frames` frames into the meter
        size_t got = 0;
        while (got < frames) {
            buf.clear();
            if (!dec.read(buf, &ts)) return false;
            size_t n = buf.size() / ch;
            meter.add(buf.data(), n);
            got += n;
        }
        return true;
    };
    const int kExcerpts = 12;
    const LONGLONG kExcerpt = 30000000, dur = dec.duration();     // 3 s
    if (dur <= kExcerpt * kExcerpts * 2) {
        take((size_t)rate * 600);               // short, or of unknown length: all of it (10 min at most)
    } else {
        // spread over the track, its first and last 5 % left out (intro, fade-out)
        for (int k = 0; k < kExcerpts; ++k) {
            LONGLONG at = dur / 20 + (dur * 9 / 10 - kExcerpt) * k / (kExcerpts - 1);
            if (!dec.seek(at)) break;
            meter.cut();
            if (!take((size_t)rate * 3)) break;
        }
    }
    return meter.lufs();
}

struct Job { HWND notify; UINT msg; std::wstring path; };
std::mutex s_mx;
std::condition_variable s_cv;
std::deque<Job> s_jobs;
bool s_running = false;

void worker() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(s_mx);
            s_cv.wait(lk, [] { return !s_jobs.empty(); });
            job = std::move(s_jobs.front());
            s_jobs.pop_front();
        }
        auto* note = new LoudnessNote{ job.path, measure(job.path) };        if (!PostMessageW(job.notify, job.msg, 0, (LPARAM)note)) delete note;
    }
    (void)co;                               // (the thread lives as long as the process)
}

} // namespace

void loudness_request(HWND notify, UINT msg, const std::wstring& path, bool urgent) {
    std::lock_guard<std::mutex> lk(s_mx);
    s_jobs.erase(std::remove_if(s_jobs.begin(), s_jobs.end(), [&](const Job& j) { return j.path == path; }), s_jobs.end());
    if (urgent) s_jobs.push_front({ notify, msg, path });
    else s_jobs.push_back({ notify, msg, path });
    // (skipping through tracks: the first and the latest matter, those between go)
    while (s_jobs.size() > 4) s_jobs.erase(s_jobs.begin() + 1);
    if (!s_running) {
        s_running = true;
        std::thread(worker).detach();
    }
    s_cv.notify_one();
}
