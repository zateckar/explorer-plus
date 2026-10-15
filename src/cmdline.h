#pragma once
#include <string>

// Text the user (or another program) hands over: the command line and a path typed
// into the address bar. Pure text handling, unit-tested (tests/).

// explorer-plus.exe [--new-window] [folder or file]
// explorer-plus.exe --play <media file, picture or playlist>   (the media player alone)
struct CommandLine {
    std::wstring path;             // as given (relative paths: resolved by the caller)
    bool newWindow = false;
    bool play = false;
};
CommandLine parse_command_line(const wchar_t* cmdline);   // the whole line, the exe first

// one argument for CommandLineToArgvW: quoted, with the backslashes before the closing
// quote doubled ("C:\" would otherwise arrive as C:")
std::wstring quote_command_arg(const std::wstring& s);

// a typed path, tidied: blanks around it, quotes pasted from a command line and
// %VARIABLES% expanded
std::wstring typed_text_clean(const std::wstring& text);
