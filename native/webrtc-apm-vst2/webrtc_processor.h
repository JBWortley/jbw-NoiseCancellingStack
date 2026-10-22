// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "frame_processor.h"

#include "api/audio/audio_processing.h"
#include "api/scoped_refptr.h"

#include <array>

namespace ncs {

webrtc::AudioProcessing::Config CreateWebRtcNoiseSuppressionConfig();

class WebRtcProcessor final : public FrameProcessor {
 public:
  WebRtcProcessor() noexcept;

  bool ProcessFrame(
      const std::array<float, 160>& input,
      std::array<float, 160>& output) noexcept override;

  [[nodiscard]] bool ready() const noexcept { return ready_; }

 private:
  webrtc::scoped_refptr<webrtc::AudioProcessing> audio_processing_;
  webrtc::StreamConfig stream_config_{16'000, 1};
  bool ready_ = false;
};

}  // namespace ncs
