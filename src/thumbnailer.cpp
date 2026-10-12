#include "thumbnailer.h"

#include "file_util.h"
#include "video_format.h"

#include <cairo.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>

namespace {

constexpr size_t kMaxThumbnails = 600;  // ~50 MB; beyond that unwanted ones are dropped
constexpr int kDecodeSize = 640;        // frames are shrunk to fit this square first

}  // namespace

// swscale may write past the end of a row (it works in SIMD blocks), so it
// gets padded rows; the result is then copied into tightly packed pixels.
bool scaleInto(SwsContext* sws, const AVFrame* frame, int width, int height,
               std::vector<uint32_t>& pixels) {
  const int stride = (width * 4 + 63) & ~63;
  std::vector<uint8_t> padded(static_cast<size_t>(stride) * (height + 2) + 256);
  uint8_t* planes[4] = {padded.data(), nullptr, nullptr, nullptr};
  int strides[4] = {stride, 0, 0, 0};
  if (sws_scale(sws, frame->data, frame->linesize, 0, frame->height, planes, strides) <= 0)
    return false;
  pixels.resize(static_cast<size_t>(width) * height);
  for (int y = 0; y < height; ++y)
    std::memcpy(&pixels[static_cast<size_t>(y) * width], padded.data() + static_cast<size_t>(y) * stride,
                static_cast<size_t>(width) * 4);
  return true;
}

namespace {

// Picks the GPU (CUDA) output format when the decoder offers it.
AVPixelFormat pickCuda(AVCodecContext*, const AVPixelFormat* formats) {
  for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f)
    if (*f == AV_PIX_FMT_CUDA) return *f;
  return formats[0];
}

// Decodes one frame (20% into a video, the picture of a photo) and shrinks
// it. With `gpu` (a CUDA device), videos are decoded on the graphics card.
bool decodeFrame(const std::string& path, AVBufferRef* gpu, int& width, int& height,
                 int& videoWidth, int& videoHeight, std::vector<uint32_t>& pixels) {
  AVFormatContext* format = nullptr;
  if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) return false;
  AVCodecContext* codec = nullptr;
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  AVFrame* downloaded = av_frame_alloc();
  bool ok = false;
  do {
    if (avformat_find_stream_info(format, nullptr) < 0) break;
    const AVCodec* decoder = nullptr;
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (stream < 0 || !decoder) break;
    codec = avcodec_alloc_context3(decoder);
    if (!codec || avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) < 0)
      break;
    if (gpu) {
      codec->hw_device_ctx = av_buffer_ref(gpu);
      codec->get_format = pickCuda;
    } else {
      // On the CPU: a quicker, slightly rougher decode is fine for a thumbnail.
      codec->thread_count = 0;
      codec->skip_loop_filter = AVDISCARD_ALL;
      codec->flags2 |= AV_CODEC_FLAG2_FAST;
    }
    if (avcodec_open2(codec, decoder, nullptr) < 0) break;
    if (!isPhotoFile(path) && format->duration > 0)
      av_seek_frame(format, -1, format->duration / 5, AVSEEK_FLAG_BACKWARD);

    bool got = false, ended = false;
    for (int reads = 0; !got && reads < 3000; ++reads) {
      if (!ended && av_read_frame(format, packet) < 0) {
        ended = true;
        avcodec_send_packet(codec, nullptr);  // drain what the decoder holds
      } else if (!ended) {
        if (packet->stream_index == stream) avcodec_send_packet(codec, packet);
        av_packet_unref(packet);
      }
      got = avcodec_receive_frame(codec, frame) == 0;
      if (ended && !got) break;
    }
    if (!got) break;
    if (frame->format == AV_PIX_FMT_CUDA) {  // copy the picture from the GPU
      if (av_hwframe_transfer_data(downloaded, frame, 0) < 0) break;
      std::swap(frame, downloaded);
    }

    videoWidth = frame->width;
    videoHeight = frame->height;
    const double scale = std::min({1.0, double(kDecodeSize) / frame->width,
                                   double(kDecodeSize) / frame->height});
    width = std::max(2, static_cast<int>(frame->width * scale));
    height = std::max(2, static_cast<int>(frame->height * scale));
    SwsContext* sws = sws_getContext(frame->width, frame->height,
                                     static_cast<AVPixelFormat>(frame->format), width, height,
                                     AV_PIX_FMT_BGRA, SWS_AREA, nullptr, nullptr, nullptr);
    if (!sws) break;
    ok = scaleInto(sws, frame, width, height, pixels);
    sws_freeContext(sws);
  } while (false);
  av_frame_free(&frame);
  av_frame_free(&downloaded);
  av_packet_free(&packet);
  avcodec_free_context(&codec);
  avformat_close_input(&format);
  return ok;
}

}  // namespace

Thumbnailer::~Thumbnailer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
  for (auto& [path, surface] : done_)
    if (surface) cairo_surface_destroy(surface);
}

void Thumbnailer::start() {
  av_log_set_level(AV_LOG_QUIET);  // FFmpeg messages could name files
  worker_ = std::thread(&Thumbnailer::work, this);
}

cairo_surface_t* Thumbnailer::find(const std::string& path) const {
  const auto it = done_.find(path);
  return it != done_.end() ? it->second : nullptr;
}

void Thumbnailer::want(const std::vector<std::string>& paths) {
  wanted_ = std::set<std::string>(paths.begin(), paths.end());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    for (const std::string& p : paths)
      if (p != working_ && !done_.count(p)) queue_.push_back(p);
  }
  wake_.notify_one();
}

void Thumbnailer::work() {
  // Videos decode on the graphics card where possible (NVDEC through CUDA).
  AVBufferRef* gpu = nullptr;
  if (av_hwdevice_ctx_create(&gpu, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) gpu = nullptr;
  while (true) {
    std::string path;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return quit_ || !queue_.empty(); });
      if (quit_) {
        av_buffer_unref(&gpu);
        return;
      }
      path = queue_.front();
      queue_.pop_front();
      working_ = path;
    }
    Frame frame;
    frame.path = path;
    // Photos on the CPU; videos on the GPU, else (e.g. H.264 wider than
    // NVDEC handles) on the CPU.
    const bool photo = isPhotoFile(path);
    frame.ok = !photo && gpu && decodeFrame(path, gpu, frame.width, frame.height,
                                            frame.videoWidth, frame.videoHeight, frame.pixels);
    if (!frame.ok)
      frame.ok = decodeFrame(path, nullptr, frame.width, frame.height, frame.videoWidth,
                             frame.videoHeight, frame.pixels);
    std::lock_guard<std::mutex> lock(mutex_);
    working_.clear();
    finished_.push_back(std::move(frame));
  }
}

bool Thumbnailer::update() {
  std::vector<Frame> frames;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    frames.swap(finished_);
  }
  if (frames.empty()) return false;
  if (done_.size() + frames.size() > kMaxThumbnails) {
    // Make room: drop the thumbnails that are not wanted at the moment.
    for (auto it = done_.begin(); it != done_.end();) {
      if (wanted_.count(it->first)) {
        ++it;
        continue;
      }
      if (it->second) cairo_surface_destroy(it->second);
      it = done_.erase(it);
    }
  }
  for (const Frame& frame : frames) done_[frame.path] = frame.ok ? makeThumbnail(frame) : nullptr;
  return true;
}

// One eye of a 3D video (guessed like the player does from name and size),
// scaled to fill the 16:9 thumbnail.
cairo_surface_t* Thumbnailer::makeThumbnail(const Frame& frame) const {
  const VideoFormat format = guessVideoFormat(frame.path, frame.videoWidth, frame.videoHeight);
  int cw = frame.width, ch = frame.height;
  if (format.stereo == StereoLayout::SideBySide) cw /= 2;
  if (format.stereo == StereoLayout::TopBottom) ch /= 2;
  // Left (or top) eye: the same pixels with a narrower (shorter) window.
  cairo_surface_t* eye = cairo_image_surface_create_for_data(
      reinterpret_cast<unsigned char*>(const_cast<uint32_t*>(frame.pixels.data())),
      CAIRO_FORMAT_RGB24, cw, ch, frame.width * 4);
  cairo_surface_t* thumb = cairo_image_surface_create(CAIRO_FORMAT_RGB24, kWidth, kHeight);
  cairo_t* cr = cairo_create(thumb);
  const double scale = std::max(double(kWidth) / cw, double(kHeight) / ch);
  cairo_translate(cr, (kWidth - cw * scale) / 2, (kHeight - ch * scale) / 2);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, eye, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(eye);
  return thumb;
}
