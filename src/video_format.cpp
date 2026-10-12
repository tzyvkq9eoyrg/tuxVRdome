#include "video_format.h"

#include "file_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>

namespace {

bool containsAny(const std::string& s, std::initializer_list<const char*> tags) {
  for (const char* tag : tags)
    if (s.find(tag) != std::string::npos) return true;
  return false;
}

}  // namespace

const char* projectionName(Projection p) {
  switch (p) {
    case Projection::Flat: return "flat screen";
    case Projection::Equirect180: return "180";
    case Projection::Equirect360: return "360";
    case Projection::Fisheye190: return "fisheye 190";
    case Projection::Fisheye200: return "fisheye 200";
    case Projection::EquiAngularCubemap: return "360 cubemap (EAC)";
    default: return "?";
  }
}

const char* stereoName(StereoLayout s) {
  switch (s) {
    case StereoLayout::Mono: return "mono";
    case StereoLayout::SideBySide: return "3D side-by-side";
    case StereoLayout::TopBottom: return "3D top-bottom";
    default: return "?";
  }
}

Projection nextProjection(Projection p) {
  return static_cast<Projection>((static_cast<int>(p) + 1) % static_cast<int>(Projection::kCount));
}

StereoLayout nextStereo(StereoLayout s) {
  return static_cast<StereoLayout>((static_cast<int>(s) + 1) %
                                   static_cast<int>(StereoLayout::kCount));
}

VideoFormat guessVideoFormat(const std::string& path, int width, int height) {
  std::string name = path.substr(path.find_last_of('/') + 1);
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  VideoFormat f;
  bool projectionTagged = true;
  if (containsAny(name, {"_eac", "-eac", ".eac"})) {
    f.projection = Projection::EquiAngularCubemap;
  } else if (containsAny(name, {"mkx200", "fisheye200", "fisheye_200"})) {
    f.projection = Projection::Fisheye200;
  } else if (containsAny(name, {"fisheye"})) {
    f.projection = Projection::Fisheye190;
  } else if (containsAny(name, {"360"})) {
    f.projection = Projection::Equirect360;
  } else if (containsAny(name, {"180"})) {
    f.projection = Projection::Equirect180;
  } else {
    projectionTagged = false;
  }

  bool stereoTagged = true;
  if (containsAny(name, {"_tb", "-tb", "_ou", "overunder", "over_under", "topbottom", "top_bottom", "3dv"})) {
    f.stereo = StereoLayout::TopBottom;
  } else if (containsAny(name, {"sbs", "_lr", "-lr", "3dh", "side_by_side", "sidebyside"})) {
    f.stereo = StereoLayout::SideBySide;
  } else {
    stereoTagged = false;
  }

  // Untagged files: big 2:1 videos are almost always VR180 side-by-side,
  // big 2:1 photos 360° panoramas (as 360° cameras and phones save them),
  // big 1:1 frames 360 top-bottom.
  if (!projectionTagged && !stereoTagged && width >= 3840 && height > 0) {
    const double aspect = static_cast<double>(width) / height;
    if (std::abs(aspect - 2.0) < 0.02 && isPhotoFile(path)) {
      f.projection = Projection::Equirect360;
    } else if (std::abs(aspect - 2.0) < 0.02) {
      f.projection = Projection::Equirect180;
      f.stereo = StereoLayout::SideBySide;
    } else if (std::abs(aspect - 1.0) < 0.02) {
      f.projection = Projection::Equirect360;
      f.stereo = StereoLayout::TopBottom;
    }
  }
  return f;
}

VideoFormat guess360StreamFormat(int width, int height) {
  // YouTube mostly serves 360° video as cubemap with non-square faces, so the
  // frame shape only hints at the layout. Seen so far: 7680x3840 mono
  // cubemap, 3840x2160 stereo with one turned cubemap per eye side by side,
  // and 3840x2048 equirectangular. Taller frames hold both eyes stacked.
  VideoFormat f;
  f.projection = Projection::EquiAngularCubemap;
  if (width <= 0 || height <= 0) return f;
  const double aspect = static_cast<double>(width) / height;
  if (aspect < 1.3) {
    f.stereo = StereoLayout::TopBottom;
  } else if (std::abs(aspect - 16.0 / 9.0) < 0.05) {
    f.stereo = StereoLayout::SideBySide;
  } else if (aspect > 1.7 && std::abs(aspect - 2.0) > 0.02) {
    f.projection = Projection::Equirect360;
  }
  return f;
}
