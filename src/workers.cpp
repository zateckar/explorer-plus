#include "workers.h"
#include "shell_utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {

constexpr int kMaxThreads = 32;                       // beyond this, jobs queue
constexpr int kMaxBulk = 8;                           // threads running bulk jobs at once
constexpr auto kIdleExit = std::chrono::seconds(30);  // an idle thread ends after this

std::mutex s_mx;
std::condition_variable s_cv;
std::deque<std::function<void()>> s_queue;            // normal jobs: taken first
std::deque<std::function<void()>> s_bulk;
int s_threads = 0;      // running pool threads
int s_idle = 0;         // of those, waiting for a job
int s_bulkRunning = 0;  // of those, running a bulk job
std::atomic<bool> s_stopping{ false };

// jobs an idle thread could take now (under s_mx)
size_t runnable() {
    size_t bulk = std::min(s_bulk.size(), (size_t)std::max(0, kMaxBulk - s_bulkRunning));
    return s_queue.size() + bulk;
}

void run_job(std::function<void()>& job) {
    try {
        job();
    } catch (...) {
        // a detached std::thread would have called std::terminate here
        OutputDebugStringW(L"explorer-plus: a background job threw; dropped\n");
    }
}

void pool_thread() {
    ComInit com;
    std::unique_lock<std::mutex> lk(s_mx);
    for (;;) {
        ++s_idle;
        bool got = s_cv.wait_for(lk, kIdleExit, [] { return runnable() > 0 || s_stopping; });
        --s_idle;
        if (s_stopping || !got) break;
        bool bulk = s_queue.empty();
        auto& q = bulk ? s_bulk : s_queue;
        std::function<void()> job = std::move(q.front());
        q.pop_front();
        if (bulk) ++s_bulkRunning;
        lk.unlock();
        run_job(job);
        job = nullptr;      // captured state is freed outside the lock
        lk.lock();
        if (bulk) {
            --s_bulkRunning;
            if (!s_bulk.empty()) s_cv.notify_one();    // a waiting bulk job may go now
        }
    }
    --s_threads;
}

}  // namespace

void worker_submit(std::function<void()> job, JobKind kind) {
    if (!job || s_stopping) return;
    bool spawn = false;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        (kind == JobKind::Bulk ? s_bulk : s_queue).push_back(std::move(job));
        // every idle thread already has a job waiting for it: start one more
        if ((size_t)s_idle < runnable() && s_threads < kMaxThreads) {
            ++s_threads;
            spawn = true;
        }
    }
    if (spawn) {
        try {
            std::thread(pool_thread).detach();
        } catch (...) {     // no thread now: an existing one (or the next submit) takes it
            std::lock_guard<std::mutex> lk(s_mx);
            --s_threads;
        }
    }
    s_cv.notify_one();
}

void workers_shutdown() {
    std::deque<std::function<void()>> dropped, droppedBulk;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        s_stopping = true;
        dropped.swap(s_queue);
        droppedBulk.swap(s_bulk);
    }
    s_cv.notify_all();
    // (dropped jobs' captures are freed here, outside the lock)
}

bool workers_stopping() { return s_stopping; }
