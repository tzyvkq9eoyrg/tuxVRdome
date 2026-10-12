// Checks for the parts of the player that decide things without the headset:
// format detection, YouTube rules and links, file sorting. Run by build.sh;
// prints each failure and exits with 1 if there was any.

#include "../src/file_util.h"
#include "../src/stream_format.h"
#include "../src/video_format.h"
#include "../src/youtube.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

#define CHECK(condition)                                                  \
  do {                                                                    \
    if (!(condition)) {                                                   \
      std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
      ++failures;                                                         \
    }                                                                     \
  } while (false)

bool is(const VideoFormat& f, Projection p, StereoLayout s) {
  return f.projection == p && f.stereo == s;
}

void fileNames() {
  CHECK(is(guessVideoFormat("/v/trip_180_SBS.mp4", 0, 0), Projection::Equirect180,
           StereoLayout::SideBySide));
  CHECK(is(guessVideoFormat("/v/city_360_TB.mkv", 0, 0), Projection::Equirect360,
           StereoLayout::TopBottom));
  CHECK(is(guessVideoFormat("/v/a_FISHEYE190.mp4", 0, 0), Projection::Fisheye190,
           StereoLayout::Mono));
  CHECK(is(guessVideoFormat("/v/b_MKX200.mp4", 0, 0), Projection::Fisheye200, StereoLayout::Mono));
  CHECK(is(guessVideoFormat("/v/c_EAC.mp4", 0, 0), Projection::EquiAngularCubemap,
           StereoLayout::Mono));
  CHECK(is(guessVideoFormat("/v/movie.mp4", 1920, 1080), Projection::Flat, StereoLayout::Mono));
}

void frameSizes() {
  // Untagged big frames: 2:1 video = VR180 3D, 2:1 photo = 360° panorama, 1:1 = 360° 3D.
  CHECK(is(guessVideoFormat("/v/clip.mp4", 7680, 3840), Projection::Equirect180,
           StereoLayout::SideBySide));
  CHECK(is(guessVideoFormat("/v/pano.jpg", 7680, 3840), Projection::Equirect360,
           StereoLayout::Mono));
  CHECK(is(guessVideoFormat("/v/clip.mp4", 4096, 4096), Projection::Equirect360,
           StereoLayout::TopBottom));
}

void youTube360() {
  CHECK(is(guess360StreamFormat(7680, 3840), Projection::EquiAngularCubemap, StereoLayout::Mono));
  CHECK(is(guess360StreamFormat(3840, 2160), Projection::EquiAngularCubemap,
           StereoLayout::SideBySide));
  CHECK(is(guess360StreamFormat(3840, 2048), Projection::Equirect360, StereoLayout::Mono));
  CHECK(guess360StreamFormat(3840, 7680).stereo == StereoLayout::TopBottom);

  std::string source;
  const std::vector<StreamFormatInfo> mesh = {{3840, 2160, "2160p, mesh"}};
  const std::vector<StreamFormatInfo> equi = {{3840, 1920, "1920p, equi"}};
  // VR180 is "mesh" on YouTube too: the search filter wins.
  CHECK(is(youTubeFormat(YouTubeVideo::Kind::VR180, mesh, "", 3840, 2160, nullptr, source),
           Projection::Equirect180, StereoLayout::SideBySide));
  CHECK(youTubeFormat(YouTubeVideo::Kind::Video360, mesh, "", 3840, 2160, nullptr, source)
            .projection == Projection::EquiAngularCubemap);
  CHECK(youTubeFormat(YouTubeVideo::Kind::Video360, equi, "", 3840, 1920, nullptr, source)
            .projection == Projection::Equirect360);
  // What the picture says about 3D overrides the frame shape.
  FrameAnalysis mono;
  mono.valid = true;
  mono.cubemap = true;
  CHECK(is(youTubeFormat(YouTubeVideo::Kind::Video360, {}, "", 3840, 2160, &mono, source),
           Projection::EquiAngularCubemap, StereoLayout::Mono));
}

void links() {
  CHECK(isYouTubeVideoLink("https://youtu.be/abc123?t=5"));
  CHECK(youTubeVideoUrl("https://youtu.be/abc123?t=5") == "https://www.youtube.com/watch?v=abc123");
  CHECK(youTubeVideoUrl("https://www.youtube.com/watch?v=XYZ&list=PL1") ==
        "https://www.youtube.com/watch?v=XYZ");
  CHECK(!isYouTubeVideoLink("https://www.youtube.com/playlist?list=PL1"));
  CHECK(isYouTubeLink("https://www.youtube.com/@channel"));
  CHECK(!isYouTubeLink("meine suche"));
}

void files() {
  CHECK(isVideoFile("/x/a.MKV"));
  CHECK(isPhotoFile("/x/b.jpeg"));
  CHECK(!isPhotoFile("/x/c.mp4"));
  CHECK(fileNameLess("apple.mp4", "Banana.mp4"));
  CHECK(!fileNameLess("banana.mp4", "Apple.mp4"));
}

}  // namespace

int main() {
  fileNames();
  frameSizes();
  youTube360();
  links();
  files();
  if (failures == 0) std::printf("TESTS OK\n");
  return failures == 0 ? 0 : 1;
}
