#include "crash.h"
#include <windows.h>
#include <dbghelp.h>
#include <cwchar>

namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// no heap use here: the heap may be what broke
LONG WINAPI on_crash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    WCHAR path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (!n || n > MAX_PATH - 80) return EXCEPTION_CONTINUE_SEARCH;
    wcscat_s(path, L"\\ExplorerPlus");
    CreateDirectoryW(path, nullptr);
    wcscat_s(path, L"\\crashes");
    CreateDirectoryW(path, nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR name[64];
    swprintf_s(name, L"\\explorer-plus-%04u%02u%02u-%02u%02u%02u-%lu.dmp", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    wcscat_s(path, name);
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(
                               reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))) : nullptr;
    if (!write) return EXCEPTION_CONTINUE_SEARCH;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    MINIDUMP_EXCEPTION_INFORMATION mei{ GetCurrentThreadId(), ep, FALSE };
    write(GetCurrentProcess(), GetCurrentProcessId(), f,
          MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
          &mei, nullptr, nullptr);
    CloseHandle(f);
    return EXCEPTION_CONTINUE_SEARCH;   // Windows Error Reporting still runs
}

}  // namespace

void crash_init() {
    SetDllDirectoryW(L"");              // no DLLs from the current directory
    SetUnhandledExceptionFilter(on_crash);
}
