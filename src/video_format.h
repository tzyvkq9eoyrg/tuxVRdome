#pragma once

#include <string>

// How the video maps onto the viewer's field of view.
// EquiAngularCubemap is YouTube's 360° format: a 3x2 grid of cube faces.
enum class Projection { Flat, Equirect180, Equirect360, Fisheye190, Fisheye200, EquiAngularCubemap,
                        kCount };

// How the two eye images are packed into one video frame.
enum class StereoLayout { Mono, SideBySide, TopBottom, kCount };

struct VideoFormat {
  Projection projection = Projection::Flat;
  StereoLayout stereo = StereoLayout::Mono;
};

const char* projectionName(Projection p);
const char* stereoName(StereoLayout s);
Projection nextProjection(Projection p);
StereoLayout nextStereo(StereoLayout s);

// Guesses the format from common tags in the file name (e.g. "_180_SBS",
// "_360_TB", "FISHEYE190"), falling back to typical VR frame sizes.
VideoFormat guessVideoFormat(const std::string& path, int width, int height);

// Format of a YouTube stream known to be 360° (from the 360° search filter),
// guessed from the frame shape: cubemap or equirectangular, mono or 3D.
VideoFormat guess360StreamFormat(int width, int height);
