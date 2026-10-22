// SPDX-License-Identifier: GPL-3.0-only
#include "webrtc_processor.h"

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"

#include <algorithm>
#include <cmath>

namespace ncs {

webrtc::AudioProcessing::Config CreateWebRtcNoiseSuppressionConfig() {
  webrtc::AudioProcessing::Config config;
  config.noise_suppression.enabled = true;
  config.noise_suppression.level =
      webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
  config.echo_canceller.enabled = false;
  config.gain_controller1.enabled = false;
  config.gain_controller2.enabled = false;
  config.high_pass_filter.enabled = false;
  config.transient_suppression.enabled = false;
  return config;
}

WebRtcProcessor::WebRtcProcessor() noexcept {
  try {
    audio_processing_ = webrtc::BuiltinAudioProcessingBuilder(
                            CreateWebRtcNoiseSuppressionConfig())
                            .Build(webrtc::CreateEnvironment());
    if (!audio_processing_) {
      return;
    }

    std::array<float, 160> silence{};
    std::array<float, 160> output{};
    const float* source[] = {silence.data()};
    float* destination[] = {output.data()};
    ready_ = audio_processing_->ProcessStream(
                 source, stream_config_, stream_config_, destination)
        == webrtc::AudioProcessing::kNoError;
  } catch (...) {
    audio_processing_ = nullptr;
    ready_ = false;
  }
}

bool WebRtcProcessor::ProcessFrame(
    const std::array<float, 160>& input,
    std::array<float, 160>& output) noexcept {
  if (!ready_ || !audio_processing_) {
    return false;
  }

  try {
    const float* source[] = {input.data()};
    float* destination[] = {output.data()};
    if (audio_processing_->ProcessStream(
            source, stream_config_, stream_config_, destination)
        != webrtc::AudioProcessing::kNoError) {
      ready_ = false;
      return false;
    }

    if (!std::all_of(output.begin(), output.end(), [](float sample) {
          return std::isfinite(sample);
        })) {
      ready_ = false;
      return false;
    }
    return true;
  } catch (...) {
    ready_ = false;
    return false;
  }
}

}  // namespace ncs
