#pragma once
#include <windows.h>

// Opt-in profiling: with EXPLORERPLUS_PERF=<log file> the app appends one line per
// event ("<ms since process start> <event> [dur=<ms>] [n=<count>] [detail]") and, at
// exit, paint statistics. Off (the default) every call is a cheap early return.
// tools/perf.py drives scenarios and reads the log.

void   perf_init();                       // reads the env var (call first in wWinMain)
bool   perf_on();
double perf_now();                        // ms since the process was created
void   perf_event(const char* name, double dur = -1, long long n = -1, const wchar_t* detail = nullptr);
void   perf_paint(const char* what, double ms);   // accumulated: count / total / max / p95
void   perf_flush();                      // paint summaries (exit)
// EXPLORERPLUS_SAMPLE (perf.cpp): this worker thread's stack is sampled too until _end
void   perf_sample_begin(const char* label);
void   perf_sample_end();
// a function's address as linked, for the log: addr2line -f -C -e explorer-plus.debug 0x<it>
unsigned long long perf_code_address(const void* code);

struct PerfScope {                        // logs its duration when it ends
    const char* name;
    double t0;
    long long n = -1;
    explicit PerfScope(const char* nm) : name(nm), t0(perf_on() ? perf_now() : 0) {}
    ~PerfScope() { if (perf_on()) perf_event(name, perf_now() - t0, n); }
};
