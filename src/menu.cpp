#include "menu.h"

#include "file_util.h"
#include "thumbnailer.h"
#include "video_memory.h"

#include <cairo.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace {

constexpr double kMargin = 40;
constexpr double kGap = 16;

// Player page, after the user's sketch, kept flat: delete, title, settings
// and quit along the top; the seek bar; transport centred with the playback
// options staggered below and the volume to its right; along the bottom
// YouTube and files on the left, the video info in the middle, recenter and
// format on the right.
constexpr int kMainPageHeight = 512;
constexpr int kCollapsedHeight = 212;  // the folded player page
constexpr double kMiniSeekY = 50;      // its seek bar
constexpr double kMiniRowY = 112;      // its buttons
constexpr double kTopY = 28;        // top row of buttons
constexpr double kEdgeButton = 80;  // the buttons along the top and bottom
constexpr double kMainSeekY = 140;  // seek bar centre line on the player page
constexpr double kSubSeekY = 150;   // ... and on the sub pages
constexpr double kSeekBand = 28;    // clickable distance above and below it
constexpr double kBigButton = 88;
constexpr double kSmallButton = 72;
constexpr double kPlayButton = 156;
constexpr double kTransportY = 206;  // top of the transport buttons
constexpr double kOptionsY = 338;    // top of the playback options
constexpr double kBottomY = 404;     // bottom row of buttons
constexpr double kInfoY = 434;       // video info line
constexpr double kVolumeX = 1124, kVolumeW = 436;  // to the panel's right margin
constexpr double kVolumeY = 272;  // between the transport row and the options row

// Sub pages: header row with "Back", then a list.
constexpr double kHeaderY = 220;
constexpr double kHeaderButtonW = 300;
constexpr double kListY = 330;
constexpr double kRowH = 58;
constexpr int kVisibleRows = 9;
constexpr double kFileRowH = 94;  // file browser rows, tall enough for thumbnails
constexpr int kFileRows = 6;

// Settings page: sub pages as a column on the left, settings on the right
// with their labels.
constexpr double kSettingsNavW = 400;
constexpr double kSettingsLabelX = 500;
constexpr double kSettingsControlX = 780;

constexpr double kButtonRadius = 18;  // the panel shader's hover outline uses the same
constexpr double kIconButtonW = 120;

// Selectable stream quality limits. They are widths: an 8K 360° video is
// 7680 pixels wide but only 3840 high.
constexpr int kQualityWidths[] = {0, 5760, 3840, 1920};
constexpr double kIconSize = 52;

constexpr double kSpeeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
constexpr int kSleepMinutes[] = {0, 15, 30, 45, 60, 90};
constexpr int kSlideshowSeconds[] = {0, 3, 5, 10, 20, 30, 60};
constexpr int kPictureStep = 5;  // brightness etc., of mpv's -100..100
constexpr int kZoomStepPercent = 10;

// "1×", "1,25×" (German decimal comma) or "1.25×".
std::string speedLabel(double speed, Language language) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%g×", speed);
  std::string s = buf;
  if (language == Language::German) std::replace(s.begin(), s.end(), '.', ',');
  return s;
}

double steadySeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string formatTime(double seconds) {
  if (!(seconds >= 0)) seconds = 0;
  const int total = static_cast<int>(seconds);
  char buf[32];
  if (total >= 3600)
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
  else
    std::snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
  return buf;
}

// Everything about the tracks the menu shows, to detect changes.
std::string trackSignature(const std::vector<Track>& tracks) {
  std::string s;
  for (const Track& t : tracks)
    s += std::to_string(t.id) + t.type + (t.selected ? "*" : "") + t.title + t.lang + "|";
  return s;
}

void roundedRect(cairo_t* cr, double x, double y, double w, double h, double r) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

void triangle(cairo_t* cr, double x0, double y0, double x1, double y1, double x2, double y2) {
  cairo_move_to(cr, x0, y0);
  cairo_line_to(cr, x1, y1);
  cairo_line_to(cr, x2, y2);
  cairo_close_path(cr);
}

// Speaker body pointing right, within the left 55% of an s-sized box.
void speaker(cairo_t* cr, double x, double y, double s) {
  cairo_rectangle(cr, x, y + s * 0.35, s * 0.2, s * 0.3);
  cairo_move_to(cr, x + s * 0.2, y + s * 0.35);
  cairo_line_to(cr, x + s * 0.5, y + s * 0.1);
  cairo_line_to(cr, x + s * 0.5, y + s * 0.9);
  cairo_line_to(cr, x + s * 0.2, y + s * 0.65);
  cairo_close_path(cr);
  cairo_fill(cr);
}

std::function<MenuCommand()> command(MenuCommand::Type type, double value = 0) {
  return [type, value] {
    MenuCommand c;
    c.type = type;
    c.value = value;
    return c;
  };
}

// Colours of one theme (cairo colours, not premultiplied).
struct Rgba {
  double r, g, b, a;
};
struct Palette {
  Rgba panel, panelEdge;          // panel fill and outline (alpha 0: none)
  Rgba button, buttonEdge, icon;  // a button and its icon / text
  Rgba buttonOn, iconOn;          // a selected button
  Rgba tileEdge;                  // format tiles' outline
  Rgba tileOn, tileInkOn;         // a selected format tile
  Rgba text, textDim;
  Rgba accent, track, buffered;   // seek bar and sliders
  Rgba play, playIcon;
  Rgba chip, chipEdge;
  double panelRadius, playRadius, tileRadius;
  bool thickTracks;  // Material: thick slider tracks with a bar handle
};

// Liquid glass over the blurred scene: buttons are nearly clear with a thin
// light rim; selected ones are frosted, more opaque white glass.
const Palette kGlassPalette = {
    {0.10, 0.14, 0.20, Menu::kGlassAlpha}, {1, 1, 1, 0.32},
    {1, 1, 1, 0.07}, {1, 1, 1, 0.38}, {1, 1, 1, 1},
    {1, 1, 1, 0.26}, {1, 1, 1, 1},
    {1, 1, 1, 0.30},
    {1, 1, 1, 0.26}, {1, 1, 1, 1},
    {1, 1, 1, 1}, {1, 1, 1, 0.75},
    {1, 1, 1, 1}, {1, 1, 1, 0.22}, {1, 1, 1, 0.40},
    {1, 1, 1, 0.12}, {1, 1, 1, 1},
    {1, 1, 1, 0.06}, {1, 1, 1, 0.30},
    64, -1, 30, false};

// Material 3, dark: opaque tonal surfaces with an ocean blue accent.
const Palette kMaterialPalette = {
    {0.090, 0.114, 0.129, 0.97}, {0, 0, 0, 0},
    {0.169, 0.227, 0.267, 1}, {0, 0, 0, 0}, {0.812, 0.898, 0.961, 1},
    {0.549, 0.804, 1.0, 1}, {0.0, 0.204, 0.310, 1},
    {0.251, 0.282, 0.302, 1},
    {0.0, 0.298, 0.431, 1}, {0.788, 0.902, 1.0, 1},
    {0.933, 0.945, 0.953, 1}, {0.753, 0.784, 0.804, 1},
    {0.549, 0.804, 1.0, 1}, {0.165, 0.200, 0.220, 1}, {0.240, 0.333, 0.400, 1},
    {0.788, 0.902, 1.0, 1}, {0.0, 0.118, 0.188, 1},
    {0, 0, 0, 0}, {0.251, 0.282, 0.302, 1},
    56, 48, 28, true};

const Palette& paletteFor(Theme theme) {
  return theme == Theme::Material ? kMaterialPalette : kGlassPalette;
}

void setColor(cairo_t* cr, const Rgba& c, double alpha = 1.0) {
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * alpha);
}

// A button of liquid glass: a faint tint, a soft inner glow along the edge
// where the glass bends the light, and a thin rim that catches the light at
// the upper left and lower right and nearly vanishes on the sides between.
void glassButton(cairo_t* cr, double x, double y, double w, double h, double r,
                 const double fill[4]) {
  roundedRect(cr, x, y, w, h, r);
  cairo_set_source_rgba(cr, fill[0], fill[1], fill[2], fill[3]);
  cairo_fill_preserve(cr);
  cairo_save(cr);
  cairo_clip_preserve(cr);  // only the inner half of the wide stroke shows
  cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
  cairo_set_line_width(cr, 10);
  cairo_stroke(cr);
  cairo_restore(cr);
  roundedRect(cr, x + 0.75, y + 0.75, w - 1.5, h - 1.5, std::max(0.0, r - 0.75));
  cairo_pattern_t* rim = cairo_pattern_create_linear(x, y, x + w, y + h);
  cairo_pattern_add_color_stop_rgba(rim, 0.0, 1, 1, 1, 0.85);
  cairo_pattern_add_color_stop_rgba(rim, 0.28, 1, 1, 1, 0.22);
  cairo_pattern_add_color_stop_rgba(rim, 0.5, 1, 1, 1, 0.08);
  cairo_pattern_add_color_stop_rgba(rim, 0.72, 1, 1, 1, 0.22);
  cairo_pattern_add_color_stop_rgba(rim, 1.0, 1, 1, 1, 0.60);
  cairo_set_source(cr, rim);
  cairo_set_line_width(cr, 1.4);
  cairo_stroke(cr);
  cairo_pattern_destroy(rim);
}

// Strokes a path drawn in a 24-unit icon box (like an SVG icon) at x, y with size s.
void beginIcon24(cairo_t* cr, double x, double y, double s) {
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, s / 24, s / 24);
  cairo_set_line_width(cr, 1.8);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
}

void ellipse(cairo_t* cr, double cx, double cy, double rx, double ry, double from = 0,
             double to = 2 * M_PI) {
  cairo_save(cr);
  cairo_translate(cr, cx, cy);
  cairo_scale(cr, rx, ry);
  cairo_new_sub_path(cr);
  cairo_arc(cr, 0, 0, 1, from, to);
  cairo_restore(cr);
}

}  // namespace

// Draws an icon in colour r, g, b centered at (cx, cy) in an s-sized box.
void Menu::drawIcon(cairo_t* cr, Icon icon, double cx, double cy, double s, double r, double g,
                    double b) {
  const double x = cx - s / 2, y = cy - s / 2;
  cairo_save(cr);
  cairo_set_source_rgba(cr, r, g, b, 1);
  cairo_set_line_width(cr, s * 0.08);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  switch (icon) {
    case Icon::Play:
      triangle(cr, x + s * 0.2, y, x + s * 0.2, y + s, x + s * 0.95, y + s / 2);
      cairo_fill(cr);
      break;
    case Icon::Pause:
      cairo_rectangle(cr, x + s * 0.15, y, s * 0.25, s);
      cairo_rectangle(cr, x + s * 0.6, y, s * 0.25, s);
      cairo_fill(cr);
      break;
    case Icon::Previous:
      cairo_rectangle(cr, x + s * 0.05, y + s * 0.1, s * 0.12, s * 0.8);
      triangle(cr, x + s * 0.9, y + s * 0.1, x + s * 0.9, y + s * 0.9, x + s * 0.2, y + s / 2);
      cairo_fill(cr);
      break;
    case Icon::Next:
      cairo_rectangle(cr, x + s * 0.83, y + s * 0.1, s * 0.12, s * 0.8);
      triangle(cr, x + s * 0.1, y + s * 0.1, x + s * 0.1, y + s * 0.9, x + s * 0.8, y + s / 2);
      cairo_fill(cr);
      break;
    case Icon::Back10:
    case Icon::Forward10: {
      // A circular arrow around a small "10".
      const bool back = icon == Icon::Back10;
      beginIcon24(cr, x, y, s);
      // Most of a circle, from the side to the upper corner, then the arrow head.
      if (back) {
        cairo_arc_negative(cr, 12, 12, 8.5, M_PI, -0.75 * M_PI);
        cairo_line_to(cr, 3.5, 8.5);
        cairo_move_to(cr, 3.5, 3.5);
        cairo_line_to(cr, 3.5, 8.5);
        cairo_line_to(cr, 8.5, 8.5);
      } else {
        cairo_arc(cr, 12, 12, 8.5, 0, 1.75 * M_PI);
        cairo_line_to(cr, 20.5, 8.5);
        cairo_move_to(cr, 20.5, 3.5);
        cairo_line_to(cr, 20.5, 8.5);
        cairo_line_to(cr, 15.5, 8.5);
      }
      cairo_stroke(cr);
      cairo_restore(cr);
      PangoLayout* layout = pango_cairo_create_layout(cr);
      PangoFontDescription* font = pango_font_description_from_string("Sans Bold");
      pango_font_description_set_absolute_size(font, s * 0.3 * PANGO_SCALE);
      pango_layout_set_font_description(layout, font);
      pango_layout_set_text(layout, "10", -1);
      int tw = 0, th = 0;
      pango_layout_get_pixel_size(layout, &tw, &th);
      cairo_move_to(cr, cx - tw / 2.0, cy - th / 2.0);
      pango_cairo_show_layout(cr, layout);
      pango_font_description_free(font);
      g_object_unref(layout);
      break;
    }
    case Icon::VolumeUp:
    case Icon::Mute:
      speaker(cr, x, y, s);
      if (icon == Icon::Mute) {
        cairo_move_to(cr, x + s * 0.65, y + s * 0.32);
        cairo_line_to(cr, x + s * 0.95, y + s * 0.68);
        cairo_move_to(cr, x + s * 0.95, y + s * 0.32);
        cairo_line_to(cr, x + s * 0.65, y + s * 0.68);
        cairo_stroke(cr);
      } else {
        for (int i = 0; i < 2; ++i) {
          cairo_new_sub_path(cr);
          cairo_arc(cr, x + s * 0.5, y + s / 2, s * (0.2 + 0.22 * i), -M_PI / 4, M_PI / 4);
          cairo_stroke(cr);
        }
      }
      break;
    case Icon::Folder:
    case Icon::FolderUp:
      cairo_move_to(cr, x, y + s * 0.15);
      cairo_line_to(cr, x + s * 0.38, y + s * 0.15);
      cairo_line_to(cr, x + s * 0.48, y + s * 0.27);
      cairo_line_to(cr, x + s, y + s * 0.27);
      cairo_line_to(cr, x + s, y + s * 0.88);
      cairo_line_to(cr, x, y + s * 0.88);
      cairo_close_path(cr);
      cairo_fill(cr);
      if (icon == Icon::FolderUp) {
        // Arrow pointing up, cut out of the folder in the button colour.
        cairo_set_source_rgba(cr, 0.17, 0.17, 0.21, 1);
        triangle(cr, cx, y + s * 0.34, cx - s * 0.2, y + s * 0.56, cx + s * 0.2, y + s * 0.56);
        cairo_fill(cr);
        cairo_rectangle(cr, cx - s * 0.07, y + s * 0.55, s * 0.14, s * 0.24);
        cairo_fill(cr);
      }
      break;
    case Icon::Settings: {
      // Gear: eight teeth around a ring.
      for (int i = 0; i < 8; ++i) {
        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_rotate(cr, i * M_PI / 4);
        cairo_rectangle(cr, -s * 0.09, -s * 0.5, s * 0.18, s * 0.25);
        cairo_restore(cr);
      }
      cairo_fill(cr);
      cairo_set_line_width(cr, s * 0.16);
      cairo_arc(cr, cx, cy, s * 0.28, 0, 2 * M_PI);
      cairo_stroke(cr);
      break;
    }
    case Icon::Recenter:
      cairo_arc(cr, cx, cy, s * 0.32, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_arc(cr, cx, cy, s * 0.08, 0, 2 * M_PI);
      cairo_fill(cr);
      for (int i = 0; i < 4; ++i) {
        const double a = i * M_PI / 2;
        cairo_move_to(cr, cx + std::cos(a) * s * 0.36, cy + std::sin(a) * s * 0.36);
        cairo_line_to(cr, cx + std::cos(a) * s * 0.5, cy + std::sin(a) * s * 0.5);
      }
      cairo_stroke(cr);
      break;
    case Icon::YouTube:
      roundedRect(cr, x, y + s * 0.15, s, s * 0.7, s * 0.18);
      cairo_set_source_rgba(cr, 0.95, 0.15, 0.15, 1);
      cairo_fill(cr);
      triangle(cr, x + s * 0.4, y + s * 0.33, x + s * 0.4, y + s * 0.67, x + s * 0.7, y + s / 2);
      cairo_set_source_rgba(cr, 1, 1, 1, 1);
      cairo_fill(cr);
      break;
    case Icon::Trash:
      // Lid with handle, then a tapered bin with two grooves.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 4, 7);
      cairo_line_to(cr, 20, 7);
      cairo_move_to(cr, 9, 7);
      cairo_line_to(cr, 9, 4.5);
      cairo_line_to(cr, 15, 4.5);
      cairo_line_to(cr, 15, 7);
      cairo_move_to(cr, 6, 7);
      cairo_line_to(cr, 7, 20);
      cairo_line_to(cr, 17, 20);
      cairo_line_to(cr, 18, 7);
      cairo_move_to(cr, 10, 11);
      cairo_line_to(cr, 10, 17);
      cairo_move_to(cr, 14, 11);
      cairo_line_to(cr, 14, 17);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Loop:
      // Two arrows chasing each other.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 17, 2.5);
      cairo_line_to(cr, 20.5, 6);
      cairo_line_to(cr, 17, 9.5);
      cairo_move_to(cr, 3.5, 11.5);
      cairo_line_to(cr, 3.5, 10);
      cairo_arc(cr, 7.5, 10, 4, M_PI, 1.5 * M_PI);
      cairo_line_to(cr, 20.5, 6);
      cairo_move_to(cr, 7, 21.5);
      cairo_line_to(cr, 3.5, 18);
      cairo_line_to(cr, 7, 14.5);
      cairo_move_to(cr, 20.5, 12.5);
      cairo_line_to(cr, 20.5, 14);
      cairo_arc(cr, 16.5, 14, 4, 0, 0.5 * M_PI);
      cairo_line_to(cr, 3.5, 18);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Globe:
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
      cairo_move_to(cr, 3, 12);
      cairo_line_to(cr, 21, 12);
      ellipse(cr, 12, 12, 4, 9);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Gauge:
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 12, 14, 9, M_PI * 0.85, M_PI * 2.15);
      cairo_move_to(cr, 12, 14);
      cairo_line_to(cr, 16.5, 9.5);
      cairo_stroke(cr);
      cairo_arc(cr, 12, 14, 1.4, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::AbLoop:
      // "AB" drawn as strokes.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 3, 17);
      cairo_line_to(cr, 6, 8);
      cairo_line_to(cr, 9, 17);
      cairo_move_to(cr, 4, 14);
      cairo_line_to(cr, 8, 14);
      cairo_move_to(cr, 14, 17);
      cairo_line_to(cr, 14, 8);
      cairo_line_to(cr, 17, 8);
      cairo_arc(cr, 17, 10.2, 2.2, -M_PI / 2, M_PI / 2);
      cairo_line_to(cr, 14, 12.4);
      cairo_move_to(cr, 14, 12.4);
      cairo_line_to(cr, 17.7, 12.4);
      cairo_arc(cr, 17.7, 14.7, 2.3, -M_PI / 2, M_PI / 2);
      cairo_line_to(cr, 14, 17);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::FormatDome:
      // A dome over a floor with the viewer in the middle: how the video surrounds you.
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 12, 15.5, 9, M_PI, 2 * M_PI);
      ellipse(cr, 12, 15.5, 9, 3);
      cairo_stroke(cr);
      cairo_arc(cr, 12, 15.5, 1.4, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::ProjFlat:
      beginIcon24(cr, x, y, s);
      cairo_rectangle(cr, 3, 6, 18, 12);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Proj180:
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 12, 16, 9, M_PI, 2 * M_PI);
      cairo_close_path(cr);
      ellipse(cr, 12, 16, 3, 9, M_PI, 2 * M_PI);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Proj360:
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
      ellipse(cr, 12, 12, 9, 3.2);
      ellipse(cr, 12, 12, 3.2, 9);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Fisheye190:
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 5, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_arc(cr, 10.2, 10.2, 1.3, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::Fisheye200:
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 12, 12, 9.5, 0, 2 * M_PI);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 6, 0, 2 * M_PI);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 2.4, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Cube:
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 12, 3);
      cairo_line_to(cr, 20, 7.5);
      cairo_line_to(cr, 20, 16.5);
      cairo_line_to(cr, 12, 21);
      cairo_line_to(cr, 4, 16.5);
      cairo_line_to(cr, 4, 7.5);
      cairo_close_path(cr);
      cairo_move_to(cr, 4, 7.5);
      cairo_line_to(cr, 12, 12);
      cairo_line_to(cr, 20, 7.5);
      cairo_move_to(cr, 12, 12);
      cairo_line_to(cr, 12, 21);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::StereoMono:
      beginIcon24(cr, x, y, s);
      cairo_rectangle(cr, 6, 6, 12, 12);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::StereoSbs:
      beginIcon24(cr, x, y, s);
      cairo_rectangle(cr, 2.5, 7, 9, 10);
      cairo_rectangle(cr, 12.5, 7, 9, 10);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::StereoTb:
      beginIcon24(cr, x, y, s);
      cairo_rectangle(cr, 6, 2.5, 12, 9);
      cairo_rectangle(cr, 6, 12.5, 12, 9);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::JumpTop:
    case Icon::JumpBottom: {
      // An arrow to a bar: to the top (or bottom) of the list.
      const bool top = icon == Icon::JumpTop;
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 5, top ? 4.5 : 19.5);
      cairo_line_to(cr, 19, top ? 4.5 : 19.5);
      cairo_move_to(cr, 12, top ? 20 : 4);
      cairo_line_to(cr, 12, top ? 9 : 15);
      cairo_move_to(cr, 7, top ? 13.5 : 10.5);
      cairo_line_to(cr, 12, top ? 8.5 : 15.5);
      cairo_line_to(cr, 17, top ? 13.5 : 10.5);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    }
    case Icon::Collapse:
    case Icon::Expand: {
      // A chevron: down folds the menu, up unfolds it.
      const bool down = icon == Icon::Collapse;
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 5, down ? 9 : 15);
      cairo_line_to(cr, 12, down ? 16 : 8);
      cairo_line_to(cr, 19, down ? 9 : 15);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    }
    case Icon::Star:
    case Icon::StarFilled: {
      // A five-pointed star, outlined or filled (a favourite).
      beginIcon24(cr, x, y, s);
      for (int i = 0; i < 10; ++i) {
        const double a = -M_PI / 2 + i * M_PI / 5, r = i % 2 ? 4.2 : 9.5;
        const double px = 12 + r * std::cos(a), py = 12.8 + r * std::sin(a);
        if (i == 0)
          cairo_move_to(cr, px, py);
        else
          cairo_line_to(cr, px, py);
      }
      cairo_close_path(cr);
      if (icon == Icon::StarFilled)
        cairo_fill(cr);
      else
        cairo_stroke(cr);
      cairo_restore(cr);
      break;
    }
    case Icon::Sort:
      // Lines getting shorter: an ordering.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 4, 6.5);
      cairo_line_to(cr, 20, 6.5);
      cairo_move_to(cr, 4, 12);
      cairo_line_to(cr, 15, 12);
      cairo_move_to(cr, 4, 17.5);
      cairo_line_to(cr, 10, 17.5);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Grid:
      beginIcon24(cr, x, y, s);
      cairo_rectangle(cr, 4, 4, 7, 7);
      cairo_rectangle(cr, 13, 4, 7, 7);
      cairo_rectangle(cr, 4, 13, 7, 7);
      cairo_rectangle(cr, 13, 13, 7, 7);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::List:
      beginIcon24(cr, x, y, s);
      for (double row : {6.5, 12.0, 17.5}) {
        cairo_move_to(cr, 9, row);
        cairo_line_to(cr, 20, row);
      }
      cairo_stroke(cr);
      for (double row : {6.5, 12.0, 17.5}) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, 5, row, 1.3, 0, 2 * M_PI);
      }
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::Slideshow:
      // A picture frame with a play triangle.
      beginIcon24(cr, x, y, s);
      cairo_rectangle(cr, 3, 5, 18, 14);
      cairo_stroke(cr);
      cairo_move_to(cr, 10, 9);
      cairo_line_to(cr, 15.5, 12);
      cairo_line_to(cr, 10, 15);
      cairo_close_path(cr);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::Curved:
      // A screen bent around the viewer.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 3, 7.5);
      cairo_curve_to(cr, 9, 5.23, 15, 5.23, 21, 7.5);
      cairo_line_to(cr, 21, 16.5);
      cairo_curve_to(cr, 15, 14.23, 9, 14.23, 3, 16.5);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Sleep:
      // Crescent moon: a disc with a smaller one cut out.
      cairo_push_group(cr);
      cairo_arc(cr, cx, cy, s * 0.4, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
      cairo_arc(cr, cx + s * 0.2, cy - s * 0.15, s * 0.33, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
      cairo_pop_group_to_source(cr);
      cairo_paint(cr);
      break;
    case Icon::Power:
      // Ring open at the top with a bar through the gap.
      cairo_set_line_width(cr, s * 0.1);
      cairo_arc(cr, cx, cy + s * 0.05, s * 0.38, -M_PI / 2 + 0.6, 3 * M_PI / 2 - 0.6);
      cairo_stroke(cr);
      cairo_move_to(cr, cx, y + s * 0.05);
      cairo_line_to(cr, cx, cy + s * 0.05);
      cairo_stroke(cr);
      break;
    case Icon::None:
      break;
  }
  cairo_restore(cr);
}

Menu::~Menu() {
  for (cairo_surface_t* s : thumbnails_)
    if (s) cairo_surface_destroy(s);
}

bool Menu::init() {
  return canvases_[kMain].init(kMainWidth, kMainHeight) &&
         canvases_[kFormat].init(kFormatWidth, kFormatHeight);
}

bool Menu::hoverRect(PanelId panel, float rect[4], float& radius, bool& danger) const {
  if (hovered_ < 0 || hovered_ >= static_cast<int>(buttons_.size())) return false;
  const Button& b = buttons_[hovered_];
  if (b.panel != panel || b.volumeSlider) return false;  // the slider shows its own state
  rect[0] = static_cast<float>(b.x);
  rect[1] = static_cast<float>(b.y);
  rect[2] = static_cast<float>(b.x + b.w);
  rect[3] = static_cast<float>(b.y + b.h);
  radius = static_cast<float>(b.radius >= 0 ? b.radius : kButtonRadius);
  danger = b.dangerHover;
  return true;
}

void Menu::update(const MenuView& view) {
  // Redraw for visible changes only; the time display has one-second steps.
  const PlaybackState& a = view.playback;
  const PlaybackState& b = view_.playback;
  const bool changed =
      static_cast<int>(a.position) != static_cast<int>(b.position) ||
      static_cast<int>(a.bufferedUntil) != static_cast<int>(b.bufferedUntil) ||
      std::abs(a.duration - b.duration) > 0.5 || a.paused != b.paused ||
      std::abs(a.volume - b.volume) > 0.5 || a.muted != b.muted || a.title != b.title ||
      trackSignature(a.tracks) != trackSignature(b.tracks) ||
      view.format.projection != view_.format.projection ||
      view.format.stereo != view_.format.stereo || view.curvedScreen != view_.curvedScreen ||
      view.currentFile != view_.currentFile || view.language != view_.language ||
      view.theme != view_.theme || view.streamSizes != view_.streamSizes ||
      a.videoCodec != b.videoCodec ||
      a.pixelFormat != b.pixelFormat || a.fps != b.fps || a.audioCodec != b.audioCodec ||
      a.audioChannels != b.audioChannels ||
      view.floatingSubtitles != view_.floatingSubtitles || view.maxVideoWidth != view_.maxVideoWidth ||
      view.streaming != view_.streaming ||
      view.streamWidth != view_.streamWidth || view.videoWidth != view_.videoWidth ||
      view.videoHeight != view_.videoHeight || view.lyingMode != view_.lyingMode ||
      a.speed != b.speed || a.abLoopA != b.abLoopA || a.abLoopB != b.abLoopB ||
      view.loop != view_.loop || view.sleepMinutesLeft != view_.sleepMinutesLeft ||
      view.brightness != view_.brightness || view.contrast != view_.contrast ||
      view.saturation != view_.saturation || view.zoom != view_.zoom ||
      view.photo != view_.photo || view.slideshowSeconds != view_.slideshowSeconds ||
      view.background != view_.background || view.menuFollows != view_.menuFollows ||
      view.subtitleSize != view_.subtitleSize || view.subtitleRaise != view_.subtitleRaise ||
      view.subtitleDistance != view_.subtitleDistance ||
      a.chapters.size() != b.chapters.size() || a.chapter != b.chapter;
  view_ = view;
  if (changed) dirty_ = true;
  // New thumbnails only matter while the file browser shows them. They are
  // drawn in batches: each redraw of the whole panel costs a little.
  if (thumbnailer_ && thumbnailer_->update() && page_ == Page::Files) thumbnailsPending_ = true;
  if (thumbnailsPending_) {
    const double now = steadySeconds();
    if (now - lastThumbnailRedraw_ > 0.7) {
      thumbnailsPending_ = false;
      lastThumbnailRedraw_ = now;
      dirty_ = true;
    }
  }

  if (youtube_.poll(videos_, searchError_)) {
    rebuildThumbnails();
    dirty_ = true;
  }
  // A playlist playing towards the end of what is listed: fetch the next page,
  // so "next" and repeating go on through the whole playlist.
  if (youtube_.listingLink() && youtube_.canLoadMore() && videos_.size() >= 3) {
    for (size_t i = videos_.size() - 3; i < videos_.size(); ++i)
      if (videos_[i].url() == view_.currentFile) {
        youtube_.loadMore();
        break;
      }
  }
  if (youtube_.busy() != searchBusy_) {
    searchBusy_ = youtube_.busy();
    dirty_ = true;
  }
}

bool Menu::inPlaylist(const std::string& url) const {
  return youtube_.listingLink() &&
         std::any_of(videos_.begin(), videos_.end(),
                     [&](const YouTubeVideo& v) { return v.url() == url; });
}

MenuCommand Menu::playlistNeighbor(const std::string& url, int step, bool wrap) const {
  if (!youtube_.listingLink()) return {};
  const int count = static_cast<int>(videos_.size());
  for (int i = 0; i < count; ++i) {
    if (videos_[i].url() != url) continue;
    int next = i + step;
    if (wrap) next = (next % count + count) % count;
    if (next < 0 || next >= count) return {};
    MenuCommand c;
    c.type = MenuCommand::PlayYouTube;
    c.path = videos_[next].url();
    c.value = static_cast<double>(videos_[next].kind);
    return c;
  }
  return {};
}

void Menu::rebuildThumbnails() {
  for (cairo_surface_t* s : thumbnails_)
    if (s) cairo_surface_destroy(s);
  thumbnails_.assign(videos_.size(), nullptr);
  for (size_t i = 0; i < videos_.size(); ++i) {
    const YouTubeVideo& v = videos_[i];
    if (!v.thumbnail) continue;
    // The surface points into the pixels, which videos_ keeps alive. cairo
    // only reads from it here, as a source.
    thumbnails_[i] = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(const_cast<uint32_t*>(v.thumbnail->data())),
        CAIRO_FORMAT_ARGB32, v.thumbnailWidth, v.thumbnailHeight, v.thumbnailWidth * 4);
  }
}

void Menu::typeText(const std::string& utf8) {
  if (!textInputActive() || utf8.empty()) return;
  query_ += utf8;
  dirty_ = true;
}

void Menu::backspace() {
  if (!textInputActive() || query_.empty()) return;
  // Remove one whole UTF-8 character.
  size_t end = query_.size() - 1;
  while (end > 0 && (static_cast<unsigned char>(query_[end]) & 0xC0) == 0x80) --end;
  query_.erase(end);
  dirty_ = true;
}

MenuCommand Menu::submitText() {
  return textInputActive() ? startSearch() : MenuCommand{};
}

MenuCommand Menu::startSearch() {
  const size_t first = query_.find_first_not_of(' ');
  if (first == std::string::npos) return {};
  const std::string text = query_.substr(first, query_.find_last_not_of(' ') - first + 1);
  if (isYouTubeVideoLink(text)) {
    // A link to one video plays right away.
    showPage(Page::Main);
    MenuCommand c;
    c.type = MenuCommand::PlayYouTube;
    c.path = youTubeVideoUrl(text);
    c.value = static_cast<double>(YouTubeVideo::Kind::Other);
    return c;
  }
  if (isYouTubeLink(text))
    youtube_.openLink(text);  // playlist or channel
  else
    youtube_.start(query_, vrOnly_);
  showPage(Page::YouTubeResults);
  return {};
}

int Menu::mainVisibleHeight() const {
  if (page_ != Page::Main) return kMainHeight;
  return collapsed_ ? kCollapsedHeight : kMainPageHeight;
}

double Menu::seekLineY() const {
  if (page_ != Page::Main) return kSubSeekY;
  return collapsed_ ? kMiniSeekY : kMainSeekY;
}

void Menu::setPointer(int panel, float u, float v) {
  pointerPanel_ = panel;
  if (panel == kMain) {
    pointerX_ = u * kMainWidth;
    pointerY_ = v * mainVisibleHeight();
  } else if (panel == kFormat) {
    pointerX_ = u * kFormatWidth;
    pointerY_ = v * kFormatHeight;
  }
  // The highlight is drawn by the panel shader, so hovering needs no redraw;
  // only a tooltip appearing or going away does.
  const int hovered = panel >= 0 ? buttonAt(panel, pointerX_, pointerY_) : -1;
  if (hovered != hovered_) {
    hovered_ = hovered;
    hoverStart_ = steadySeconds();
    if (tooltip_ >= 0) {
      tooltip_ = -1;
      dirty_ = true;
    }
  } else if (tooltip_ < 0 && hovered_ >= 0 && !buttons_[hovered_].tooltip.empty() &&
             steadySeconds() - hoverStart_ > 0.6) {
    tooltip_ = hovered_;
    dirty_ = true;
  }
}

MenuCommand Menu::click() {
  if (pointerPanel_ < 0) return {};
  if (dialog_ != Dialog::None) {
    // Modal: only the dialog's own buttons react, a click next to it does nothing.
    const int index = buttonAt(pointerPanel_, pointerX_, pointerY_);
    if (index < 0 || !buttons_[index].popup) return {};
  }
  if (dropdown_ != Dropdown::None) {
    // An open dropdown takes the click: an entry is chosen, anything else closes it.
    const int index = buttonAt(pointerPanel_, pointerX_, pointerY_);
    if (index < 0 || !buttons_[index].popup) {
      dropdown_ = Dropdown::None;
      dirty_ = true;
      return {};
    }
  }
  if (pointerPanel_ == kMain && std::abs(pointerY_ - seekLineY()) <= kSeekBand &&
      view_.playback.duration > 0 && !view_.photo) {
    // The seek bar: the knob follows the pointer while the trigger is held,
    // the video jumps there on letting go (drag()).
    draggingSeek_ = true;
    lastScrub_ = seekAtPointer();
    dirty_ = true;
    return {};
  }
  const int index = buttonAt(pointerPanel_, pointerX_, pointerY_);
  if (index < 0) return {};
  if (buttons_[index].volumeSlider) {
    draggingVolume_ = true;
    return volumeAtPointer(buttons_[index]);
  }
  dirty_ = true;
  // Copy first: the action may rebuild buttons_.
  const std::function<MenuCommand()> action = buttons_[index].action;
  return action();
}

MenuCommand Menu::drag(bool held) {
  if (!held) {
    draggingVolume_ = false;
    if (draggingSeek_ && lastScrub_ >= 0) {
      // Let go: the video jumps to where the knob was dropped.
      draggingSeek_ = false;
      MenuCommand c;
      c.type = MenuCommand::SeekTo;
      c.value = lastScrub_;
      return c;
    }
    draggingSeek_ = false;
  }
  if (pointerPanel_ != kMain) return {};
  if (draggingSeek_) {
    // Only the knob and the preview frame follow; the video itself jumps
    // once, on letting go (seeking big videos continuously stutters).
    const double seconds = seekAtPointer();
    if (std::abs(seconds - lastScrub_) >= view_.playback.duration / 1500) {
      lastScrub_ = seconds;
      dirty_ = true;
    }
    return {};
  }
  if (!draggingVolume_) return {};
  for (const Button& b : buttons_)
    if (b.volumeSlider) return volumeAtPointer(b);
  return {};
}

double Menu::seekAtPointer() const {
  const double fraction = std::clamp((pointerX_ - kMargin) / (kMainWidth - 2 * kMargin), 0.0, 1.0);
  return fraction * view_.playback.duration;
}

// The slider's track; the space to its right shows the value.
namespace {
constexpr double kSliderInset = 28;
constexpr double kSliderValueW = 110;  // the number, with room for the knob at 100
}  // namespace

MenuCommand Menu::volumeAtPointer(const Button& slider) const {
  const double x0 = slider.x + kSliderInset, x1 = slider.x + slider.w - kSliderValueW;
  MenuCommand c;
  c.type = MenuCommand::SetVolume;
  c.value = std::round(std::clamp((pointerX_ - x0) / (x1 - x0), 0.0, 1.0) * 100.0);
  return c;
}

void Menu::drawVolumeSlider(cairo_t* cr, const Button& b) const {
  const Palette& pal = paletteFor(view_.theme);
  const double x0 = b.x + kSliderInset, x1 = b.x + b.w - kSliderValueW;
  const double y = b.y + b.h / 2;
  const double fraction = std::clamp(view_.playback.volume / 100.0, 0.0, 1.0);
  drawTrack(cr, x0, x1, y, fraction, -1, view_.playback.muted ? 0.4 : 1.0);
  drawText(cr, std::to_string(static_cast<int>(std::round(view_.playback.volume))), x1 + 28, b.y,
           kSliderValueW - 36, b.h, 26, true, true, pal.text.r, pal.text.g, pal.text.b);
}

// A slider track from x0 to x1 at height y, filled to `fraction`, with an
// optional buffered part (< 0: none). Glass: thin with a round knob;
// Material: thick, with a gap around a bar-shaped handle.
void Menu::drawTrack(cairo_t* cr, double x0, double x1, double y, double fraction,
                     double buffered, double alpha) const {
  const Palette& pal = paletteFor(view_.theme);
  const double knob = x0 + (x1 - x0) * fraction;
  auto bar = [&](double from, double to, double h, const Rgba& color, double a) {
    if (to - from < 1) return;
    roundedRect(cr, from, y - h / 2, to - from, h, std::min(h / 2, (to - from) / 2));
    setColor(cr, color, a);
    cairo_fill(cr);
  };
  if (pal.thickTracks) {
    bar(knob + 9, x1, 16, pal.track, 1);
    if (buffered > fraction) bar(knob + 9, x0 + (x1 - x0) * buffered, 16, pal.buffered, 1);
    bar(x0, knob - 9, 16, pal.accent, alpha);
    bar(knob - 3, knob + 3, 40, pal.accent, alpha);
  } else {
    bar(x0, x1, 8, pal.track, 1);
    if (buffered > fraction) bar(x0, x0 + (x1 - x0) * buffered, 8, pal.buffered, 1);
    bar(x0, knob, 8, pal.accent, alpha);
    cairo_arc(cr, knob, y, 16, 0, 2 * M_PI);
    setColor(cr, pal.accent, alpha);
    cairo_fill(cr);
  }
}

void Menu::scroll(double rows) {
  if (!listOpen() || rows == 0) return;
  const int maxOffset = std::max(0, listCount_ - visibleRows_);
  const double position = std::clamp(scrollOffset_ + scrollFraction_ + rows, 0.0,
                                     static_cast<double>(maxOffset));
  const int offset = static_cast<int>(std::floor(position));
  const double fraction = position - offset;
  if (offset != scrollOffset_ || std::abs(fraction - scrollFraction_) > 1e-4) {
    scrollOffset_ = offset;
    scrollFraction_ = fraction;
    dirty_ = true;
  }
  // At the end of the results, fetch the next page.
  if (page_ == Page::YouTubeResults && rows > 0 && position >= maxOffset &&
      youtube_.canLoadMore()) {
    youtube_.loadMore();
    dirty_ = true;
  }
}

bool Menu::seekHover(double& fraction) const {
  if (pointerPanel_ != kMain || page_ != Page::Main || dialog_ != Dialog::None ||
      dropdown_ != Dropdown::None || view_.playback.duration <= 0 || view_.photo)
    return false;
  // While dragging the knob, the preview stays even if the pointer drifts off the bar.
  if (!draggingSeek_ && std::abs(pointerY_ - seekLineY()) > kSeekBand) return false;
  fraction = std::clamp((pointerX_ - kMargin) / (kMainWidth - 2 * kMargin), 0.0, 1.0);
  return true;
}

void Menu::showPage(Page page) {
  page_ = page;
  scrollOffset_ = 0;
  dropdown_ = Dropdown::None;
  dialog_ = Dialog::None;  // e.g. reopening the menu never resumes a pending deletion
  dirty_ = true;
}

void Menu::openDialog(Dialog dialog, const std::string& path, const std::string& error) {
  dialog_ = dialog;
  dialogPath_ = path;
  dialogError_ = error;
  dropdown_ = Dropdown::None;
  dirty_ = true;
}

void Menu::askDelete(const std::string& path, bool trashAvailable) {
  openDialog(trashAvailable ? Dialog::ConfirmTrash : Dialog::ConfirmDelete, path);
}

void Menu::askDeleteAfterTrashFailed(const std::string& path) {
  openDialog(Dialog::TrashFailed, path);
}

void Menu::showDeleteError(const std::string& error) { openDialog(Dialog::Error, "", error); }

void Menu::refreshDirectory() {
  if (directory_.empty()) return;
  const int offset = scrollOffset_;
  openDirectory(directory_);
  scrollOffset_ = offset;  // clamped to the shorter list on the next layout
}

void Menu::showFileBrowser() { openFiles(FileKind::Video); }

void Menu::openFiles(FileKind kind) {
  fileKind_ = kind;
  // Start next to the current video; later reopen where we left off.
  std::string dir = directory_;
  if (dir.empty() && !view_.currentFile.empty() && view_.currentFile.find("://") == std::string::npos)
    dir = fs::path(view_.currentFile).parent_path().string();
  if (dir.empty()) dir = std::getenv("HOME") ? std::getenv("HOME") : "/";
  openDirectory(dir);
  showPage(Page::Files);
}

void Menu::rememberFolderView() {
  if (folders_ && !directory_.empty())
    folders_->set(directory_, {static_cast<int>(sortMode_), gallery_});
}

void Menu::openDirectory(const std::string& dir) {
  directory_ = dir;
  entries_.clear();
  scrollOffset_ = 0;
  // The folder's own order and view, if remembered (else as before).
  FolderView view;
  if (folders_ && folders_->get(dir, view) && view.sort >= 0 &&
      view.sort < static_cast<int>(SortMode::kCount)) {
    sortMode_ = static_cast<SortMode>(view.sort);
    gallery_ = view.gallery;
  }
  std::error_code ec;
  for (const auto& item :
       fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
    const std::string name = item.path().filename().string();
    if (name.empty() || name[0] == '.') continue;
    std::error_code typeError;
    const bool wanted =
        fileKind_ == FileKind::Video ? isVideoFile(item.path()) || isPhotoFile(item.path())
                                     : isSubtitleFile(item.path());
    if (item.is_directory(typeError)) {
      entries_.push_back({name, item.path().string(), true});
    } else if (wanted) {
      FileEntry entry{name, item.path().string(), false};
      // Dates and sizes cost a file system query each (slow on network
      // shares), so they are only read when the order needs them.
      if (sortMode_ == SortMode::Newest || sortMode_ == SortMode::Oldest) {
        std::error_code timeError;
        const auto time = item.last_write_time(timeError);
        if (!timeError) entry.modified = time.time_since_epoch().count();
      } else if (sortMode_ == SortMode::Largest) {
        std::error_code sizeError;
        const auto size = item.file_size(sizeError);
        if (!sizeError) entry.size = size;
      }
      entries_.push_back(entry);
    }
  }
  sortEntries();
  dirty_ = true;
}

// Folders first (always by name), then the files in the chosen order.
void Menu::sortEntries() {
  const SortMode mode = sortMode_;
  std::sort(entries_.begin(), entries_.end(), [mode](const FileEntry& a, const FileEntry& b) {
    if (a.directory != b.directory) return a.directory;
    if (!a.directory) {
      switch (mode) {
        case SortMode::NameDown: return fileNameLess(b.name, a.name);
        case SortMode::Newest:
          if (a.modified != b.modified) return a.modified > b.modified;
          break;
        case SortMode::Oldest:
          if (a.modified != b.modified) return a.modified < b.modified;
          break;
        case SortMode::Largest:
          if (a.size != b.size) return a.size > b.size;
          break;
        default: break;
      }
    }
    return fileNameLess(a.name, b.name);
  });
}

// Searches from the end: later buttons (an open dropdown) are drawn on top.
int Menu::buttonAt(int panel, double px, double py) const {
  for (size_t i = buttons_.size(); i-- > 0;) {
    const Button& b = buttons_[i];
    // While a dialog is open, only its buttons can be hovered or clicked.
    if (dialog_ != Dialog::None && !b.popup) continue;
    if (b.scrolls && (py < listTop_ || py > listTop_ + visibleRows_ * rowHeight_)) continue;
    if (b.panel == panel && b.action && !b.disabled && px >= b.x && px <= b.x + b.w &&
        py >= b.y && py <= b.y + b.h)
      return static_cast<int>(i);
  }
  return -1;
}

double Menu::textWidth(const std::string& text, int size, bool bold) const {
  PangoLayout* layout = pango_cairo_create_layout(canvases_[kMain].cr());
  PangoFontDescription* font = pango_font_description_from_string(bold ? "Sans Bold" : "Sans");
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, text.c_str(), -1);
  int w = 0, h = 0;
  pango_layout_get_pixel_size(layout, &w, &h);
  pango_font_description_free(font);
  g_object_unref(layout);
  return w;
}

Menu::Button Menu::iconButton(Icon icon, std::function<MenuCommand()> action, bool selected) const {
  Button b;
  b.icon = icon;
  b.w = kIconButtonW;
  b.selected = selected;
  b.action = std::move(action);
  return b;
}

Menu::Button Menu::textButton(const std::string& label, Icon icon,
                              std::function<MenuCommand()> action, bool selected) const {
  Button b;
  b.label = label;
  b.icon = icon;
  b.w = textWidth(label, 32, false) + 70 + (icon != Icon::None ? kIconSize * 0.75 + 20 : 0);
  b.selected = selected;
  b.action = std::move(action);
  return b;
}

// A folder row: opens the folder; its star adds it to or removes it from the favourites.
Menu::ListItem Menu::folderItem(const std::string& label, const std::string& path) {
  ListItem item{label, false, nullptr};
  item.folder = true;
  item.action = [this, path] {
    showFavorites_ = false;
    openDirectory(path);
    return MenuCommand{};
  };
  item.starred = favorites_.contains(path);
  item.star = [this, path] {
    favorites_.toggle(path);
    dirty_ = true;
    return MenuCommand{};
  };
  return item;
}

// A file row: opens the file (a video or photo, or a subtitle file to load).
Menu::ListItem Menu::fileItem(const std::string& label, const std::string& path) {
  ListItem item{label, path == view_.currentFile, nullptr};
  item.action = [this, path] {
    MenuCommand c;
    c.path = path;
    c.type = fileKind_ == FileKind::Video ? MenuCommand::OpenFile : MenuCommand::LoadSubtitle;
    showFavorites_ = false;
    showPage(Page::Main);
    return c;
  };
  if (fileKind_ == FileKind::Video) {
    item.thumbPath = path;
    item.starred = favorites_.contains(path);
    item.star = [this, path] {
      favorites_.toggle(path);
      dirty_ = true;
      return MenuCommand{};
    };
  }
  return item;
}

// The favourite folders and files, by name with the folder they are in.
std::vector<Menu::ListItem> Menu::favoriteItems() {
  std::vector<ListItem> items;
  for (const std::string& path : favorites_.paths()) {
    const fs::path p(path);
    const std::string name = p.filename().empty() ? path : p.filename().string();
    const std::string where = "   ·   " + p.parent_path().string();
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      items.push_back(folderItem(name + "/" + where, path));
    } else {
      ListItem item = fileItem(name + where, path);
      item.selected = false;
      items.push_back(item);
    }
  }
  return items;
}

void Menu::showFavorites() {
  openFiles(FileKind::Video);
  showFavorites_ = true;
}

std::vector<Menu::ListItem> Menu::fileItems() {
  std::vector<ListItem> items;
  for (const FileEntry& entry : entries_) {
    if (entry.directory) {
      items.push_back(folderItem(entry.name + "/", entry.path));
      continue;
    }
    ListItem item = fileItem(entry.name, entry.path);
    if (fileKind_ == FileKind::Video) {
      item.trash = [path = entry.path] {
        MenuCommand c;
        c.type = MenuCommand::RequestDelete;
        c.path = path;
        return c;
      };
    }
    items.push_back(item);
  }
  return items;
}

// "English (eng)", "eng" or "Track 2".
std::string Menu::trackName(const Track& track) const {
  std::string s = !track.title.empty() ? track.title
                  : !track.lang.empty() ? track.lang
                                        : std::string(text().track) + " " + std::to_string(track.id);
  if (!track.title.empty() && !track.lang.empty()) s += " (" + track.lang + ")";
  return s;
}

// Lays out buttons with their own widths, centered on the main panel.
void Menu::addCenteredRow(double y, double h, std::vector<Button> items) {
  double total = 0;
  for (size_t i = 0; i < items.size(); ++i) total += items[i].w + (i ? kGap : 0);
  double x = (kMainWidth - total) / 2;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) x += kGap;
    items[i].x = x;
    items[i].y = y;
    items[i].h = h;
    buttons_.push_back(items[i]);
    x += items[i].w;
  }
}

// Lays out equally wide buttons from x0 to the right margin of the main panel.
void Menu::addRow(double y, double h, std::vector<Button> items, double x0) {
  const double w = (kMainWidth - kMargin - x0 - kGap * (items.size() - 1)) / items.size();
  for (size_t i = 0; i < items.size(); ++i) {
    items[i].x = x0 + i * (w + kGap);
    items[i].y = y;
    items[i].w = w;
    items[i].h = h;
    buttons_.push_back(items[i]);
  }
}

void Menu::clampScroll(int count, int rows, int& first, int& end) {
  const int maxOffset = std::max(0, count - rows);
  scrollOffset_ = std::clamp(scrollOffset_, 0, maxOffset);
  if (scrollOffset_ >= maxOffset) scrollFraction_ = 0;
  first = scrollOffset_;
  // Part-way scrolled: one more row peeks in at the bottom.
  end = std::min(count, scrollOffset_ + rows + (scrollFraction_ > 0 ? 1 : 0));
}

void Menu::addList(const std::vector<ListItem>& items, double x0, double rowH, int rows) {
  visibleRows_ = rows;
  rowHeight_ = rowH;
  listTop_ = kListY;
  listCount_ = static_cast<int>(items.size());
  int first, end;
  clampScroll(listCount_, rows, first, end);
  constexpr double trashW = 90;
  for (int i = first; i < end; ++i) {
    Button b;
    b.x = x0;
    b.y = kListY + (i - scrollOffset_ - scrollFraction_) * rowH;
    b.w = listRight_ - x0 - 30;
    b.h = rowH - 6;
    b.label = items[i].label;
    b.selected = items[i].selected;
    b.action = items[i].action;
    b.listRow = true;
    b.scrolls = true;
    b.thumbPath = items[i].thumbPath;
    double right = b.x + b.w;
    if (items[i].trash) {
      Button trash;
      trash.x = right - trashW;
      trash.y = b.y;
      trash.w = trashW;
      trash.h = b.h;
      trash.icon = Icon::Trash;
      trash.action = items[i].trash;
      trash.scrolls = true;
      right -= trashW + 10;
      b.w = right - b.x;
      buttons_.push_back(b);
      buttons_.push_back(trash);
    } else {
      buttons_.push_back(b);
    }
    if (items[i].star) {  // favourite star, at the row's right end
      Button& row = buttons_[buttons_.size() - (items[i].trash ? 2 : 1)];
      row.w -= kStarW + 10;
      buttons_.push_back(starButton(items[i], row.x + row.w + 10, b.y, kStarW, b.h));
    }
  }
  wantThumbnails(items, first, end, rows);
}

Menu::Button Menu::starButton(const ListItem& item, double x, double y, double w, double h) {
  Button star;
  star.x = x;
  star.y = y;
  star.w = w;
  star.h = h;
  star.icon = item.starred ? Icon::StarFilled : Icon::Star;
  star.tooltip = item.starred ? text().tipUnfavorite : text().tipFavorite;
  star.action = item.star;
  star.bare = true;  // a background only when it is a favourite
  star.selected = item.starred;
  star.scrolls = true;
  return star;
}

void Menu::addGallery(const std::vector<ListItem>& items, double x0) {
  // 4 cards per row, 2 rows on screen; scrolling counts grid rows.
  constexpr int columns = 4;
  constexpr double gap = 16, rowH = 270;
  const double cardW = (listRight_ - 30 - x0 - (columns - 1) * gap) / columns;
  const int count = static_cast<int>(items.size());
  visibleRows_ = 2;
  rowHeight_ = rowH;
  listTop_ = kListY;
  listCount_ = (count + columns - 1) / columns;
  int firstRow, endRow;
  clampScroll(listCount_, visibleRows_, firstRow, endRow);
  const int first = firstRow * columns;
  const int end = std::min(count, endRow * columns);
  for (int i = first; i < end; ++i) {
    const int cell = i - first;
    Button b;
    b.x = x0 + (cell % columns) * (cardW + gap);
    b.y = kListY + (cell / columns - scrollFraction_) * rowH;
    b.w = cardW;
    b.h = rowH - gap;
    b.label = items[i].label;
    b.selected = items[i].selected;
    b.action = items[i].action;
    b.card = true;
    b.scrolls = true;
    b.thumbPath = items[i].thumbPath;
    if (items[i].folder) b.icon = Icon::Folder;
    buttons_.push_back(b);
    if (items[i].trash) {  // small, in the card's upper right corner
      Button trash;
      trash.x = b.x + b.w - 66;
      trash.y = b.y + 12;
      trash.w = trash.h = 54;
      trash.radius = 27;
      trash.icon = Icon::Trash;
      trash.dangerHover = true;
      trash.action = items[i].trash;
      trash.scrolls = true;
      buttons_.push_back(trash);
    }
    if (items[i].star) {  // favourite star in the card's upper left corner
      Button star = starButton(items[i], b.x + 12, b.y + 12, 54, 54);
      star.radius = 27;
      buttons_.push_back(star);
    }
  }
  wantThumbnails(items, first, end, visibleRows_ * columns);
}

void Menu::wantThumbnails(const std::vector<ListItem>& items, int first, int end, int page) {
  if (!thumbnailer_ || page_ != Page::Files) return;
  // Most urgent first: on screen, the next pages, the end and the start of
  // the folder (for the jump buttons), a little above. Bounded, so a huge
  // folder doesn't keep the decoder busy for minutes or fill the memory.
  const int count = static_cast<int>(items.size());
  std::vector<std::string> wanted;
  std::set<int> taken;
  auto add = [&](int i) {
    if (i >= 0 && i < count && !items[i].thumbPath.empty() && taken.insert(i).second)
      wanted.push_back(items[i].thumbPath);
  };
  for (int i = first; i < end; ++i) add(i);
  for (int i = end; i < end + std::max(30, page * 3); ++i) add(i);
  for (int i = count - page * 2; i < count; ++i) add(i);
  for (int i = 0; i < page * 2; ++i) add(i);
  for (int i = first - 1; i >= first - page * 2; --i) add(i);
  thumbnailer_->want(wanted);
}

void Menu::layoutFileTools() {
  const Strings& t = text();
  const double x = kMainWidth - kMargin - kBigButton;
  addRound(Icon::JumpTop, t.tipTop,
           [this] {
             scrollOffset_ = 0;
             scrollFraction_ = 0;
             return MenuCommand{};
           },
           x, kListY);
  addRound(Icon::JumpBottom, t.tipBottom,
           [this] {
             scrollOffset_ = 1 << 30;  // clamped to the last page by the layout
             return MenuCommand{};
           },
           x, kListY + 100);
  const Button sort = addRound(Icon::Sort, t.sortModes[static_cast<int>(sortMode_)],
                               [this] {
                                 dropdown_ = dropdown_ == Dropdown::Sort ? Dropdown::None
                                                                          : Dropdown::Sort;
                                 return MenuCommand{};
                               },
                               x, kListY + 240, kBigButton, sortMode_ != SortMode::NameUp);
  if (fileKind_ == FileKind::Video) {
    addRound(gallery_ ? Icon::List : Icon::Grid, gallery_ ? t.tipList : t.tipGallery,
             [this] {
               // Keep roughly the same place: gallery rows hold 4 items.
               scrollOffset_ = gallery_ ? scrollOffset_ * 4 : scrollOffset_ / 4;
               gallery_ = !gallery_;
               rememberFolderView();
               return MenuCommand{};
             },
             x, kListY + 340);
  }
  if (dropdown_ != Dropdown::Sort) return;
  std::vector<Button> entries;
  for (int i = 0; i < static_cast<int>(SortMode::kCount); ++i) {
    Button e;
    e.label = t.sortModes[i];
    e.selected = static_cast<int>(sortMode_) == i;
    e.action = [this, i] {
      dropdown_ = Dropdown::None;
      sortMode_ = static_cast<SortMode>(i);
      rememberFolderView();
      const int offset = scrollOffset_;
      openDirectory(directory_);  // reads dates or sizes if needed
      scrollOffset_ = offset;
      return MenuCommand{};
    };
    entries.push_back(e);
  }
  addDropdown(sort, entries);
}

void Menu::layoutMain() {
  const Strings& t = text();
  const PlaybackState& p = view_.playback;

  const double right = kMainWidth - kMargin;
  const double cx = kMainWidth / 2.0;
  if (collapsed_) {
    layoutCollapsed();
    return;
  }

  // Fold the menu to a slim bar, left of settings.
  addRound(Icon::Collapse, t.tipCollapse,
           [this] {
             collapsed_ = true;
             formatOpen_ = false;
             dropdown_ = Dropdown::None;
             return MenuCommand{};
           },
           right - 60 - 2 * (kEdgeButton + kGap), kTopY, kEdgeButton);

  // Corners. Delete (local files) top left; quit, smaller, and settings top
  // right; files (favourites above) and YouTube bottom left; recenter and
  // format bottom right.
  // Quit and delete turn red under the pointer.
  if (!view_.currentFile.empty() && !view_.streaming) {
    Button& trash = addRound(Icon::Trash, view_.photo ? t.tipDeletePhoto : t.tipDelete,
                             [path = view_.currentFile] {
                               MenuCommand c;
                               c.type = MenuCommand::RequestDelete;
                               c.path = path;
                               return c;
                             },
                             kMargin, kTopY, kEdgeButton);
    trash.dangerHover = true;
    trash.plain = true;
    // Next to it: the current file as a favourite (or no longer).
    const bool starred = favorites_.contains(view_.currentFile);
    Button& star = addRound(starred ? Icon::StarFilled : Icon::Star,
                            starred ? t.tipUnfavorite : t.tipFavorite,
                            [this, path = view_.currentFile] {
                              favorites_.toggle(path);
                              return MenuCommand{};
                            },
                            kMargin + kEdgeButton + kGap, kTopY, kEdgeButton, starred);
    star.plain = true;
  }
  Button& quit = addRound(Icon::Power, t.quit,
                          [this] {
                            openDialog(Dialog::ConfirmQuit, "");
                            return MenuCommand{};
                          },
                          right - 60, kTopY + 10, 60);
  quit.dangerHover = true;
  quit.plain = true;
  addRound(Icon::Settings, t.settings,
           [this] {
             showPage(Page::Settings);
             return MenuCommand{};
           },
           right - 60 - kGap - kEdgeButton, kTopY, kEdgeButton);
  addRound(Icon::Globe, t.youtube,
           [this] {
             showPage(videos_.empty() && !searchBusy_ ? Page::YouTubeKeyboard
                                                      : Page::YouTubeResults);
             return MenuCommand{};
           },
           kMargin + kEdgeButton + kGap, kBottomY, kEdgeButton);
  addRound(Icon::Folder, t.files,
           [this] {
             showFileBrowser();
             return MenuCommand{};
           },
           kMargin, kBottomY, kEdgeButton);
  // Above the files button: straight to the favourites.
  addRound(Icon::StarFilled, t.favorites,
           [this] {
             showFavorites();
             return MenuCommand{};
           },
           kMargin, kBottomY - kEdgeButton - kGap, kEdgeButton);
  addRound(Icon::Recenter, t.recenter, command(MenuCommand::Recenter),
           right - 2 * kEdgeButton - kGap, kBottomY, kEdgeButton);
  addRound(Icon::FormatDome, t.format,
           [this] {
             formatOpen_ = !formatOpen_;
             return MenuCommand{};
           },
           right - kEdgeButton, kBottomY, kEdgeButton, formatOpen_);

  if (view_.photo) {
    // Photos: previous, a large slideshow button and next. Nothing to play,
    // seek or loop. The slideshow button opens the choice of seconds; while
    // the slideshow runs it is a pause button that stops it.
    const bool running = view_.slideshowSeconds > 0;
    std::string tip = t.tipSlideshow;
    if (running) tip = std::string(t.tipPause) + " (" + std::to_string(view_.slideshowSeconds) + " s)";
    addRound(Icon::Previous, t.tipPrevious, command(MenuCommand::PreviousVideo), cx - 190,
             kTransportY);
    std::function<MenuCommand()> action = [this] {
      dropdown_ = dropdown_ == Dropdown::Slideshow ? Dropdown::None : Dropdown::Slideshow;
      return MenuCommand{};
    };
    if (running) action = command(MenuCommand::SetSlideshow, 0);
    Button& slideshow = addRound(running ? Icon::Pause : Icon::Slideshow, tip, std::move(action),
                                 cx - kPlayButton / 2,
                                 kTransportY - (kPlayButton - kBigButton) / 2, kPlayButton);
    if (!glass()) slideshow.radius = 48;
    addRound(Icon::Next, t.tipNext, command(MenuCommand::NextVideo), cx + 102, kTransportY);
  } else {
    // Transport, centred, with a large play button.
    addRound(Icon::Previous, t.tipPrevious, command(MenuCommand::PreviousVideo), cx - 302,
             kTransportY);
    addRound(Icon::Back10, t.tipBack10, command(MenuCommand::Seek, -10), cx - 190, kTransportY);
    Button& play =
        addRound(p.paused ? Icon::Play : Icon::Pause, p.paused ? t.tipPlay : t.tipPause,
                 command(MenuCommand::TogglePause), cx - kPlayButton / 2,
                 kTransportY - (kPlayButton - kBigButton) / 2, kPlayButton);
    if (!glass()) play.radius = 48;  // Material: a rounded square
    addRound(Icon::Forward10, t.tipForward10, command(MenuCommand::Seek, 10), cx + 102,
             kTransportY);
    addRound(Icon::Next, t.tipNext, command(MenuCommand::NextVideo), cx + 214, kTransportY);
  }

  // Volume: a pill right of the transport with mute and the slider.
  addVolume(kVolumeY);

  layoutPlaybackRow();
  layoutInfoLine();  // last: its open list must lie on top of the option row
}

// The folded player page: the seek bar, and below it unfold, the transport
// (photos: previous, slideshow, next) and mute.
void Menu::layoutCollapsed() {
  const Strings& t = text();
  const PlaybackState& p = view_.playback;
  const double cx = kMainWidth / 2.0;
  addRound(Icon::Expand, t.tipExpand,
           [this] {
             collapsed_ = false;
             return MenuCommand{};
           },
           kMargin, kMiniRowY, kEdgeButton);
  if (view_.photo) {
    const bool running = view_.slideshowSeconds > 0;
    addRound(Icon::Previous, t.tipPrevious, command(MenuCommand::PreviousVideo), cx - 148,
             kMiniRowY);
    addRound(running ? Icon::Pause : Icon::Slideshow, running ? t.tipPause : t.tipSlideshow,
             command(MenuCommand::SetSlideshow, running ? 0 : 5), cx - 44, kMiniRowY);
    addRound(Icon::Next, t.tipNext, command(MenuCommand::NextVideo), cx + 60, kMiniRowY);
  } else {
    addRound(Icon::Previous, t.tipPrevious, command(MenuCommand::PreviousVideo), cx - 252,
             kMiniRowY);
    addRound(Icon::Back10, t.tipBack10, command(MenuCommand::Seek, -10), cx - 148, kMiniRowY);
    addRound(p.paused ? Icon::Play : Icon::Pause, p.paused ? t.tipPlay : t.tipPause,
             command(MenuCommand::TogglePause), cx - 44, kMiniRowY, kBigButton, true);
    addRound(Icon::Forward10, t.tipForward10, command(MenuCommand::Seek, 10), cx + 60, kMiniRowY);
    addRound(Icon::Next, t.tipNext, command(MenuCommand::NextVideo), cx + 164, kMiniRowY);
  }
  addVolume(kMiniRowY);
}

// The volume pill's buttons: mute on the left, the slider filling the rest.
void Menu::addVolume(double y) {
  const PlaybackState& p = view_.playback;
  Button& mute = addRound(p.muted ? Icon::Mute : Icon::VolumeUp, text().tipMute,
                          command(MenuCommand::ToggleMute), kVolumeX + 12, y + 12, 64, p.muted);
  mute.bare = true;  // sits on the volume pill
  Button volume;
  volume.x = kVolumeX + 84;
  volume.y = y;
  volume.w = kVolumeW - 92;
  volume.h = kBigButton;
  volume.radius = 40;
  volume.volumeSlider = true;
  volume.action = [] { return MenuCommand{}; };  // handled in click()/drag()
  buttons_.push_back(volume);
}

void Menu::drawVolumePill(cairo_t* cr, double y) const {
  const Palette& pal = paletteFor(view_.theme);
  if (glass()) {
    const double fill[4] = {pal.button.r, pal.button.g, pal.button.b, pal.button.a};
    glassButton(cr, kVolumeX, y, kVolumeW, kBigButton, kBigButton / 2, fill);
  } else {
    roundedRect(cr, kVolumeX, y, kVolumeW, kBigButton, 28);
    setColor(cr, pal.track);
    cairo_fill(cr);
  }
}

Menu::Button& Menu::addRound(Icon icon, const std::string& tooltip,
                             std::function<MenuCommand()> action, double x, double y,
                             double size, bool selected) {
  Button b = iconButton(icon, std::move(action), selected);
  b.x = x;
  b.y = y;
  b.w = b.h = size;
  b.radius = size / 2;
  b.tooltip = tooltip;
  buttons_.push_back(b);
  return buttons_.back();
}

// The playback options, staggered between the transport buttons, and the
// stream quality left of them. Speed, sleep timer and quality open their
// choices as a strip above themselves.
void Menu::layoutPlaybackRow() {
  const Strings& t = text();
  const PlaybackState& p = view_.playback;
  const double cx = kMainWidth / 2.0;
  auto toggle = [this](Dropdown d) {
    return [this, d] {
      dropdown_ = dropdown_ == d ? Dropdown::None : d;
      return MenuCommand{};
    };
  };
  auto entry = [this](const std::string& label, bool selected, MenuCommand::Type type,
                      double value) {
    Button b;
    b.label = label;
    b.selected = selected;
    b.action = [this, type, value] {
      dropdown_ = Dropdown::None;
      MenuCommand c;
      c.type = type;
      c.value = value;
      return c;
    };
    return b;
  };

  Button quality;
  if (view_.streaming) {
    // "2160p (max) ▾": what is playing now, and whether the quality is unlimited.
    std::string label = view_.videoHeight > 0 ? std::to_string(view_.videoHeight) + "p"
                                              : qualityLabel(view_.streamWidth);
    if (view_.videoHeight > 0 && view_.streamWidth == 0) label += " (max)";
    quality = textButton(label + "  ▾", Icon::None, toggle(Dropdown::Quality),
                         dropdown_ == Dropdown::Quality);
    quality.w = std::min(quality.w, 310.0);
    quality.x = kMargin + kBigButton + 40;
    quality.y = kTransportY + 8;
    quality.h = 72;
    quality.radius = 36;
    buttons_.push_back(quality);
  }

  // Repeat: each click goes on to the next mode: off, video, then the folder,
  // or for a stream its playlist if it has one.
  const int loopMode = static_cast<int>(view_.loop);
  const bool playlist = view_.streaming && inPlaylist(view_.currentFile);
  const int modes = view_.streaming && !playlist ? 2 : 3;
  const bool groupMode = view_.loop == LoopMode::Folder;
  // Small round buttons (Material: rounded squares) centred between the
  // transport buttons above them.
  const double smallRadius = glass() ? kSmallButton / 2 : 24;
  // Photos have none of these; their slideshow button is the big one in the
  // middle (it opens its choices like these do).
  Button speed, sleep, slideshow;
  slideshow.x = cx - kPlayButton / 2;
  slideshow.y = kTransportY - (kPlayButton - kBigButton) / 2;
  slideshow.w = slideshow.h = kPlayButton;
  if (!view_.photo) {
    Button& loop = addRound(Icon::Loop, groupMode && playlist ? t.loopPlaylist : t.loopModes[loopMode],
                            command(MenuCommand::SetLoop, (loopMode + 1) % modes), cx - 238,
                            kOptionsY, kSmallButton, view_.loop != LoopMode::Off);
    loop.radius = smallRadius;
    // On: a badge says what repeats, "1" = this video, "≡" = the whole folder/playlist.
    if (view_.loop == LoopMode::File) loop.badge = "1";
    if (groupMode) loop.badge = "≡";
    speed = addRound(Icon::Gauge,
                     std::string(t.tipSpeed) + ": " + speedLabel(p.speed, view_.language),
                     toggle(Dropdown::Speed), cx - 109, kOptionsY, kSmallButton,
                     std::abs(p.speed - 1.0) > 0.01);
    buttons_.back().radius = smallRadius;
    addRound(Icon::AbLoop, t.tipAbLoop, command(MenuCommand::ToggleAbLoop), cx + 37, kOptionsY,
             kSmallButton, p.abLoopA >= 0)
        .radius = smallRadius;
    std::string sleepTip = t.tipSleep;
    if (view_.sleepMinutesLeft > 0)
      sleepTip += ": " + std::to_string(view_.sleepMinutesLeft) + " min";
    sleep = addRound(Icon::Sleep, sleepTip, toggle(Dropdown::Sleep), cx + 166, kOptionsY,
                     kSmallButton, view_.sleepMinutesLeft > 0);
    buttons_.back().radius = smallRadius;
  }

  std::vector<Button> entries;
  switch (dropdown_) {
    case Dropdown::Quality:
      // The sizes this video really has, else the usual limits.
      entries.push_back(entry(qualityLabel(0), view_.streamWidth == 0,
                              MenuCommand::SetStreamQuality, 0));
      if (!view_.streamSizes.empty()) {
        for (const auto& [width, height] : view_.streamSizes)
          entries.push_back(entry(std::to_string(height) + "p", view_.streamWidth == width,
                                  MenuCommand::SetStreamQuality, width));
      } else {
        for (int width : kQualityWidths)
          if (width > 0)
            entries.push_back(entry(qualityLabel(width), view_.streamWidth == width,
                                    MenuCommand::SetStreamQuality, width));
      }
      addDropdown(quality, entries);
      break;
    case Dropdown::Speed:
      for (double s : kSpeeds)
        entries.push_back(entry(speedLabel(s, view_.language), std::abs(p.speed - s) < 0.01,
                                MenuCommand::SetSpeed, s));
      addDropdown(speed, entries);
      break;
    case Dropdown::Sleep:
      for (int minutes : kSleepMinutes)
        entries.push_back(entry(minutes == 0 ? std::string(t.sleepOff)
                                             : std::to_string(minutes) + " min",
                                minutes == 0 && view_.sleepMinutesLeft == 0,
                                MenuCommand::SetSleepTimer, minutes));
      addDropdown(sleep, entries);
      break;
    case Dropdown::Slideshow:
      for (int seconds : kSlideshowSeconds)
        entries.push_back(entry(seconds == 0 ? std::string(t.off) : std::to_string(seconds) + " s",
                                view_.slideshowSeconds == seconds, MenuCommand::SetSlideshow,
                                seconds));
      addDropdown(slideshow, entries);
      break;
    case Dropdown::Sort:       // the file browser's, laid out by layoutFileTools()
    case Dropdown::Audio:      // the info line's lists, laid out by layoutInfoLine()
    case Dropdown::Subtitles:
    case Dropdown::Chapters:
    case Dropdown::None:
      break;
  }
}

// The choices as a strip of buttons just above their opener, centred on it
// where there is room, drawn on top of everything else.
void Menu::addDropdown(const Button& opener, std::vector<Button> entries) {
  double total = 0;
  for (Button& e : entries) {
    e.w = std::max(120.0, textWidth(e.label, 32, false) + 60);
    total += e.w;
  }
  total += kGap * (entries.size() - 1);
  double x = std::max(kMargin, std::min(opener.x + opener.w / 2 - total / 2,
                                        kMainWidth - kMargin - total));
  for (Button& e : entries) {
    e.popup = true;
    e.radius = glass() ? 36 : 20;  // pills on glass
    e.x = x;
    e.h = 72;
    e.y = opener.y - 18 - e.h;
    buttons_.push_back(e);
    x += e.w + kGap;
  }
}

std::string Menu::qualityLabel(int width) const {
  switch (width) {
    case 0: return text().qualityMax;
    case 5760: return "5.7K";
    case 3840: return "4K";
    case 1920: return "1080p";
    default: return std::to_string(width) + " px";
  }
}

void Menu::layoutFormat() {
  // Tiles: an icon with a short label under it; the full name as tooltip.
  // Projections in two rows of three, then the 3D layouts, then "curved".
  const Strings& t = text();
  constexpr double pad = 30, tileW = 120, tileH = 118, gap = 15;
  auto add = [&](double x, double y, double w, Icon icon, const char* caption, const char* name,
                 bool selected, std::function<MenuCommand()> action, bool disabled = false) {
    Button b;
    b.panel = kFormat;
    b.x = x;
    b.y = y;
    b.w = w;
    b.h = w == tileW ? tileH : 92;
    b.radius = glass() ? 30 : 28;
    b.icon = icon;
    b.caption = caption;
    b.tooltip = name;
    b.selected = selected;
    b.disabled = disabled;
    b.action = std::move(action);
    buttons_.push_back(b);
  };
  const Icon projections[] = {Icon::ProjFlat,   Icon::Proj180,    Icon::Proj360,
                              Icon::Fisheye190, Icon::Fisheye200, Icon::Cube};
  for (int i = 0; i < 6; ++i)
    add(pad + (i % 3) * (tileW + gap), 34 + (i / 3) * (tileH + 14), tileW, projections[i],
        t.projectionShort[i], t.projections[i], static_cast<int>(view_.format.projection) == i,
        command(MenuCommand::SetProjection, i));
  const Icon layouts[] = {Icon::StereoMono, Icon::StereoSbs, Icon::StereoTb};
  for (int i = 0; i < 3; ++i)
    add(pad + i * (tileW + gap), 318, tileW, layouts[i], t.stereoShort[i], t.stereo[i],
        static_cast<int>(view_.format.stereo) == i, command(MenuCommand::SetStereo, i));
  // Only the flat-screen projection has a screen to curve.
  add(pad, 470, kFormatWidth - 2 * pad, Icon::Curved, t.curved, t.curved, view_.curvedScreen,
      command(MenuCommand::ToggleCurved), view_.format.projection != Projection::Flat);
}

void Menu::layoutSettings() {
  const Strings& t = text();
  // Left: the sub pages, one under the other.
  auto subPage = [this](const std::string& label, Icon icon, std::function<MenuCommand()> action,
                        double y) {
    Button b = textButton(label + "  ▸", icon, std::move(action));
    b.x = kMargin;
    b.y = y;
    b.w = kSettingsNavW;
    b.h = 80;
    buttons_.push_back(b);
  };
  auto open = [this](Page page) {
    return [this, page] {
      showPage(page);
      return MenuCommand{};
    };
  };
  subPage(t.picture, Icon::None, open(Page::Picture), 330);
  subPage(t.subtitles, Icon::None, open(Page::SubtitleStyle), 425);
  subPage(t.youtube, Icon::None, open(Page::YouTubeSettings), 520);

  // Right: each setting as a choice of buttons, its label drawn in front.
  addRow(330, 80,
         {textButton("Deutsch", Icon::None,
                     command(MenuCommand::SetLanguage, static_cast<double>(Language::German)),
                     view_.language == Language::German),
          textButton("English", Icon::None,
                     command(MenuCommand::SetLanguage, static_cast<double>(Language::English)),
                     view_.language == Language::English)},
         kSettingsControlX);
  addRow(430, 80,
         {textButton(t.themes[0], Icon::None,
                     command(MenuCommand::SetTheme, static_cast<double>(Theme::Glass)), glass()),
          textButton(t.themes[1], Icon::None,
                     command(MenuCommand::SetTheme, static_cast<double>(Theme::Material)),
                     !glass())},
         kSettingsControlX);
  // On/off pairs; clicking the other one toggles.
  const std::function<MenuCommand()> nothing = [] { return MenuCommand{}; };
  auto onOff = [&](double y, bool on, MenuCommand::Type toggle) {
    addRow(y, 80,
           {textButton(t.off, Icon::None, on ? command(toggle) : nothing, !on),
            textButton(t.on, Icon::None, on ? nothing : command(toggle), on)},
           kSettingsControlX);
  };
  onOff(530, view_.lyingMode, MenuCommand::ToggleLyingMode);
  onOff(660, view_.background, MenuCommand::ToggleBackground);
  onOff(760, view_.menuFollows, MenuCommand::ToggleMenuFollows);
}

// Brightness, contrast, saturation and zoom of the running video, each as
// "−  value  +", and a reset button.
void Menu::addStepper(double y, int value, int lo, int hi, int step, MenuCommand::Type type,
                      bool disabled) {
  Button minus = textButton("−", Icon::None, command(type, std::max(lo, value - step)));
  Button plus = textButton("+", Icon::None, command(type, std::min(hi, value + step)));
  minus.x = 520;
  plus.x = 860;
  for (Button* b : {&minus, &plus}) {
    b->y = y;
    b->w = 120;
    b->h = 80;
    b->disabled = disabled;
    buttons_.push_back(*b);
  }
}

void Menu::layoutPicture() {
  const Strings& t = text();
  addStepper(330, view_.brightness, -100, 100, kPictureStep, MenuCommand::SetBrightness);
  addStepper(430, view_.contrast, -100, 100, kPictureStep, MenuCommand::SetContrast);
  addStepper(530, view_.saturation, -100, 100, kPictureStep, MenuCommand::SetSaturation);
  // Zoom in percent; only 180°/360° video has a view to zoom.
  addStepper(630, static_cast<int>(std::lround(view_.zoom * 100)), 50, 250, kZoomStepPercent,
             MenuCommand::SetZoom, view_.format.projection == Projection::Flat);
  Button reset = textButton(t.reset, Icon::None, command(MenuCommand::ResetPicture));
  reset.x = 1080;
  reset.y = 330;
  reset.h = 80;
  buttons_.push_back(reset);
}

// 3D subtitles on or off, then size, height and distance as "−  value  +".
void Menu::layoutSubtitleStyle() {
  const Strings& t = text();
  const bool on = view_.floatingSubtitles;
  const std::function<MenuCommand()> nothing = [] { return MenuCommand{}; };
  addRow(330, 80,
         {textButton(t.off, Icon::None,
                     on ? command(MenuCommand::ToggleFloatingSubtitles) : nothing, !on),
          textButton(t.on, Icon::None,
                     on ? nothing : command(MenuCommand::ToggleFloatingSubtitles), on)},
         520);
  addStepper(470, view_.subtitleSize, 50, 200, 10, MenuCommand::SetSubtitleSize);
  addStepper(570, view_.subtitleRaise, -100, 100, 5, MenuCommand::SetSubtitleRaise);
  addStepper(670, view_.subtitleDistance, 25, 600, 25, MenuCommand::SetSubtitleDistance);
}

namespace {
constexpr double kDialogW = 1160, kDialogH = 400;
}  // namespace

// The dialog box is centred on the visible part of the main panel.
void Menu::layoutDialog() {
  const Strings& t = text();
  const double boxY = (mainVisibleHeight() - kDialogH) / 2;
  auto close = [this] {
    dialog_ = Dialog::None;
    return MenuCommand{};
  };
  auto confirm = [this](bool permanent) {
    return [this, permanent] {
      MenuCommand c;
      c.type = MenuCommand::DeleteFile;
      c.path = dialogPath_;
      c.value = permanent ? 1 : 0;
      dialog_ = Dialog::None;
      return c;
    };
  };

  std::vector<Button> row;
  if (dialog_ == Dialog::Error) {
    row.push_back(textButton(t.ok, Icon::None, close));
  } else if (dialog_ == Dialog::ConfirmQuit) {
    row.push_back(textButton(t.cancel, Icon::None, close));
    Button go = textButton(t.quit, Icon::Power, [this] {
      dialog_ = Dialog::None;
      MenuCommand c;
      c.type = MenuCommand::Quit;
      return c;
    });
    go.danger = true;
    row.push_back(go);
  } else {
    row.push_back(textButton(t.cancel, Icon::None, close));
    const bool permanent = dialog_ != Dialog::ConfirmTrash;
    Button go = textButton(permanent ? t.deletePermanently : t.moveToTrash, Icon::Trash,
                           confirm(permanent));
    go.danger = true;
    row.push_back(go);
  }
  // Centred like addCenteredRow, but marked as part of the modal dialog.
  double total = 0;
  for (const Button& b : row) total += b.w;
  total += kGap * 3 * (row.size() - 1);
  double x = (kMainWidth - total) / 2;
  for (Button& b : row) {
    b.popup = true;
    b.x = x;
    b.y = boxY + kDialogH - 125;
    b.h = 85;
    buttons_.push_back(b);
    x += b.w + kGap * 3;
  }
}

void Menu::drawDialog(cairo_t* cr) {
  const Strings& t = text();
  const double boxX = (kMainWidth - kDialogW) / 2, boxY = (mainVisibleHeight() - kDialogH) / 2;
  // Dim the menu behind, then the box.
  cairo_rectangle(cr, 0, 0, kMainWidth, mainVisibleHeight());
  cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
  cairo_fill(cr);
  roundedRect(cr, boxX, boxY, kDialogW, kDialogH, 30);
  cairo_set_source_rgba(cr, 0.1, 0.1, 0.13, 1);
  cairo_fill(cr);

  const double textX = boxX + 50, textW = kDialogW - 100;
  if (dialog_ == Dialog::ConfirmQuit) {
    drawText(cr, t.quitTitle, textX, boxY + 90, textW, 60, 38, true, true, 1, 1, 1);
  } else if (dialog_ == Dialog::Error) {
    drawText(cr, t.deleteFailed, textX, boxY + 40, textW, 60, 38, true, true, 1, 1, 1);
    drawText(cr, dialogError_, textX, boxY + 120, textW, 60, 26, false, true, 0.75, 0.75, 0.8);
  } else {
    drawText(cr, isPhotoFile(dialogPath_) ? t.deletePhotoTitle : t.deleteTitle, textX, boxY + 30,
             textW, 60, 38, true, true, 1, 1, 1);
    drawText(cr, fs::path(dialogPath_).filename().string(), textX, boxY + 95, textW, 50, 28, false,
             true, 0.85, 0.85, 0.9);
    if (dialog_ == Dialog::ConfirmTrash)
      drawText(cr, t.deleteToTrashHint, textX, boxY + 160, textW, 50, 26, false, true, 0.7, 0.7,
               0.75);
    else
      drawText(cr, dialog_ == Dialog::TrashFailed ? t.trashFailedWarning : t.deletePermanentWarning,
               textX, boxY + 160, textW, 50, 26, true, true, 1.0, 0.45, 0.4);
  }
  drawButtons(cr, kMain, true);
}

void Menu::layoutYouTubeSettings() {
  std::vector<Button> row;
  for (int width : kQualityWidths)
    row.push_back(textButton(qualityLabel(width), Icon::None,
                             command(MenuCommand::SetQuality, width), view_.maxVideoWidth == width));
  addRow(400, 85, row, kMargin);
}

// The search keyboard: German or English layout, following the menu language.
void Menu::layoutKeyboard() {
  const Strings& t = text();
  static const std::vector<std::vector<std::string>> kGerman = {
      {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "ß"},
      {"q", "w", "e", "r", "t", "z", "u", "i", "o", "p", "ü"},
      {"a", "s", "d", "f", "g", "h", "j", "k", "l", "ö", "ä"},
      {"y", "x", "c", "v", "b", "n", "m", ",", ".", "-"}};
  static const std::vector<std::vector<std::string>> kEnglish = {
      {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-"},
      {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "'"},
      {"a", "s", "d", "f", "g", "h", "j", "k", "l", "?", "!"},
      {"z", "x", "c", "v", "b", "n", "m", ",", ".", "&"}};
  auto upper = [](const std::string& key) {
    if (key == "ä") return std::string("Ä");
    if (key == "ö") return std::string("Ö");
    if (key == "ü") return std::string("Ü");
    if (key.size() == 1 && std::islower(static_cast<unsigned char>(key[0])))
      return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(key[0]))));
    return key;
  };

  const auto& rows = view_.language == Language::German ? kGerman : kEnglish;
  constexpr double keyW = 112, keyH = 78, rowStep = 88;
  for (size_t r = 0; r < rows.size(); ++r) {
    std::vector<Button> keys;
    for (const std::string& key : rows[r]) {
      const std::string label = shift_ ? upper(key) : key;
      Button b;
      b.w = keyW;
      b.label = label;
      b.action = [this, label] {
        query_ += label;
        if (shift_) shift_ = false;  // one capital, like a phone keyboard
        return MenuCommand{};
      };
      keys.push_back(b);
    }
    addCenteredRow(410 + r * rowStep, keyH, keys);
  }

  Button shift = textButton("⇧", Icon::None, [this] {
    shift_ = !shift_;
    return MenuCommand{};
  }, shift_);
  Button space = textButton(t.space, Icon::None, [this] {
    query_ += ' ';
    return MenuCommand{};
  });
  space.w = 440;
  Button erase = textButton("⌫", Icon::None, [this] {
    backspace();
    return MenuCommand{};
  });
  // Pastes the desktop clipboard, e.g. a YouTube link copied on the PC.
  Button paste = textButton(t.paste, Icon::None, command(MenuCommand::Paste));
  Button search = textButton(t.search, Icon::None, [this] { return startSearch(); }, true);
  addCenteredRow(410 + 4 * rowStep, keyH, {shift, space, erase, paste, search});
}

// Search results as a gallery: 4 cards per row, 2 rows on screen. Scrolling
// (scrollOffset_, listCount_) counts grid rows here, not videos.
void Menu::layoutResults() {
  constexpr int columns = 4;
  constexpr double cardGap = 16, rowH = 288;
  const double cardW = (kMainWidth - 2 * kMargin - 30 - (columns - 1) * cardGap) / columns;
  visibleRows_ = 2;
  rowHeight_ = rowH;
  listTop_ = 320;
  listCount_ = (static_cast<int>(videos_.size()) + columns - 1) / columns;
  int firstRow, endRow;
  clampScroll(listCount_, visibleRows_, firstRow, endRow);
  const int first = firstRow * columns;
  const int end = std::min(static_cast<int>(videos_.size()), endRow * columns);
  for (int i = first; i < end; ++i) {
    const YouTubeVideo& v = videos_[i];
    std::string detail = v.channel;
    if (v.duration > 0) detail += "   ·   " + formatTime(v.duration);
    else detail += std::string("   ·   ") + text().live;
    if (v.kind == YouTubeVideo::Kind::Video360) detail += "   ·   360°";
    else if (v.kind == YouTubeVideo::Kind::VR180) detail += "   ·   VR180";

    const int cell = i - first;
    Button b;
    b.x = kMargin + (cell % columns) * (cardW + cardGap);
    b.y = 320 + (cell / columns - scrollFraction_) * rowH;
    b.w = cardW;
    b.h = rowH - cardGap;
    b.label = v.title;
    b.detail = detail;
    b.thumbnail = i;
    b.listRow = true;
    b.scrolls = true;
    const std::string url = v.url();
    const double kind = static_cast<double>(v.kind);
    b.action = [this, url, kind] {
      showPage(Page::Main);
      MenuCommand c;
      c.type = MenuCommand::PlayYouTube;
      c.path = url;
      c.value = kind;
      return c;
    };
    buttons_.push_back(b);
  }
}

void Menu::layout() {
  layoutPage();
  if (dialog_ != Dialog::None) layoutDialog();  // last, so it is on top
}

void Menu::layoutPage() {
  buttons_.clear();
  listRight_ = kMainWidth - kMargin;
  listCount_ = 0;
  if (page_ == Page::Main) {
    layoutMain();
    if (formatOpen_) layoutFormat();
    return;
  }

  Page parent = Page::Main;
  if (page_ == Page::YouTubeSettings || page_ == Page::Picture || page_ == Page::SubtitleStyle)
    parent = Page::Settings;
  else if (page_ == Page::YouTubeKeyboard && (!videos_.empty() || searchBusy_))
    parent = Page::YouTubeResults;
  Button back;
  back.x = kMargin;
  back.y = kHeaderY;
  back.w = kHeaderButtonW;
  back.h = 80;
  back.label = text().back;
  back.action = [this, parent] {
    showPage(parent);
    return MenuCommand{};
  };
  buttons_.push_back(back);

  switch (page_) {
    case Page::Settings:
      layoutSettings();
      break;
    case Page::YouTubeSettings:
      layoutYouTubeSettings();
      break;
    case Page::Picture:
      layoutPicture();
      break;
    case Page::SubtitleStyle:
      layoutSubtitleStyle();
      break;
    case Page::Files: {
      Button up = back;
      up.x = kMargin + kHeaderButtonW + kGap;
      up.w = kIconButtonW;
      up.label.clear();  // icon only
      up.icon = Icon::FolderUp;
      up.action = [this] {
        showFavorites_ = false;
        openDirectory(fs::path(directory_).parent_path().string());
        return MenuCommand{};
      };
      buttons_.push_back(up);
      // The favourite folders instead of the folder's contents (and back).
      Button favs = up;
      favs.x = up.x + kIconButtonW + kGap;
      favs.icon = showFavorites_ ? Icon::StarFilled : Icon::Star;
      favs.selected = showFavorites_;
      favs.tooltip = text().favorites;
      favs.action = [this] {
        showFavorites_ = !showFavorites_;
        scrollOffset_ = 0;
        scrollFraction_ = 0;
        return MenuCommand{};
      };
      buttons_.push_back(favs);
      if (fileKind_ == FileKind::Video) {
        // Direct way to YouTube, also from the start screen.
        Button youtube = back;
        youtube.x = kMainWidth - kMargin - kHeaderButtonW;
        youtube.label = text().youtube;
        youtube.icon = Icon::YouTube;
        youtube.action = [this] {
          showPage(videos_.empty() && !searchBusy_ ? Page::YouTubeKeyboard : Page::YouTubeResults);
          return MenuCommand{};
        };
        buttons_.push_back(youtube);
      }
      const double listX = kMargin;
      // The list (or gallery) leaves room for the tool column on the right;
      // the tools come last, so their sort choices lie on top.
      listRight_ = kMainWidth - kMargin - kBigButton - kGap;
      if (showFavorites_) {
        addList(favoriteItems(), listX, kFileRowH, kFileRows);
        break;
      }
      if (gallery_ && fileKind_ == FileKind::Video)
        addGallery(fileItems(), listX);
      else
        addList(fileItems(), listX, kFileRowH, kFileRows);
      layoutFileTools();
      break;
    }
    case Page::YouTubeKeyboard:
    case Page::YouTubeResults: {
      Button toggle = back;
      toggle.x = kMargin + kHeaderButtonW + kGap;
      toggle.label = text().vrOnly;
      toggle.selected = vrOnly_;
      toggle.action = [this] {
        vrOnly_ = !vrOnly_;
        if (page_ == Page::YouTubeResults) startSearch();  // redo with the new filter
        return MenuCommand{};
      };
      buttons_.push_back(toggle);
      if (page_ == Page::YouTubeKeyboard) {
        layoutKeyboard();
      } else {
        Button newSearch = back;
        newSearch.x = kMainWidth - kMargin - kHeaderButtonW;
        newSearch.label = text().search;
        newSearch.action = [this] {
          showPage(Page::YouTubeKeyboard);
          return MenuCommand{};
        };
        buttons_.push_back(newSearch);
        layoutResults();
      }
      break;
    }
    case Page::Main:
      break;
  }
}

void Menu::drawText(cairo_t* cr, const std::string& text, double x, double y, double w, double h,
                    int size, bool bold, bool center, double r, double g, double b,
                    double a) const {
  PangoLayout* layout = pango_cairo_create_layout(cr);
  PangoFontDescription* font = pango_font_description_from_string(bold ? "Sans Bold" : "Sans");
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, text.c_str(), -1);
  pango_layout_set_width(layout, static_cast<int>(w * PANGO_SCALE));
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_MIDDLE);
  pango_layout_set_alignment(layout, center ? PANGO_ALIGN_CENTER : PANGO_ALIGN_LEFT);
  int textW = 0, textH = 0;
  pango_layout_get_pixel_size(layout, &textW, &textH);
  cairo_set_source_rgba(cr, r, g, b, a);
  cairo_move_to(cr, x, y + (h - textH) / 2);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(font);
  g_object_unref(layout);
}

void Menu::drawButtons(cairo_t* cr, int panel, bool popups) {
  for (size_t i = 0; i < buttons_.size(); ++i) {
    const Button& b = buttons_[i];
    if (b.panel != panel || b.popup != popups) continue;
    // Rows of a smoothly scrolled list are cut off at the list's edges.
    struct Clip {
      cairo_t* cr;
      bool on;
      ~Clip() {
        if (on) cairo_restore(cr);
      }
    } clip{cr, b.scrolls};
    if (b.scrolls) {
      cairo_save(cr);
      cairo_rectangle(cr, 0, listTop_ - 2, kMainWidth, visibleRows_ * rowHeight_ + 2);
      cairo_clip(cr);
    }
    const Palette& pal = paletteFor(view_.theme);
    if (!b.action) {  // list heading
      drawText(cr, b.label, b.x + 8, b.y, b.w - 16, b.h, 28, true, false, pal.textDim.r,
               pal.textDim.g, pal.textDim.b);
      continue;
    }
    double fill[4], ink[4];
    buttonColors(b, fill, ink);
    if (b.disabled) cairo_push_group(cr);  // faded as a whole below
    if (!b.volumeSlider) {  // the slider sits on the volume pill
      const double radius =
          b.radius >= 0 ? std::min(b.radius, std::min(b.w, b.h) / 2) : kButtonRadius;
      if (glass() && fill[3] > 0 && !b.danger) {
        glassButton(cr, b.x, b.y, b.w, b.h, radius, fill);
      } else {
        roundedRect(cr, b.x, b.y, b.w, b.h, radius);
        cairo_set_source_rgba(cr, fill[0], fill[1], fill[2], fill[3]);
        cairo_fill_preserve(cr);
        if (!glass() && !b.caption.empty() && !b.selected) {  // outlined tile
          setColor(cr, pal.tileEdge);
          cairo_set_line_width(cr, 2);
          cairo_stroke_preserve(cr);
        }
        cairo_new_path(cr);
      }
    }
    drawButtonContent(cr, b, ink);
    if (!b.badge.empty()) {  // a small bright disc on the top-right edge
      const Palette& pal = paletteFor(view_.theme);
      const double r = 17, bx = b.x + b.w - r * 0.6, by = b.y + r * 0.6;
      cairo_arc(cr, bx, by, r, 0, 2 * M_PI);
      setColor(cr, pal.play);
      cairo_fill(cr);
      drawText(cr, b.badge, bx - r, by - r, 2 * r, 2 * r, 20, true, true, pal.playIcon.r,
               pal.playIcon.g, pal.playIcon.b);
    }
    if (b.disabled) {
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, 0.35);
    }
  }
}

void Menu::buttonColors(const Button& b, double fill[4], double ink[4]) const {
  const Palette& pal = paletteFor(view_.theme);
  const bool tile = !b.caption.empty();
  Rgba f = pal.button, k = pal.icon;
  if (b.w >= kPlayButton && b.w == b.h && !b.selected) {  // the big play button
    f = pal.play;
    k = pal.playIcon;
  } else if (b.selected) {
    f = tile ? pal.tileOn : pal.buttonOn;
    k = tile ? pal.tileInkOn : pal.iconOn;
  } else if (b.bare || ((b.plain || tile) && !glass())) {
    f = {0, 0, 0, 0};  // bare buttons; Material: plain icon buttons and outlined tiles
  }
  if (b.danger) {  // confirm button of a destructive dialog
    f = {0.72, 0.18, 0.16, 1};
    k = {1, 1, 1, 1};
  }
  fill[0] = f.r, fill[1] = f.g, fill[2] = f.b, fill[3] = f.a;
  ink[0] = k.r, ink[1] = k.g, ink[2] = k.b, ink[3] = k.a;
}

void Menu::drawButtonContent(cairo_t* cr, const Button& b, const double ink[4]) const {
  {
    if (b.volumeSlider) {
      drawVolumeSlider(cr, b);
      return;
    }
    if (!b.caption.empty()) {  // format tile: icon, short label under it
      drawIcon(cr, b.icon, b.x + b.w / 2, b.y + b.h / 2 - 14, b.h > 100 ? 46 : 40, ink[0], ink[1],
               ink[2]);
      drawText(cr, b.caption, b.x + 4, b.y + b.h - 42, b.w - 8, 30, 19, true, true, ink[0], ink[1],
               ink[2]);
      return;
    }
    if (b.card) {  // file browser gallery: large thumbnail (or folder), name below
      const double tw = b.w - 16, th = tw * 9 / 16, tx = b.x + 8, ty = b.y + 8;
      roundedRect(cr, tx, ty, tw, th, 12);
      cairo_surface_t* thumb =
          thumbnailer_ && !b.thumbPath.empty() ? thumbnailer_->find(b.thumbPath) : nullptr;
      if (thumb) {
        const double sw = cairo_image_surface_get_width(thumb);
        const double sh = cairo_image_surface_get_height(thumb);
        const double scale = std::max(tw / sw, th / sh);
        cairo_save(cr);
        cairo_clip(cr);
        cairo_translate(cr, tx + (tw - sw * scale) / 2, ty + (th - sh * scale) / 2);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, thumb, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
      } else {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
        cairo_fill(cr);
        if (b.icon != Icon::None)
          drawIcon(cr, b.icon, tx + tw / 2, ty + th / 2, 96, ink[0], ink[1], ink[2]);
      }
      drawText(cr, b.label, b.x + 12, ty + th + 4, b.w - 24, 36, 24, false, true, ink[0], ink[1],
               ink[2]);
      return;
    }
    if (b.thumbnail >= 0) {  // search result card: thumbnail, title and details below
      const double thumbW = b.w - 16, thumbH = thumbW * 9 / 16;
      const double tx = b.x + 8, ty = b.y + 8;
      cairo_surface_t* thumb =
          b.thumbnail < static_cast<int>(thumbnails_.size()) ? thumbnails_[b.thumbnail] : nullptr;
      roundedRect(cr, tx, ty, thumbW, thumbH, 8);
      if (thumb) {
        cairo_save(cr);
        cairo_clip_preserve(cr);
        // Fill the 16:9 box, cropping what sticks out.
        const double sw = cairo_image_surface_get_width(thumb);
        const double sh = cairo_image_surface_get_height(thumb);
        const double scale = std::max(thumbW / sw, thumbH / sh);
        cairo_translate(cr, tx + (thumbW - sw * scale) / 2, ty + (thumbH - sh * scale) / 2);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, thumb, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_new_path(cr);
      } else {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
        cairo_fill(cr);
      }
      const double textX = b.x + 12, textW = b.w - 24, textY = ty + thumbH + 4;
      drawText(cr, b.label, textX, textY, textW, 34, 24, true, false, ink[0], ink[1], ink[2]);
      drawText(cr, b.detail, textX, textY + 34, textW, 26, 19, false, false, ink[0], ink[1],
               ink[2], 0.7);
      return;
    }
    if (b.label.empty()) {  // icon only: round buttons scale it, slim list rows shrink it
      const double size = b.w == b.h ? b.w * (b.w >= kPlayButton ? 0.42 : 0.45)
                                     : std::min(kIconSize, b.h * 0.62);
      drawIcon(cr, b.icon, b.x + b.w / 2, b.y + b.h / 2, size, ink[0], ink[1], ink[2]);
      return;
    }
    double textX = b.x + (b.listRow ? 24 : 8);
    double textW = b.w - (b.listRow ? 48 : 16);
    bool center = !b.listRow;
    if (!b.thumbPath.empty()) {
      // Video row: thumbnail (filling a 16:9 box) or a placeholder until it is made.
      const double th = b.h - 8, tw = th * 16 / 9, tx = b.x + 10, ty = b.y + 4;
      cairo_surface_t* thumb = thumbnailer_ ? thumbnailer_->find(b.thumbPath) : nullptr;
      roundedRect(cr, tx, ty, tw, th, 6);
      if (thumb) {
        const double sw = cairo_image_surface_get_width(thumb);
        const double sh = cairo_image_surface_get_height(thumb);
        const double scale = std::max(tw / sw, th / sh);
        cairo_save(cr);
        cairo_clip(cr);
        cairo_translate(cr, tx + (tw - sw * scale) / 2, ty + (th - sh * scale) / 2);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, thumb, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
      } else {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
        cairo_fill(cr);
      }
      textX += tw + 6;
      textW -= tw + 6;
    }
    if (b.icon != Icon::None) {
      const double iconSize = kIconSize * 0.75;
      drawIcon(cr, b.icon, b.x + 35 + iconSize / 2, b.y + b.h / 2, iconSize, ink[0], ink[1],
               ink[2]);
      textX = b.x + 35 + iconSize + 20;
      textW = b.w - (textX - b.x) - 35;
      center = false;
    }
    drawText(cr, b.label, textX, b.y, textW, b.h, b.fontSize > 0 ? b.fontSize : b.listRow ? 30 : 32,
             false, center, ink[0], ink[1], ink[2]);
  }
}

// The player page's decoration under its buttons: the volume pill and the
// line of video info.
void Menu::drawMainPage(cairo_t* cr) {
  const Palette& pal = paletteFor(view_.theme);
  drawVolumePill(cr, kVolumeY);

  // The info chips (placed by layoutInfoLine).
  for (const InfoChip& chip : infoChips_) {
    roundedRect(cr, chip.x, kInfoY, chip.w, 38, glass() ? 19 : 12);
    setColor(cr, pal.chip);
    cairo_fill_preserve(cr);
    setColor(cr, pal.chipEdge);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    drawText(cr, chip.text, chip.x, kInfoY, chip.w, 38, 21, false, true, pal.textDim.r,
             pal.textDim.g, pal.textDim.b, pal.textDim.a);
  }
}

// "5760 × 2880 · HEVC 10-bit · 59,94 fps · 180° 3D" as chips, then the
// buttons "OPUS 5.1 (Track 1) ▾" and "Subtitles: Off ▾", all centred.
void Menu::layoutInfoLine() {
  const PlaybackState& p = view_.playback;
  const Strings& t = text();
  auto upper = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
  };
  std::vector<std::string> info;
  if (view_.videoWidth > 0)
    info.push_back(std::to_string(view_.videoWidth) + " × " + std::to_string(view_.videoHeight));
  if (!p.videoCodec.empty() && !view_.photo)
    info.push_back(upper(p.videoCodec) +
                   (p.pixelFormat.find("10") != std::string::npos ? " 10-bit" : ""));
  if (p.fps > 0 && !view_.photo) {  // photos report a meaningless 1 fps
    char fps[32];
    std::snprintf(fps, sizeof(fps), "%.3g fps", p.fps);
    std::string s = fps;
    if (view_.language == Language::German) std::replace(s.begin(), s.end(), '.', ',');
    info.push_back(s);
  }
  if (!view_.currentFile.empty()) {
    std::string format = t.projectionShort[static_cast<int>(view_.format.projection)];
    if (view_.format.stereo != StereoLayout::Mono) format += " · 3D";
    info.push_back(format);
  }

  // The selected tracks.
  const Track* audio = nullptr;
  const Track* subtitle = nullptr;
  bool anyAudio = false;
  for (const Track& track : p.tracks) {
    anyAudio = anyAudio || track.type == "audio";
    if (track.selected && track.type == "audio") audio = &track;
    if (track.selected && track.type == "sub") subtitle = &track;
  }
  auto toggle = [this](Dropdown d) {
    return [this, d] {
      dropdown_ = dropdown_ == d ? Dropdown::None : d;
      return MenuCommand{};
    };
  };
  auto small = [&](const std::string& label, Dropdown d) {
    Button b = textButton(label + "  ▾", Icon::None, toggle(d), dropdown_ == d);
    b.fontSize = 21;
    b.w = std::min(320.0, textWidth(b.label, 21, false) + 40);
    b.h = 46;
    b.radius = glass() ? 23 : 12;
    return b;
  };
  std::vector<Button> buttons;
  std::vector<Dropdown> opens;  // which list each button opens
  if (p.chapters.size() > 1 && !view_.photo) {
    buttons.push_back(small(std::string(t.chapter) + " " + std::to_string(std::max(0, p.chapter) + 1) +
                                "/" + std::to_string(p.chapters.size()),
                            Dropdown::Chapters));
    opens.push_back(Dropdown::Chapters);
  }
  if (anyAudio && !view_.photo) {
    std::string label = audio ? upper(audio->codec) : std::string(t.noAudio);
    if (audio && p.audioChannels == 1) label += " Mono";
    else if (audio && p.audioChannels == 2) label += " Stereo";
    else if (audio && p.audioChannels == 6) label += " 5.1";
    else if (audio && p.audioChannels == 8) label += " 7.1";
    if (audio) label += " (" + std::string(t.track) + " " + std::to_string(audio->id) + ")";
    buttons.push_back(small(label, Dropdown::Audio));
    opens.push_back(Dropdown::Audio);
  }
  if (!view_.currentFile.empty() && !view_.photo) {
    buttons.push_back(small(std::string(t.subtitles) + ": " +
                                (subtitle ? trackName(*subtitle) : std::string(t.off)),
                            Dropdown::Subtitles));
    opens.push_back(Dropdown::Subtitles);
  }

  // Centred: chips, then the buttons.
  infoChips_.clear();
  double total = 0;
  for (const std::string& s : info) {
    infoChips_.push_back({0, textWidth(s, 21, false) + 32, s});
    total += infoChips_.back().w + 10;
  }
  for (const Button& b : buttons) total += b.w + 10;
  double x = (kMainWidth - total + 10) / 2;
  for (InfoChip& chip : infoChips_) {
    chip.x = x;
    x += chip.w + 10;
  }
  for (Button& b : buttons) {
    b.x = x;
    b.y = kInfoY - 4;
    buttons_.push_back(b);
    x += b.w + 10;
  }

  // The open list, above its button.
  const auto open = std::find(opens.begin(), opens.end(), dropdown_);
  if (open == opens.end()) return;
  const Button& opener = buttons[open - opens.begin()];
  std::vector<Button> entries;
  auto entry = [&](const std::string& label, bool selected, std::function<MenuCommand()> action) {
    Button e;
    e.label = label;
    e.selected = selected;
    e.action = [this, action] {
      dropdown_ = Dropdown::None;
      return action();
    };
    entries.push_back(e);
  };
  if (dropdown_ == Dropdown::Chapters) {
    // Up to 15 chapters around the current one.
    const int count = static_cast<int>(p.chapters.size());
    const int first = std::clamp(p.chapter - 7, 0, std::max(0, count - 15));
    for (int i = first; i < std::min(count, first + 15); ++i) {
      const Chapter& c = p.chapters[i];
      entry(std::to_string(i + 1) + ".  " + (c.title.empty() ? "" : c.title + "  ·  ") +
                formatTime(c.time),
            i == p.chapter, command(MenuCommand::SeekTo, c.time));
    }
  } else if (dropdown_ == Dropdown::Audio) {
    for (const Track& track : p.tracks)
      if (track.type == "audio")
        entry(trackName(track) + (track.codec.empty() ? "" : "  ·  " + upper(track.codec)),
              track.selected, command(MenuCommand::SelectAudio, static_cast<double>(track.id)));
  } else {
    entry(t.off, subtitle == nullptr, command(MenuCommand::SelectSubtitle, 0));
    for (const Track& track : p.tracks)
      if (track.type == "sub")
        entry(trackName(track), track.selected,
              command(MenuCommand::SelectSubtitle, static_cast<double>(track.id)));
    entry(std::string(t.subtitleFromFile) + " …", false, [this] {
      openFiles(FileKind::Subtitle);
      return MenuCommand{};
    });
  }
  addDropdownList(opener, entries);
}

// The choices stacked above their opener, in columns of five if there are
// many, drawn on top of everything else.
void Menu::addDropdownList(const Button& opener, std::vector<Button> entries) {
  constexpr int perColumn = 5;
  constexpr double h = 60, step = 68;
  double w = 0;
  for (const Button& e : entries) w = std::max(w, textWidth(e.label, 26, false) + 60);
  const int count = static_cast<int>(entries.size());
  const int columns = (count + perColumn - 1) / perColumn;
  const int rows = std::min(count, perColumn);
  // At most 520 wide, and all columns within the panel.
  w = std::min({w, 520.0, (kMainWidth - 2 * kMargin - (columns - 1) * kGap) / columns});
  const double total = columns * w + (columns - 1) * kGap;
  const double x0 = std::max(kMargin, std::min(opener.x + opener.w / 2 - total / 2,
                                               kMainWidth - kMargin - total));
  for (int i = 0; i < count; ++i) {
    Button& e = entries[i];
    e.popup = true;
    e.fontSize = 26;
    e.radius = glass() ? h / 2 : 16;
    e.x = x0 + (i / perColumn) * (w + kGap);
    e.y = opener.y - 14 - (rows - i % perColumn) * step;
    e.w = w;
    e.h = h;
    buttons_.push_back(e);
  }
}

void Menu::drawMain(cairo_t* cr) {
  const Strings& t = text();
  const PlaybackState& p = view_.playback;

  const Palette& pal = paletteFor(view_.theme);
  const double panelFill[4] = {pal.panel.r, pal.panel.g, pal.panel.b, pal.panel.a};
  if (glass()) {
    glassButton(cr, 0.75, 0.75, kMainWidth - 1.5, mainVisibleHeight() - 1.5, pal.panelRadius,
                panelFill);
  } else {
    roundedRect(cr, 0, 0, kMainWidth, mainVisibleHeight(), pal.panelRadius);
    setColor(cr, pal.panel);
    cairo_fill(cr);
  }

  // Title and time: on the player page the title under the corner buttons
  // and the times under the seek bar, on sub pages both along the top.
  const std::string title = view_.currentFile.empty() ? t.noVideo
                            : p.title.empty()         ? t.loading
                                                      : p.title;
  const bool mainPage = page_ == Page::Main;
  const double trackX = kMargin, trackW = kMainWidth - 2 * kMargin;
  const double trackY = seekLineY();
  const bool timeline = !view_.photo;  // photos have no time to show or seek in
  if (mainPage && !collapsed_) {
    // Between the delete button and the fold, settings and quit buttons.
    const double titleX = kMargin + 2 * kEdgeButton + kGap + 24;
    const double titleRight = kMainWidth - kMargin - 60 - 2 * (kEdgeButton + kGap) - 24;
    drawText(cr, title, titleX, kTopY, titleRight - titleX, kEdgeButton, 38, true, false,
             pal.text.r, pal.text.g, pal.text.b);
  }
  if (mainPage && timeline) {
    const std::string total = formatTime(p.duration);
    const double shownTime = draggingSeek_ && lastScrub_ >= 0 ? lastScrub_ : p.position;
    drawText(cr, formatTime(shownTime), kMargin, trackY + 22, 300, 36, 25, false, false,
             pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a);
    const double totalW = textWidth(total, 25, false);
    drawText(cr, total, kMainWidth - kMargin - totalW - 2, trackY + 22, totalW + 4, 36, 25, false,
             false, pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a);
  } else if (!mainPage) {
    drawText(cr, title, kMargin, 30, 1060, 70, 38, true, false, pal.text.r, pal.text.g,
             pal.text.b);
    const std::string time = formatTime(p.position) + " / " + formatTime(p.duration);
    const double timeW = textWidth(time, 34, false);
    if (timeline)
      drawText(cr, time, kMainWidth - kMargin - timeW - 2, 30, timeW + 4, 70, 34, false, false,
               pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a);
  }

  // Seek bar, with the downloaded part of a stream under the played part.
  // While it is dragged, the knob shows where it is being dragged to.
  const double shown = draggingSeek_ && lastScrub_ >= 0 ? lastScrub_ : p.position;
  const double fraction = p.duration > 0 ? std::clamp(shown / p.duration, 0.0, 1.0) : 0.0;
  const double buffered = view_.streaming && p.duration > 0
                              ? std::clamp(p.bufferedUntil / p.duration, 0.0, 1.0)
                              : -1.0;
  if (timeline) drawTrack(cr, trackX, trackX + trackW, trackY, fraction, buffered, 1.0);
  if (timeline && p.duration > 0 && p.chapters.size() > 1) {
    // Chapter starts as small ticks.
    for (const Chapter& c : p.chapters)
      if (c.time > 0 && c.time < p.duration)
        cairo_rectangle(cr, trackX + trackW * c.time / p.duration - 1.5, trackY - 14, 3, 28);
    setColor(cr, pal.text, 0.55);
    cairo_fill(cr);
  }
  if (timeline && p.duration > 0 && p.abLoopA >= 0) {
    // A-B loop: the repeated part in yellow, with its ends marked.
    const double a = trackX + trackW * std::clamp(p.abLoopA / p.duration, 0.0, 1.0);
    const double b = p.abLoopB >= 0 ? trackX + trackW * std::clamp(p.abLoopB / p.duration, 0.0, 1.0)
                                    : a;
    if (b > a) {
      cairo_rectangle(cr, a, trackY - 7, b - a, 14);
      cairo_set_source_rgba(cr, 1.0, 0.8, 0.2, 0.55);
      cairo_fill(cr);
    }
    for (double x : {a, b}) cairo_rectangle(cr, x - 3, trackY - 20, 6, 40);
    cairo_set_source_rgba(cr, 1.0, 0.8, 0.2, 1);
    cairo_fill(cr);
  }

  // Page headings and labels.
  const double headingX = kMargin + kHeaderButtonW + kGap + 10;
  switch (page_) {
    case Page::Main:
      if (collapsed_)
        drawVolumePill(cr, kMiniRowY);
      else
        drawMainPage(cr);
      break;
    case Page::Settings:
      drawText(cr, t.settings, headingX, kHeaderY, 800, 80, 32, true, false, 0.85, 0.85, 0.9);
      // Right column: a label left of each setting, a hint under the toggles.
      drawText(cr, t.language, kSettingsLabelX, 330, 260, 80, 30, true, false, 0.7, 0.7, 0.75);
      drawText(cr, t.design, kSettingsLabelX, 430, 260, 80, 30, true, false, 0.7, 0.7, 0.75);
      drawText(cr, t.lyingMode, kSettingsLabelX, 530, 260, 80, 30, true, false, 0.7, 0.7, 0.75);
      drawText(cr, t.lyingModeHint, kSettingsLabelX, 610, kMainWidth - kMargin - kSettingsLabelX,
               40, 21, false, false, 0.55, 0.55, 0.6);
      drawText(cr, t.background, kSettingsLabelX, 660, 260, 80, 30, true, false, 0.7, 0.7, 0.75);
      drawText(cr, t.menuFollows, kSettingsLabelX, 760, 270, 80, 26, true, false, 0.7, 0.7,
               0.75);
      break;
    case Page::SubtitleStyle: {
      drawText(cr, t.subtitles, headingX, kHeaderY, 800, 80, 32, true, false, 0.85, 0.85, 0.9);
      drawText(cr, t.subtitles3d, kMargin, 330, 460, 80, 30, true, false, 0.7, 0.7, 0.75);
      drawText(cr, t.subtitles3dHint, kMargin, 410, kMainWidth - 2 * kMargin, 40, 21, false, false,
               0.55, 0.55, 0.6);
      char distance[32];
      std::snprintf(distance, sizeof(distance), "%g m", view_.subtitleDistance / 100.0);
      std::string distanceText = distance;
      if (view_.language == Language::German)
        std::replace(distanceText.begin(), distanceText.end(), '.', ',');
      const char* labels[] = {t.subtitleSize, t.subtitleRaise, t.subtitleDistance};
      const std::string values[] = {std::to_string(view_.subtitleSize) + " %",
                                    std::to_string(view_.subtitleRaise), distanceText};
      for (int i = 0; i < 3; ++i) {
        drawText(cr, labels[i], kMargin, 470 + i * 100, 460, 80, 30, true, false, 0.7, 0.7, 0.75);
        drawText(cr, values[i], 640, 470 + i * 100, 220, 80, 32, false, true, 1, 1, 1);
      }
      break;
    }
    case Page::YouTubeSettings:
      drawText(cr, t.youtube, headingX, kHeaderY, 800, 80, 32, true, false, 0.85, 0.85, 0.9);
      drawText(cr, t.defaultQuality, kMargin, 330, kMainWidth - 2 * kMargin, 60, 30, true, false,
               0.7, 0.7, 0.75);
      drawText(cr, t.defaultQualityHint, kMargin, 500, kMainWidth - 2 * kMargin, 50, 24, false,
               false, 0.6, 0.6, 0.65);
      break;
    case Page::Picture: {
      drawText(cr, t.picture, headingX, kHeaderY, 800, 80, 32, true, false, 0.85, 0.85, 0.9);
      const char* labels[] = {t.brightness, t.contrast, t.saturation, t.zoom};
      const std::string values[] = {std::to_string(view_.brightness),
                                    std::to_string(view_.contrast),
                                    std::to_string(view_.saturation),
                                    std::to_string(std::lround(view_.zoom * 100)) + " %"};
      const bool zoomOff = view_.format.projection == Projection::Flat;
      for (int i = 0; i < 4; ++i) {
        const double y = 330 + i * 100, a = i == 3 && zoomOff ? 0.35 : 1.0;
        drawText(cr, labels[i], kMargin, y, 460, 80, 30, true, false, 0.7, 0.7, 0.75, a);
        drawText(cr, values[i], 640, y, 220, 80, 32, false, true, 1, 1, 1, a);
      }
      drawText(cr, t.pictureHint, kMargin, 740, kMainWidth - 2 * kMargin, 50, 24, false, false,
               0.6, 0.6, 0.65);
      break;
    }
    case Page::Files: {
      const double x = kMargin + kHeaderButtonW + kGap + 2 * (kIconButtonW + kGap) + 10;
      const std::string heading =
          showFavorites_ ? std::string(t.favorites)
          : fileKind_ == FileKind::Subtitle ? std::string(t.chooseSubtitleFile) + ":  " + directory_
                                            : directory_;
      const double right = fileKind_ == FileKind::Video
                               ? kMainWidth - kMargin - kHeaderButtonW - kGap
                               : kMainWidth - kMargin;
      drawText(cr, heading, x, kHeaderY, right - x, 80, 26, false, false, 0.7, 0.7, 0.75);
      break;
    }
    case Page::YouTubeKeyboard: {
      // Search field with a cursor bar.
      roundedRect(cr, kMargin, 310, kMainWidth - 2 * kMargin, 80, 18);
      cairo_set_source_rgba(cr, 1, 1, 1, 0.1);
      cairo_fill(cr);
      if (query_.empty())
        drawText(cr, t.typeToSearch, kMargin + 24, 310, kMainWidth - 2 * kMargin - 48, 80, 34,
                 false, false, 0.55, 0.55, 0.6);
      else
        drawText(cr, query_ + "▏", kMargin + 24, 310, kMainWidth - 2 * kMargin - 48, 80, 34, false,
                 false, 1, 1, 1);
      break;
    }
    case Page::YouTubeResults: {
      const double x = kMargin + 2 * (kHeaderButtonW + kGap) + 10;
      drawText(cr, query_, x, kHeaderY, kMainWidth - kMargin - kHeaderButtonW - kGap - x, 80, 30,
               true, false, 0.85, 0.85, 0.9);
      const char* status = searchBusy_               ? t.searching
                           : !searchError_.empty()   ? t.searchFailed
                           : videos_.empty()         ? t.noResults
                                                     : nullptr;
      if (status && videos_.empty())
        drawText(cr, status, kMargin, 330, kMainWidth - 2 * kMargin, 80, 32, false, true, 0.7, 0.7,
                 0.75);
      else if (searchBusy_)  // the next page is on its way
        drawText(cr, t.loadingMore, kMargin, kHeaderY + 80, kMainWidth - 2 * kMargin, 20, 20,
                 false, true, 0.6, 0.6, 0.65);
      break;
    }
  }

  drawButtons(cr, kMain);
  if (dropdown_ != Dropdown::None) {
    // Backdrop behind the dropdown entries, then the entries on top.
    double x0 = kMainWidth, y0 = kMainHeight, x1 = 0, y1 = 0;
    for (const Button& b : buttons_) {
      if (!b.popup) continue;
      x0 = std::min(x0, b.x);
      y0 = std::min(y0, b.y);
      x1 = std::max(x1, b.x + b.w);
      y1 = std::max(y1, b.y + b.h);
    }
    // Glass: a darker pane of glass with a light edge; Material: a raised surface.
    const double radius = glass() ? (y1 - y0 + 24) / 2 : 28;
    roundedRect(cr, x0 - 12, y0 - 12, x1 - x0 + 24, y1 - y0 + 24, radius);
    if (glass())
      cairo_set_source_rgba(cr, 0.06, 0.10, 0.16, 0.72);
    else
      cairo_set_source_rgba(cr, 0.145, 0.169, 0.184, 1.0);
    cairo_fill_preserve(cr);
    if (glass()) {
      cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
      cairo_set_line_width(cr, 1.5);
      cairo_stroke_preserve(cr);
    }
    cairo_new_path(cr);
    drawButtons(cr, kMain, true);
  }
  if (dialog_ != Dialog::None) drawDialog(cr);
  if (tooltip_ >= 0 && tooltip_ < static_cast<int>(buttons_.size()) &&
      buttons_[tooltip_].panel == kMain) {
    // The hovered icon's name in a small box just above it.
    const Button& b = buttons_[tooltip_];
    const double w = textWidth(b.tooltip, 26, false) + 40, h = 50;
    const double x = std::clamp(b.x + b.w / 2 - w / 2, 10.0, kMainWidth - 10 - w);
    const double y = std::max(4.0, b.y - h - 8);
    roundedRect(cr, x, y, w, h, 12);
    cairo_set_source_rgba(cr, 0.02, 0.02, 0.03, 0.96);
    cairo_fill(cr);
    drawText(cr, b.tooltip, x, y, w, h, 26, false, true, 1, 1, 1);
  }

  if (page_ == Page::Files && showFavorites_ && favorites_.paths().empty())
    drawText(cr, t.noFavorites, kMargin, kListY, kMainWidth - 2 * kMargin, kRowH, 30, false, false,
             0.7, 0.7, 0.75);
  else if (page_ == Page::Files && !showFavorites_ && entries_.empty())
    drawText(cr, fileKind_ == FileKind::Video ? t.noVideos : t.noSubtitleFiles, kMargin, kListY,
             kMainWidth - 2 * kMargin, kRowH, 30, false, false, 0.7, 0.7, 0.75);
  if (listOpen() && listCount_ > visibleRows_) {
    // Scroll position indicator.
    const double listY = page_ == Page::YouTubeResults ? 320 : kListY;
    const double trackH = visibleRows_ * rowHeight_ - 6;
    const double barH = trackH * visibleRows_ / listCount_;
    const double barY = listY + (trackH - barH) * std::min(1.0, (scrollOffset_ + scrollFraction_) /
                                                                   (listCount_ - visibleRows_));
    roundedRect(cr, listRight_ - 12, barY, 10, barH, 5);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.4);
    cairo_fill(cr);
  }
}

void Menu::drawFormat(cairo_t* cr) {
  const Palette& pal = paletteFor(view_.theme);
  const double panelFill[4] = {pal.panel.r, pal.panel.g, pal.panel.b, pal.panel.a};
  if (glass()) {
    glassButton(cr, 0.75, 0.75, kFormatWidth - 1.5, kFormatHeight - 1.5, 56, panelFill);
  } else {
    roundedRect(cr, 0, 0, kFormatWidth, kFormatHeight, 48);
    setColor(cr, pal.panel);
    cairo_fill(cr);
  }
  // Dividers between projections, 3D layouts and "curved".
  for (double y : {300.0, 452.0}) cairo_rectangle(cr, 38, y, kFormatWidth - 76, 1.5);
  setColor(cr, pal.chipEdge);
  cairo_fill(cr);
  drawButtons(cr, kFormat);
}

void Menu::redraw() {
  dirty_ = false;
  layout();
  // Hover index refers to the new layout.
  hovered_ = pointerPanel_ >= 0 ? buttonAt(pointerPanel_, pointerX_, pointerY_) : -1;
  if (tooltip_ != hovered_) tooltip_ = -1;

  for (int i = 0; i < 2; ++i) {
    if (i == kFormat && !formatPanelOpen()) continue;
    CairoCanvas& canvas = canvases_[i];
    canvas.clear();
    if (i == kMain) drawMain(canvas.cr()); else drawFormat(canvas.cr());
    canvas.upload();
  }
}

GLuint Menu::texture(PanelId panel) {
  if (dirty_) redraw();
  return canvases_[panel].texture();
}
