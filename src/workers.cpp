#include "workers.h"
#include "shell_utils.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {

constexpr int kMaxThreads = 32;                       // beyond this, jobs queue
constexpr auto kIdleExit = std::chrono::seconds(30);  // an idle thread ends after this

std::mutex s_mx;
std::condition_variable s_cv;
std::deque<std::function<void()>> s_queue;
int s_threads = 0;      // running pool threads
int s_idle = 0;         // of those, waiting for a job
std::atomic<bool> s_stopping{ false };

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
        bool got = s_cv.wait_for(lk, kIdleExit, [] { return !s_queue.empty() || s_stopping; });
        --s_idle;
        if (s_stopping || !got) break;
        std::function<void()> job = std::move(s_queue.front());
        s_queue.pop_front();
        lk.unlock();
        run_job(job);
        job = nullptr;      // captured state is freed outside the lock
        lk.lock();
    }
    --s_threads;
}

}  // namespace

void worker_submit(std::function<void()> job) {
    if (!job || s_stopping) return;
    bool spawn = false;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        s_queue.push_back(std::move(job));
        // every idle thread already has a job waiting for it: start one more
        if ((size_t)s_idle < s_queue.size() && s_threads < kMaxThreads) {
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
    std::deque<std::function<void()>> dropped;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        s_stopping = true;
        dropped.swap(s_queue);
    }
    s_cv.notify_all();
    // (dropped jobs' captures are freed here, outside the lock)
}

bool workers_stopping() { return s_stopping; }
