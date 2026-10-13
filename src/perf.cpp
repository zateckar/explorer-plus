#include "perf.h"
#include <cstdio>
#include <cstring>
#include <share.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

static bool s_on = false;
static FILE* s_log = nullptr;
static std::mutex s_mx;
static LARGE_INTEGER s_freq{}, s_qpc0{};
static double s_startOffset = 0;          // ms from process creation to s_qpc0
static std::map<std::string, std::vector<double>> s_paints;

void perf_init() {
    WCHAR path[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"EXPLORERPLUS_PERF", path, MAX_PATH) || !path[0]) return;
    s_log = _wfsopen(path, L"a, ccs=UTF-8", _SH_DENYNO);     // readable while the app runs
    if (!s_log) return;
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_qpc0);
    // anchor at process creation, so "startup" includes the loader and CRT init
    FILETIME created, exitT, kernel, user, now;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exitT, &kernel, &user)) {
        GetSystemTimePreciseAsFileTime(&now);
        ULARGE_INTEGER c{ { created.dwLowDateTime, created.dwHighDateTime } };
        ULARGE_INTEGER t{ { now.dwLowDateTime, now.dwHighDateTime } };
        s_startOffset = (double)(t.QuadPart - c.QuadPart) / 10000.0;
    }
    s_on = true;
    perf_event("process.start", -1, -1, nullptr);
}

bool perf_on() { return s_on; }

double perf_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return s_startOffset + (double)(t.QuadPart - s_qpc0.QuadPart) * 1000.0 / (double)s_freq.QuadPart;
}

void perf_event(const char* name, double dur, long long n, const wchar_t* detail) {
    if (!s_on) return;
    double t = perf_now();
    std::lock_guard<std::mutex> lk(s_mx);
    fwprintf(s_log, L"%10.2f %hs", name && !strcmp(name, "process.start") ? 0.0 : t, name);
    if (dur >= 0) fwprintf(s_log, L" dur=%.2f", dur);
    if (n >= 0) fwprintf(s_log, L" n=%lld", n);
    if (detail) fwprintf(s_log, L" %ls", detail);
    fwprintf(s_log, L"\n");
    fflush(s_log);
}

unsigned long long perf_code_address(const void* code) {
    // ASLR loads the exe anywhere: back to the base the file is linked at (ld's default
    // for 64-bit executables), where nm / addr2line find it
    return (unsigned long long)((const BYTE*)code - (const BYTE*)GetModuleHandleW(nullptr)) + 0x140000000ULL;
}

void perf_paint(const char* what, double ms) {
    if (!s_on) return;
    std::lock_guard<std::mutex> lk(s_mx);
    s_paints[what].push_back(ms);
}

void perf_flush() {
    if (!s_on) return;
    std::lock_guard<std::mutex> lk(s_mx);
    for (auto& [what, v] : s_paints) {
        if (v.empty()) continue;
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        double total = 0;
        for (double x : s) total += x;
        fwprintf(s_log, L"%10.2f paint.%hs n=%zu total=%.1f mean=%.2f p95=%.2f max=%.2f\n", perf_now(),
                 what.c_str(), s.size(), total, total / s.size(), s[(s.size() * 95) / 100 < s.size() ? (s.size() * 95) / 100 : s.size() - 1],
                 s.back());
    }
    s_paints.clear();
    fflush(s_log);
}
