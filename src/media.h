#pragma once
#include "shell_utils.h"
#include <vector>

// Media Foundation playback of one file at a time (Media Session, the enhanced video
// renderer into a window, the streaming audio renderer). Used on the UI thread; Media
// Foundation's own threads report through MediaNotes posted to the `notify` window as
// `msg` (lParam = MediaNote*, handed to MediaPlayer::handle, which frees it).
//
// mf.dll / mfplat.dll load on first use, not at startup; where they are missing
// (Windows N without the Media Feature Pack, a bare Windows Server) nothing plays and
// media_available says why.

bool media_available(std::wstring* why = nullptr);

struct MediaNote;

// the audio stream that plays (as the file has it, before decoding)
struct AudioInfo {
    unsigned long format = 0;       // subtype's first field: WAVE_FORMAT_* / FOURCC (audio_format_name)
    unsigned rate = 0, channels = 0, bits = 0, bitrate = 0;     // 0: not known
};

class MediaPlayer {
public:
    enum class State { Closed, Opening, Stopped, Playing, Paused, Ended, Failed };
    // what a note changed, for the window
    enum class Change { None, Opened, State, Failed, DeviceLost };   // DeviceLost: open it again (same place)

    MediaPlayer(HWND notify, UINT msg, HWND video);
    ~MediaPlayer();
    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    // starts opening `path` (whatever was open is closed); once open it plays (`play`) or
    // shows paused at `startAt` (100-ns units)
    bool open(const std::wstring& path, bool play, LONGLONG startAt = 0);
    void close();
    void play();                        // from the start after Stop / the end
    void pause();
    void stop();
    void seek(LONGLONG hns);            // 100-ns units; keeps playing or paused

    State state() const { return m_state; }
    bool busy() const { return m_state == State::Opening; }
    // inside close(), which answers sent messages while it waits: the window mustn't be
    // destroyed (nor the player deleted) from one of them
    bool closing() const { return m_closing; }
    // playing, or on its way to (opening to play, resuming): what Play/Pause shows
    bool will_play() const;
    LONGLONG position();                // 100-ns units
    LONGLONG duration() const { return m_duration; }
    bool has_video() const { return m_hasVideo; }
    HRESULT error() const { return m_error; }
    const std::wstring& title() const { return m_title; }
    const std::wstring& artist() const { return m_artist; }
    const std::wstring& album() const { return m_album; }
    const AudioInfo& audio() const { return m_audio; }      // once Opened

    void set_volume(float amplitude);   // 0..1 (0 also for muted)
    void video_resized();               // the video window changed size
    bool video_paint();                 // WM_PAINT of the video window (after BeginPaint); false: paint it yourself

    Change handle(MediaNote* note);     // the window's `msg`: takes the note

private:
    struct Impl;
    Impl* d;
    State m_state = State::Closed;
    LONGLONG m_duration = 0;
    bool m_hasVideo = false;
    bool m_closing = false;
    HRESULT m_error = S_OK;
    std::wstring m_title, m_artist, m_album;
    AudioInfo m_audio;
    void start_at(LONGLONG hns, bool keepPosition);
    void topology_ready();
    void source_ready(struct IUnknown* obj, HRESULT hr);
    void fail(HRESULT hr);
};

// a sentence for a Media Foundation error ("This file type can't be played", ...)
std::wstring media_error_text(HRESULT hr);

// A file's audio decoded to float PCM, apart from playback (the spectrogram's): used on a
// thread of its own (COM initialised there), after media_available() said yes.
class AudioDecoder {
public:
    AudioDecoder() = default;
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;
    bool open(const std::wstring& path, LONGLONG from);     // false: no audio it can decode
    unsigned channels() const { return m_channels; }
    unsigned rate() const { return m_rate; }
    LONGLONG duration() const { return m_duration; }        // 100-ns units, 0: not known
    bool seek(LONGLONG to);
    // the next block, interleaved, appended to `out`; `time`: its start (100-ns units).
    // false: the end, or an error
    bool read(std::vector<float>& out, LONGLONG* time);
private:
    struct IMFSourceReader* m_reader = nullptr;
    struct IMFMediaSource* m_source = nullptr;
    unsigned m_channels = 0, m_rate = 0;
    LONGLONG m_duration = 0;
};
