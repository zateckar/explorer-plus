#include "crash.h"
#include <windows.h>
#include <dbghelp.h>
#include <cwchar>
#include <cstdlib>
#include <exception>

namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// resolved at startup: at crash time the loader lock or the heap may be what broke
MiniDumpWriteDumpFn s_write = nullptr;
WCHAR s_dir[MAX_PATH]{};                 // %LOCALAPPDATA%\ExplorerPlus\crashes

constexpr DWORD kTerminateCode = 0xE0000001;   // "uncaught C++ exception" in the dump

struct DumpJob { EXCEPTION_POINTERS* ep; DWORD tid; };

// no heap use here
void write_dump(EXCEPTION_POINTERS* ep, DWORD tid) {
    if (!s_write || !s_dir[0]) return;
    WCHAR path[MAX_PATH];
    wcscpy_s(path, s_dir);
    CreateDirectoryW(path, nullptr);     // the parent was created at startup
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR name[64];
    swprintf_s(name, L"\\explorer-plus-%04u%02u%02u-%02u%02u%02u-%lu.dmp", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    if (wcscat_s(path, name)) return;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei{ tid, ep, FALSE };
    s_write(GetCurrentProcess(), GetCurrentProcessId(), f,
            MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
            &mei, nullptr, nullptr);
    CloseHandle(f);
}

DWORD WINAPI dump_thread(void* p) {
    auto* job = static_cast<DumpJob*>(p);
    write_dump(job->ep, job->tid);
    return 0;
}

// The dump is written from a fresh thread: the crashing one may have overflowed its
// stack, and dbghelp walks the faulting thread best from outside it.
void dump_once(EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return;
    DumpJob job{ ep, GetCurrentThreadId() };
    HANDLE t = CreateThread(nullptr, 256 * 1024, dump_thread, &job, 0, nullptr);
    if (t) {
        WaitForSingleObject(t, 120000);
        CloseHandle(t);
    } else {
        write_dump(ep, job.tid);
    }
}

LONG WINAPI on_crash(EXCEPTION_POINTERS* ep) {
    dump_once(ep);
    return EXCEPTION_CONTINUE_SEARCH;   // Windows Error Reporting still runs
}

// an uncaught C++ exception ends in std::terminate -> abort, which never reaches the
// unhandled-exception filter: dump the current context instead
[[noreturn]] void on_terminate() {
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = kTerminateCode;
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_M_X64) || defined(__x86_64__)
    rec.ExceptionAddress = reinterpret_cast<void*>(ctx.Rip);
#elif defined(_M_ARM64) || defined(__aarch64__)
    rec.ExceptionAddress = reinterpret_cast<void*>(ctx.Pc);
#endif
    EXCEPTION_POINTERS ep{ &rec, &ctx };
    dump_once(&ep);
    std::abort();
}

}  // namespace

void crash_init() {
    SetDllDirectoryW(L"");              // no DLLs from the current directory

    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dbghelp)
        s_write = reinterpret_cast<MiniDumpWriteDumpFn>(
            reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    WCHAR dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (n && n < MAX_PATH - 80) {
        wcscat_s(dir, L"\\ExplorerPlus");
        CreateDirectoryW(dir, nullptr);
        wcscat_s(dir, L"\\crashes");
        wcscpy_s(s_dir, dir);
    }

    // room for the filter to run after a stack overflow on the UI thread
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(on_crash);
    std::set_terminate(on_terminate);
}
