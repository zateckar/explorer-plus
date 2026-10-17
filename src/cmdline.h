#pragma once
#include <string>
#include <vector>

// Text the user (or another program) hands over: the command line and a path typed
// into the address bar. Pure text handling, unit-tested (tests/).

// explorer-plus.exe [--new-window] [folder or file]
// explorer-plus.exe --play <media file, picture or playlist>   (the media player alone)
// explorer-plus.exe --notepad [text file]                      (the Notepad alone)
// explorer-plus.exe --sync [--headless]                        (the sync agent, docs/sync-plan.md)
// explorer-plus.exe --sync-folders                             (the Sync folders window alone)
// explorer-plus.exe --sync-cli <command> [key=value ...]       (one request to the sync agent)
struct CommandLine {
    std::wstring path;             // as given (relative paths: resolved by the caller)
    bool newWindow = false;
    bool play = false;
    bool notepad = false;
    bool sync = false, headless = false, syncFolders = false, syncCli = false;
    std::vector<std::wstring> syncArgs;    // --sync-cli: everything after it
};
CommandLine parse_command_line(const wchar_t* cmdline);   // the whole line, the exe first

// one argument for CommandLineToArgvW: quoted, with the backslashes before the closing
// quote doubled ("C:\" would otherwise arrive as C:")
std::wstring quote_command_arg(const std::wstring& s);

// a typed path, tidied: blanks around it, quotes pasted from a command line and
// %VARIABLES% expanded
std::wstring typed_text_clean(const std::wstring& text);
