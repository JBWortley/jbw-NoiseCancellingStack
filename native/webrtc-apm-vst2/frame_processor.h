// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>

namespace ncs {

class FrameProcessor {
 public:
  virtual ~FrameProcessor() = default;

  virtual bool ProcessFrame(
      const std::array<float, 160>& input,
      std::array<float, 160>& output) = 0;
};

}  // namespace ncs
