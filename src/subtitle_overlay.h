#pragma once

#include "cairo_canvas.h"

#include <string>

// Renders the current subtitle line as outlined text into a texture, so it
// can float in front of the viewer for 3D and surround video.
class SubtitleOverlay {
 public:
  static constexpr int kWidth = 1600;
  static constexpr int kHeight = 400;
  static constexpr float kAspect = static_cast<float>(kWidth) / kHeight;

  bool init() { return canvas_.init(kWidth, kHeight); }

  void setText(const std::string& text);
  bool empty() const { return text_.empty(); }
  // Texture (sRGB, premultiplied alpha) with the text at the bottom edge.
  GLuint texture();

 private:
  CairoCanvas canvas_;
  std::string text_;
  bool dirty_ = true;
};
