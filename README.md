# Explorer Plus

A Windows 11 style file manager written from scratch in **C++20 and raw Win32**: no
frameworks, no third-party libraries. It hosts the real Windows shell (folders, icons,
thumbnails, context menus, preview handlers, file operations), so it behaves like
File Explorer while starting in well under 100 ms.

## Features

- **Win11 layout**: tabs in the title bar, one navigation row (Back / Forward / Up /
  Refresh, breadcrumb address bar, search, "…" menu with the command bar's commands),
  navigation pane, details / preview pane, status bar with view toggles. Dark and light
  mode follow Windows, Per-Monitor DPI v2.
- **Pinned tabs** (right-click a tab ▸ Pin tab) stay first, can't be closed, and keep
  their folder: a folder opened from one (file list, navigation pane, address bar, Up,
  search) comes up in a new tab, whose Back leads to the pinned folder.
- **Dual pane** ("…" ▸ Dual pane, Ctrl+Shift+D): a second file list beside the first, as
  in Total Commander, each with its own folder, view and history. The list you click or
  tab into is the active one (its caption is marked): the address bar, Back / Forward,
  the navigation pane, search, the details pane and the commands act on it. Drag files
  from one list to the other to move or copy them; drag the gap between them to resize.
  Each tab remembers whether it shows one list or two, and their folders, across restarts.
- **Navigation pane**: Home, Gallery, OneDrive, pinned Quick access folders, This PC,
  Network; children listed in the background; follows the current folder. Each tab keeps
  its own folders expanded and its scroll position, also across restarts.
- **Views**: Details (with Win11 row spacing, or Compact like Windows' setting), Extra large /
  Large / Medium / Small icons, List, Tiles; per-folder columns (any shell property), sort,
  Group by; item check boxes; This PC drive tiles with free-space bars. The sort order
  you choose is remembered across restarts. Each folder
  remembers its own view; View ▸ "Apply this view to all folders" makes the current view
  and columns the default (like Explorer's Folder Options), "Reset all folders" goes back.
  Double-click on empty space goes up a level.
- **Folder sizes**: the Size column shows each subfolder's total (all levels, hidden files
  included; junctions and symbolic links not followed, and show no size themselves;
  folders you have no access to left out), worked out in the background
  after the listing and filled in as known; sorting by Size takes them into account.
  "…" ▸ Folder sizes turns it off.
- **Text size**: "…" ▸ Text size, or Ctrl+Plus / Ctrl+Minus / Ctrl+0, scales the text of
  the navigation pane, the file list and the details pane (80–200 %), remembered.
- **Command bar**: folded into the "…" menu by default; "…" ▸ Show command bar puts the
  row back, and its own "…" menu (or a right-click on it) has "Hide command bar".
- **Search**: Windows Search where the folder is indexed, otherwise a recursive search;
  results as their own location with a Folder column.
- **Compact context menu** like Windows 11: icon row (Cut / Copy / Rename / Share /
  Delete), then your promoted items with their icons (see Settings) and Open in Terminal,
  the common verbs, and "Show more options" for the full classic menu (Shift+right-click /
  Shift+F10).
- **File operations** through the shell (`IFileOperation`): copy, move, rename, delete to
  the Recycle Bin, new folder / New ▸ items, drag and drop, clipboard. Deleting the open
  folder in the navigation pane (Del, Shift+Del) opens the folder next to it; a folder
  removed by another program sends the view to its parent.
- **Large folders**: listings stream in; 30,000 files are on screen in ~60 ms and
  complete in under 0.2 s.
- **Media player**: "Play in Explorer Plus" / "Queue in Explorer Plus" in the compact menu
  of audio and video files (and of folders with some: their files by name, subfolders
  after) open a player window — the video, or for sound the album cover (cover.jpg,
  folder.jpg, … in the track's folder, else its biggest picture, else the art in the
  file), the tags (album, year, genre, track, …) with the format, bitrate and sample
  rate, and a scrolling spectrogram or a visualization (mirrored bars, waveform, circle,
  particles, ribbons, a synthwave horizon, a tunnel, a warp through stars, ridges,
  fireworks: right-click the player ▸ Visualization, or V; "Another with each track" picks
  one by chance as tracks change; they move with the beat, found in the bass drum, eased so
  nothing flashes; drawn on the GPU at the display's refresh rate; in full screen it fills
  the screen);
  the play queue (Enter plays, Delete removes, drop files on it); seek, volume, shuffle,
  repeat, full screen. Tracks are evened out in
  loudness (each measured as it comes up, the next one ahead; right-click the player to
  turn it off). "View in Explorer Plus" on a picture shows its folder's pictures in the
  player: Left / Right or the mouse wheel step through them, Play runs a slide show (its
  interval in the right-click menu), full screen as for video; photos are turned upright
  as the camera recorded. Its own taskbar button, with
  the track's progress and Previous / Play / Next in the thumbnail. Windows' own Media
  Foundation plays (the formats and codecs Windows has, Opus in MP4 too; Windows N needs
  the Media Feature Pack), nothing to install. Keys: Space
  play / pause, Left / Right ±5 s (Shift: 30 s), Ctrl+Left / Ctrl+Right previous / next,
  Up / Down volume, M mute, V / Shift+V next / previous visualization, F or F11 full
  screen, Ctrl+H shuffle, Ctrl+T repeat, S next subtitles, A next audio track; the media
  keys too. One player for all windows; it stays open after the main window closes.
- **Subtitles and audio tracks**: a video's subtitle files beside it (`Film.srt`,
  `Film.en.srt`, `Film.English.vtt`; SubRip, WebVTT, SubStation `.ass` / `.ssa`) show over
  it, the one in your language first; right-click ▸ Subtitles picks another, turns them off
  or chooses a file elsewhere. Files with several audio streams (films' languages,
  commentary) list them under right-click ▸ Audio track by name, language and format.
- **MIDI and CDs**: `.mid` / `.midi` / `.rmi` / `.kar` play through Windows' own
  synthesizer (its volume is the player's, in the Windows volume mixer), and an audio CD's
  tracks (`Track01.cda`) from the drive.
- **Playlists** (`.m3u`, `.m3u8`, `.pls`, `.wpl` / `.zpl`, `.asx` / `.wax` / `.wvx`, `.xspf`):
  play one like a folder (its tracks in order, with their titles, nested playlists too;
  missing files are skipped). Web addresses in them are internet radio and other streams:
  they play live (tried once more if the connection drops). "Create playlist..." in the
  compact menu of media files and folders writes one, and the player's queue menu has
  "Save queue as playlist..." (Ctrl+S). Paths inside the playlist's folder are written
  relative, so the folder can move; `.m3u8` is UTF-8.
- **Media file types** ("…" ▸ Media file types...): tick the music, video, picture and
  playlist types Explorer Plus should open and press Apply. They are registered for your
  account only; Windows lets only you pick a default app, so Apply opens Settings at
  Explorer Plus' page (double-click a type to pick it for that type alone). Files opened
  this way go to the one player; several opened together are queued.
- **Screenshots** (Alt+Q anywhere while Explorer Plus runs, or "…" ▸ Take a screenshot):
  drag a region or click for the whole screen (Enter: the screen under the mouse, Esc
  cancels). It is copied to the clipboard and opens in the picture editor.
- **Picture editor** (screenshots; "Edit picture" in the compact menu of a picture, or
  Ctrl+E in the player's picture view): rectangles and ellipses (Shift: square, circle),
  outlined or filled, and text, in any colour, line width and text size. Until saved every
  object stays one: select it to move, resize, recolour or delete it, double-click a text
  to change it; undo / redo. Save overwrites PNG, JPEG, BMP, GIF and TIFF files, Save as
  writes PNG, JPEG, BMP, GIF or TIFF; Copy puts the picture on the clipboard. Keys: V select, R rectangle,
  E ellipse, T text, F fill, Del delete, arrows nudge, Ctrl+Z / Ctrl+Y, Ctrl+C, Ctrl+S,
  Ctrl+Shift+S.

## Requirements

Windows 10 (1809 or later) or Windows 11, x64.

## Install

Download `explorer-plus-<version>-win-x64.zip` from the
[Releases](../../releases) page, check it against `SHA256SUMS.txt`, unzip anywhere
and run `explorer-plus.exe`. It is a single self-contained exe; nothing is installed.

```powershell
# in the download folder: True for each file whose hash matches its line
Get-Content SHA256SUMS.txt | ForEach-Object {
    $hash, $file = $_ -split '  ', 2
    '{0}  {1}' -f ((Get-FileHash $file -Algorithm SHA256).Hash -eq $hash), $file
}
```

Unsigned builds trigger a SmartScreen prompt on first run ("More info" → "Run anyway").

Command line: `explorer-plus.exe [--new-window] [folder or file]`; a file opens its folder
with the file selected. `explorer-plus.exe --play <file>` plays a media file or playlist (or
shows a picture) in the player, as the registered file types do.

## Settings

Stored in `%APPDATA%\ExplorerPlus\settings.ini` (UTF-16). Most are set from the UI;
two sections are meant for hand editing:

```ini
[contextmenu]
; items from "Show more options" to list at the top of the compact menu:
; text (or verb) to match, optionally |icon file,index
promote1=7-Zip
promote2=NotepadNext
promote3=Open in My Editor|%ProgramFiles%\MyEditor\editor.exe,0

[prefs]
; 0: Alt+Q is left to other programs (Take a screenshot stays in the "…" menu)
screenshotHotkey=1
```

`[tree]` caches the Quick access pins so they show instantly; it is rewritten
automatically.

Environment variables (diagnostics and tests):

| Variable | Effect |
|---|---|
| `EXPLORERPLUS_SETTINGS=<file>` | use another settings file |
| `EXPLORERPLUS_THEME=light\|dark` | override the Windows theme |
| `EXPLORERPLUS_PERF=<log file>` | write timing events (startup, listings, paints, slow UI messages) |

## Troubleshooting

If the app (or a shell extension loaded into it) crashes, a minidump is written to
`%LOCALAPPDATA%\ExplorerPlus\crashes` (the newest 10 are kept). Please attach it to a
bug report together with the version from the exe's Properties → Details.

## Build from source

Prerequisites: MinGW-w64 g++ 13+ (with `windres`), CMake 3.20+, Ninja or GNU Make
(`tools/package.ps1` falls back to MinGW's `mingw32-make`).
With MSYS2: `pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja}`.

```powershell
cmake -S . -B build -G Ninja          # Release by default
cmake --build build
```

Release zip, the same way CI builds it:

```powershell
pwsh tools/package.ps1 -Version 1.2.0     # -> dist/explorer-plus-1.2.0-win-x64.zip
```

## Tests and profiling

```powershell
ctest --test-dir build --output-on-failure                          # unit tests (INI, command line, play queue)
python -I tools/smoke_test.py build\explorer-plus.exe build\smoke   # GUI smoke test
python -I tools/perf.py build\explorer-plus.exe build\perf 5         # startup / 30k files / scroll / sort / views / resize
```

The smoke test works in a scratch folder with a scratch settings file. Note that its
delete checks send one scratch file and one empty scratch folder to the Recycle Bin.
Next to its screenshots it keeps the app's event log (`shots\events.log`); on a failure
it prints the navigation and watcher events from it.
Keep the work folder out of hidden folders (e.g. `%TEMP%` is under the hidden AppData):
the navigation pane checks need it to show up in the tree. A run that takes longer than
`SMOKE_DEADLINE` seconds (default 480) is stopped and fails; with `CI` set, a check
skipped because the desktop got in the way fails too.

## Releases

`.github/workflows/build.yml` builds, tests (unit + smoke) and packages every push and
pull request (artifact on the run). Every build of `main` that passes the tests is
published as a GitHub release, nothing to do by hand:

- version: the date of the commit built (UTC) and the CI run number, e.g.
  `2026.10.10.57`, tag `v2026.10.10.57` (also in the exe's Properties → Details);
- release notes: the commits since the previous release, subject and description, so
  write commit messages for the people reading them.

Pull requests are built and tested but not released. To sign the exe, add the
repository secrets `SIGNING_CERT_PFX_BASE64` (base64 of the .pfx) and `SIGNING_CERT_PASSWORD`.

## Source layout

```
src/main.cpp           wWinMain, main window, command routing
src/chrome, tabs, navbar, breadcrumb, searchbox, cmdbar, statusbar, layout   window parts
src/tree               navigation pane
src/listview, enumerate, columns, groups, thumbs, foldersizes   content view and listing
src/contextmenu, flyout    classic + Win11 compact context menus
src/fileops, dnd, watcher  file operations, drag and drop, change notifications
src/detailspane, preview   details / preview pane
src/player, media, playlist   media player window, Media Foundation playback, play queue
src/mediainfo, mediadata, spectrum   player's cover and tags, their pure helpers, spectrogram
src/visual             player's visualizations (Direct2D on Direct3D 11, a thread of their own)
src/imageview, loudness   player's pictures (WIC), loudness levelling
src/screenshot, imageedit, shapes   Alt+Q screenshots, picture editor, its objects' geometry
src/assoc              media file type registration (Media file types)
src/settings, ini, theme, perf, crash   settings.ini, colours and fonts, profiling, crash dumps
src/cmdline            command line and typed-path text
tests/                 unit tests (ctest)
tools/                 smoke test, perf scenarios, packaging, icon generator
```

## License

[MIT](LICENSE)
