// SPDX-License-Identifier: GPL-3.0-only
#include "frame_adapter.h"
#include "frame_processor.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace {

using ncs::FrameAdapter;
using ncs::FrameProcessor;

class IdentityProcessor final : public FrameProcessor {
 public:
  bool ProcessFrame(
      const std::array<float, FrameAdapter::kFrameSamples>& input,
      std::array<float, FrameAdapter::kFrameSamples>& output) noexcept override {
    output = input;
    ++calls;
    return true;
  }

  std::size_t calls = 0;
};

class ScaledProcessor final : public FrameProcessor {
 public:
  explicit ScaledProcessor(float scale) : scale_(scale) {}

  bool ProcessFrame(
      const std::array<float, FrameAdapter::kFrameSamples>& input,
      std::array<float, FrameAdapter::kFrameSamples>& output) noexcept override {
    std::transform(input.begin(), input.end(), output.begin(), [this](float value) {
      return value * scale_;
    });
    return true;
  }

 private:
  float scale_;
};

class NonFiniteProcessor final : public FrameProcessor {
 public:
  bool ProcessFrame(
      const std::array<float, FrameAdapter::kFrameSamples>&,
      std::array<float, FrameAdapter::kFrameSamples>& output) noexcept override {
    output.fill(0.0F);
    output[37] = std::numeric_limits<float>::infinity();
    return true;
  }
};

class FailOnFrameProcessor final : public FrameProcessor {
 public:
  explicit FailOnFrameProcessor(std::size_t fail_on_frame) : fail_on_frame_(fail_on_frame) {}

  bool ProcessFrame(
      const std::array<float, FrameAdapter::kFrameSamples>& input,
      std::array<float, FrameAdapter::kFrameSamples>& output) noexcept override {
    ++calls;
    if (calls == fail_on_frame_) {
      return false;
    }
    std::transform(input.begin(), input.end(), output.begin(), [](float value) {
      return value * 2.0F;
    });
    return true;
  }

  std::size_t calls = 0;

 private:
  std::size_t fail_on_frame_;
};

class ThrowingProcessor final : public FrameProcessor {
 public:
  bool ProcessFrame(
      const std::array<float, FrameAdapter::kFrameSamples>&,
      std::array<float, FrameAdapter::kFrameSamples>&) override {
    ++calls;
    throw std::runtime_error("synthetic processor failure");
  }

  std::size_t calls = 0;
};

std::vector<float> Sequence(std::size_t count, float offset = 1.0F) {
  std::vector<float> values(count);
  for (std::size_t index = 0; index < count; ++index) {
    values[index] = offset + static_cast<float>(index);
  }
  return values;
}

void RequireDelayed(
    const std::vector<float>& input,
    const std::vector<float>& output,
    float processed_scale) {
  NCS_REQUIRE(input.size() == output.size());
  for (std::size_t index = 0; index < output.size(); ++index) {
    const float expected = index < FrameAdapter::kLatencySamples
        ? 0.0F
        : input[index - FrameAdapter::kLatencySamples] * processed_scale;
    NCS_REQUIRE(output[index] == expected);
  }
}

void ExactFrameDelayIsStable() {
  ScaledProcessor processor(2.0F);
  FrameAdapter adapter(processor);
  auto input = Sequence(320);
  std::vector<float> output(input.size(), -1.0F);

  adapter.ProcessBlock(input.data(), output.data(), output.size());

  RequireDelayed(input, output, 2.0F);
  NCS_REQUIRE(!adapter.bypassed());
}

void ArbitraryBlockSizesPreserveOrdering() {
  IdentityProcessor processor;
  FrameAdapter adapter(processor);
  const std::array<std::size_t, 6> block_sizes = {0, 1, 159, 160, 161, 4097};
  const auto total = std::accumulate(block_sizes.begin(), block_sizes.end(), std::size_t{0});
  auto input = Sequence(total);
  std::vector<float> output(total, -1.0F);

  adapter.ProcessBlock(nullptr, nullptr, 0);
  std::size_t position = 0;
  for (const auto block_size : block_sizes) {
    adapter.ProcessBlock(input.data() + position, output.data() + position, block_size);
    position += block_size;
  }

  RequireDelayed(input, output, 1.0F);
  NCS_REQUIRE(processor.calls == total / FrameAdapter::kFrameSamples);
}

void ResetRestoresSilencePrefillAndProcessing() {
  IdentityProcessor processor;
  FrameAdapter adapter(processor);
  auto warmup = Sequence(200);
  std::vector<float> discarded(warmup.size());
  adapter.ProcessBlock(warmup.data(), discarded.data(), warmup.size());

  adapter.Reset();
  auto input = Sequence(320, 1000.0F);
  std::vector<float> output(input.size(), -1.0F);
  adapter.ProcessBlock(input.data(), output.data(), input.size());

  RequireDelayed(input, output, 1.0F);
  NCS_REQUIRE(!adapter.bypassed());
}

void LongRunUsesExactlyOneProcessorCallPerFrame() {
  IdentityProcessor processor;
  FrameAdapter adapter(processor);
  auto input = Sequence(160 * 2048 + 79);
  std::vector<float> output(input.size());

  for (std::size_t position = 0; position < input.size();) {
    const auto count = std::min<std::size_t>(17, input.size() - position);
    adapter.ProcessBlock(input.data() + position, output.data() + position, count);
    position += count;
  }

  RequireDelayed(input, output, 1.0F);
  NCS_REQUIRE(processor.calls == 2048);
  NCS_REQUIRE(sizeof(FrameAdapter) < 4096);
}

void FailurePermanentlyUsesDelayedDryAudio() {
  FailOnFrameProcessor processor(2);
  FrameAdapter adapter(processor);
  auto input = Sequence(640);
  std::vector<float> output(input.size(), -1.0F);

  adapter.ProcessBlock(input.data(), output.data(), input.size());

  for (std::size_t index = 0; index < output.size(); ++index) {
    float expected = 0.0F;
    if (index >= 160 && index < 320) {
      expected = input[index - 160] * 2.0F;
    } else if (index >= 320) {
      expected = input[index - 160];
    }
    NCS_REQUIRE(output[index] == expected);
  }
  NCS_REQUIRE(adapter.bypassed());
  NCS_REQUIRE(processor.calls == 2);
}

void NonFiniteOutputTriggersDelayedDryAudio() {
  NonFiniteProcessor processor;
  FrameAdapter adapter(processor);
  auto input = Sequence(480);
  std::vector<float> output(input.size(), -1.0F);

  adapter.ProcessBlock(input.data(), output.data(), input.size());

  RequireDelayed(input, output, 1.0F);
  NCS_REQUIRE(adapter.bypassed());
}

void MidBlockFailureDoesNotDropPartiallyBufferedSamples() {
  FailOnFrameProcessor processor(2);
  FrameAdapter adapter(processor);
  auto input = Sequence(640);
  std::vector<float> output(input.size(), -1.0F);

  adapter.ProcessBlock(input.data(), output.data(), 200);
  adapter.ProcessBlock(input.data() + 200, output.data() + 200, 120);
  adapter.ProcessBlock(input.data() + 320, output.data() + 320, input.size() - 320);

  for (std::size_t index = 0; index < output.size(); ++index) {
    float expected = 0.0F;
    if (index >= 160 && index < 320) {
      expected = input[index - 160] * 2.0F;
    } else if (index >= 320) {
      expected = input[index - 160];
    }
    NCS_REQUIRE(output[index] == expected);
  }
  NCS_REQUIRE(adapter.bypassed());
}

void ExceptionPermanentlyUsesDelayedDryAudioAcrossReset() {
  ThrowingProcessor processor;
  FrameAdapter adapter(processor);
  auto input = Sequence(480);
  std::vector<float> output(input.size(), -1.0F);

  adapter.ProcessBlock(input.data(), output.data(), input.size());

  RequireDelayed(input, output, 1.0F);
  NCS_REQUIRE(adapter.bypassed());
  NCS_REQUIRE(processor.calls == 1);

  adapter.Reset();
  auto second_input = Sequence(320, 2000.0F);
  std::vector<float> second_output(second_input.size(), -1.0F);
  adapter.ProcessBlock(second_input.data(), second_output.data(), second_input.size());

  RequireDelayed(second_input, second_output, 1.0F);
  NCS_REQUIRE(adapter.bypassed());
  NCS_REQUIRE(processor.calls == 1);
}

}  // namespace

int main() {
  static_assert(FrameAdapter::kFrameSamples == 160);
  static_assert(FrameAdapter::kLatencySamples == 160);
  return ncs::test::Run({
      {"exact frame delay", ExactFrameDelayIsStable},
      {"arbitrary block sizes", ArbitraryBlockSizesPreserveOrdering},
      {"reset", ResetRestoresSilencePrefillAndProcessing},
      {"bounded long run", LongRunUsesExactlyOneProcessorCallPerFrame},
      {"permanent bypass", FailurePermanentlyUsesDelayedDryAudio},
      {"non-finite rejection", NonFiniteOutputTriggersDelayedDryAudio},
      {"partial-frame failure", MidBlockFailureDoesNotDropPartiallyBufferedSamples},
      {"exception enters permanent bypass", ExceptionPermanentlyUsesDelayedDryAudioAcrossReset},
  });
}
