#pragma once
#include <string>
#include <string_view>
#include <vector>

// The Notepad's text files and its remembered tabs. Pure text handling, unit-tested (tests/).

enum class TextEncoding { Utf8, Utf8Bom, Utf16Le, Utf16Be, Ansi, Count };
enum class LineEnding { CrLf, Lf, Cr, Count };

const wchar_t* encoding_name(TextEncoding e);      // "UTF-8", "UTF-16 LE", ...
const wchar_t* line_ending_name(LineEnding e);     // "Windows (CRLF)", ...

struct TextFile {
    std::wstring text;              // its lines ended by CR, as the Notepad's text box has them (whatever the file has)
    TextEncoding enc = TextEncoding::Utf8;
    LineEnding eol = LineEnding::CrLf;   // the file's own (the most used, CRLF when none)
    bool binary = false;            // NUL characters: not a text file
    bool lossy = false;             // written back as read, some bytes would differ (an odd byte at the
                                    // end of UTF-16, bytes the ANSI code page has no character for)
    bool mixedEol = false;          // more than one kind of line ending (saving makes them all `eol`)
};
// the bytes of a file: the BOM, else UTF-16 by its zero bytes, else UTF-8 where it is valid,
// else the ANSI code page (a UTF-8 BOM before bytes that aren't UTF-8: ANSI, the BOM included,
// so that saving gives back the same bytes)
TextFile text_decode(const std::string& bytes);
// the text (any of CR, LF, CRLF ending its lines) as a file's bytes; `lossy`: the encoding
// (ANSI) can't hold some of its characters. Empty for a non-empty text: the conversion failed.
std::string text_encode(const std::wstring& text, TextEncoding enc, LineEnding eol, bool* lossy = nullptr);

// a new file with the bytes (in parts, as they are held), on the disk before it returns, so that
// a crash or a power cut after a move never leaves an empty file in its place. `exclusive`:
// only where there is no file yet (else one there is written over).
enum class Wrote { Ok, Failed, NotCreated };
Wrote write_new(const std::wstring& path, std::string_view data, std::string_view more = {}, bool exclusive = false);
// the bytes in place of a file: written beside it (under a name of its own, never over another
// file), then put in its place. A failed write leaves the old one; the file keeps its attributes
// and permissions. Only a folder that takes no new files has it written over in place.
bool text_file_write(const std::wstring& path, const std::string& data);

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
    std::wstring syntax;            // its colours' language when chosen (syntax.h's key); empty: by its name
    int zoom = 100;                 // percent (each tab its own; a session from before: the window's)
};
struct NoteSession {
    std::vector<NoteTabState> tabs;
    int active = 0;
    bool wrap = true, statusBar = true;
    bool lineNumbers = true, whitespace = false;
    int x = 0, y = 0, w = 0, h = 0; // the window (w 0: never placed)
    bool maximized = false;
};
std::wstring note_session_serialize(const NoteSession& s);
NoteSession note_session_parse(const std::wstring& text);
