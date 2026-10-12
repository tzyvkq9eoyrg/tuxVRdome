#include "photo_preloader.h"

#include "thumbnailer.h"  // scaleInto

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>

namespace {

constexpr int kMaxSize = 16384;               // the GL texture limit
constexpr size_t kUploadBytes = 8u << 20;     // per frame

// Turns an RGBA picture clockwise by 90, 180 or 270 degrees.
void rotate(std::vector<uint32_t>& pixels, int& width, int& height, int degrees) {
  if (degrees != 90 && degrees != 180 && degrees != 270) return;
  std::vector<uint32_t> out(pixels.size());
  const int w = width, h = height;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint32_t p = pixels[static_cast<size_t>(y) * w + x];
      if (degrees == 180) out[static_cast<size_t>(h - 1 - y) * w + (w - 1 - x)] = p;
      else if (degrees == 90) out[static_cast<size_t>(x) * h + (h - 1 - y)] = p;
      else out[static_cast<size_t>(w - 1 - x) * h + y] = p;
    }
  pixels.swap(out);
  if (degrees != 180) std::swap(width, height);
}

// Decodes a photo at full size (at most kMaxSize) into RGBA, upright.
bool decodePhoto(const std::string& path, std::vector<uint32_t>& pixels, int& width, int& height) {
  AVFormatContext* format = nullptr;
  if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) return false;
  AVCodecContext* codec = nullptr;
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool ok = false;
  do {
    if (avformat_find_stream_info(format, nullptr) < 0) break;
    const AVCodec* decoder = nullptr;
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (stream < 0 || !decoder) break;
    codec = avcodec_alloc_context3(decoder);
    if (!codec || avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) < 0)
      break;
    codec->thread_count = 0;
    if (avcodec_open2(codec, decoder, nullptr) < 0) break;
    bool got = false, ended = false;
    for (int reads = 0; !got && reads < 100; ++reads) {
      if (!ended && av_read_frame(format, packet) < 0) {
        ended = true;
        avcodec_send_packet(codec, nullptr);
      } else if (!ended) {
        if (packet->stream_index == stream) avcodec_send_packet(codec, packet);
        av_packet_unref(packet);
      }
      got = avcodec_receive_frame(codec, frame) == 0;
      if (ended && !got) break;
    }
    if (!got) break;

    const double scale =
        std::min({1.0, double(kMaxSize) / frame->width, double(kMaxSize) / frame->height});
    width = std::max(1, static_cast<int>(frame->width * scale));
    height = std::max(1, static_cast<int>(frame->height * scale));
    SwsContext* sws = sws_getContext(frame->width, frame->height,
                                     static_cast<AVPixelFormat>(frame->format), width, height,
                                     AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!sws) break;
    const bool scaled = scaleInto(sws, frame, width, height, pixels);
    sws_freeContext(sws);
    if (!scaled) break;
    // Camera orientation (EXIF), as FFmpeg reports it.
    if (const AVFrameSideData* side = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX)) {
      const double angle = av_display_rotation_get(reinterpret_cast<const int32_t*>(side->data));
      if (!std::isnan(angle)) {
        const int clockwise = ((static_cast<int>(std::lround(-angle)) % 360) + 360) % 360;
        rotate(pixels, width, height, clockwise);
      }
    }
    ok = true;
  } while (false);
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&codec);
  avformat_close_input(&format);
  return ok;
}

}  // namespace

PhotoPreloader::~PhotoPreloader() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
  for (Slot& s : slots_)
    if (s.texture) glDeleteTextures(1, &s.texture);
}

void PhotoPreloader::start() { worker_ = std::thread(&PhotoPreloader::work, this); }

void PhotoPreloader::request(const std::string& current, const std::string& next) {
  current_ = current;
  next_ = next;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    wanted_.clear();
    for (const std::string& path : {current, next}) {
      if (path.empty()) continue;
      bool known = decoding_ == path || (done_ && decoded_.path == path);
      for (const Slot& s : slots_) known = known || s.path == path;  // there, or on its way up
      if (!known) wanted_.push_back(path);
    }
  }
  wake_.notify_one();
}

void PhotoPreloader::work() {
  while (true) {
    std::string path;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return quit_ || !wanted_.empty(); });
      if (quit_) return;
      path = wanted_.front();
      wanted_.pop_front();
      decoding_ = path;
    }
    Slot slot;
    slot.path = path;
    const bool ok = decodePhoto(path, slot.pixels, slot.width, slot.height);
    std::unique_lock<std::mutex> lock(mutex_);
    if (ok) {
      // One finished photo waits at a time, until the GL thread takes it.
      wake_.wait(lock, [this] { return quit_ || !done_; });
      if (quit_) return;
      decoded_ = std::move(slot);
      done_ = true;
    }
    decoding_.clear();
  }
}

void PhotoPreloader::update() {
  Slot incoming;
  bool have = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (done_) {
      incoming = std::move(decoded_);
      done_ = false;
      have = true;
    }
  }
  if (have) {
    wake_.notify_all();  // the worker may wait to hand over the next one
    // Into a slot holding neither the current nor the next photo (else the
    // one not on screen).
    int index = shown_ == 0 ? 1 : 0;
    for (int i = 0; i < 2; ++i)
      if (slots_[i].path != current_ && slots_[i].path != next_) index = i;
    Slot& s = slots_[index];
    if (s.texture) glDeleteTextures(1, &s.texture);
    s = std::move(incoming);
    const int levels = 1 + static_cast<int>(std::floor(std::log2(std::max(s.width, s.height))));
    glGenTextures(1, &s.texture);
    glBindTexture(GL_TEXTURE_2D, s.texture);
    glTexStorage2D(GL_TEXTURE_2D, levels, GL_SRGB8_ALPHA8, s.width, s.height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 16.0f);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  // A few megabytes per frame, so a 50 MP photo goes up over a few frames.
  for (Slot& s : slots_) {
    if (s.ready || s.pixels.empty()) continue;
    const int rows = std::max(1, static_cast<int>(kUploadBytes / (static_cast<size_t>(s.width) * 4)));
    const int count = std::min(rows, s.height - s.uploadedRows);
    glBindTexture(GL_TEXTURE_2D, s.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, s.uploadedRows, s.width, count, GL_RGBA, GL_UNSIGNED_BYTE,
                    s.pixels.data() + static_cast<size_t>(s.uploadedRows) * s.width);
    s.uploadedRows += count;
    if (s.uploadedRows >= s.height) {
      glGenerateMipmap(GL_TEXTURE_2D);
      s.pixels.clear();
      s.pixels.shrink_to_fit();
      s.ready = true;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    break;  // one slot per frame
  }
}

GLuint PhotoPreloader::texture(const std::string& path, int& width, int& height) {
  for (int i = 0; i < 2; ++i) {
    if (!slots_[i].ready || slots_[i].path != path) continue;
    shown_ = i;
    width = slots_[i].width;
    height = slots_[i].height;
    return slots_[i].texture;
  }
  return 0;
}
