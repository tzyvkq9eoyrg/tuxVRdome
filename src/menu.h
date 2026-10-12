#pragma once

#include "cairo_canvas.h"
#include "favorites.h"
#include "settings.h"
#include "strings.h"
#include "video_format.h"
#include "video_player.h"
#include "youtube.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

class Thumbnailer;
class FolderMemory;

// Something the menu asks the app to do.
struct MenuCommand {
  enum Type { None, TogglePause, Seek, SeekTo, ToggleMute, PreviousVideo, NextVideo,
              SetProjection, SetStereo, ToggleCurved, Recenter, OpenFile, SelectAudio,
              SelectSubtitle, LoadSubtitle, SetLanguage, ToggleFloatingSubtitles, PlayYouTube,
              SetQuality, SetVolume, SetStreamQuality, ToggleLyingMode, RequestDelete,
              DeleteFile, Quit, SetSpeed, SetLoop, ToggleAbLoop, SetSleepTimer, SetBrightness,
              SetContrast, SetSaturation, SetZoom, ResetPicture, Paste, SetTheme,
              SetSlideshow, ToggleBackground, ToggleMenuFollows, SetSubtitleSize,
              SetSubtitleRaise, SetSubtitleDistance };
  Type type = None;
  // seconds (Seek, SeekTo), enum index, track id (0 = off),
  // YouTubeVideo::Kind (PlayYouTube), maximum stream width (SetQuality for the
  // default, SetStreamQuality for the running video; 0 = best), volume 0..100,
  // or 1 = delete for good instead of trashing (DeleteFile), playback speed,
  // LoopMode, sleep timer minutes (0 = off), picture value -100..100, zoom in
  // percent, slideshow seconds per photo (0 = off)
  double value = 0;
  // OpenFile, LoadSubtitle, PlayYouTube (URL), RequestDelete and DeleteFile.
  // RequestDelete asks the app to check the trash and call askDelete();
  // DeleteFile is the confirmed deletion.
  std::string path;
};

enum class LoopMode { Off, File, Folder };

// Everything the menu shows about the app.
struct MenuView {
  PlaybackState playback;
  VideoFormat format;
  bool curvedScreen = false;
  std::string currentFile;
  Language language = Language::English;
  Theme theme = Theme::Glass;
  bool floatingSubtitles = false;
  int maxVideoWidth = 0;   // default quality for streams
  bool streaming = false;  // the current video is a stream (YouTube etc.)
  int streamWidth = 0;     // quality limit of the running stream
  // The sizes the running stream is offered in (from 1080p up), largest
  // first; empty until known.
  std::vector<std::pair<int, int>> streamSizes;  // width, height
  int videoWidth = 0, videoHeight = 0;
  bool lyingMode = false;
  bool background = true;  // night sky around the flat screen
  bool menuFollows = true;
  int subtitleSize = 100, subtitleRaise = 0, subtitleDistance = 150;  // see Settings
  LoopMode loop = LoopMode::Off;
  int sleepMinutesLeft = 0;  // 0 = no sleep timer
  bool photo = false;        // the current file is a photo
  int slideshowSeconds = 0;  // 0 = no slideshow
  // Picture settings of the running video.
  int brightness = 0, contrast = 0, saturation = 0;
  float zoom = 1.0f;
};

// The VR menu: a main panel (playback controls, then settings, tracks and a
// file browser as sub pages) and an optional format panel to its right.
// Drawn with cairo/pango into textures that are only redrawn on changes.
class Menu {
 public:
  enum PanelId { kMain = 0, kFormat = 1 };
  // Texture sizes; both have the same pixels per meter in VR.
  static constexpr int kMainWidth = 1600;
  static constexpr int kMainHeight = 900;
  static constexpr int kFormatWidth = 450;
  static constexpr int kFormatHeight = 600;
  // Glass theme: the panels' own coverage where they are most transparent;
  // the renderer shows the blurred scene through them.
  static constexpr float kGlassAlpha = 0.34f;

  ~Menu();
  bool init();
  // Source of the file browser's thumbnails (optional, not owned).
  void setThumbnailer(Thumbnailer* thumbnailer) { thumbnailer_ = thumbnailer; }
  // Where each folder's order and list/gallery choice is kept (optional, not owned).
  void setFolderMemory(FolderMemory* folders) { folders_ = folders; }

  void update(const MenuView& view);
  // Pointer on a panel in 0..1 of its visible area (v = 0 at the top);
  // panel -1 = pointing at neither.
  void setPointer(int panel, float u, float v);
  // Clicks at the pointer. Navigation inside the menu is handled here; the
  // rest is returned for the app to carry out.
  MenuCommand click();
  // Call every frame with whether the trigger is held: while a slider grabbed
  // by click() is held, returns its new value as the pointer moves.
  MenuCommand drag(bool held);
  // Scrolls a list page by (fractions of) rows, positive = down.
  void scroll(double rows);
  // While the pointer is over the player page's seek bar: where it points,
  // 0..1 of the video. For the preview frame.
  bool seekHover(double& fraction) const;
  // Switches to the video file browser, next to the current video or in $HOME.
  void showFileBrowser();
  // Switches to the file browser's list of favourite folders and files.
  void showFavorites();
  // Switches to the player page; other pages keep their state for later.
  void showMainPage() { showPage(Page::Main); }

  // Confirmation dialogs for deleting a video. While one is open, clicks
  // outside it are ignored.
  void askDelete(const std::string& path, bool trashAvailable);
  void askDeleteAfterTrashFailed(const std::string& path);
  void showDeleteError(const std::string& error);
  // Re-reads the file browser's folder (e.g. after a file was deleted).
  void refreshDirectory();
  // True on pages with a scrollable list (files, tracks, search results).
  bool listOpen() const {
    return page_ == Page::Files || page_ == Page::YouTubeResults;
  }

  // Whether a YouTube video is part of the listed playlist (or channel).
  bool inPlaylist(const std::string& url) const;
  // The playlist video `step` places away from `url` as a PlayYouTube
  // command, wrapping around at the ends if `wrap`; None if there is none.
  MenuCommand playlistNeighbor(const std::string& url, int step, bool wrap) const;

  // While the search keyboard is shown, the desktop keyboard types into it too.
  bool textInputActive() const { return page_ == Page::YouTubeKeyboard; }
  void typeText(const std::string& utf8);
  void backspace();
  // Enter: searches, or for a YouTube video link returns the command to play it.
  MenuCommand submitText();
  bool formatPanelOpen() const { return formatOpen_ && page_ == Page::Main; }

  // Height in pixels of the main panel's visible part; it depends on the page.
  int mainVisibleHeight() const;
  // Panel texture (sRGB, premultiplied alpha), redrawn first if needed.
  GLuint texture(PanelId panel);
  // The button under the pointer on this panel, in texture pixels
  // (x0, y0, x1, y1), with its corner radius and whether it is destructive
  // (highlighted red), for the shader. False if there is none.
  bool hoverRect(PanelId panel, float rect[4], float& radius, bool& danger) const;
  bool glass() const { return view_.theme == Theme::Glass; }

 private:
  enum class Page { Main, Settings, YouTubeSettings, Picture, SubtitleStyle, Files, YouTubeKeyboard,
                    YouTubeResults };
  enum class FileKind { Video, Subtitle };
  enum class Icon { None, Play, Pause, Previous, Next, Back10, Forward10, VolumeUp,
                    Mute, Folder, FolderUp, Settings, Recenter, YouTube, Trash, Power, Loop,
                    Sleep, Globe, Gauge, AbLoop, FormatDome, ProjFlat, Proj180, Proj360,
                    Fisheye190, Fisheye200, Cube, StereoMono, StereoSbs, StereoTb, Curved,
                    Slideshow, JumpTop, JumpBottom, Sort, Grid, List, Collapse, Expand, Star,
                    StarFilled };
  enum class Dialog { None, ConfirmTrash, ConfirmDelete, TrashFailed, Error, ConfirmQuit };
  // Small menus that open as a strip above their button.
  enum class Dropdown { None, Quality, Speed, Sleep, Slideshow, Sort, Audio, Subtitles, Chapters };
  // File browser order (folders always come first).
  enum class SortMode { NameUp, NameDown, Newest, Oldest, Largest, kCount };
  struct Button {
    int panel = kMain;
    double x = 0, y = 0, w = 0, h = 0;
    std::string label;
    Icon icon = Icon::None;
    bool selected = false;
    std::function<MenuCommand()> action;  // null = not clickable (list heading)
    bool listRow = false;
    int thumbnail = -1;    // search result row: index into thumbnails_
    std::string detail;    // search result row: second line
    std::string thumbPath; // file browser video row: whose thumbnail to show
    std::string tooltip;   // icon-only button: its name, shown after hovering a while
    bool volumeSlider = false;  // click/drag sets the volume from the pointer's x
    bool popup = false;         // part of the open dropdown or dialog, drawn on top
    bool danger = false;        // destructive action, drawn red
    bool dangerHover = false;   // destructive, but only shown red when hovered
    bool plain = false;         // no background of its own (Material theme)
    bool bare = false;          // no background in any theme (unless selected)
    double radius = -1;         // corner radius, -1 = the default
    std::string caption;        // format tile: small label under the icon
    bool disabled = false;      // not relevant right now: greyed out, not clickable
    bool card = false;          // file browser gallery: large thumbnail, name under it
    int fontSize = 0;           // label size in pixels, 0 = the default
    std::string badge;          // short mark on a bright disc at the top-right corner
    bool scrolls = false;       // part of a smoothly scrolled list: cut off at its edges
  };
  struct FileEntry {
    std::string name;
    std::string path;
    bool directory;
    int64_t modified = 0;  // only read when sorting by date
    uint64_t size = 0;     // only read when sorting by size
  };
  struct ListItem {
    std::string label;
    bool selected;
    std::function<MenuCommand()> action;
    std::function<MenuCommand()> trash = nullptr;  // optional delete button at the row's end
    std::string thumbPath = {};                    // video file: show its thumbnail
    bool folder = false;
    std::function<MenuCommand()> star = nullptr;   // folder: favourite star at the row's end
    bool starred = false;
  };
  static constexpr double kStarW = 90;
  Button starButton(const ListItem& item, double x, double y, double w, double h);
  // The file browser's favourite folders and files; a star toggles one.
  std::vector<ListItem> favoriteItems();
  ListItem folderItem(const std::string& label, const std::string& path);
  ListItem fileItem(const std::string& label, const std::string& path);
  bool showFavorites_ = false;  // the file browser lists the favourites
  Favorites favorites_;
  const Strings& text() const { return strings(view_.language); }
  Button iconButton(Icon icon, std::function<MenuCommand()> action, bool selected = false) const;
  Button textButton(const std::string& label, Icon icon, std::function<MenuCommand()> action,
                    bool selected = false) const;
  void layout();
  void layoutPage();
  void layoutMain();
  void layoutFormat();
  void layoutSettings();
  void layoutYouTubeSettings();
  void layoutPicture();
  void layoutSubtitleStyle();
  // The player page folded to a slim bar: seek bar and transport only.
  void layoutCollapsed();
  // The volume pill at height y: its buttons, and its background.
  void addVolume(double y);
  void drawVolumePill(cairo_t* cr, double y) const;
  // "−  value  +" at height y; the value is drawn by the page.
  void addStepper(double y, int value, int lo, int hi, int step, MenuCommand::Type type,
                  bool disabled = false);
  // Height of the player page's seek bar line.
  double seekLineY() const;
  void layoutPlaybackRow();
  // Adds a round icon button with a tooltip; returns it (valid until the next add).
  Button& addRound(Icon icon, const std::string& tooltip, std::function<MenuCommand()> action,
                   double x, double y, double size = 88, bool selected = false);
  void addDropdown(const Button& opener, std::vector<Button> entries);
  void layoutDialog();
  void drawDialog(cairo_t* cr);
  void openDialog(Dialog dialog, const std::string& path, const std::string& error = "");
  std::string qualityLabel(int width) const;
  void layoutKeyboard();
  void layoutResults();
  MenuCommand startSearch();
  void rebuildThumbnails();
  void addCenteredRow(double y, double h, std::vector<Button> items);
  void addRow(double y, double h, std::vector<Button> items, double x0);
  void addList(const std::vector<ListItem>& items, double x0 = 40, double rowH = 58,
               int rows = 9);
  // The file browser as a grid of large thumbnails.
  void addGallery(const std::vector<ListItem>& items, double x0);
  // Asks for the thumbnails of the items on screen [first, end) and, bounded,
  // of those around them and at both ends of the folder. `page` = items per screen.
  void wantThumbnails(const std::vector<ListItem>& items, int first, int end, int page);
  // The column right of the file list: jump to top / bottom, sort, list or gallery.
  void layoutFileTools();
  std::vector<ListItem> fileItems();
  std::string trackName(const Track& track) const;
  // The info line under the player page: plain facts as chips, then the
  // audio track and subtitle buttons (each opens its list).
  void layoutInfoLine();
  void addDropdownList(const Button& opener, std::vector<Button> entries);
  struct InfoChip {
    double x, w;
    std::string text;
  };
  std::vector<InfoChip> infoChips_;
  void openFiles(FileKind kind);
  void openDirectory(const std::string& dir);
  void sortEntries();
  void showPage(Page page);
  int buttonAt(int panel, double px, double py) const;
  void redraw();
  void drawMain(cairo_t* cr);
  void drawFormat(cairo_t* cr);
  void drawButtons(cairo_t* cr, int panel, bool popups = false);
  void drawVolumeSlider(cairo_t* cr, const Button& b) const;
  void drawTrack(cairo_t* cr, double x0, double x1, double y, double fraction, double buffered,
                 double alpha) const;
  void drawButtonContent(cairo_t* cr, const Button& b, const double ink[4]) const;
  MenuCommand volumeAtPointer(const Button& slider) const;
  static void drawIcon(cairo_t* cr, Icon icon, double cx, double cy, double s, double r = 1,
                       double g = 1, double b = 1);
  // The button's fill and its icon/text colour in the current theme.
  void buttonColors(const Button& b, double fill[4], double ink[4]) const;
  void drawMainPage(cairo_t* cr);
  double textWidth(const std::string& text, int size, bool bold) const;
  void drawText(cairo_t* cr, const std::string& text, double x, double y, double w, double h,
                int size, bool bold, bool center, double r, double g, double b, double a = 1.0) const;

  CairoCanvas canvases_[2];
  bool dirty_ = true;

  MenuView view_;

  int pointerPanel_ = -1;
  double pointerX_ = 0, pointerY_ = 0;  // texture pixels of pointerPanel_
  int hovered_ = -1;
  double hoverStart_ = 0;  // seconds (steady clock) since hovered_ points at its button
  int tooltip_ = -1;       // button whose tooltip is drawn, -1 = none
  bool draggingVolume_ = false;
  bool draggingSeek_ = false;  // the seek bar follows the pointer while the trigger is held
  double lastScrub_ = -1;      // seconds last sent while dragging it
  double seekAtPointer() const;  // seconds under the pointer on the seek bar
  Dropdown dropdown_ = Dropdown::None;
  bool thumbnailsPending_ = false;  // new thumbnails wait for the next redraw
  double lastThumbnailRedraw_ = 0;  // seconds, steady clock
  Thumbnailer* thumbnailer_ = nullptr;
  FolderMemory* folders_ = nullptr;
  void rememberFolderView();
  Dialog dialog_ = Dialog::None;
  std::string dialogPath_;
  std::string dialogError_;

  Page page_ = Page::Main;
  bool formatOpen_ = false;
  bool collapsed_ = false;  // the player page folded to a slim bar
  FileKind fileKind_ = FileKind::Video;
  std::string directory_;
  std::vector<FileEntry> entries_;
  SortMode sortMode_ = SortMode::NameUp;
  bool gallery_ = false;  // file browser as a grid of large thumbnails
  int scrollOffset_ = 0;
  double scrollFraction_ = 0;  // smooth scrolling: part of a row scrolled past scrollOffset_
  double listTop_ = 0;         // y of the list's first visible row
  // Clamps the scroll position to `count` rows of which `rows` fit, and
  // returns the first and last (exclusive) row to lay out.
  void clampScroll(int count, int rows, int& first, int& end);
  int listCount_ = 0;     // items (gallery: grid rows) on the current list page, for scrolling
  int visibleRows_ = 9;   // rows the current list page shows at once
  double rowHeight_ = 58;
  double listRight_ = 1560;  // right edge of the list (left of the scroll bar)

  YouTubeSearch youtube_;
  std::string query_;
  bool shift_ = false;
  bool vrOnly_ = true;
  bool searchBusy_ = false;
  std::string searchError_;
  std::vector<YouTubeVideo> videos_;
  std::vector<cairo_surface_t*> thumbnails_;  // per video, null until downloaded

  std::vector<Button> buttons_;
};
