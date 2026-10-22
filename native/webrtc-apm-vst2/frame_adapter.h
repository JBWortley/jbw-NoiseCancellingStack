// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "frame_processor.h"

#include <array>
#include <cstddef>

namespace ncs {

class FrameAdapter final {
 public:
  static constexpr std::size_t kFrameSamples = 160;
  static constexpr std::size_t kLatencySamples = kFrameSamples;

  explicit FrameAdapter(FrameProcessor& processor) noexcept;

  void ProcessBlock(const float* input, float* output, std::size_t sample_count) noexcept;
  void Reset() noexcept;
  [[nodiscard]] bool bypassed() const noexcept { return bypassed_; }

 private:
  FrameProcessor& processor_;
  std::array<float, kFrameSamples> input_frame_{};
  std::array<float, kFrameSamples> delayed_output_{};
  std::array<float, kFrameSamples> processed_frame_{};
  std::size_t frame_position_ = 0;
  bool bypassed_ = false;
};

}  // namespace ncs
