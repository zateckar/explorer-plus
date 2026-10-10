#pragma once

// Production safety net: an unhandled exception (in our code or in a shell extension
// loaded into the process) writes a minidump to %LOCALAPPDATA%\ExplorerPlus\crashes
// before Windows Error Reporting takes over. Also drops the current directory from the
// DLL search path.
void crash_init();
