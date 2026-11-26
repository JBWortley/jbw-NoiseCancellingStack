// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <vector>

namespace ncs {

// Fixed delay for interleaved audio. The echo canceller's fallback path uses it so the output
// latency the APO reports stays constant whether a block was echo-cancelled or passed through.
class DelayLine final {
 public:
  // Non-real-time.
  void Configure(std::size_t delay_frames, std::size_t channels) {
    line_.assign(delay_frames * channels, 0.0F);
    position_ = 0;
  }

  [[nodiscard]] std::size_t delay_samples() const noexcept { return line_.size(); }

  // Real-time. Writes `input` delayed by the configured amount to `output`; the buffers may be
  // the same.
  void Process(const float* input, float* output, std::size_t samples) noexcept {
    if (line_.empty()) {
      if (output != input) {
        for (std::size_t i = 0; i < samples; ++i) {
          output[i] = input[i];
        }
      }
      return;
    }
    for (std::size_t i = 0; i < samples; ++i) {
      const float delayed = line_[position_];
      line_[position_] = input[i];
      output[i] = delayed;
      position_ = position_ + 1 == line_.size() ? 0 : position_ + 1;
    }
  }

 private:
  std::vector<float> line_;
  std::size_t position_ = 0;
};

}  // namespace ncs
