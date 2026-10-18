#pragma once
#include <random>
#include <string>
#include <vector>

// The media player's queue: its entries, the current one, shuffle and repeat, and where
// Next / Previous / the end of a track lead. Pure logic (unit tested).

enum class Repeat { Off = 0, All = 1, One = 2 };

struct QueueEntry {
    unsigned id = 0;                // stays the entry's while the queue changes around it (none other has it, in any queue)
    std::wstring path;
    std::wstring title;             // from the file's tags once known; empty: the file name is shown
    long long duration = -1;        // 100-ns units, -1: not known yet
    bool failed = false;            // couldn't be played
};

class PlayQueue {
public:
    const std::vector<QueueEntry>& entries() const { return m_entries; }
    QueueEntry& at(int i) { return m_entries[(size_t)i]; }
    int size() const { return (int)m_entries.size(); }
    bool empty() const { return m_entries.empty(); }
    int current() const { return m_current; }           // -1: none
    int index_of(unsigned id) const;                     // -1: not (any more) in the queue
    int first() const;                                   // the first to play (shuffled: a random one), -1: empty
    void set_current(int i);

    void replace(const std::vector<std::wstring>& paths);     // current: the first
    int append(const std::vector<std::wstring>& paths);       // index of the first one added
    // removes the entries at `indices`; true when the current one was among them (the
    // current one is then the entry that followed it, or none)
    bool remove(std::vector<int> indices);
    void clear();
    // the entries at `indices` moved, in their order, to before the entry at `to` (size(): to
    // the end); the current entry stays current, a shuffled play order stays. Returns the
    // index of the first of them now (-1: none of them was in the queue)
    int move(std::vector<int> indices, int to);
    // entries put in before the entry at `at` (out of range: at the end), their ids kept
    // (moved from another queue) or, `fresh`, new ones (copies). Returns `at` as used
    int insert(int at, std::vector<QueueEntry> entries, bool fresh);

    // the entry to play after the current one, -1 when the queue is done. `automatic`:
    // the current track ended by itself (Repeat one plays it again)
    int next(bool automatic) const;
    int previous() const;                                // -1: none before the current one

    bool shuffle() const { return m_shuffle; }
    void set_shuffle(bool on);                           // the current entry stays current
    Repeat repeat = Repeat::Off;

    void seed(unsigned s) { m_rng.seed(s); }

private:
    std::vector<QueueEntry> m_entries;
    std::vector<int> m_order;       // play order (indices into m_entries): shuffled or 0..n-1
    int m_current = -1;
    bool m_shuffle = false;
    std::mt19937 m_rng{ std::random_device{}() };
    int order_pos(int index) const;
};

// "3:07", "1:02:03"; empty for an unknown duration (< 0)
std::wstring fmt_duration(long long hns);

// files the player offers to play: audio and video by their perceived type (playlists
// aside), MIDI (Windows' synthesizer plays it) and CD tracks
bool media_extension_excluded(const std::wstring& ext);
bool midi_file(const std::wstring& path);            // .mid .midi .rmi .kar
bool cd_track_file(const std::wstring& path);        // .cda
bool media_file_playable(const std::wstring& path);
bool media_file_video(const std::wstring& path);     // its type is a video's (.mp4 too: may hold only sound)

// WM_COPYDATA payload between processes: paths separated by NUL
std::wstring pack_paths(const std::vector<std::wstring>& paths);
std::vector<std::wstring> unpack_paths(const wchar_t* data, size_t chars);

// a web address the player streams (http, https, mms, rtsp): internet radio, a playlist's
bool media_url(const std::wstring& s);

// ------------------------------------------------------------------
// playlists: M3U / M3U8 (read and written), PLS, WPL / ZPL, ASX / WAX / WVX, XSPF (read)
// ------------------------------------------------------------------
struct PlaylistEntry {
    std::wstring path;              // absolute, or a web address (media_url)
    std::wstring title;             // the playlist's, else empty
    long long duration = -1;        // 100-ns units, -1: not given
};

enum class PlaylistKind { None, M3U, PLS, XML };
PlaylistKind playlist_kind(const std::wstring& path);   // by the extension
bool playlist_file(const std::wstring& path);

// the entries of an M3U's text: #EXTINF titles and durations; relative paths (also
// "\Music\a.mp3", "../a.mp3", '/' for '\') resolved against `dir` (the playlist's folder),
// file:/// URLs as paths, web addresses as they are; other comments and schemes left out
std::vector<PlaylistEntry> playlist_parse(const std::wstring& text, const std::wstring& dir);
std::vector<PlaylistEntry> playlist_parse_pls(const std::wstring& text, const std::wstring& dir);
// WPL, ZPL, ASX and XSPF: one tolerant reading of their XML
std::vector<PlaylistEntry> playlist_parse_xml(const std::wstring& text, const std::wstring& dir);
// by `kind`, unless the text says otherwise ("[playlist]": PLS, '<': XML)
std::vector<PlaylistEntry> playlist_parse_any(const std::wstring& text, const std::wstring& dir, PlaylistKind kind);
// a playlist's text (#EXTM3U, an #EXTINF where the title or duration is known, CRLF lines);
// paths inside `dir` written relative to it (the folder can move with its playlist)
std::wstring playlist_write(const std::vector<PlaylistEntry>& entries, const std::wstring& dir);
// the file's bytes: .m3u8 as UTF-8; .m3u in the ANSI code page when that holds every
// character (as players of old expect), else UTF-8 too
std::string playlist_encode(const std::wstring& text, bool m3u8);

// the entries on another computer (\\server\...) than the playlist's own `dir` taken out: a
// downloaded playlist doesn't make Windows sign in to a server it names
void playlist_drop_remote(std::vector<PlaylistEntry>& entries, const std::wstring& dir);
// read / write a playlist file (false: couldn't; a downloaded one: playlist_drop_remote)
bool playlist_load(const std::wstring& file, std::vector<PlaylistEntry>& out);
bool playlist_save(const std::wstring& file, const std::vector<PlaylistEntry>& entries);

// ------------------------------------------------------------------
// the player's queues (its tabs) as they were left: an INI document ([player], then
// [queue1], [queue2], ... with an entry a line), kept in a folder of its own
// ------------------------------------------------------------------
struct QueueState {
    std::wstring name;
    std::vector<PlaylistEntry> entries;
    int current = -1;               // -1: none
};
struct PlayerSession {
    std::vector<QueueState> queues;
    int active = 0;                 // the tab shown
};
std::wstring player_session_serialize(const PlayerSession& s);
PlayerSession player_session_parse(const std::wstring& text);
