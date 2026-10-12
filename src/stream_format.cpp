#include "stream_format.h"

VideoFormat youTubeFormat(YouTubeVideo::Kind kind, const std::vector<StreamFormatInfo>& formats,
                          const std::string& url, int width, int height,
                          const FrameAnalysis* picture, std::string& source) {
  std::string note;
  for (const StreamFormatInfo& f : formats)
    if (f.width == width && f.height == height) note += f.note + " ";
  const bool mesh = note.find("mesh") != std::string::npos;
  const bool equi = note.find("equi") != std::string::npos;
  const bool seen = picture && picture->valid;
  const double aspect = height > 0 ? static_cast<double>(width) / height : 2.0;

  VideoFormat f;
  if (kind == YouTubeVideo::Kind::VR180) {
    f = {Projection::Equirect180, StereoLayout::SideBySide};
    source = "VR180 search filter";
  } else if (mesh || equi) {
    f.projection = mesh ? Projection::EquiAngularCubemap : Projection::Equirect360;
    source = mesh ? "YouTube metadata: cubemap" : "YouTube metadata: equirectangular";
    if (!seen && equi && aspect < 1.3) f.stereo = StereoLayout::TopBottom;
    if (!seen && mesh) f.stereo = guess360StreamFormat(width, height).stereo;
  } else if (kind == YouTubeVideo::Kind::Video360) {
    if (seen) {
      f.projection = picture->cubemap ? Projection::EquiAngularCubemap : Projection::Equirect360;
      source = "picture";
    } else {
      f = guess360StreamFormat(width, height);
      source = "frame shape";
    }
  } else {
    f = guessVideoFormat(url, width, height);  // an ordinary video
    source = "not 360";
  }
  if (seen && kind != YouTubeVideo::Kind::VR180) {
    f.stereo = picture->stereo;
    source += ", 3D from the picture";
  } else if (seen && picture->stereo != StereoLayout::Mono) {
    f.stereo = picture->stereo;  // VR180 is 3D; the picture says how it is packed
  }
  return f;
}
