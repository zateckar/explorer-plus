#pragma once
#include <functional>

// Background work: one shared, elastic pool instead of a detached thread per job.
//
// An idle thread takes the next job; a new one starts only when every thread is busy
// (a job stuck on an unreachable network share must not hold up the others), up to a
// cap, and threads idle for a while end. Each thread has COM initialized (STA).
// A job that throws is dropped, the app goes on. Results go to the UI thread with
// post_async as before.
void worker_submit(std::function<void()> job);

// Stop taking jobs and drop the queued ones (at exit). Running jobs finish on their
// own; their post_async results are discarded.
void workers_shutdown();
bool workers_stopping();
