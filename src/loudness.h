#pragma once
#include <windows.h>
#include <string>

// The player's loudness levelling: a track's loudness estimated on a thread of its own
// from a dozen 3-second excerpts (all of a short track), decoded apart from playback. The
// result is posted to `notify` as `msg` with lParam a LoudnessNote* (the window deletes it).
struct LoudnessNote {
    std::wstring path;
    double lufs;                    // not finite: couldn't be measured (no audio it can decode, silence)
};

// `urgent`: ahead of those waiting (the track starting now); others: in order, a few kept
void loudness_request(HWND notify, UINT msg, const std::wstring& path, bool urgent);
