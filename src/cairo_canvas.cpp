#include "cairo_canvas.h"

#include <cairo.h>

CairoCanvas::~CairoCanvas() {
  if (cr_) cairo_destroy(cr_);
  if (surface_) cairo_surface_destroy(surface_);
  if (texture_) glDeleteTextures(1, &texture_);
}

bool CairoCanvas::init(int width, int height) {
  width_ = width;
  height_ = height;
  surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  if (cairo_surface_status(surface_) != CAIRO_STATUS_SUCCESS) return false;
  cr_ = cairo_create(surface_);

  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexStorage2D(GL_TEXTURE_2D, 4, GL_SRGB8_ALPHA8, width, height);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.0f);
  glBindTexture(GL_TEXTURE_2D, 0);
  return true;
}

void CairoCanvas::clear() {
  cairo_save(cr_);
  cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr_);
  cairo_restore(cr_);
}

void CairoCanvas::upload() {
  cairo_surface_flush(surface_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, cairo_image_surface_get_stride(surface_) / 4);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  // cairo's ARGB32 is B, G, R, A in memory on little-endian machines.
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_BGRA, GL_UNSIGNED_BYTE,
                  cairo_image_surface_get_data(surface_));
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glGenerateMipmap(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);
}
