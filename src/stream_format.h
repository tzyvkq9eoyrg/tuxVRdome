#pragma once

#include "frame_analysis.h"
#include "video_format.h"
#include "youtube.h"

#include <string>
#include <vector>

// Format of a YouTube video. The projection: VR180 from the search filter
// (YouTube stores VR180 as a "mesh" too), else YouTube's own metadata for the
// playing format ("mesh" = cubemap, "equi" = equirectangular), else a seam
// found in the picture. The 3D layout: from the picture (two nearly equal
// halves). Without a look at the picture yet (null), the frame shape
// decides. `source` says what decided, for the log.
VideoFormat youTubeFormat(YouTubeVideo::Kind kind, const std::vector<StreamFormatInfo>& formats,
                          const std::string& url, int width, int height,
                          const FrameAnalysis* picture, std::string& source);
