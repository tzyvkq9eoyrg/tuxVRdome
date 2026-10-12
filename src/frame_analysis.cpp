#include "frame_analysis.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Halves that differ by less than this share of the picture's contrast are
// the two eyes of a 3D video.
constexpr double kSameHalves = 0.45;
// A row (or column) step this many times larger than the usual one, in most
// places along it, is a seam between cube faces.
constexpr double kSeam = 3.0;

double median(std::vector<double> values) {
  if (values.empty()) return 0;
  const size_t mid = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + mid, values.end());
  return values[mid];
}

}  // namespace

FrameAnalysis analyzeFrame(GLuint texture, int width, int height, int level) {
  FrameAnalysis a;
  const int w = std::max(1, width >> level), h = std::max(1, height >> level);
  if (!texture || w < 32 || h < 32) return a;
  std::vector<float> rgba(static_cast<size_t>(w) * h * 4);
  glBindTexture(GL_TEXTURE_2D, texture);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_FLOAT, rgba.data());
  glBindTexture(GL_TEXTURE_2D, 0);

  std::vector<double> lum(static_cast<size_t>(w) * h);
  double mean = 0;
  for (size_t i = 0; i < lum.size(); ++i) {
    lum[i] = 0.2126 * rgba[i * 4] + 0.7152 * rgba[i * 4 + 1] + 0.0722 * rgba[i * 4 + 2];
    mean += lum[i];
  }
  mean /= lum.size();
  double contrast = 0;
  for (double l : lum) contrast += std::abs(l - mean);
  contrast /= lum.size();
  if (contrast < 0.01) return a;  // a dark or flat frame (e.g. a fade): ask again later
  auto at = [&](int x, int y) { return lum[static_cast<size_t>(y) * w + x]; };

  // Left half against right half, top half against bottom half.
  double lr = 0, tb = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w / 2; ++x) lr += std::abs(at(x, y) - at(x + w / 2, y));
  for (int y = 0; y < h / 2; ++y)
    for (int x = 0; x < w; ++x) tb += std::abs(at(x, y) - at(x, y + h / 2));
  a.sideBySide = lr / (h * (w / 2)) / contrast;
  a.topBottom = tb / ((h / 2) * w) / contrast;
  if (a.sideBySide < kSameHalves && a.sideBySide <= a.topBottom)
    a.stereo = StereoLayout::SideBySide;
  else if (a.topBottom < kSameHalves)
    a.stereo = StereoLayout::TopBottom;

  // The first eye: a seam across its middle (between the two rows of cube
  // faces), or down its middle if the cube is turned.
  const int ew = a.stereo == StereoLayout::SideBySide ? w / 2 : w;
  const int eh = a.stereo == StereoLayout::TopBottom ? h / 2 : h;
  const bool rows = ew >= eh;
  const int length = rows ? ew : eh;     // along the seam
  const int across = rows ? eh : ew;     // steps across it
  const int mid = across / 2;
  auto step = [&](int i, int k) {        // step k-1 -> k at position i along the seam
    return rows ? std::abs(at(i, k) - at(i, k - 1)) : std::abs(at(k, i) - at(k - 1, i));
  };
  std::vector<double> seamSteps, usualSteps;
  for (int i = 0; i < length; ++i) seamSteps.push_back(step(i, mid));
  for (int k = 1; k < across; ++k) {
    if (std::abs(k - mid) <= 2) continue;
    double sum = 0;
    for (int i = 0; i < length; ++i) sum += step(i, k);
    usualSteps.push_back(sum / length);
  }
  a.seam = median(seamSteps) / std::max(1e-4, median(usualSteps));
  a.cubemap = a.seam > kSeam;
  a.valid = true;
  return a;
}
