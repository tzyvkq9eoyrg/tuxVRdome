#pragma once

#include "gl_util.h"
#include "video_format.h"

// What a look at a video frame tells about its packing.
struct FrameAnalysis {
  bool valid = false;  // false if the frame was too dark or flat to judge
  StereoLayout stereo = StereoLayout::Mono;
  // The (first) eye picture has a hard seam across its middle, as a cubemap's
  // rows of faces do (a column seam if the eye is taller than wide).
  bool cubemap = false;
  // The measurements, for the log.
  double sideBySide = 0, topBottom = 0, seam = 0;
};

// Reads a small mip level of `texture` (width × height at level 0, `level`
// chosen by the caller so the copy is a few hundred pixels wide) and judges
// it: two nearly equal halves are 3D, a seam through the eye's middle is a
// cubemap. Requires the GL context to be current.
FrameAnalysis analyzeFrame(GLuint texture, int width, int height, int level);
