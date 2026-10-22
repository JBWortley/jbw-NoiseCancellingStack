// SPDX-License-Identifier: GPL-3.0-only
#include "frame_adapter.h"

#include <algorithm>
#include <cmath>

namespace ncs {

FrameAdapter::FrameAdapter(FrameProcessor& processor) noexcept : processor_(processor) {
  Reset();
}

void FrameAdapter::ProcessBlock(
    const float* input,
    float* output,
    std::size_t sample_count) noexcept {
  if (sample_count == 0) {
    return;
  }
  if (input == nullptr || output == nullptr) {
    bypassed_ = true;
    return;
  }

  for (std::size_t sample = 0; sample < sample_count; ++sample) {
    const float dry_sample = input[sample];
    output[sample] = delayed_output_[frame_position_];
    input_frame_[frame_position_] = dry_sample;
    ++frame_position_;

    if (frame_position_ != kFrameSamples) {
      continue;
    }

    bool frame_is_valid = false;
    if (!bypassed_) {
      processed_frame_.fill(0.0F);
      frame_is_valid = processor_.ProcessFrame(input_frame_, processed_frame_)
          && std::all_of(processed_frame_.begin(), processed_frame_.end(), [](float value) {
               return std::isfinite(value);
             });
    }

    if (frame_is_valid) {
      delayed_output_ = processed_frame_;
    } else {
      bypassed_ = true;
      delayed_output_ = input_frame_;
    }
    frame_position_ = 0;
  }
}

void FrameAdapter::Reset() noexcept {
  input_frame_.fill(0.0F);
  delayed_output_.fill(0.0F);
  processed_frame_.fill(0.0F);
  frame_position_ = 0;
  bypassed_ = false;
}

}  // namespace ncs
