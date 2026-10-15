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
// Queue for pictures only; playlists play what they list)
enum class PlayerOffer { None, Play, View };
PlayerOffer player_offer(const std::vector<MediaItem>& items);
// Play (the queue becomes these; one picture: its folder's pictures, from it) or Queue
// (appended; plays when nothing does)
void player_open(const std::vector<MediaItem>& items, bool enqueue);
// "Create playlist": asks where, then writes an .m3u8 / .m3u of the items' sound and video
// (folders: as Play would queue them)
void player_save_playlist(HWND owner, const std::vector<MediaItem>& items);
// explorer-plus.exe --play <file> (a media file, picture or playlist opened by its type):
// to a player already running, else to one of this process. True: this process has the
// player (it ends when the player closes); false: nothing more to do here
bool player_open_file(const std::wstring& path);
// the player brought forward (this process's or another's); an empty one when there is none
void player_show();

HWND player_window();                       // this process's player, or nullptr
bool player_owns(HWND h);                   // h is the player or one of its windows
bool player_translate(MSG* m);              // message loop: the player's keys (true: handled)
bool player_app_command(int cmd);           // a media key pressed in another window of the app
