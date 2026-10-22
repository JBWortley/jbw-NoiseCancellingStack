// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "frame_adapter.h"
#include "webrtc_processor.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>

namespace {

using ncs::FrameAdapter;
using ncs::WebRtcProcessor;

class ImportHook final {
 public:
  ImportHook() = default;
  ImportHook(const ImportHook&) = delete;
  ImportHook& operator=(const ImportHook&) = delete;

  ~ImportHook() { Restore(); }

  bool Install(const char* function_name, void* replacement) noexcept {
    auto* base = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
      return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
      return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
      return false;
    }
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
      return false;
    }

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
      if (descriptor->OriginalFirstThunk == 0) {
        continue;
      }
      auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(
          base + descriptor->OriginalFirstThunk);
      auto* functions = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
      for (; names->u1.AddressOfData != 0; ++names, ++functions) {
        if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
          continue;
        }
        const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
            base + names->u1.AddressOfData);
        if (std::strcmp(reinterpret_cast<const char*>(import->Name), function_name) != 0) {
          continue;
        }

        slot_ = reinterpret_cast<void**>(&functions->u1.Function);
        original_ = *slot_;
        DWORD previous_protection = 0;
        if (!VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &previous_protection)) {
          slot_ = nullptr;
          original_ = nullptr;
          return false;
        }
        *slot_ = replacement;
        DWORD ignored = 0;
        VirtualProtect(slot_, sizeof(*slot_), previous_protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), slot_, sizeof(*slot_));
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] void* original() const noexcept { return original_; }

 private:
  void Restore() noexcept {
    if (slot_ == nullptr) {
      return;
    }
    DWORD previous_protection = 0;
    if (VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &previous_protection)) {
      *slot_ = original_;
      DWORD ignored = 0;
      VirtualProtect(slot_, sizeof(*slot_), previous_protection, &ignored);
      FlushInstructionCache(GetCurrentProcess(), slot_, sizeof(*slot_));
    }
    slot_ = nullptr;
  }

  void** slot_ = nullptr;
  void* original_ = nullptr;
};

using EnterCriticalSectionFunction = void(WINAPI*)(LPCRITICAL_SECTION);
using AcquireSrwLockExclusiveFunction = void(WINAPI*)(PSRWLOCK);

std::atomic<bool> track_synchronization = false;
std::atomic<std::size_t> critical_section_calls = 0;
std::atomic<std::size_t> srw_lock_calls = 0;
EnterCriticalSectionFunction original_enter_critical_section = nullptr;
AcquireSrwLockExclusiveFunction original_acquire_srw_lock_exclusive = nullptr;

void WINAPI TrackEnterCriticalSection(LPCRITICAL_SECTION critical_section) {
  if (track_synchronization.load(std::memory_order_relaxed)) {
    critical_section_calls.fetch_add(1, std::memory_order_relaxed);
  }
  original_enter_critical_section(critical_section);
}

void WINAPI TrackAcquireSrwLockExclusive(PSRWLOCK lock) {
  if (track_synchronization.load(std::memory_order_relaxed)) {
    srw_lock_calls.fetch_add(1, std::memory_order_relaxed);
  }
  original_acquire_srw_lock_exclusive(lock);
}

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

void WarmCallbackUsesNoOperatingSystemSynchronization() {
  WebRtcProcessor processor;
  NCS_REQUIRE(processor.ready());
  std::array<float, FrameAdapter::kFrameSamples> input{};
  std::array<float, FrameAdapter::kFrameSamples> output{};
  NCS_REQUIRE(processor.ProcessFrame(input, output));

  ImportHook critical_section_hook;
  ImportHook srw_lock_hook;
  NCS_REQUIRE(critical_section_hook.Install(
      "EnterCriticalSection", reinterpret_cast<void*>(&TrackEnterCriticalSection)));
  NCS_REQUIRE(srw_lock_hook.Install(
      "AcquireSRWLockExclusive", reinterpret_cast<void*>(&TrackAcquireSrwLockExclusive)));
  original_enter_critical_section = reinterpret_cast<EnterCriticalSectionFunction>(
      critical_section_hook.original());
  original_acquire_srw_lock_exclusive = reinterpret_cast<AcquireSrwLockExclusiveFunction>(
      srw_lock_hook.original());

  critical_section_calls.store(0, std::memory_order_relaxed);
  srw_lock_calls.store(0, std::memory_order_relaxed);
  track_synchronization.store(true, std::memory_order_release);
  const bool processed = processor.ProcessFrame(input, output);
  track_synchronization.store(false, std::memory_order_release);

  NCS_REQUIRE(processed);
  NCS_REQUIRE(critical_section_calls.load(std::memory_order_relaxed) == 0);
  NCS_REQUIRE(srw_lock_calls.load(std::memory_order_relaxed) == 0);
}

}  // namespace

int main() {
  static_assert(FrameAdapter::kFrameSamples == 160);
  return ncs::test::Run({
      {"noise suppression configuration", ConfigurationEnablesOnlyHighNoiseSuppression},
      {"construction and silence", ConstructionWarmsApmAndSilenceStaysFinite},
      {"stationary noise attenuation", StationaryNoiseIsAttenuatedAfterAdaptation},
      {"warm callback has no operating-system synchronization",
       WarmCallbackUsesNoOperatingSystemSynchronization},
  });
}
