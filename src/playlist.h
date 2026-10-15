#pragma once
#include <random>
#include <string>
#include <vector>

// The media player's queue: its entries, the current one, shuffle and repeat, and where
// Next / Previous / the end of a track lead. Pure logic (unit tested).

enum class Repeat { Off = 0, All = 1, One = 2 };

struct QueueEntry {
    unsigned id = 0;                // stays the entry's while the queue changes around it
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
    unsigned m_nextId = 1;
    bool m_shuffle = false;
    std::mt19937 m_rng{ std::random_device{}() };
    int order_pos(int index) const;
};

// "3:07", "1:02:03"; empty for an unknown duration (< 0)
std::wstring fmt_duration(long long hns);

// files the player offers to play: audio and video by their perceived type, except
// playlists and MIDI (Media Foundation plays neither)
bool media_extension_excluded(const std::wstring& ext);
bool media_file_playable(const std::wstring& path);
bool media_file_video(const std::wstring& path);     // its type is a video's (.mp4 too: may hold only sound)

// WM_COPYDATA payload between processes: paths separated by NUL
std::wstring pack_paths(const std::vector<std::wstring>& paths);
std::vector<std::wstring> unpack_paths(const wchar_t* data, size_t chars);

// ------------------------------------------------------------------
// M3U / M3U8 playlists
// ------------------------------------------------------------------
struct PlaylistEntry {
    std::wstring path;              // absolute
    std::wstring title;             // #EXTINF's, else empty
    long long duration = -1;        // 100-ns units, -1: not given
};

bool playlist_file(const std::wstring& path);        // .m3u / .m3u8

// the entries of a playlist's text: #EXTINF titles and durations; relative paths (also
// "\Music\a.mp3", "../a.mp3", '/' for '\') resolved against `dir` (the playlist's folder),
// file:/// URLs as paths; web addresses and other comments left out
std::vector<PlaylistEntry> playlist_parse(const std::wstring& text, const std::wstring& dir);
// a playlist's text (#EXTM3U, an #EXTINF where the title or duration is known, CRLF lines);
// paths inside `dir` written relative to it (the folder can move with its playlist)
std::wstring playlist_write(const std::vector<PlaylistEntry>& entries, const std::wstring& dir);
// the file's bytes: .m3u8 as UTF-8; .m3u in the ANSI code page when that holds every
// character (as players of old expect), else UTF-8 too
std::string playlist_encode(const std::wstring& text, bool m3u8);

// read / write a playlist file (false: couldn't)
bool playlist_load(const std::wstring& file, std::vector<PlaylistEntry>& out);
bool playlist_save(const std::wstring& file, const std::vector<PlaylistEntry>& entries);
