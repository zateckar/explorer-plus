# Explorer Plus

A Windows 11 style file manager written from scratch in **C++20 and raw Win32**: no
frameworks, no third-party libraries. It hosts the real Windows shell (folders, icons,
thumbnails, context menus, preview handlers, file operations), so it behaves like
File Explorer while starting in well under 100 ms.

## Features

- **Win11 layout**: tabs in the title bar, one navigation row with a breadcrumb address bar,
  search and a "…" menu, navigation pane, details / preview pane, status bar. Dark and light
  mode follow Windows; Per-Monitor DPI v2.
- **Pinned tabs** stay first and keep their folder: folders opened from them open in a new tab.
- **Dual pane** (Ctrl+Shift+D): two file lists side by side, each with its own folder, view
  and history; drag files between them. Remembered per tab.
- **Navigation pane**: Home, Gallery, OneDrive, Quick access, This PC, Network; follows the
  current folder. Each tab keeps its expanded folders and scroll position.
- **Views**: Details, icons in four sizes, List, Tiles; columns from any shell property, sort,
  Group by, check boxes. Each folder remembers its view; one view can be applied to all.
  Double-click empty space to go up.
- **Folder sizes**: the Size column shows each subfolder's total, worked out in the background.
- **Text size**: Ctrl+Plus / Ctrl+Minus / Ctrl+0 scales the panes' text (80–200 %).
- **Command bar**: in the "…" menu by default; "Show command bar" brings the row back.
- **Search**: Windows Search where the folder is indexed, otherwise a recursive search.
- **Compact context menu** as in Windows 11, with your own promoted items (see Settings);
  "Show more options" (Shift+F10) for the classic menu.
- **File operations** through the shell: copy, move, rename, delete, new items, drag and
  drop, clipboard.
- **Large folders**: 30,000 files are on screen in ~60 ms and complete in under 0.2 s.
- **Media player** ("Play" / "Queue in Explorer Plus" in the context menu of media files and
  folders, or its button beside Refresh): video, or for music the album cover, tags and a
  spectrogram or one of a dozen beat-driven visualizations (V). Queues in tabs at the top of
  the window, kept across restarts: one plays while you edit another (Ctrl+N new, Ctrl+W
  close, Ctrl+Tab switch, F2 or double-click to rename, drag to reorder). Drag a queue's
  items to reorder them, or onto another tab (or "+") to move them there; Ctrl copies.
  Shuffle, repeat, loudness levelling, full screen, media keys and taskbar thumbnail
  buttons. Plays whatever Windows' Media Foundation can, nothing to install. Keys: Space
  play / pause, Left / Right seek, Ctrl+Left / Right previous / next, Up / Down volume,
  M mute, F full screen, Ctrl+H shuffle, Ctrl+T repeat.
- **Pictures in the player** ("View in Explorer Plus"): step through the folder with Left /
  Right or the wheel, slide show, zoom (Ctrl+wheel, + / −, 1, 0) and drag. JPEG, PNG, GIF,
  BMP, TIFF pages, HEIC / WebP / AVIF / raw (with their Store extensions), SVG, EMF / WMF,
  TGA, QOI, HDR, PSD, PCX; animated GIF and APNG.
- **Subtitles and audio tracks**: subtitle files beside a video (SRT, VTT, ASS / SSA) show
  over it; S next subtitles, A next audio track, or right-click to choose.
- **MIDI and audio CDs** play too.
- **Playlists** (M3U / M3U8, PLS, WPL / ZPL, ASX, XSPF) play like folders, internet radio
  included. "Create playlist..." writes one from files and folders; Ctrl+S saves a queue.
- **Media file types** ("…" ▸ Media file types...): make Explorer Plus the default app for the
  music, video, picture and playlist types you tick, for your account only.
- **Notepad** (its button beside Refresh, or "Edit in Explorer Plus"): text files in tabs, as
  in Windows 11, kept with unsaved text across restarts. Keeps each file's encoding and line
  endings; find and replace, go to line, word wrap, zoom; syntax colours for some 25
  languages; JSON / XML formatting; line tools (duplicate, move, sort, remove duplicates,
  join) and case / Base64 / URL conversion; double-click marks a word everywhere. Files up
  to 64 MB open at once.
- **Screenshots** (Alt+Q anywhere, or "…" ▸ Take a screenshot): a region or the whole screen,
  copied to the clipboard and opened in the picture editor.
- **Picture editor**: rectangles, ellipses and text that stay editable until saved; undo /
  redo; saves PNG, JPEG, BMP, GIF or TIFF.

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
shows a picture) in the player, as the registered file types do. `explorer-plus.exe --notepad
[file]` opens the Notepad (with the file in a tab); a Notepad already open takes it.

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
automatically. The Notepad keeps its tabs in `Notepad\session.ini` beside the settings file,
and each tab's unsaved text in a `note-*.txt` (UTF-16) there; the player keeps its queues in
`Player\queues.ini`.

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
ctest --test-dir build --output-on-failure                          # unit tests (INI, command line, play queue, kept queues, text files)
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
src/imageview, loudness   player's pictures (WIC, Direct2D SVG, GDI+ metafiles), loudness levelling
src/imageformats       TGA, QOI, HDR, PSD, PCX decoders; APNG frames; SVG size
src/screenshot, imageedit, shapes   Alt+Q screenshots, picture editor, its objects' geometry
src/assoc, userchoice  media file type registration (Media file types), the default-app hash
src/notepad, textfile  the Notepad window; text encodings, line endings, its kept tabs
src/syntax, textops    the Notepad's lexers, JSON / XML formatting; line, case, Base64, URL operations
src/settings, ini, theme, perf, crash   settings.ini, colours and fonts, profiling, crash dumps
src/cmdline            command line and typed-path text
tests/                 unit tests (ctest)
tools/                 smoke test, perf scenarios, packaging, icon generator
```

## License

[MIT](LICENSE)
