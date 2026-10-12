#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct YouTubeVideo {
  enum class Kind { Video360, VR180, Other };
  std::string id;
  std::string title;
  std::string channel;
  double duration = 0;  // seconds, 0 for live streams
  Kind kind = Kind::Other;
  // Premultiplied ARGB32 thumbnail, null until downloaded. Shared, so copying
  // result lists between threads does not copy pixels.
  int thumbnailWidth = 0, thumbnailHeight = 0;
  std::shared_ptr<const std::vector<uint32_t>> thumbnail;

  std::string url() const { return "https://www.youtube.com/watch?v=" + id; }
};

// The installed yt-dlp's version, or an empty string if it cannot be run.
std::string ytDlpVersion();

// True for YouTube links; video links play directly, playlist and channel
// links are listed like search results.
bool isYouTubeLink(const std::string& text);
bool isYouTubeVideoLink(const std::string& text);
// The plain watch URL of a video link (without playlist parameters).
std::string youTubeVideoUrl(const std::string& link);

// One video format YouTube offers for a video, with yt-dlp's format note,
// which can name the 360° projection ("mesh" = cubemap, "equi" =
// equirectangular).
struct StreamFormatInfo {
  int width = 0, height = 0;
  std::string note;  // lower case
};
// Asks yt-dlp for the formats of a video. Blocks for a few seconds.
std::vector<StreamFormatInfo> youTubeFormats(const std::string& url);

// Searches YouTube through yt-dlp in a background thread (no API key needed),
// then downloads the thumbnails. With vrOnly, YouTube's own 360° and VR180
// search filters are used and both result lists are interleaved.
class YouTubeSearch {
 public:
  ~YouTubeSearch();

  void start(const std::string& query, bool vrOnly);
  // Lists the videos of a playlist or channel link.
  void openLink(const std::string& url);
  // Appends the next page of results to the last search or link.
  void loadMore();
  bool canLoadMore() const { return !busy_ && !exhausted_ && !request_.query.empty(); }
  bool busy() const { return busy_; }
  // The results are a playlist's or channel's videos, not search results.
  bool listingLink() const { return request_.link; }
  // Copies results and error if anything changed since the last call.
  bool poll(std::vector<YouTubeVideo>& results, std::string& error);

 private:
  struct Request {
    std::string query;  // search words, or the link
    bool link = false;
    bool vrOnly = false;
    int start = 1;      // first result, counted from 1
  };
  void launch(const Request& request, std::vector<YouTubeVideo> earlier);
  void run(Request request, std::vector<YouTubeVideo> results, int searchId);
  void publish(int searchId, const std::vector<YouTubeVideo>& results, const std::string& error);

  std::mutex mutex_;
  std::vector<std::thread> workers_;  // joined on destruction
  std::vector<YouTubeVideo> results_;
  std::string error_;
  int generation_ = 0;  // bumped on every published change
  int seenGeneration_ = 0;
  std::atomic<int> searchId_{0};  // newer searches make older workers stop
  std::atomic<bool> busy_{false};
  std::atomic<bool> exhausted_{false};  // the last page came back short
  Request request_;                     // the last search, for loadMore()
};
