#include "youtube.h"

#include <curl/curl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

extern char** environ;

namespace {

constexpr int kResultsPerSearch = 20;
// YouTube's search filter parameter "sp": a base64 protobuf whose field 2
// holds the filters. All of ours include type = video (field 2 = 1), plus
// 360° (field 15 = 1) or VR180 (field 26 = 1).
constexpr const char* kFilterVideos = "EgIQAQ%3D%3D";
constexpr const char* kFilter360 = "EgQQAXgB";
constexpr const char* kFilterVR180 = "EgUQAdABAQ%3D%3D";

std::string urlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

// Runs a program without a shell and returns its stdout. stderr is dropped
// so nothing about the system ends up in our logs.
bool runProgram(const std::vector<std::string>& args, std::string& output) {
  int fds[2];
  if (pipe(fds) != 0) return false;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  posix_spawn_file_actions_addclose(&actions, fds[1]);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

  std::vector<char*> argv;
  for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  if (rc != 0) {
    close(fds[0]);
    return false;
  }
  char buf[65536];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof(buf))) > 0) output.append(buf, static_cast<size_t>(n));
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string jsonString(const nlohmann::json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Lists one page of entries of a YouTube page (search results, playlist or
// channel), starting at entry `start` (counted from 1). count is the number
// of entries the page had, to tell whether more may follow.
bool listEntries(const std::string& url, int start, YouTubeVideo::Kind kind,
                 std::vector<YouTubeVideo>& out, int& count) {
  count = 0;
  std::string json;
  if (!runProgram({"yt-dlp", "--flat-playlist", "-J", "--no-warnings", "--playlist-start",
                   std::to_string(start), "--playlist-end",
                   std::to_string(start + kResultsPerSearch - 1), url},
                  json))
    return false;
  const nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
  if (root.is_discarded() || !root.contains("entries") || !root["entries"].is_array()) return false;
  for (const auto& entry : root["entries"]) {
    ++count;
    // Channel pages can also list playlists or tabs; only videos play.
    const std::string ieKey = jsonString(entry, "ie_key");
    if (!ieKey.empty() && ieKey != "Youtube") continue;
    YouTubeVideo v;
    v.id = jsonString(entry, "id");
    if (v.id.empty()) continue;
    v.title = jsonString(entry, "title");
    v.channel = jsonString(entry, "channel");
    if (v.channel.empty()) v.channel = jsonString(entry, "uploader");
    if (entry.contains("duration") && entry["duration"].is_number())
      v.duration = entry["duration"].get<double>();
    v.kind = kind;
    out.push_back(v);
  }
  return true;
}

std::string searchUrl(const std::string& query, const char* filter) {
  return "https://www.youtube.com/results?search_query=" + urlEncode(query) + "&sp=" + filter;
}

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// The value of a URL query parameter such as "v" or "list".
std::string queryParam(const std::string& url, const std::string& name) {
  for (const char* sep : {"?", "&"}) {
    const size_t pos = url.find(sep + name + "=");
    if (pos == std::string::npos) continue;
    const size_t start = pos + name.size() + 2;
    return url.substr(start, url.find_first_of("&#", start) - start);
  }
  return {};
}

// A channel link without a tab lists the channel's tabs; ask for its videos.
std::string channelVideos(std::string url) {
  const bool channel = contains(url, "/@") || contains(url, "/channel/") || contains(url, "/c/") ||
                       contains(url, "/user/");
  if (!channel) return url;
  for (const char* tab : {"/videos", "/shorts", "/streams", "/playlists", "/featured"})
    if (contains(url, tab)) return url;
  url = url.substr(0, url.find_first_of("?#"));
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url + "/videos";
}

// The video id of a video link, or empty.
std::string videoId(const std::string& text) {
  if (!isYouTubeLink(text)) return {};
  const std::string v = queryParam(text, "v");
  if (!v.empty()) return v;
  for (const char* marker : {"youtu.be/", "/shorts/", "/live/"}) {
    const size_t pos = text.find(marker);
    if (pos == std::string::npos) continue;
    const size_t start = pos + std::string(marker).size();
    return text.substr(start, text.find_first_of("?&#/", start) - start);
  }
  return {};
}

size_t appendToString(char* data, size_t size, size_t count, void* target) {
  static_cast<std::string*>(target)->append(data, size * count);
  return size * count;
}

std::string download(const std::string& url) {
  std::string body;
  CURL* curl = curl_easy_init();
  if (!curl) return body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendToString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (curl_easy_perform(curl) != CURLE_OK) body.clear();
  curl_easy_cleanup(curl);
  return body;
}

// Decodes an image (JPEG etc.) into premultiplied ARGB32 for cairo.
bool decodeImage(const std::string& data, YouTubeVideo& v) {
  GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
  const bool ok =
      gdk_pixbuf_loader_write(loader, reinterpret_cast<const guchar*>(data.data()), data.size(),
                              nullptr) &&
      gdk_pixbuf_loader_close(loader, nullptr);
  GdkPixbuf* loaded = ok ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
  if (!loaded) {
    g_object_unref(loader);
    return false;
  }
  // Big enough for the gallery cards; keeps memory low for long result lists.
  constexpr int kMaxWidth = 480;
  const int loadedW = gdk_pixbuf_get_width(loaded), loadedH = gdk_pixbuf_get_height(loaded);
  GdkPixbuf* pixbuf =
      loadedW > kMaxWidth
          ? gdk_pixbuf_scale_simple(loaded, kMaxWidth, loadedH * kMaxWidth / loadedW,
                                    GDK_INTERP_BILINEAR)
          : GDK_PIXBUF(g_object_ref(loaded));
  const int w = gdk_pixbuf_get_width(pixbuf), h = gdk_pixbuf_get_height(pixbuf);
  const int channels = gdk_pixbuf_get_n_channels(pixbuf);
  const int stride = gdk_pixbuf_get_rowstride(pixbuf);
  const bool alpha = gdk_pixbuf_get_has_alpha(pixbuf);
  const guchar* pixels = gdk_pixbuf_get_pixels(pixbuf);
  auto argb = std::make_shared<std::vector<uint32_t>>(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const guchar* p = pixels + y * stride + x * channels;
      const uint32_t a = alpha ? p[3] : 255;
      const uint32_t r = p[0] * a / 255, g = p[1] * a / 255, b = p[2] * a / 255;
      (*argb)[static_cast<size_t>(y) * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  v.thumbnail = std::move(argb);
  v.thumbnailWidth = w;
  v.thumbnailHeight = h;
  g_object_unref(pixbuf);
  g_object_unref(loader);
  return true;
}

}  // namespace

std::string ytDlpVersion() {
  std::string out;
  if (!runProgram({"yt-dlp", "--version"}, out)) return {};
  while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
  return out;
}

bool isYouTubeLink(const std::string& text) {
  return contains(text, "youtube.com/") || contains(text, "youtu.be/");
}

bool isYouTubeVideoLink(const std::string& text) { return !videoId(text).empty(); }

std::string youTubeVideoUrl(const std::string& link) {
  const std::string id = videoId(link);
  return id.empty() ? std::string() : "https://www.youtube.com/watch?v=" + id;
}

std::vector<StreamFormatInfo> youTubeFormats(const std::string& url) {
  std::vector<StreamFormatInfo> formats;
  std::string json;
  if (!runProgram({"yt-dlp", "-J", "--no-playlist", "--no-warnings", url}, json)) return formats;
  const nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
  if (root.is_discarded() || !root.contains("formats") || !root["formats"].is_array())
    return formats;
  for (const auto& f : root["formats"]) {
    if (!f.is_object()) continue;
    StreamFormatInfo info;
    if (f.contains("width") && f["width"].is_number()) info.width = f["width"].get<int>();
    if (f.contains("height") && f["height"].is_number()) info.height = f["height"].get<int>();
    if (info.width <= 0 || info.height <= 0) continue;  // audio only
    info.note = jsonString(f, "format_note") + " " + jsonString(f, "format") + " " +
                jsonString(f, "projection") + " " + jsonString(f, "projection_type");
    std::transform(info.note.begin(), info.note.end(), info.note.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    formats.push_back(info);
  }
  return formats;
}

YouTubeSearch::~YouTubeSearch() {
  ++searchId_;  // tell running workers to stop early
  for (std::thread& t : workers_)
    if (t.joinable()) t.join();
}

void YouTubeSearch::launch(const Request& request, std::vector<YouTubeVideo> earlier) {
  static std::once_flag curlInit;
  std::call_once(curlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  const int id = ++searchId_;
  busy_ = true;
  if (earlier.empty()) publish(id, {}, "");
  workers_.emplace_back(&YouTubeSearch::run, this, request, std::move(earlier), id);
}

void YouTubeSearch::start(const std::string& query, bool vrOnly) {
  request_ = {query, false, vrOnly, 1};
  exhausted_ = false;
  launch(request_, {});
}

void YouTubeSearch::openLink(const std::string& url) {
  std::string target = channelVideos(url);
  const std::string list = queryParam(url, "list");
  if (!list.empty()) target = "https://www.youtube.com/playlist?list=" + list;
  request_ = {target, true, false, 1};
  exhausted_ = false;
  launch(request_, {});
}

void YouTubeSearch::loadMore() {
  if (!canLoadMore()) return;
  request_.start += kResultsPerSearch;
  std::vector<YouTubeVideo> earlier;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    earlier = results_;
  }
  launch(request_, std::move(earlier));
}

void YouTubeSearch::publish(int searchId, const std::vector<YouTubeVideo>& results,
                            const std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (searchId != searchId_) return;  // a newer search took over
  results_ = results;
  error_ = error;
  ++generation_;
}

bool YouTubeSearch::poll(std::vector<YouTubeVideo>& results, std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation_ == seenGeneration_) return false;
  seenGeneration_ = generation_;
  results = results_;
  error = error_;
  return true;
}

void YouTubeSearch::run(Request r, std::vector<YouTubeVideo> results, int searchId) {
  const size_t firstNew = results.size();
  std::set<std::string> seen;
  for (const YouTubeVideo& v : results) seen.insert(v.id);
  auto append = [&](const std::vector<YouTubeVideo>& page) {
    for (const YouTubeVideo& v : page)
      if (seen.insert(v.id).second) results.push_back(v);
  };
  bool ok;
  int count = 0;
  if (r.link) {
    std::vector<YouTubeVideo> page;
    ok = listEntries(r.query, r.start, YouTubeVideo::Kind::Other, page, count);
    append(page);
  } else if (r.vrOnly) {
    std::vector<YouTubeVideo> a, b;
    int countA = 0, countB = 0;
    const bool okA = listEntries(searchUrl(r.query, kFilter360), r.start,
                                 YouTubeVideo::Kind::Video360, a, countA);
    const bool okB = listEntries(searchUrl(r.query, kFilterVR180), r.start,
                                 YouTubeVideo::Kind::VR180, b, countB);
    ok = okA || okB;
    count = std::max(countA, countB);
    // Interleave both lists, skipping videos that appear in both.
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
      for (const auto* list : {&a, &b})
        if (i < list->size()) append({(*list)[i]});
    }
  } else {
    std::vector<YouTubeVideo> page;
    ok = listEntries(searchUrl(r.query, kFilterVideos), r.start, YouTubeVideo::Kind::Other, page,
                     count);
    append(page);
  }
  if (searchId != searchId_) return;
  exhausted_ = count < kResultsPerSearch;
  publish(searchId, results, ok ? "" : "failed");
  busy_ = false;

  // Thumbnails one by one, so the list fills in while they arrive.
  for (size_t i = firstNew; i < results.size(); ++i) {
    if (searchId != searchId_) return;
    YouTubeVideo& v = results[i];
    // The sharper 1280x720 version where YouTube has one, else 320x180.
    std::string data = download("https://i.ytimg.com/vi/" + v.id + "/hq720.jpg");
    if (data.empty()) data = download("https://i.ytimg.com/vi/" + v.id + "/mqdefault.jpg");
    if (!data.empty() && decodeImage(data, v)) publish(searchId, results, "");
  }
}
