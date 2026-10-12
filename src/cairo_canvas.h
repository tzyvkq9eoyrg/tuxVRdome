#pragma once

#include "gl_util.h"

typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

// A cairo drawing surface with a matching sRGB GL texture (premultiplied
// alpha, mipmapped) that it can be uploaded to.
class CairoCanvas {
 public:
  CairoCanvas() = default;
  CairoCanvas(const CairoCanvas&) = delete;
  CairoCanvas& operator=(const CairoCanvas&) = delete;
  ~CairoCanvas();

  // Requires the GL context to be current.
  bool init(int width, int height);

  cairo_t* cr() const { return cr_; }
  cairo_surface_t* surface() const { return surface_; }
  GLuint texture() const { return texture_; }
  int width() const { return width_; }
  int height() const { return height_; }

  // Makes the whole surface transparent.
  void clear();
  // Copies the surface into the texture and rebuilds its mipmaps.
  void upload();

 private:
  cairo_surface_t* surface_ = nullptr;
  cairo_t* cr_ = nullptr;
  GLuint texture_ = 0;
  int width_ = 0, height_ = 0;
};
