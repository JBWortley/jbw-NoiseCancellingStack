// SPDX-License-Identifier: GPL-3.0-only
#include "frame_adapter.h"
#include "webrtc_processor.h"

#include "aeffectx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace {

constexpr float kSupportedSampleRate = 16'000.0F;

void CopyString(void* destination, const char* source, std::size_t capacity) noexcept {
  if (destination == nullptr || capacity == 0) {
    return;
  }
  auto* output = static_cast<char*>(destination);
  const std::size_t length = std::min(std::strlen(source), capacity - 1);
  std::memcpy(output, source, length);
  output[length] = '\0';
}

class PluginInstance final {
 public:
  explicit PluginInstance(audioMasterCallback host_callback) noexcept
      : host_callback_(host_callback), adapter_(processor_) {
    effect_.magic = kEffectMagic;
    effect_.dispatcher = &Dispatch;
    effect_.process = &ProcessReplacing;
    effect_.setParameter = &SetParameter;
    effect_.getParameter = &GetParameter;
    effect_.numPrograms = 1;
    effect_.numParams = 0;
    effect_.numInputs = 1;
    effect_.numOutputs = 1;
    effect_.flags = effFlagsCanReplacing;
    effect_.initialDelay = static_cast<VstInt32>(ncs::FrameAdapter::kLatencySamples);
    effect_.object = this;
    effect_.uniqueID = CCONST('N', 'C', '1', '6');
    effect_.version = 0x010100;
    effect_.processReplacing = &ProcessReplacing;
  }

  AEffect* effect() noexcept { return &effect_; }

 private:
  static PluginInstance* From(AEffect* effect) noexcept {
    return effect == nullptr ? nullptr : static_cast<PluginInstance*>(effect->object);
  }

  static VstIntPtr Dispatch(
      AEffect* effect,
      int opcode,
      int,
      VstIntPtr value,
      void* pointer,
      float option) noexcept {
    try {
      PluginInstance* instance = From(effect);
      if (instance == nullptr) {
        return 0;
      }

      switch (opcode) {
        case effOpen:
          return 0;
        case effClose:
          delete instance;
          return 0;
        case effSetSampleRate:
          instance->sample_rate_ = option;
          instance->adapter_.Reset();
          return 0;
        case effSetBlockSize:
          instance->block_size_ = value > 0 ? static_cast<std::size_t>(value) : 0;
          return 0;
        case effMainsChanged:
          instance->active_ = value != 0;
          instance->adapter_.Reset();
          return 0;
        case effGetEffectName:
          CopyString(pointer, "NCS WebRTC APM 16k", 64);
          return 1;
        case effGetVendorString:
          CopyString(pointer, "JBWortley", kVstMaxVendorStrLen);
          return 1;
        case effGetProductString:
          CopyString(pointer, "Noise Cancelling Stack", kVstMaxProductStrLen);
          return 1;
        case effGetVendorVersion:
          return 0x010100;
        case effGetPlugCategory:
          return kPlugCategRestoration;
        case effGetVstVersion:
          return kVstVersion;
        case effCanDo:
          return -1;
        default:
          return 0;
      }
    } catch (...) {
      return 0;
    }
  }

  static void ProcessReplacing(
      AEffect* effect,
      float** inputs,
      float** outputs,
      int sample_count) noexcept {
    if (sample_count <= 0) {
      return;
    }
    PluginInstance* instance = From(effect);
    if (instance == nullptr || inputs == nullptr || outputs == nullptr || inputs[0] == nullptr
        || outputs[0] == nullptr) {
      return;
    }

    try {
      const auto count = static_cast<std::size_t>(sample_count);
      if (instance->sample_rate_ != kSupportedSampleRate) {
        std::memmove(outputs[0], inputs[0], count * sizeof(float));
        return;
      }
      instance->adapter_.ProcessBlock(inputs[0], outputs[0], count);
    } catch (...) {
      instance->adapter_.EnterPermanentBypass();
      instance->adapter_.ProcessBlock(
          inputs[0], outputs[0], static_cast<std::size_t>(sample_count));
    }
  }

  static void SetParameter(AEffect*, int, float) noexcept {}
  static float GetParameter(AEffect*, int) noexcept { return 0.0F; }

  AEffect effect_{};
  audioMasterCallback host_callback_ = nullptr;
  ncs::WebRtcProcessor processor_;
  ncs::FrameAdapter adapter_;
  float sample_rate_ = 0.0F;
  std::size_t block_size_ = 0;
  bool active_ = false;
};

}  // namespace

extern "C" __declspec(dllexport) AEffect* VSTPluginMain(
    audioMasterCallback host_callback) noexcept {
  try {
    if (host_callback == nullptr) {
      return nullptr;
    }
    auto* instance = new (std::nothrow) PluginInstance(host_callback);
    return instance == nullptr ? nullptr : instance->effect();
  } catch (...) {
    return nullptr;
  }
}

extern "C" __declspec(dllexport) std::int32_t NcsWebRtcApmSelfTest() noexcept {
  try {
    ncs::WebRtcProcessor processor;
    if (!processor.ready()) {
      return 1;
    }
    std::array<float, ncs::FrameAdapter::kFrameSamples> silence{};
    std::array<float, ncs::FrameAdapter::kFrameSamples> output{};
    if (!processor.ProcessFrame(silence, output)) {
      return 2;
    }
    return std::all_of(output.begin(), output.end(), [](float sample) {
             return std::isfinite(sample);
           })
        ? 0
        : 3;
  } catch (...) {
    return 4;
  }
}
