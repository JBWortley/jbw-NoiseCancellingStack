// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "frame_adapter.h"
#include "webrtc_processor.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>

namespace {

using ncs::FrameAdapter;
using ncs::WebRtcProcessor;

void ConfigurationEnablesOnlyHighNoiseSuppression() {
  const auto config = ncs::CreateWebRtcNoiseSuppressionConfig();

  NCS_REQUIRE(config.noise_suppression.enabled);
  NCS_REQUIRE(
      config.noise_suppression.level == webrtc::AudioProcessing::Config::NoiseSuppression::kHigh);
  NCS_REQUIRE(!config.echo_canceller.enabled);
  NCS_REQUIRE(!config.gain_controller1.enabled);
  NCS_REQUIRE(!config.gain_controller2.enabled);
  NCS_REQUIRE(!config.high_pass_filter.enabled);
  NCS_REQUIRE(!config.transient_suppression.enabled);
}

void ConstructionWarmsApmAndSilenceStaysFinite() {
  WebRtcProcessor processor;
  NCS_REQUIRE(processor.ready());

  std::array<float, FrameAdapter::kFrameSamples> input{};
  std::array<float, FrameAdapter::kFrameSamples> output{};
  NCS_REQUIRE(processor.ProcessFrame(input, output));
  for (const float sample : output) {
    NCS_REQUIRE(std::isfinite(sample));
    NCS_REQUIRE(std::abs(sample) < 0.000001F);
  }
}

float NextNoise(std::uint32_t& state) {
  state ^= state << 13U;
  state ^= state >> 17U;
  state ^= state << 5U;
  constexpr float kScale = 0.0866025404F / static_cast<float>(UINT32_MAX);
  return static_cast<float>(state) * (2.0F * kScale) - 0.0866025404F;
}

void StationaryNoiseIsAttenuatedAfterAdaptation() {
  WebRtcProcessor processor;
  NCS_REQUIRE(processor.ready());

  std::uint32_t random_state = 0xC0FFEEU;
  double input_energy = 0.0;
  double output_energy = 0.0;
  std::size_t measured_samples = 0;
  for (std::size_t frame = 0; frame < 600; ++frame) {
    std::array<float, FrameAdapter::kFrameSamples> input{};
    std::array<float, FrameAdapter::kFrameSamples> output{};
    for (float& sample : input) {
      sample = NextNoise(random_state);
    }

    NCS_REQUIRE(processor.ProcessFrame(input, output));
    for (const float sample : output) {
      NCS_REQUIRE(std::isfinite(sample));
    }

    if (frame >= 400) {
      for (std::size_t sample = 0; sample < input.size(); ++sample) {
        input_energy += static_cast<double>(input[sample]) * input[sample];
        output_energy += static_cast<double>(output[sample]) * output[sample];
        ++measured_samples;
      }
    }
  }

  const double input_rms = std::sqrt(input_energy / static_cast<double>(measured_samples));
  const double output_rms = std::sqrt(output_energy / static_cast<double>(measured_samples));
  const double attenuation_db = 20.0 * std::log10(input_rms / output_rms);
  NCS_REQUIRE(input_rms > 0.045 && input_rms < 0.055);
  NCS_REQUIRE(attenuation_db >= 3.0);
}

}  // namespace

int main() {
  static_assert(FrameAdapter::kFrameSamples == 160);
  return ncs::test::Run({
      {"noise suppression configuration", ConfigurationEnablesOnlyHighNoiseSuppression},
      {"construction and silence", ConstructionWarmsApmAndSilenceStaysFinite},
      {"stationary noise attenuation", StationaryNoiseIsAttenuatedAfterAdaptation},
  });
}
