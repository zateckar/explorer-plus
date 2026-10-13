#pragma once
#include "appstate.h"

// The media player: a window of its own (video, or the title of what is playing; the
// play queue; transport controls) playing files with Media Foundation, and showing
// pictures (a slide show when played). One per desktop session: a process without one
// hands the files to the process that has it.

struct MediaItem {
    std::wstring path;
    bool folder = false;            // its media files are played (sorted by name, subfolders after)
};

// the file list's context menu offers Play / Queue in Explorer Plus for these (View /
// Queue for pictures only)
enum class PlayerOffer { None, Play, View };
PlayerOffer player_offer(const std::vector<MediaItem>& items);
// Play (the queue becomes these; one picture: its folder's pictures, from it) or Queue
// (appended; plays when nothing does)
void player_open(const std::vector<MediaItem>& items, bool enqueue);

HWND player_window();                       // this process's player, or nullptr
bool player_owns(HWND h);                   // h is the player or one of its windows
bool player_translate(MSG* m);              // message loop: the player's keys (true: handled)
bool player_app_command(int cmd);           // a media key pressed in another window of the app
// the main window is closing: true when the player stays open (the process then ends
// when it closes)
bool player_main_closed();
