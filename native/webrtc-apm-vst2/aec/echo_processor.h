// SPDX-License-Identifier: MIT
#pragma once

#include "api/audio/audio_processing.h"
#include "api/scoped_refptr.h"

#include <cstddef>
#include <vector>

namespace ncs {

// AEC3 only: noise suppression stays in the Equalizer APO stage, and gain control, transient
// suppression and the deprecated stream-delay hint are not used. Processing runs internally at
// up to 48 kHz so the 48 kHz RNNoise path keeps its full bandwidth.
webrtc::AudioProcessing::Config CreateEchoCancellationConfig();

// Owns one WebRTC APM instance configured by CreateEchoCancellationConfig().
//
// Configure() and Reset() are non-real-time. After Configure() returns true, ProcessRender() and
// ProcessCapture() neither allocate nor take OS locks (with the pinned single-thread patches) and
// must be called from one thread at a time. Either returns false on a WebRTC error or a
// non-finite result; the caller then stops using the instance.
class EchoProcessor final {
 public:
  static constexpr std::size_t kMaxCaptureChannels = 8;
  static constexpr std::size_t kMaxRenderChannels = 2;

  [[nodiscard]] static bool IsSupportedRate(int sample_rate) noexcept {
    return sample_rate == 16'000 || sample_rate == 32'000 || sample_rate == 48'000;
  }

  // Fixed capture-path delay added by AEC3's block framing and band-split filter bank in the
  // pinned WebRTC release, in frames at the capture rate (measured; guarded by a unit test).
  [[nodiscard]] static std::size_t PipelineDelayFrames(int sample_rate) noexcept {
    switch (sample_rate) {
      case 16'000: return 128;
      case 32'000: return 260;
      case 48'000: return 430;
      default: return 0;
    }
  }

  bool Configure(int capture_rate, std::size_t capture_channels, int render_rate,
                 std::size_t render_channels) noexcept;
  void Reset() noexcept;

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] std::size_t capture_frames() const noexcept { return capture_frames_; }
  [[nodiscard]] std::size_t capture_channels() const noexcept { return capture_planes_.size(); }
  [[nodiscard]] std::size_t render_frames() const noexcept { return render_frames_; }
  [[nodiscard]] std::size_t render_channels() const noexcept { return render_planes_.size(); }

  // One 10 ms block: render_frames() x render_channels() interleaved loopback samples.
  bool ProcessRender(const float* interleaved) noexcept;

  // One 10 ms block: capture_frames() x capture_channels() interleaved microphone samples,
  // replaced with the echo-cancelled result only when processing succeeds.
  bool ProcessCapture(float* interleaved) noexcept;

 private:
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm_;
  webrtc::StreamConfig capture_config_;
  webrtc::StreamConfig render_config_;
  std::size_t capture_frames_ = 0;
  std::size_t render_frames_ = 0;
  std::vector<std::vector<float>> capture_planes_;
  std::vector<std::vector<float>> capture_output_planes_;
  std::vector<std::vector<float>> render_planes_;
  std::vector<const float*> capture_inputs_;
  std::vector<float*> capture_outputs_;
  std::vector<const float*> render_inputs_;
  std::vector<float*> render_outputs_;
  bool ready_ = false;
};

}  // namespace ncs
