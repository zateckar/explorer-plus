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
    if (dur <= 0) {
        take((size_t)rate * 120);               // of unknown length (a stream...): its first two minutes
    } else if (dur <= kExcerpt * kExcerpts * 2) {
        take((size_t)rate * 600);               // short: all of it (10 min at most)
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

// two lanes, a thread each: the track about to play (urgent) isn't held up behind the
// next one's measuring (a long file, one on a slow share)
struct Job { HWND notify; UINT msg; std::wstring path; };
struct Lane {
    std::condition_variable cv;
    std::deque<Job> jobs;
    bool running = false;
};
std::mutex s_mx;
Lane s_lanes[2];                            // [0] urgent, [1] the next tracks

void worker(Lane* lane) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(s_mx);
            lane->cv.wait(lk, [lane] { return !lane->jobs.empty(); });
            job = std::move(lane->jobs.front());
            lane->jobs.pop_front();
        }
        auto* note = new LoudnessNote{ job.path, measure(job.path) };
        if (!PostMessageW(job.notify, job.msg, 0, (LPARAM)note)) delete note;
    }
    (void)co;                               // (the threads live as long as the process)
}

} // namespace

void loudness_request(HWND notify, UINT msg, const std::wstring& path, bool urgent) {
    std::lock_guard<std::mutex> lk(s_mx);
    for (Lane& l : s_lanes)
        l.jobs.erase(std::remove_if(l.jobs.begin(), l.jobs.end(), [&](const Job& j) { return j.path == path; }), l.jobs.end());
    Lane& lane = s_lanes[urgent ? 0 : 1];
    if (urgent) lane.jobs.push_front({ notify, msg, path });
    else lane.jobs.push_back({ notify, msg, path });
    // (skipping through tracks: the first and the latest matter, those between go)
    while (lane.jobs.size() > 4) lane.jobs.erase(lane.jobs.begin() + 1);
    if (!lane.running) {
        try {
            std::thread(worker, &lane).detach();
            lane.running = true;
        } catch (...) {}
    }
    lane.cv.notify_one();
}
