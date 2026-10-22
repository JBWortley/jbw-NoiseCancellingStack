// SPDX-License-Identifier: GPL-3.0-only
#include "aeffectx.h"
#include "test_support.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

using PluginEntry = AEffect* (*)(audioMasterCallback);
using SelfTest = std::int32_t (*)();

HMODULE module = nullptr;
PluginEntry plugin_entry = nullptr;
SelfTest self_test = nullptr;
std::filesystem::path plugin_path;

VstIntPtr HostCallback(
    AEffect*, int opcode, int, VstIntPtr, void*, float) {
  return opcode == audioMasterVersion ? kVstVersion : 0;
}

AEffect* OpenEffect() {
  AEffect* effect = plugin_entry(&HostCallback);
  NCS_REQUIRE(effect != nullptr);
  NCS_REQUIRE(effect->dispatcher != nullptr);
  effect->dispatcher(effect, effOpen, 0, 0, nullptr, 0.0F);
  return effect;
}

void CloseEffect(AEffect* effect) {
  if (effect != nullptr) {
    effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0F);
  }
}

void SetSampleRate(AEffect* effect, float sample_rate) {
  effect->dispatcher(effect, effSetSampleRate, 0, 0, nullptr, sample_rate);
}

void Process(AEffect* effect, float* input, float* output, std::size_t samples) {
  float* inputs[] = {input};
  float* outputs[] = {output};
  effect->processReplacing(effect, inputs, outputs, static_cast<int>(samples));
}

void ExportsAndMetadataAreStable() {
  NCS_REQUIRE(GetProcAddress(module, "VSTPluginMain") != nullptr);
  NCS_REQUIRE(GetProcAddress(module, "main") != nullptr);
  NCS_REQUIRE(self_test != nullptr);
  NCS_REQUIRE(self_test() == 0);

  AEffect* effect = OpenEffect();
  NCS_REQUIRE(effect->magic == kEffectMagic);
  NCS_REQUIRE(effect->uniqueID == CCONST('N', 'C', '1', '6'));
  NCS_REQUIRE(effect->numInputs == 1);
  NCS_REQUIRE(effect->numOutputs == 1);
  NCS_REQUIRE(effect->numParams == 0);
  NCS_REQUIRE((effect->flags & effFlagsCanReplacing) != 0);
  NCS_REQUIRE(effect->initialDelay == 160);
  NCS_REQUIRE(effect->processReplacing != nullptr);

  std::array<char, 64> effect_name{};
  std::array<char, kVstMaxVendorStrLen> vendor{};
  std::array<char, kVstMaxProductStrLen> product{};
  NCS_REQUIRE(effect->dispatcher(effect, effGetEffectName, 0, 0, effect_name.data(), 0.0F) == 1);
  NCS_REQUIRE(effect->dispatcher(effect, effGetVendorString, 0, 0, vendor.data(), 0.0F) == 1);
  NCS_REQUIRE(effect->dispatcher(effect, effGetProductString, 0, 0, product.data(), 0.0F) == 1);
  NCS_REQUIRE(std::string(effect_name.data()) == "NCS WebRTC APM 16k");
  NCS_REQUIRE(std::string(vendor.data()) == "JBWortley");
  NCS_REQUIRE(std::string(product.data()) == "Noise Cancelling Stack");
  NCS_REQUIRE(effect->dispatcher(effect, effGetVstVersion, 0, 0, nullptr, 0.0F) == kVstVersion);
  CloseEffect(effect);
}

void ZeroFramesAndProcessingBeforeActivationAreSafe() {
  AEffect* effect = OpenEffect();
  effect->processReplacing(effect, nullptr, nullptr, 0);
  SetSampleRate(effect, 16'000.0F);

  std::vector<float> input(320, 0.04F);
  std::vector<float> output(input.size(), -1.0F);
  Process(effect, input.data(), output.data(), input.size());
  for (std::size_t sample = 0; sample < 160; ++sample) {
    NCS_REQUIRE(output[sample] == 0.0F);
  }
  for (const float sample : output) {
    NCS_REQUIRE(std::isfinite(sample));
  }
  CloseEffect(effect);
}

void UnsupportedRatesAreBitExactPassThrough() {
  AEffect* effect = OpenEffect();
  SetSampleRate(effect, 48'000.0F);
  std::vector<float> input(4097);
  for (std::size_t sample = 0; sample < input.size(); ++sample) {
    input[sample] = static_cast<float>(static_cast<int>(sample % 257) - 128) / 128.0F;
  }
  std::vector<float> output(input.size(), 42.0F);

  Process(effect, input.data(), output.data(), input.size());

  NCS_REQUIRE(std::memcmp(input.data(), output.data(), input.size() * sizeof(float)) == 0);
  CloseEffect(effect);
}

void TwoInstancesKeepStateIsolatedAcrossThreads() {
  AEffect* silence_effect = OpenEffect();
  AEffect* noise_effect = OpenEffect();
  SetSampleRate(silence_effect, 16'000.0F);
  SetSampleRate(noise_effect, 16'000.0F);
  constexpr std::size_t kFrames = 40;
  constexpr std::size_t kSamples = kFrames * 160;
  std::vector<float> silence_input(kSamples, 0.0F);
  std::vector<float> noise_input(kSamples);
  for (std::size_t sample = 0; sample < kSamples; ++sample) {
    noise_input[sample] = 0.05F * std::sin(static_cast<float>(sample) * 0.173F);
  }
  std::vector<float> silence_output(kSamples, -1.0F);
  std::vector<float> noise_output(kSamples, -1.0F);
  std::atomic<std::size_t> turn{0};

  auto run_instance = [&](AEffect* effect,
                          std::vector<float>& input,
                          std::vector<float>& output,
                          std::size_t parity) {
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
      const std::size_t expected_turn = frame * 2 + parity;
      while (turn.load(std::memory_order_acquire) != expected_turn) {
        std::this_thread::yield();
      }
      Process(effect, input.data() + frame * 160, output.data() + frame * 160, 160);
      turn.fetch_add(1, std::memory_order_release);
    }
  };

  std::thread silence_thread(run_instance,
                             silence_effect,
                             std::ref(silence_input),
                             std::ref(silence_output),
                             0);
  std::thread noise_thread(run_instance,
                           noise_effect,
                           std::ref(noise_input),
                           std::ref(noise_output),
                           1);
  silence_thread.join();
  noise_thread.join();

  for (const float sample : silence_output) {
    NCS_REQUIRE(sample == 0.0F);
  }
  double noise_energy = 0.0;
  for (std::size_t sample = 160; sample < noise_output.size(); ++sample) {
    NCS_REQUIRE(std::isfinite(noise_output[sample]));
    noise_energy += static_cast<double>(noise_output[sample]) * noise_output[sample];
  }
  NCS_REQUIRE(noise_energy > 0.0001);
  CloseEffect(silence_effect);
  CloseEffect(noise_effect);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    return 2;
  }
  plugin_path = std::filesystem::absolute(argv[1]);
  module = LoadLibraryW(plugin_path.c_str());
  if (module == nullptr) {
    return 3;
  }
  plugin_entry = reinterpret_cast<PluginEntry>(GetProcAddress(module, "VSTPluginMain"));
  self_test = reinterpret_cast<SelfTest>(GetProcAddress(module, "NcsWebRtcApmSelfTest"));

  const int result = ncs::test::Run({
      {"exports and metadata", ExportsAndMetadataAreStable},
      {"zero frames and pre-activation", ZeroFramesAndProcessingBeforeActivationAreSafe},
      {"unsupported-rate pass-through", UnsupportedRatesAreBitExactPassThrough},
      {"two-instance isolation", TwoInstancesKeepStateIsolatedAcrossThreads},
  });
  FreeLibrary(module);
  return result;
}
