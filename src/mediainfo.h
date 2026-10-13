#pragma once
#include "shell_utils.h"
#include <cstdint>
#include <memory>
#include <vector>

// What the player shows about the file that plays, read on a thread of its own: the tags
// (as Explorer's property system has them) and the album cover — a picture in the file's
// folder (cover.jpg, folder.jpg, ...; else the biggest one), else the art in the file.

struct MediaInfo {
    // the tags beyond title / artist / album, in display order: (label, value)
    std::vector<std::pair<std::wstring, std::wstring>> tags;
    unsigned bitrate = 0;                   // bps, from the tags; 0: not known
    unsigned long long size = 0;            // bytes
    // the cover: 32-bit premultiplied BGRA, top-down; null: none
    std::shared_ptr<const std::vector<uint32_t>> cover;
    int coverW = 0, coverH = 0;
    std::wstring coverFrom;                 // the picture's path (the file itself: its own art)
};

// Reads `path`'s info on the module's thread; posts `msg` to `notify` with
// lParam = MediaInfoNote* (the receiver deletes it). Only the newest request is read: one
// still waiting is replaced.
struct MediaInfoNote {
    unsigned id;                            // the caller's, back
    MediaInfo info;
};
void media_info_request(HWND notify, UINT msg, unsigned id, const std::wstring& path, int maxCoverPx);
