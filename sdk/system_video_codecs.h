#pragma once
#include <memory>
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory.h"
namespace compartilhagram {
std::unique_ptr<webrtc::VideoEncoderFactory> CreateSystemEncoderFactory();
std::unique_ptr<webrtc::VideoDecoderFactory> CreateSystemDecoderFactory();
}
