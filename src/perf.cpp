#include "perf.h"
#include <cstdio>
#include <cstring>
#include <share.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>

static bool s_on = false;
static FILE* s_log = nullptr;
static std::mutex s_mx;
static LARGE_INTEGER s_freq{}, s_qpc0{};
static double s_startOffset = 0;          // ms from process creation to s_qpc0
static std::map<std::string, std::vector<double>> s_paints;

// ---------------------------------------------------------------
// EXPLORERPLUS_SAMPLE=<file>: the UI thread's call stack (and workers' between
// perf_sample_begin / _end) every ~0.5 ms for the first EXPLORERPLUS_SAMPLE_MS (default
// 3000) ms, then written out, a line per sample: "<ms> <thread label> <module>+<offset> ..."
// (innermost frame first; the exe's offsets as linked, for addr2line). tools/samples.py
// sums them up per function. Shows where the time goes inside Windows' code too (by
// module), which the events can't.

static constexpr int kSampleFrames = 40;
struct Sample { double t; int slot; int n; DWORD64 pc[kSampleFrames]; };
// the threads sampled: the UI thread, and workers while they say so (perf_sample_begin)
struct SampleSlot { HANDLE h; DWORD tid; const char* label; std::atomic<bool> on; };
static constexpr int kSlots = 16;
static SampleSlot s_slots[kSlots];
static std::atomic<int> s_slotCount{ 0 };
static std::atomic<bool> s_sampling{ false };

static int add_slot(const char* label) {
    int i = s_slotCount.load();
    if (i >= kSlots) return -1;
    SampleSlot& sl = s_slots[i];
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sl.h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return -1;
    sl.tid = GetCurrentThreadId();
    sl.label = label;
    sl.on = true;
    s_slotCount = i + 1;                       // (published last: the sampler reads up to here)
    return i;
}

void perf_sample_begin(const char* label) {
    if (!s_sampling) return;
    static std::mutex mx;
    std::lock_guard<std::mutex> lk(mx);
    for (int i = 1; i < s_slotCount.load(); ++i)          // (this thread sampled before)
        if (s_slots[i].tid == GetCurrentThreadId()) { s_slots[i].label = label; s_slots[i].on = true; return; }
    add_slot(label);
}

void perf_sample_end() {
    if (!s_sampling) return;
    for (int i = 1; i < s_slotCount.load(); ++i)
        if (s_slots[i].tid == GetCurrentThreadId()) s_slots[i].on = false;
}

static void sample_thread(HANDLE h, Sample& s) {
    // (nothing that takes a lock while the thread is stopped: it may hold the heap's)
    if (SuspendThread(h) == (DWORD)-1) return;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(h, &c)) {
        while (s.n < kSampleFrames && c.Rip) {
            s.pc[s.n++] = c.Rip;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
            if (!f) {                                       // a leaf: the return address is on top
                DWORD64 ret = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)c.Rsp, &ret, sizeof(ret), nullptr)) break;
                c.Rip = ret;
                c.Rsp += 8;
                continue;
            }
            PVOID data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &data, &frame, nullptr);
        }
    }
    ResumeThread(h);
}

static void sampler(std::wstring out, double untilMs) {
    std::vector<Sample> samples;
    samples.reserve(40000);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    while (perf_now() < untilMs) {
        LARGE_INTEGER due{};
        due.QuadPart = -5000;                                  // 0.5 ms
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
        else Sleep(1);
        int n = s_slotCount.load();
        if (samples.size() + n > samples.capacity()) break;
        for (int i = 0; i < n; ++i) {
            if (!s_slots[i].on) continue;
            Sample& s = samples.emplace_back();
            s.t = perf_now();
            s.slot = i;
            s.n = 0;
            sample_thread(s_slots[i].h, s);
            if (!s.n) samples.pop_back();
        }
    }
    s_sampling = false;
    if (timer) CloseHandle(timer);
    FILE* f = _wfsopen(out.c_str(), L"w", _SH_DENYNO);
    if (!f) return;
    HMODULE exe = GetModuleHandleW(nullptr);
    std::map<HMODULE, std::string> names;
    for (const Sample& s : samples) {
        fprintf(f, "%.2f %s", s.t, s_slots[s.slot].label);
        for (int i = 0; i < s.n; ++i) {
            HMODULE m = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)s.pc[i], &m);
            auto it = names.find(m);
            if (it == names.end()) {
                char p[MAX_PATH] = "?";
                if (m) GetModuleFileNameA(m, p, MAX_PATH);
                const char* slash = strrchr(p, '\\');
                it = names.emplace(m, m == exe ? std::string("exe") : std::string(slash ? slash + 1 : p)).first;
            }
            unsigned long long off = s.pc[i] - (DWORD64)m + (m == exe ? 0x140000000ULL : 0);
            fprintf(f, " %s+%llx", it->second.c_str(), off);
        }
        fprintf(f, "\n");
    }
    fclose(f);
}

static void start_sampler() {
    WCHAR out[MAX_PATH]{}, ms[32]{};
    if (!GetEnvironmentVariableW(L"EXPLORERPLUS_SAMPLE", out, MAX_PATH) || !out[0]) return;
    double until = GetEnvironmentVariableW(L"EXPLORERPLUS_SAMPLE_MS", ms, 32) ? _wtof(ms) : 3000;
    if (add_slot("ui") != 0) return;
    s_sampling = true;
    std::thread(sampler, std::wstring(out), until).detach();
}
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
    start_sampler();
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
