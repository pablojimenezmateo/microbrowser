#include "DecoderBackend.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <dav1d/dav1d.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
}
#include <opus.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include "ColorConvert.h"

namespace microbrowser::decoder_tool {

namespace {

constexpr int kOpusMaxFrameSamples = 5760;

class Av1Backend final : public DecoderBackend {
 public:
  explicit Av1Backend(FrameEmitter emit) : emit_(std::move(emit)) {}

  ~Av1Backend() override {
    if (context_ != nullptr) {
      dav1d_close(&context_);
    }
  }

  bool Configure(std::span<const std::uint8_t> extra_data, std::string& error) override {
    if (context_ != nullptr) {
      dav1d_close(&context_);
      context_ = nullptr;
    }
    Dav1dSettings settings;
    dav1d_default_settings(&settings);
    settings.n_threads = 1;
    if (dav1d_open(&context_, &settings) != 0) {
      error = "dav1d_open";
      return false;
    }
    pending_extra_.assign(extra_data.begin(), extra_data.end());
    return true;
  }

  bool DecodeSample(const ipc::SampleMessage& sample, std::string& error) override {
    if (context_ == nullptr) {
      error = "not_configured";
      return false;
    }
    if (!pending_extra_.empty()) {
      if (!SendBytes(pending_extra_, sample.timestamp_us, error)) {
        return false;
      }
      pending_extra_.clear();
    }
    return SendBytes(sample.bytes, sample.timestamp_us, error);
  }

  bool Flush(std::string& error) override {
    if (context_ == nullptr) {
      error = "not_configured";
      return false;
    }
    if (!pending_extra_.empty()) {
      if (!SendBytes(pending_extra_, 0, error)) {
        return false;
      }
      pending_extra_.clear();
    }
    dav1d_flush(context_);
    return DrainPictures(error);
  }

 private:
  bool SendBytes(const std::vector<std::uint8_t>& bytes, std::int64_t timestamp_us,
                 std::string& error) {
    if (bytes.empty()) {
      return true;
    }
    auto owned = std::make_unique<std::vector<std::uint8_t>>(bytes);
    Dav1dData data = {};
    data.m.timestamp = timestamp_us;
    if (dav1d_data_wrap(&data, owned->data(), owned->size(),
                        [](const std::uint8_t*, void* cookie) {
                          delete static_cast<std::vector<std::uint8_t>*>(cookie);
                        },
                        owned.release()) != 0) {
      error = "dav1d_data_wrap";
      return false;
    }
    if (dav1d_send_data(context_, &data) != 0) {
      error = "dav1d_send_data";
      return false;
    }
    return DrainPictures(error);
  }

  bool DrainPictures(std::string& error) {
    Dav1dPicture picture;
    while (true) {
      const int result = dav1d_get_picture(context_, &picture);
      if (result == DAV1D_ERR(EAGAIN)) {
        return true;
      }
      if (result < 0) {
        error = "dav1d_get_picture";
        return false;
      }
      ipc::FrameMessage frame;
      frame.timestamp_us = picture.m.timestamp;
      frame.width = static_cast<std::uint32_t>(picture.p.w);
      frame.height = static_cast<std::uint32_t>(picture.p.h);
      Yuv420ToRgba(std::span<const std::uint8_t>(
                       static_cast<const std::uint8_t*>(picture.data[0]),
                       static_cast<std::size_t>(picture.stride[0]) * static_cast<std::size_t>(picture.p.h)),
                   std::span<const std::uint8_t>(
                       static_cast<const std::uint8_t*>(picture.data[1]),
                       static_cast<std::size_t>(picture.stride[1]) *
                           static_cast<std::size_t>((picture.p.h + 1) / 2)),
                   std::span<const std::uint8_t>(
                       static_cast<const std::uint8_t*>(picture.data[2]),
                       static_cast<std::size_t>(picture.stride[1]) *
                           static_cast<std::size_t>((picture.p.h + 1) / 2)),
                   static_cast<int>(picture.stride[0]), static_cast<int>(picture.stride[1]),
                   frame.width, frame.height, frame.bytes);
      emit_(frame);
      dav1d_picture_unref(&picture);
    }
  }

  FrameEmitter emit_;
  Dav1dContext* context_ = nullptr;
  std::vector<std::uint8_t> pending_extra_;
};

class Vp9Backend final : public DecoderBackend {
 public:
  explicit Vp9Backend(FrameEmitter emit) : emit_(std::move(emit)) {}

  ~Vp9Backend() override { vpx_codec_destroy(&decoder_); }

  bool Configure(std::span<const std::uint8_t> extra_data, std::string& error) override {
    vpx_codec_destroy(&decoder_);
    std::memset(&decoder_, 0, sizeof(decoder_));
    vpx_codec_dec_cfg_t config = {};
    config.threads = 1;
    if (vpx_codec_dec_init(&decoder_, vpx_codec_vp9_dx(), &config, 0) != VPX_CODEC_OK) {
      error = "vpx_codec_dec_init";
      return false;
    }
    pending_extra_.assign(extra_data.begin(), extra_data.end());
    return true;
  }

  bool DecodeSample(const ipc::SampleMessage& sample, std::string& error) override {
    if (!pending_extra_.empty()) {
      if (vpx_codec_decode(&decoder_, pending_extra_.data(),
                           static_cast<unsigned int>(pending_extra_.size()), nullptr, 0) !=
          VPX_CODEC_OK) {
        error = "vpx_codec_decode";
        return false;
      }
      pending_extra_.clear();
    }
    if (vpx_codec_decode(&decoder_, sample.bytes.data(), static_cast<unsigned int>(sample.bytes.size()),
                         nullptr, 0) != VPX_CODEC_OK) {
      error = "vpx_codec_decode";
      return false;
    }
    return EmitFrames(sample.timestamp_us, error);
  }

  bool Flush(std::string& error) override {
    if (vpx_codec_decode(&decoder_, nullptr, 0, nullptr, 0) != VPX_CODEC_OK) {
      error = "vpx_codec_decode_flush";
      return false;
    }
    return EmitFrames(0, error);
  }

 private:
  bool EmitFrames(std::int64_t timestamp_us, std::string& error) {
    vpx_codec_iter_t iter = nullptr;
    while (true) {
      vpx_image_t* image = vpx_codec_get_frame(&decoder_, &iter);
      if (image == nullptr) {
        return true;
      }
      if (image->fmt != VPX_IMG_FMT_I420) {
        error = "vpx_format";
        return false;
      }
      ipc::FrameMessage frame;
      frame.timestamp_us = timestamp_us;
      frame.width = static_cast<std::uint32_t>(image->d_w);
      frame.height = static_cast<std::uint32_t>(image->d_h);
      Yuv420ToRgba(std::span<const std::uint8_t>(image->planes[0],
                                                 static_cast<std::size_t>(image->stride[0]) *
                                                     static_cast<std::size_t>(image->d_h)),
                   std::span<const std::uint8_t>(image->planes[1],
                                                 static_cast<std::size_t>(image->stride[1]) *
                                                     static_cast<std::size_t>((image->d_h + 1) / 2)),
                   std::span<const std::uint8_t>(image->planes[2],
                                                 static_cast<std::size_t>(image->stride[2]) *
                                                     static_cast<std::size_t>((image->d_h + 1) / 2)),
                   image->stride[0], image->stride[1], frame.width, frame.height, frame.bytes);
      emit_(frame);
    }
  }

  FrameEmitter emit_;
  vpx_codec_ctx_t decoder_ = {};
  std::vector<std::uint8_t> pending_extra_;
};

class OpusBackend final : public DecoderBackend {
 public:
  explicit OpusBackend(FrameEmitter emit) : emit_(std::move(emit)) {}

  ~OpusBackend() override {
    if (decoder_ != nullptr) {
      opus_decoder_destroy(decoder_);
    }
  }

  bool Configure(std::span<const std::uint8_t> extra_data, std::string& error) override {
    if (decoder_ != nullptr) {
      opus_decoder_destroy(decoder_);
      decoder_ = nullptr;
    }
    if (extra_data.size() < 19 || std::memcmp(extra_data.data(), "OpusHead", 8) != 0) {
      error = "opus_head";
      return false;
    }
    channels_ = extra_data[9];
    sample_rate_ = static_cast<std::uint32_t>(extra_data[12]) |
                   (static_cast<std::uint32_t>(extra_data[13]) << 8) |
                   (static_cast<std::uint32_t>(extra_data[14]) << 16) |
                   (static_cast<std::uint32_t>(extra_data[15]) << 24);
    if (channels_ == 0 || sample_rate_ == 0) {
      error = "opus_head";
      return false;
    }
    int opus_error = 0;
    decoder_ = opus_decoder_create(static_cast<opus_int32>(sample_rate_),
                                 static_cast<int>(channels_), &opus_error);
    if (decoder_ == nullptr || opus_error != OPUS_OK) {
      error = "opus_decoder_create";
      return false;
    }
    return true;
  }

  bool DecodeSample(const ipc::SampleMessage& sample, std::string& error) override {
    if (decoder_ == nullptr) {
      error = "not_configured";
      return false;
    }
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(channels_) *
                                  static_cast<std::size_t>(kOpusMaxFrameSamples));
    const int decoded =
        opus_decode(decoder_, sample.bytes.data(), static_cast<opus_int32>(sample.bytes.size()),
                    pcm.data(), kOpusMaxFrameSamples, 0);
    if (decoded < 0) {
      error = "opus_decode";
      return false;
    }
    if (decoded == 0) {
      return true;
    }
    ipc::FrameMessage frame;
    frame.timestamp_us = sample.timestamp_us;
    frame.sample_count = static_cast<std::uint32_t>(decoded);
    frame.channels = channels_;
    S16InterleavedToBytes(std::span<const std::int16_t>(pcm.data(),
                                                        static_cast<std::size_t>(decoded) * channels_),
                          frame.bytes);
    emit_(frame);
    return true;
  }

  bool Flush(std::string& error) override {
    (void)error;
    return true;
  }

 private:
  FrameEmitter emit_;
  OpusDecoder* decoder_ = nullptr;
  std::uint8_t channels_ = 0;
  std::uint32_t sample_rate_ = 0;
};

class LibavVideoBackend final : public DecoderBackend {
 public:
  explicit LibavVideoBackend(FrameEmitter emit) : emit_(std::move(emit)) {}

  ~LibavVideoBackend() override { Reset(); }

  bool Configure(std::span<const std::uint8_t> extra_data, std::string& error) override {
    Reset();
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
      error = "avcodec_find_decoder";
      return false;
    }
    context_ = avcodec_alloc_context3(codec);
    if (context_ == nullptr) {
      error = "avcodec_alloc_context3";
      return false;
    }
    if (!extra_data.empty()) {
      context_->extradata =
          static_cast<std::uint8_t*>(av_malloc(extra_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
      if (context_->extradata == nullptr) {
        error = "extradata_alloc";
        return false;
      }
      std::memcpy(context_->extradata, extra_data.data(), extra_data.size());
      context_->extradata_size = static_cast<int>(extra_data.size());
    }
    if (avcodec_open2(context_, codec, nullptr) < 0) {
      error = "avcodec_open2";
      return false;
    }
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (packet_ == nullptr || frame_ == nullptr) {
      error = "av_alloc";
      return false;
    }
    return true;
  }

  bool DecodeSample(const ipc::SampleMessage& sample, std::string& error) override {
    if (context_ == nullptr || packet_ == nullptr || frame_ == nullptr) {
      error = "not_configured";
      return false;
    }
    av_packet_unref(packet_);
    if (av_new_packet(packet_, static_cast<int>(sample.bytes.size())) < 0) {
      error = "av_new_packet";
      return false;
    }
    std::memcpy(packet_->data, sample.bytes.data(), sample.bytes.size());
    packet_->pts = sample.timestamp_us;
    return SendPacket(error, sample.timestamp_us);
  }

  bool Flush(std::string& error) override {
    if (context_ == nullptr) {
      error = "not_configured";
      return false;
    }
    return SendFlush(error);
  }

 private:
  void Reset() {
    if (packet_ != nullptr) {
      av_packet_free(&packet_);
    }
    if (frame_ != nullptr) {
      av_frame_free(&frame_);
    }
    if (context_ != nullptr) {
      avcodec_free_context(&context_);
    }
  }

  bool SendFlush(std::string& error) {
    const int send_result = avcodec_send_packet(context_, nullptr);
    if (send_result < 0 && send_result != AVERROR(EAGAIN) && send_result != AVERROR_EOF) {
      error = "avcodec_send_packet";
      return false;
    }
    return ReceiveFrames(error, 0);
  }

  bool SendPacket(std::string& error, std::int64_t timestamp_us) {
    const int send_result = avcodec_send_packet(context_, packet_);
    if (send_result < 0 && send_result != AVERROR(EAGAIN) && send_result != AVERROR_EOF) {
      error = "avcodec_send_packet";
      return false;
    }
    return ReceiveFrames(error, timestamp_us);
  }

  bool ReceiveFrames(std::string& error, std::int64_t timestamp_us) {
    while (true) {
      const int receive_result = avcodec_receive_frame(context_, frame_);
      if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
        return true;
      }
      if (receive_result < 0) {
        error = "avcodec_receive_frame";
        return false;
      }
      if (frame_->format != AV_PIX_FMT_YUV420P) {
        error = "av_pix_fmt";
        return false;
      }
      ipc::FrameMessage frame_message;
      frame_message.timestamp_us = frame_->pts != AV_NOPTS_VALUE ? frame_->pts : timestamp_us;
      frame_message.width = static_cast<std::uint32_t>(frame_->width);
      frame_message.height = static_cast<std::uint32_t>(frame_->height);
      Yuv420ToRgba(std::span<const std::uint8_t>(frame_->data[0],
                                                 static_cast<std::size_t>(frame_->linesize[0]) *
                                                     static_cast<std::size_t>(frame_->height)),
                   std::span<const std::uint8_t>(frame_->data[1],
                                                 static_cast<std::size_t>(frame_->linesize[1]) *
                                                     static_cast<std::size_t>((frame_->height + 1) / 2)),
                   std::span<const std::uint8_t>(frame_->data[2],
                                                 static_cast<std::size_t>(frame_->linesize[2]) *
                                                     static_cast<std::size_t>((frame_->height + 1) / 2)),
                   frame_->linesize[0], frame_->linesize[1], frame_message.width,
                   frame_message.height, frame_message.bytes);
      emit_(frame_message);
      av_frame_unref(frame_);
    }
  }

  FrameEmitter emit_;
  AVCodecContext* context_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
};

class LibavAudioBackend final : public DecoderBackend {
 public:
  explicit LibavAudioBackend(FrameEmitter emit) : emit_(std::move(emit)) {}

  ~LibavAudioBackend() override { Reset(); }

  bool Configure(std::span<const std::uint8_t> extra_data, std::string& error) override {
    Reset();
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (codec == nullptr) {
      error = "avcodec_find_decoder";
      return false;
    }
    context_ = avcodec_alloc_context3(codec);
    if (context_ == nullptr) {
      error = "avcodec_alloc_context3";
      return false;
    }
    if (!extra_data.empty()) {
      context_->extradata =
          static_cast<std::uint8_t*>(av_malloc(extra_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
      if (context_->extradata == nullptr) {
        error = "extradata_alloc";
        return false;
      }
      std::memcpy(context_->extradata, extra_data.data(), extra_data.size());
      context_->extradata_size = static_cast<int>(extra_data.size());
    }
    if (avcodec_open2(context_, codec, nullptr) < 0) {
      error = "avcodec_open2";
      return false;
    }
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (packet_ == nullptr || frame_ == nullptr) {
      error = "av_alloc";
      return false;
    }
    return true;
  }

  bool DecodeSample(const ipc::SampleMessage& sample, std::string& error) override {
    if (context_ == nullptr || packet_ == nullptr || frame_ == nullptr) {
      error = "not_configured";
      return false;
    }
    av_packet_unref(packet_);
    if (av_new_packet(packet_, static_cast<int>(sample.bytes.size())) < 0) {
      error = "av_new_packet";
      return false;
    }
    std::memcpy(packet_->data, sample.bytes.data(), sample.bytes.size());
    packet_->pts = sample.timestamp_us;
    return SendPacket(error, sample.timestamp_us);
  }

  bool Flush(std::string& error) override {
    if (context_ == nullptr) {
      error = "not_configured";
      return false;
    }
    return SendFlush(error);
  }

 private:
  void Reset() {
    if (packet_ != nullptr) {
      av_packet_free(&packet_);
    }
    if (frame_ != nullptr) {
      av_frame_free(&frame_);
    }
    if (context_ != nullptr) {
      avcodec_free_context(&context_);
    }
  }

  bool SendFlush(std::string& error) {
    const int send_result = avcodec_send_packet(context_, nullptr);
    if (send_result < 0 && send_result != AVERROR(EAGAIN) && send_result != AVERROR_EOF) {
      error = "avcodec_send_packet";
      return false;
    }
    return ReceiveAudioFrames(error, 0);
  }

  bool SendPacket(std::string& error, std::int64_t timestamp_us) {
    const int send_result = avcodec_send_packet(context_, packet_);
    if (send_result < 0 && send_result != AVERROR(EAGAIN) && send_result != AVERROR_EOF) {
      error = "avcodec_send_packet";
      return false;
    }
    return ReceiveAudioFrames(error, timestamp_us);
  }

  bool ReceiveAudioFrames(std::string& error, std::int64_t timestamp_us) {
    while (true) {
      const int receive_result = avcodec_receive_frame(context_, frame_);
      if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
        return true;
      }
      if (receive_result < 0) {
        error = "avcodec_receive_frame";
        return false;
      }
      ipc::FrameMessage frame_message;
      frame_message.timestamp_us = frame_->pts != AV_NOPTS_VALUE ? frame_->pts : timestamp_us;
      frame_message.sample_count = static_cast<std::uint32_t>(frame_->nb_samples);
      frame_message.channels = static_cast<std::uint8_t>(frame_->ch_layout.nb_channels);
      if (frame_->format == AV_SAMPLE_FMT_FLTP) {
        const float* planes[8] = {};
        for (int channel = 0; channel < frame_->ch_layout.nb_channels; ++channel) {
          planes[channel] = reinterpret_cast<const float*>(frame_->data[channel]);
        }
        FloatPlanarToS16Interleaved(
            std::span<const float*>(planes, static_cast<std::size_t>(frame_->ch_layout.nb_channels)),
            static_cast<std::uint32_t>(frame_->ch_layout.nb_channels),
            static_cast<std::uint32_t>(frame_->nb_samples), frame_message.bytes);
      } else if (frame_->format == AV_SAMPLE_FMT_S16 ||
                 frame_->format == AV_SAMPLE_FMT_S16P) {
        if (frame_->format == AV_SAMPLE_FMT_S16) {
          S16InterleavedToBytes(
              std::span<const std::int16_t>(reinterpret_cast<const std::int16_t*>(frame_->data[0]),
                                            static_cast<std::size_t>(frame_->nb_samples) *
                                                static_cast<std::size_t>(frame_->ch_layout.nb_channels)),
              frame_message.bytes);
        } else {
          std::vector<std::int16_t> interleaved(
              static_cast<std::size_t>(frame_->nb_samples) *
              static_cast<std::size_t>(frame_->ch_layout.nb_channels));
          for (int sample = 0; sample < frame_->nb_samples; ++sample) {
            for (int channel = 0; channel < frame_->ch_layout.nb_channels; ++channel) {
              interleaved[static_cast<std::size_t>(sample) *
                              static_cast<std::size_t>(frame_->ch_layout.nb_channels) +
                          static_cast<std::size_t>(channel)] =
                  reinterpret_cast<const std::int16_t*>(frame_->data[channel])[sample];
            }
          }
          S16InterleavedToBytes(interleaved, frame_message.bytes);
        }
      } else {
        error = "av_sample_fmt";
        return false;
      }
      emit_(frame_message);
      av_frame_unref(frame_);
    }
  }

  FrameEmitter emit_;
  AVCodecContext* context_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
};

}  // namespace

std::unique_ptr<DecoderBackend> CreateBackend(ipc::WireCodec codec, FrameEmitter emit) {
  switch (codec) {
    case ipc::WireCodec::Av1:
      return std::make_unique<Av1Backend>(std::move(emit));
    case ipc::WireCodec::Vp9:
      return std::make_unique<Vp9Backend>(std::move(emit));
    case ipc::WireCodec::Opus:
      return std::make_unique<OpusBackend>(std::move(emit));
    case ipc::WireCodec::H264:
      return std::make_unique<LibavVideoBackend>(std::move(emit));
    case ipc::WireCodec::Aac:
      return std::make_unique<LibavAudioBackend>(std::move(emit));
  }
  return nullptr;
}

}  // namespace microbrowser::decoder_tool
