#include "subtitle_overlay.h"

#include <cairo.h>
#include <pango/pangocairo.h>

void SubtitleOverlay::setText(const std::string& text) {
  if (text == text_) return;
  text_ = text;
  dirty_ = true;
}

GLuint SubtitleOverlay::texture() {
  if (!dirty_) return canvas_.texture();
  dirty_ = false;

  cairo_t* cr = canvas_.cr();
  canvas_.clear();
  PangoLayout* layout = pango_cairo_create_layout(cr);
  PangoFontDescription* font = pango_font_description_from_string("Sans Bold");
  pango_font_description_set_absolute_size(font, 72 * PANGO_SCALE);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, text_.c_str(), -1);
  pango_layout_set_width(layout, (kWidth - 80) * PANGO_SCALE);
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
  int textW = 0, textH = 0;
  pango_layout_get_pixel_size(layout, &textW, &textH);

  // Bottom-aligned, dark outline under white fill for readability on any picture.
  cairo_move_to(cr, 40, kHeight - 20 - textH);
  pango_cairo_layout_path(cr, layout);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_width(cr, 10);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.85);
  cairo_stroke_preserve(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, 1);
  cairo_fill(cr);

  pango_font_description_free(font);
  g_object_unref(layout);
  canvas_.upload();
  return canvas_.texture();
}
