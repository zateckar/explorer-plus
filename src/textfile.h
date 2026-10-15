#pragma once
#include <string>
#include <vector>

// The Notepad's text files and its remembered tabs. Pure text handling, unit-tested (tests/).

enum class TextEncoding { Utf8, Utf8Bom, Utf16Le, Utf16Be, Ansi, Count };
enum class LineEnding { CrLf, Lf, Cr, Count };

const wchar_t* encoding_name(TextEncoding e);      // "UTF-8", "UTF-16 LE", ...
const wchar_t* line_ending_name(LineEnding e);     // "Windows (CRLF)", ...

struct TextFile {
    std::wstring text;              // its lines ended by CRLF (whatever the file has)
    TextEncoding enc = TextEncoding::Utf8;
    LineEnding eol = LineEnding::CrLf;   // the file's own (the most used, CRLF when none)
    bool binary = false;            // NUL characters: not a text file
};
// the bytes of a file: the BOM, else UTF-16 by its zero bytes, else UTF-8 where it is valid,
// else the ANSI code page
TextFile text_decode(const std::string& bytes);
// the text (any of CR, LF, CRLF ending its lines) as a file's bytes; `lossy`: the encoding
// (ANSI) can't hold some of its characters
std::string text_encode(const std::wstring& text, TextEncoding enc, LineEnding eol, bool* lossy = nullptr);
std::wstring text_to_crlf(const std::wstring& s);

// the tabs the Notepad had, with what wasn't saved (kept in its own folder)
struct NoteTabState {
    std::wstring path;              // its file; empty: never saved
    std::wstring cache;             // the file in the notes folder with its unsaved text; empty: none
    int untitled = 0;               // "Untitled N" (no file)
    bool dirty = false;             // changed since its file was read or saved
    TextEncoding enc = TextEncoding::Utf8;
    LineEnding eol = LineEnding::CrLf;
    int selStart = 0, selEnd = 0;   // the caret and selection
    int top = 0;                    // the first line shown
    unsigned long long fileTime = 0;     // its file's last write time when read or saved
};
struct NoteSession {
    std::vector<NoteTabState> tabs;
    int active = 0;
    bool wrap = true, statusBar = true;
    int zoom = 100;                 // percent
    int x = 0, y = 0, w = 0, h = 0; // the window (w 0: never placed)
    bool maximized = false;
};
std::wstring note_session_serialize(const NoteSession& s);
NoteSession note_session_parse(const std::wstring& text);
