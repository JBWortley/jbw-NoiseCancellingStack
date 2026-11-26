// SPDX-License-Identifier: MIT
#include "echo_processor.h"

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"

#include <cmath>

namespace ncs {

webrtc::AudioProcessing::Config CreateEchoCancellationConfig() {
  webrtc::AudioProcessing::Config config;
  config.echo_canceller.enabled = true;
  config.echo_canceller.enforce_high_pass_filtering = true;
  config.noise_suppression.enabled = false;
  config.gain_controller1.enabled = false;
  config.gain_controller2.enabled = false;
  config.transient_suppression.enabled = false;
  config.pipeline.maximum_internal_processing_rate = 48'000;
  config.pipeline.multi_channel_render = true;
  config.pipeline.multi_channel_capture = true;
  return config;
}

bool EchoProcessor::Configure(int capture_rate, std::size_t capture_channels, int render_rate,
                              std::size_t render_channels) noexcept {
  Reset();
  if (!IsSupportedRate(capture_rate) || !IsSupportedRate(render_rate) || capture_channels == 0
      || capture_channels > kMaxCaptureChannels || render_channels == 0
      || render_channels > kMaxRenderChannels) {
    return false;
  }

  try {
    capture_frames_ = static_cast<std::size_t>(capture_rate / 100);
    render_frames_ = static_cast<std::size_t>(render_rate / 100);
    capture_config_ = webrtc::StreamConfig(capture_rate, capture_channels);
    render_config_ = webrtc::StreamConfig(render_rate, render_channels);
    capture_planes_.assign(capture_channels, std::vector<float>(capture_frames_, 0.0F));
    capture_output_planes_.assign(capture_channels, std::vector<float>(capture_frames_, 0.0F));
    render_planes_.assign(render_channels, std::vector<float>(render_frames_, 0.0F));
    for (std::size_t channel = 0; channel < capture_channels; ++channel) {
      capture_inputs_.push_back(capture_planes_[channel].data());
      capture_outputs_.push_back(capture_output_planes_[channel].data());
    }
    for (auto& plane : render_planes_) {
      render_inputs_.push_back(plane.data());
      render_outputs_.push_back(plane.data());
    }

    apm_ = webrtc::BuiltinAudioProcessingBuilder(CreateEchoCancellationConfig())
               .Build(webrtc::CreateEnvironment());
    if (!apm_) {
      Reset();
      return false;
    }

    // Warm both paths so APM performs its format-dependent initialization and allocation here,
    // not in the first real-time callback.
    if (apm_->ProcessReverseStream(render_inputs_.data(), render_config_, render_config_,
                                   render_outputs_.data())
            != webrtc::AudioProcessing::kNoError
        || apm_->ProcessStream(capture_inputs_.data(), capture_config_, capture_config_,
                               capture_outputs_.data())
               != webrtc::AudioProcessing::kNoError) {
      Reset();
      return false;
    }
    ready_ = true;
    return true;
  } catch (...) {
    Reset();
    return false;
  }
}

void EchoProcessor::Reset() noexcept {
  ready_ = false;
  apm_ = nullptr;
  capture_inputs_.clear();
  capture_outputs_.clear();
  render_inputs_.clear();
  render_outputs_.clear();
  capture_planes_.clear();
  capture_output_planes_.clear();
  render_planes_.clear();
  capture_frames_ = 0;
  render_frames_ = 0;
}

bool EchoProcessor::ProcessRender(const float* interleaved) noexcept {
  if (!ready_ || interleaved == nullptr) {
    return false;
  }
  const std::size_t channels = render_planes_.size();
  for (std::size_t frame = 0; frame < render_frames_; ++frame) {
    for (std::size_t channel = 0; channel < channels; ++channel) {
      render_planes_[channel][frame] = interleaved[frame * channels + channel];
    }
  }
  if (apm_->ProcessReverseStream(render_inputs_.data(), render_config_, render_config_,
                                 render_outputs_.data())
      != webrtc::AudioProcessing::kNoError) {
    ready_ = false;
    return false;
  }
  return true;
}

bool EchoProcessor::ProcessCapture(float* interleaved) noexcept {
  if (!ready_ || interleaved == nullptr) {
    return false;
  }
  const std::size_t channels = capture_planes_.size();
  for (std::size_t frame = 0; frame < capture_frames_; ++frame) {
    for (std::size_t channel = 0; channel < channels; ++channel) {
      capture_planes_[channel][frame] = interleaved[frame * channels + channel];
    }
  }
  if (apm_->ProcessStream(capture_inputs_.data(), capture_config_, capture_config_,
                          capture_outputs_.data())
      != webrtc::AudioProcessing::kNoError) {
    ready_ = false;
    return false;
  }
  for (const auto& plane : capture_output_planes_) {
    for (const float sample : plane) {
      if (!std::isfinite(sample)) {
        ready_ = false;
        return false;
      }
    }
  }
  for (std::size_t frame = 0; frame < capture_frames_; ++frame) {
    for (std::size_t channel = 0; channel < channels; ++channel) {
      interleaved[frame * channels + channel] = capture_output_planes_[channel][frame];
    }
  }
  return true;
}

}  // namespace ncs
