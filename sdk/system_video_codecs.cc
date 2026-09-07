#include "system_video_codecs.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "api/video_codecs/video_decoder_software_fallback_wrapper.h"
#include "api/video/i420_buffer.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "third_party/libyuv/include/libyuv.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <glob.h>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace compartilhagram {
using namespace webrtc;
namespace {
struct Entry { std::string direction, mode = "pending", reason = "Waiting for codec initialization", implementation; };
std::mutex status_mutex;
std::map<const void*, Entry> statuses;
struct Status {
  explicit Status(std::string direction) { std::lock_guard<std::mutex> lock(status_mutex); statuses[this].direction = direction; }
  ~Status() { std::lock_guard<std::mutex> lock(status_mutex); statuses.erase(this); }
  void reason(std::string text) { std::lock_guard<std::mutex> lock(status_mutex); statuses[this].reason = std::move(text); }
  void update(bool hardware, std::string implementation) {
    std::lock_guard<std::mutex> lock(status_mutex); auto& s = statuses[this];
    s.mode = hardware ? "hardware" : "software"; s.implementation = std::move(implementation);
    if (hardware) s.reason.clear();
  }
};
std::string error(int code) { char text[AV_ERROR_MAX_STRING_SIZE]; av_strerror(code, text, sizeof(text)); return text; }
std::string quote(const std::string& text) {
  std::string out = "\"";
  for (unsigned char c : text) { if (c == '"' || c == '\\') out += '\\'; if (c >= 32) out += c; }
  return out + '"';
}
AVBufferRef* device(std::string& reason) {
  if (const char* disabled = std::getenv("COMPARTILHAGRAM_DISABLE_GPU"); disabled && std::string(disabled) == "1") {
    reason = "Disabled by COMPARTILHAGRAM_DISABLE_GPU=1"; return nullptr;
  }
  std::vector<std::string> paths;
  if (const char* selected = std::getenv("COMPARTILHAGRAM_VAAPI_DEVICE"); selected && *selected) paths.emplace_back(selected);
  else {
    glob_t matches{};
    if (glob("/dev/dri/renderD*", 0, nullptr, &matches) == 0) {
      for (size_t i = 0; i < matches.gl_pathc; ++i) paths.emplace_back(matches.gl_pathv[i]);
    }
    globfree(&matches);
  }
  reason = "No accessible /dev/dri/renderD* GPU render device";
  for (const auto& path : paths) {
    AVBufferRef* hw = nullptr;
    int result = av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_VAAPI, path.c_str(), nullptr, 0);
    if (result >= 0) return hw;
    reason = "VA-API initialization failed on " + path + ": " + error(result);
  }
  return nullptr;
}
bool hardwareFormat(const SdpVideoFormat& format) {
  if (format.name != "H264") return false;
  auto mode = format.parameters.find("packetization-mode");
  auto profile = format.parameters.find("profile-level-id");
  return mode != format.parameters.end() && mode->second == "1" &&
    (profile == format.parameters.end() || profile->second.starts_with("42"));
}
void preferH264(std::vector<SdpVideoFormat>& formats) {
  std::stable_partition(formats.begin(), formats.end(), hardwareFormat);
}

class VaapiEncoder final : public VideoEncoder {
 public:
  explicit VaapiEncoder(std::shared_ptr<Status> status) : status_(std::move(status)) {}
  ~VaapiEncoder() override { Release(); }
  int InitEncode(const VideoCodec* codec, const Settings&) override {
    Release(); width_ = codec->width; height_ = codec->height;
    bitrate_ = std::max(10000u, codec->startBitrate * 1000); fps_ = std::max(1u, codec->maxFramerate);
    if (codec->numberOfSimulcastStreams > 1 || (width_ & 1) || (height_ & 1)) return fail("Unsupported simulcast or odd frame dimensions");
    return open();
  }
  int RegisterEncodeCompleteCallback(EncodedImageCallback* callback) override { callback_ = callback; return WEBRTC_VIDEO_CODEC_OK; }
  int Release() override {
    avcodec_free_context(&context_); av_buffer_unref(&frames_); av_buffer_unref(&device_); pending_.clear(); return WEBRTC_VIDEO_CODEC_OK;
  }
  int Encode(const VideoFrame& frame, const std::vector<VideoFrameType>* types) override {
    if (paused_) { if (callback_) callback_->OnDroppedFrame(EncodedImageCallback::DropReason::kDroppedByEncoder); return WEBRTC_VIDEO_CODEC_OK; }
    bool key = !types || std::find(types->begin(), types->end(), VideoFrameType::kVideoFrameKey) != types->end();
    if (reopen_ || frame.width() != width_ || frame.height() != height_) {
      width_ = frame.width(); height_ = frame.height(); Release(); if (open() != WEBRTC_VIDEO_CODEC_OK) return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
      reopen_ = false; key = true;
    }
    if (!context_ || !callback_) return fail("Encoder is not initialized");
    auto i420 = frame.video_frame_buffer()->ToI420();
    if (!i420) return fail("Unable to convert capture frame to I420");
    AVFrame* cpu = av_frame_alloc(); AVFrame* gpu = av_frame_alloc();
    if (!cpu || !gpu) { av_frame_free(&cpu); av_frame_free(&gpu); return fail("Video frame allocation failed"); }
    cpu->format = AV_PIX_FMT_NV12; cpu->width = width_; cpu->height = height_;
    int result = av_frame_get_buffer(cpu, 32);
    if (result >= 0) result = libyuv::I420ToNV12(i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(), i420->DataV(), i420->StrideV(), cpu->data[0], cpu->linesize[0], cpu->data[1], cpu->linesize[1], width_, height_);
    if (result >= 0) result = av_hwframe_get_buffer(frames_, gpu, 0);
    if (result >= 0) result = av_hwframe_transfer_data(gpu, cpu, 0);
    av_frame_free(&cpu);
    int64_t pts = sequence_++;
    gpu->pts = pts; gpu->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (result >= 0) { pending_[pts] = frame.rtp_timestamp(); result = avcodec_send_frame(context_, gpu); }
    av_frame_free(&gpu);
    if (result < 0) return fail("VA-API encode/upload failed: " + error(result));
    AVPacket* packet = av_packet_alloc();
    if (!packet) return fail("Encoded packet allocation failed");
    while ((result = avcodec_receive_packet(context_, packet)) >= 0) {
      auto timestamp = pending_.find(packet->pts);
      if (timestamp == pending_.end()) { av_packet_free(&packet); return fail("Encoder returned an unknown frame timestamp"); }
      EncodedImage image; image.SetEncodedData(EncodedImageBuffer::Create(packet->data, packet->size));
      image.SetRtpTimestamp(timestamp->second); pending_.erase(timestamp);
      image._encodedWidth = width_; image._encodedHeight = height_;
      image._frameType = (packet->flags & AV_PKT_FLAG_KEY) ? VideoFrameType::kVideoFrameKey : VideoFrameType::kVideoFrameDelta;
      CodecSpecificInfo info{}; info.codecType = kVideoCodecH264;
      info.codecSpecific.H264.packetization_mode = H264PacketizationMode::NonInterleaved;
      info.codecSpecific.H264.idr_frame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
      callback_->OnEncodedImage(image, &info); av_packet_unref(packet);
    }
    av_packet_free(&packet);
    if (pending_.size() > 16) return fail("Hardware encoder stopped returning frames");
    return result == AVERROR(EAGAIN) || result == AVERROR_EOF ? WEBRTC_VIDEO_CODEC_OK : fail("VA-API output failed: " + error(result));
  }
  void SetRates(const RateControlParameters& rates) override {
    unsigned bitrate = rates.bitrate.get_sum_bps(); paused_ = bitrate == 0;
    if (paused_) return;
    unsigned fps = rates.framerate_fps > 0 ? std::clamp(unsigned(rates.framerate_fps), 1u, 60u) : fps_;
    // VA-API's rate-control parameters are fixed at codec initialization. Apply
    // decreases immediately; avoid resetting on every small bandwidth increase.
    if (bitrate < bitrate_ || bitrate > bitrate_ + bitrate_ / 4 || fps != fps_) {
      bitrate_ = std::max(10000u, bitrate); fps_ = fps; reopen_ = true;
    }
  }
  EncoderInfo GetEncoderInfo() const override {
    EncoderInfo info; info.implementation_name = "FFmpeg VA-API H264";
    info.is_hardware_accelerated = true; info.supports_native_handle = false;
    info.requested_resolution_alignment = 2; info.has_trusted_rate_controller = false;
    return info;
  }
 private:
  int fail(std::string reason) { status_->reason(std::move(reason)); return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE; }
  int open() {
    std::string reason; device_ = device(reason); if (!device_) return fail(reason);
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!codec) return fail("System FFmpeg does not provide h264_vaapi");
    frames_ = av_hwframe_ctx_alloc(device_); if (!frames_) return fail("VA-API frame pool allocation failed");
    auto pool = reinterpret_cast<AVHWFramesContext*>(frames_->data);
    pool->format = AV_PIX_FMT_VAAPI; pool->sw_format = AV_PIX_FMT_NV12;
    pool->width = width_; pool->height = height_; pool->initial_pool_size = 8;
    int result = av_hwframe_ctx_init(frames_); if (result < 0) return fail("VA-API frame pool failed: " + error(result));
    context_ = avcodec_alloc_context3(codec); if (!context_) return fail("Encoder allocation failed");
    context_->width = width_; context_->height = height_; context_->pix_fmt = AV_PIX_FMT_VAAPI;
    context_->time_base = AVRational{1, int(fps_)}; context_->framerate = AVRational{int(fps_), 1};
    context_->bit_rate = bitrate_; context_->rc_max_rate = bitrate_; context_->rc_buffer_size = bitrate_;
    context_->gop_size = int(fps_) * 2; context_->max_b_frames = 0;
    context_->profile = AV_PROFILE_H264_CONSTRAINED_BASELINE;
    context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context_->hw_frames_ctx = av_buffer_ref(frames_);
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rc_mode", "CBR", 0); av_dict_set(&options, "async_depth", "1", 0);
    int resultOpen = avcodec_open2(context_, codec, &options); av_dict_free(&options);
    return resultOpen >= 0 ? WEBRTC_VIDEO_CODEC_OK : fail("System H264 encoder initialization failed: " + error(resultOpen));
  }
  std::shared_ptr<Status> status_; EncodedImageCallback* callback_ = nullptr;
  AVCodecContext* context_ = nullptr; AVBufferRef *device_ = nullptr, *frames_ = nullptr;
  int width_ = 0, height_ = 0; unsigned bitrate_ = 2000000, fps_ = 30;
  bool reopen_ = false, paused_ = false; int64_t sequence_ = 0;
  std::map<int64_t, uint32_t> pending_;
};

class VaapiDecoder final : public VideoDecoder {
 public:
  explicit VaapiDecoder(std::shared_ptr<Status> status) : status_(std::move(status)) {}
  ~VaapiDecoder() override { Release(); }
  bool Configure(const Settings&) override {
    Release(); std::string reason; device_ = device(reason); if (!device_) { fail(reason); return false; }
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) { fail("System FFmpeg H264 decoder is unavailable"); return false; }
    bool supported = false;
    for (int i = 0; const auto config = avcodec_get_hw_config(codec, i); ++i)
      if (config->device_type == AV_HWDEVICE_TYPE_VAAPI && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) supported = true;
    if (!supported) { fail("System H264 decoder has no VA-API backend"); return false; }
    context_ = avcodec_alloc_context3(codec); if (!context_) { fail("Decoder allocation failed"); return false; }
    context_->hw_device_ctx = av_buffer_ref(device_); context_->thread_count = 1;
    context_->get_format = [](AVCodecContext*, const AVPixelFormat* formats) {
      for (auto f = formats; *f != AV_PIX_FMT_NONE; ++f) if (*f == AV_PIX_FMT_VAAPI) return *f;
      return AV_PIX_FMT_NONE; // Never silently use FFmpeg's CPU decoder.
    };
    int result = avcodec_open2(context_, codec, nullptr);
    if (result < 0) { fail("System H264 decoder initialization failed: " + error(result)); return false; }
    return true;
  }
  int RegisterDecodeCompleteCallback(DecodedImageCallback* callback) override { callback_ = callback; return WEBRTC_VIDEO_CODEC_OK; }
  int Release() override { avcodec_free_context(&context_); av_buffer_unref(&device_); return WEBRTC_VIDEO_CODEC_OK; }
  int Decode(const EncodedImage& image, int64_t) override {
    if (!context_ || !callback_) return fail("Decoder is not initialized");
    AVPacket* packet = av_packet_alloc(); AVFrame* gpu = av_frame_alloc(); AVFrame* cpu = av_frame_alloc();
    if (!packet || !gpu || !cpu) { av_packet_free(&packet); av_frame_free(&gpu); av_frame_free(&cpu); return fail("Decode buffer allocation failed"); }
    int result = av_new_packet(packet, image.size());
    if (result >= 0) {
      std::memcpy(packet->data, image.data(), image.size()); packet->pts = image.RtpTimestamp();
      result = avcodec_send_packet(context_, packet);
    }
    av_packet_free(&packet);
    if (result >= 0) while ((result = avcodec_receive_frame(context_, gpu)) >= 0) {
      if (gpu->format != AV_PIX_FMT_VAAPI) { result = AVERROR(ENOSYS); break; }
      result = av_hwframe_transfer_data(cpu, gpu, 0);
      if (result < 0) break;
      auto output = I420Buffer::Create(cpu->width, cpu->height);
      if (cpu->format == AV_PIX_FMT_NV12) result = libyuv::NV12ToI420(cpu->data[0], cpu->linesize[0], cpu->data[1], cpu->linesize[1], output->MutableDataY(), output->StrideY(), output->MutableDataU(), output->StrideU(), output->MutableDataV(), output->StrideV(), cpu->width, cpu->height);
      else if (cpu->format == AV_PIX_FMT_YUV420P) result = libyuv::I420Copy(cpu->data[0], cpu->linesize[0], cpu->data[1], cpu->linesize[1], cpu->data[2], cpu->linesize[2], output->MutableDataY(), output->StrideY(), output->MutableDataU(), output->StrideU(), output->MutableDataV(), output->StrideV(), cpu->width, cpu->height);
      else result = AVERROR(ENOSYS);
      if (result < 0) break;
      auto frame = VideoFrame::Builder().set_video_frame_buffer(output).set_rtp_timestamp(uint32_t(gpu->pts)).set_timestamp_us(0).build();
      callback_->Decoded(frame); av_frame_unref(cpu); av_frame_unref(gpu);
    }
    av_frame_free(&gpu); av_frame_free(&cpu);
    return result == AVERROR(EAGAIN) || result == AVERROR_EOF || result >= 0 ? WEBRTC_VIDEO_CODEC_OK : fail("VA-API decoding failed: " + error(result));
  }
  DecoderInfo GetDecoderInfo() const override { return {"FFmpeg VA-API H264", true}; }
 private:
  int fail(std::string reason) { status_->reason(std::move(reason)); return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE; }
  std::shared_ptr<Status> status_; DecodedImageCallback* callback_ = nullptr;
  AVCodecContext* context_ = nullptr; AVBufferRef* device_ = nullptr;
};

// Observe actual output from the fallback wrapper. A successfully opened GPU
// device alone never counts as an accelerated stream.
class ObservedEncoder final : public VideoEncoder, private EncodedImageCallback {
 public:
  ObservedEncoder(std::unique_ptr<VideoEncoder> codec, std::shared_ptr<Status> status) : codec_(std::move(codec)), status_(std::move(status)) {}
  int InitEncode(const VideoCodec* codec, const Settings& settings) override { return codec_->InitEncode(codec, settings); }
  int RegisterEncodeCompleteCallback(EncodedImageCallback* callback) override { callback_ = callback; return codec_->RegisterEncodeCompleteCallback(this); }
  int Release() override { return codec_->Release(); }
  int Encode(const VideoFrame& frame, const std::vector<VideoFrameType>* types) override { return codec_->Encode(frame, types); }
  void SetRates(const RateControlParameters& rates) override { codec_->SetRates(rates); }
  EncoderInfo GetEncoderInfo() const override { return codec_->GetEncoderInfo(); }
 private:
  Result OnEncodedImage(const EncodedImage& image, const CodecSpecificInfo* info) override {
    auto implementation = codec_->GetEncoderInfo(); status_->update(implementation.is_hardware_accelerated, implementation.implementation_name);
    return callback_ ? callback_->OnEncodedImage(image, info) : Result(Result::ERROR_SEND_FAILED);
  }
  void OnDroppedFrame(DropReason reason) override { if (callback_) callback_->OnDroppedFrame(reason); }
  std::unique_ptr<VideoEncoder> codec_; std::shared_ptr<Status> status_; EncodedImageCallback* callback_ = nullptr;
};
class ObservedDecoder final : public VideoDecoder, private DecodedImageCallback {
 public:
  ObservedDecoder(std::unique_ptr<VideoDecoder> codec, std::shared_ptr<Status> status) : codec_(std::move(codec)), status_(std::move(status)) {}
  bool Configure(const Settings& settings) override { return codec_->Configure(settings); }
  int RegisterDecodeCompleteCallback(DecodedImageCallback* callback) override { callback_ = callback; return codec_->RegisterDecodeCompleteCallback(this); }
  int Release() override { return codec_->Release(); }
  int Decode(const EncodedImage& image, int64_t time) override { return codec_->Decode(image, time); }
  DecoderInfo GetDecoderInfo() const override { return codec_->GetDecoderInfo(); }
 private:
  int Decoded(VideoFrame& frame) override {
    auto info = codec_->GetDecoderInfo(); status_->update(info.is_hardware_accelerated, info.implementation_name);
    return callback_ ? callback_->Decoded(frame) : WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  void Decoded(VideoFrame& frame, std::optional<int32_t> time, std::optional<uint8_t> qp) override {
    auto info = codec_->GetDecoderInfo(); status_->update(info.is_hardware_accelerated, info.implementation_name);
    if (callback_) callback_->Decoded(frame, time, qp);
  }
  std::unique_ptr<VideoDecoder> codec_; std::shared_ptr<Status> status_; DecodedImageCallback* callback_ = nullptr;
};
class EncoderFactory final : public VideoEncoderFactory {
 public:
  std::vector<SdpVideoFormat> GetSupportedFormats() const override { auto formats = builtin_->GetSupportedFormats(); preferH264(formats); return formats; }
  std::unique_ptr<VideoEncoder> Create(const Environment& env, const SdpVideoFormat& format) override {
    auto software = builtin_->Create(env, format); if (!software) return nullptr;
    auto status = std::make_shared<Status>("encode");
    if (hardwareFormat(format)) software = CreateVideoEncoderSoftwareFallbackWrapper(env, std::move(software), std::make_unique<VaapiEncoder>(status), false);
    else status->reason("Negotiated " + format.name + " profile has no configured system GPU encoder; using WebRTC");
    return std::make_unique<ObservedEncoder>(std::move(software), status);
  }
 private:
  std::unique_ptr<VideoEncoderFactory> builtin_ = CreateBuiltinVideoEncoderFactory();
};
class DecoderFactory final : public VideoDecoderFactory {
 public:
  std::vector<SdpVideoFormat> GetSupportedFormats() const override { auto formats = builtin_->GetSupportedFormats(); preferH264(formats); return formats; }
  std::unique_ptr<VideoDecoder> Create(const Environment& env, const SdpVideoFormat& format) override {
    auto software = builtin_->Create(env, format); if (!software) return nullptr;
    auto status = std::make_shared<Status>("decode");
    if (format.name == "H264") software = CreateVideoDecoderSoftwareFallbackWrapper(env, std::move(software), std::make_unique<VaapiDecoder>(status));
    else status->reason("Negotiated " + format.name + " has no configured system GPU decoder; using WebRTC");
    return std::make_unique<ObservedDecoder>(std::move(software), status);
  }
 private:
  std::unique_ptr<VideoDecoderFactory> builtin_ = CreateBuiltinVideoDecoderFactory();
};
} // namespace
std::unique_ptr<VideoEncoderFactory> CreateSystemEncoderFactory() { return std::make_unique<EncoderFactory>(); }
std::unique_ptr<VideoDecoderFactory> CreateSystemDecoderFactory() { return std::make_unique<DecoderFactory>(); }

// Buffer-copy C ABI keeps the Qt app independent of WebRTC's internal ABI.
extern "C" __attribute__((visibility("default"))) size_t CompartilhagramCodecStatus(char* buffer, size_t capacity) {
  std::lock_guard<std::mutex> lock(status_mutex); std::ostringstream json; json << "["; bool first = true;
  for (const auto& [id, s] : statuses) {
    if (!first) json << ','; first = false;
    json << "{\"direction\":" << quote(s.direction) << ",\"mode\":" << quote(s.mode)
         << ",\"reason\":" << quote(s.reason) << ",\"implementation\":" << quote(s.implementation) << '}';
  }
  json << ']'; auto text = json.str();
  if (buffer && capacity > text.size()) std::memcpy(buffer, text.c_str(), text.size() + 1);
  return text.size() + 1;
}
} // namespace compartilhagram
