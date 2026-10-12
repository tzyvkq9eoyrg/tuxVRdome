# tuxVRdome

A small VR video player for Linux (OpenXR + libmpv), tested with SteamVR and the Steam Frame.
Videos and photos play on a flat or curved virtual screen in front of a calm night sky, or as
180°/360° or fisheye video, mono or 3D. Decoding runs on the GPU via NVDEC (HEVC, AV1, VP9, H.264,
up to 8K, HDR too). YouTube (search, links, playlists) works through yt-dlp.

The menus are in German by default; switch to English under Settings → Sprache (Language).

## Requirements

- Linux with an NVIDIA graphics card (NVDEC; without it, `--hwdec=no` works but is slower)
- SteamVR as the active OpenXR runtime (SteamVR settings → *OpenXR*)
- for YouTube: [yt-dlp](https://github.com/yt-dlp/yt-dlp), ideally a current one via pipx:
  `pipx install yt-dlp` (update: `pipx upgrade yt-dlp`)

## Install (AppImage)

```
chmod +x tuxVRdome-1.0.0-x86_64.AppImage
./tuxVRdome-1.0.0-x86_64.AppImage                 # starts in the file browser
./tuxVRdome-1.0.0-x86_64.AppImage video.mkv       # plays a video
```

The AppImage needs FUSE (`sudo apt install libfuse2t64`) and a system at least as new as the one
it was built on (Ubuntu 26.04).

## Build from source

Dependencies (Ubuntu 26.04):

```
sudo apt install build-essential cmake pkg-config libopenxr-dev libmpv-dev libglfw3-dev libgl-dev \
  libvulkan-dev libpango1.0-dev libcairo2-dev libcurl4-openssl-dev libgdk-pixbuf-2.0-dev \
  nlohmann-json3-dev libglib2.0-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
```

```
./build.sh                    # builds into build/ and runs the automatic tests
./build/tuxvrdome [video]     # run
sudo cmake --install build    # optional: install, with an application menu entry
./make-appimage.sh            # optional: build the AppImage
```

Options: `--hwdec=nvdec-copy` or `--hwdec=no` (if NVDEC causes trouble), `--no-mirror` (no preview
window on the desktop), `--timing` (print frame timings), `--version`.

## Controllers

| Input | Action |
|---|---|
| Trigger | Open the menu, click in the menu, aim elsewhere = close the menu; hold and drag on the seek bar |
| Menu button (left) | Open/close the menu |
| A (right) | Play / pause |
| Upper button (right) | Recenter |
| Right stick left/right | Seek; holding it seeks faster and faster (10 s → 30 s → 60 s) |
| Left stick left/right | Turn the view by 30° |
| Stick up/down | Volume; in lists, smooth scrolling (the further you tilt, the faster) |
| Hold grip | Move the screen, or the menu if the laser points at it, with your hand; stick up/down meanwhile = screen size |

## The menu

**Player page:** seek bar with preview frame and chapter marks, playback buttons, volume, repeat
(video / folder or playlist; a badge shows what repeats), speed, A-B loop, sleep timer. Below: info
about the video and buttons for chapters, audio track, subtitles (also from a file) and, for
YouTube, the quality. Bottom left: files, with favourites above and YouTube next to it; bottom
right: recenter and format. Top left: delete and the favourite star for the current file. The arrow
at the top folds the menu into a slim bar. Turn more than 30° away and the menu glides after you
(can be switched off; moved by hand, it keeps its height).

**File browser:** list or gallery with thumbnails, sorted by name, date or size (view and order are
remembered per folder). Folders and files can be added to the favourites with their star; the star
button at the top shows the favourites.

**Settings:** language (German/English), design (Transparent or Material), lying mode, background,
"menu follows gaze"; sub pages Picture (brightness, contrast, saturation, zoom), Subtitles (size,
height, distance) and YouTube (default quality).

## Formats

The format is detected from the file name (e.g. `_180_SBS`, `_360_TB`, `FISHEYE190`, `MKX200`).
Untagged: a large 2:1 video = 180° 3D side-by-side, a large 1:1 video = 360° 3D top-bottom, a large
2:1 photo = 360° panorama. Formats chosen by hand are remembered per video. For YouTube, the search
filter (360°/VR180), YouTube's format info and a look at the picture decide.

For 3D and 180°/360° video, the player draws subtitles itself as a floating panel that follows your
head. For 2D video they sit in the picture, or optionally float as well.

## Lying mode

"Recenter" then takes the full tilt of your head: lie down, look at the ceiling and recenter –
screen, video, subtitles and menu are then up there.

## Keyboard (desktop window focused)

| Key | Action |
|---|---|
| Tab | Open/close the menu |
| Space | Play / pause |
| ← / → | Seek |
| A / D | Turn the view left / right |
| ↑ / ↓ | Next / previous video in the folder |
| + / − , M | Volume, mute |
| P / L / C | Cycle projection / 3D layout / curved screen |
| W / S, Q / E | Screen closer/farther, smaller/larger |
| R | Recenter |
| F | Flip the video vertically |
| Esc, Esc | Quit |

## Stored data and privacy

Everything lives in `~/.config/tuxvrdome/`:

- `settings.conf` – settings
- `videos.conf`, `folders.conf` – what is remembered per video or folder (format, picture, volume,
  sort order); only hashes of the paths or YouTube IDs, no file or folder names
- `favorites.conf` – the favourites, as paths (there is no other way to find them again)

Thumbnails are kept in memory only. File paths are redacted in the console output; video titles and
descriptions are not printed.

Deleted videos go to the trash where there is one; on network drives the dialog warns that the file
will be deleted for good.

## License

GPL-3.0 – see [LICENSE](LICENSE).
