// SPDX-License-Identifier: MIT
#include "echo_processor.h"
#include "aec_test_support.h"

#include <algorithm>
#include <limits>

namespace {

using ncs::EchoProcessor;
using ncs::aec_test::Db;
using ncs::aec_test::Noise;

// Smooth (low-passed) noise so 48 kHz render can be decimated into a 16 kHz echo without aliasing.
class SmoothNoise final {
 public:
  explicit SmoothNoise(std::uint32_t seed) : noise_(seed) {}
  float Next() noexcept {
    a_ += 0.35F * (noise_.Next() - a_);
    b_ += 0.35F * (a_ - b_);
    return 2.5F * b_;
  }

 private:
  Noise noise_;
  float a_ = 0.0F;
  float b_ = 0.0F;
};

// A three-tap echo path with a bulk delay, applied to one render channel at the capture rate.
// Uses a fixed circular history so the measurement itself never allocates.
class EchoPath final {
 public:
  EchoPath(std::size_t delay, float gain) : delay_(delay), gain_(gain), history_(delay + 32, 0.0F) {}
  float Apply(float sample) {
    position_ = (position_ + 1) % history_.size();
    history_[position_] = sample;
    return gain_ * (At(delay_) - 0.35F * At(delay_ + 5) + 0.15F * At(delay_ + 23));
  }

 private:
  float At(std::size_t age) const { return history_[(position_ + history_.size() - age) % history_.size()]; }

  std::size_t delay_;
  float gain_;
  std::vector<float> history_;
  std::size_t position_ = 0;
};

struct Scenario {
  int capture_rate;
  std::size_t capture_channels;
  int render_rate;
  std::size_t render_channels;
};

// Runs `seconds` of far-end-only audio and returns the echo reduction over the final second.
double MeasureEchoReduction(const Scenario& scenario, int seconds) {
  EchoProcessor processor;
  AEC_REQUIRE(processor.Configure(scenario.capture_rate, scenario.capture_channels, scenario.render_rate,
                                  scenario.render_channels));
  const std::size_t decimation = static_cast<std::size_t>(scenario.render_rate / scenario.capture_rate);
  AEC_REQUIRE(decimation >= 1 && scenario.render_rate % scenario.capture_rate == 0);

  std::vector<SmoothNoise> sources;
  for (std::size_t channel = 0; channel < scenario.render_channels; ++channel) {
    sources.emplace_back(0xBEEF0000U + static_cast<std::uint32_t>(channel));
  }
  // Each microphone channel hears every render channel through its own path (60-75 ms delays).
  std::vector<std::vector<EchoPath>> paths(scenario.capture_channels);
  const std::size_t ms = static_cast<std::size_t>(scenario.capture_rate / 1000);
  for (std::size_t mic = 0; mic < scenario.capture_channels; ++mic) {
    for (std::size_t render = 0; render < scenario.render_channels; ++render) {
      paths[mic].emplace_back((60 + 7 * render + 3 * mic) * ms, render == 0 ? 0.5F : 0.3F);
    }
  }

  std::vector<float> render(processor.render_frames() * scenario.render_channels);
  std::vector<float> capture(processor.capture_frames() * scenario.capture_channels);
  std::vector<float> current(scenario.render_channels);
  double echo_in = 0.0, echo_out = 0.0;
  const int blocks = seconds * 100;
  for (int block = 0; block < blocks; ++block) {
    for (std::size_t frame = 0; frame < processor.render_frames(); ++frame) {
      for (std::size_t channel = 0; channel < scenario.render_channels; ++channel) {
        current[channel] = 0.3F * sources[channel].Next();
        render[frame * scenario.render_channels + channel] = current[channel];
      }
      if (frame % decimation == 0) {
        const std::size_t capture_frame = frame / decimation;
        for (std::size_t mic = 0; mic < scenario.capture_channels; ++mic) {
          float echo = 0.0F;
          for (std::size_t channel = 0; channel < scenario.render_channels; ++channel) {
            echo += paths[mic][channel].Apply(current[channel]);
          }
          capture[capture_frame * scenario.capture_channels + mic] = echo;
        }
      }
    }
    AEC_REQUIRE(processor.ProcessRender(render.data()));
    const std::vector<float> original = capture;
    AEC_REQUIRE(processor.ProcessCapture(capture.data()));
    if (block >= blocks - 100) {
      for (std::size_t i = 0; i < capture.size(); ++i) {
        AEC_REQUIRE(std::isfinite(capture[i]));
        echo_in += static_cast<double>(original[i]) * original[i];
        echo_out += static_cast<double>(capture[i]) * capture[i];
      }
    }
  }
  const double reduction = Db(echo_in / std::max(echo_out, 1e-30));
  std::cout << "  echo reduction " << reduction << " dB (capture " << scenario.capture_rate << " Hz x"
            << scenario.capture_channels << ", render " << scenario.render_rate << " Hz x"
            << scenario.render_channels << ")\n";
  return reduction;
}

void ConfigurationEnablesOnlyFullBandAec3() {
  const auto config = ncs::CreateEchoCancellationConfig();
  AEC_REQUIRE(config.echo_canceller.enabled);
  AEC_REQUIRE(!config.noise_suppression.enabled);
  AEC_REQUIRE(!config.gain_controller1.enabled);
  AEC_REQUIRE(!config.gain_controller2.enabled);
  AEC_REQUIRE(!config.transient_suppression.enabled);
  AEC_REQUIRE(config.pipeline.maximum_internal_processing_rate == 48'000);
  AEC_REQUIRE(config.pipeline.multi_channel_render);
  AEC_REQUIRE(config.pipeline.multi_channel_capture);
}

void RejectsUnsupportedFormats() {
  EchoProcessor processor;
  AEC_REQUIRE(!processor.Configure(44'100, 1, 48'000, 2));
  AEC_REQUIRE(!processor.Configure(48'000, 0, 48'000, 2));
  AEC_REQUIRE(!processor.Configure(48'000, 9, 48'000, 2));
  AEC_REQUIRE(!processor.Configure(48'000, 1, 48'000, 3));
  AEC_REQUIRE(!processor.Configure(48'000, 1, 8'000, 1));
  AEC_REQUIRE(!processor.ready());
  float block[480] = {};
  AEC_REQUIRE(!processor.ProcessCapture(block));
  AEC_REQUIRE(!processor.ProcessRender(block));
}

void CancelsEchoAt48kMonoMicrophone() { AEC_REQUIRE(MeasureEchoReduction({48'000, 1, 48'000, 1}, 6) >= 20.0); }

void CancelsEchoFromStereoRender() { AEC_REQUIRE(MeasureEchoReduction({48'000, 1, 48'000, 2}, 8) >= 20.0); }

void CancelsEchoAt16kMicrophoneWith48kRender() {
  AEC_REQUIRE(MeasureEchoReduction({16'000, 1, 48'000, 2}, 8) >= 15.0);
}

void CancelsEchoOnStereoMicrophone() { AEC_REQUIRE(MeasureEchoReduction({48'000, 2, 48'000, 2}, 8) >= 20.0); }

// With no far-end audio, speech-band near-end audio keeps its level. (AEC3 always applies its
// ~80 Hz high-pass filter and, at 48 kHz, attenuates content above ~19 kHz; neither carries
// meaningful speech energy, and both are disclosed in the documentation.)
void PreservesNearEndWithoutFarEnd() {
  EchoProcessor processor;
  AEC_REQUIRE(processor.Configure(48'000, 1, 48'000, 2));
  SmoothNoise near_end(42);
  std::vector<float> render(processor.render_frames() * 2, 0.0F);
  std::vector<float> capture(processor.capture_frames());
  double in = 0.0, out = 0.0;
  for (int block = 0; block < 300; ++block) {
    for (float& sample : capture) {
      sample = 0.2F * near_end.Next();
    }
    const std::vector<float> original = capture;
    AEC_REQUIRE(processor.ProcessRender(render.data()));
    AEC_REQUIRE(processor.ProcessCapture(capture.data()));
    if (block >= 50) {
      for (std::size_t i = 0; i < capture.size(); ++i) {
        in += static_cast<double>(original[i]) * original[i];
        out += static_cast<double>(capture[i]) * capture[i];
      }
    }
  }
  const double change = Db(out / in);
  std::cout << "  near-end level change " << change << " dB\n";
  AEC_REQUIRE(std::fabs(change) <= 1.0);
}

// The APO reports EchoProcessor::PipelineDelayFrames() as its latency and uses it for the
// delayed-dry fallback, so the table must match the pinned WebRTC pipeline exactly.
void PipelineDelayTableMatchesWebRtc() {
  for (const int rate : {16'000, 32'000, 48'000}) {
    EchoProcessor processor;
    AEC_REQUIRE(processor.Configure(rate, 1, 48'000, 2));
    SmoothNoise source(9);
    std::vector<float> render(processor.render_frames() * 2, 0.0F);
    std::vector<float> capture(processor.capture_frames());
    std::vector<float> input, output;
    for (int block = 0; block < 200; ++block) {
      for (float& sample : capture) {
        sample = 0.2F * source.Next();
      }
      input.insert(input.end(), capture.begin(), capture.end());
      AEC_REQUIRE(processor.ProcessRender(render.data()));
      AEC_REQUIRE(processor.ProcessCapture(capture.data()));
      output.insert(output.end(), capture.begin(), capture.end());
    }
    std::size_t best_lag = 0;
    double best = -1.0;
    const std::size_t start = static_cast<std::size_t>(rate) / 2;
    for (std::size_t lag = 0; lag <= 1'000; ++lag) {
      double xy = 0.0, xx = 0.0, yy = 0.0;
      for (std::size_t i = start; i + 1'000 < input.size(); ++i) {
        xy += static_cast<double>(input[i]) * output[i + lag];
        xx += static_cast<double>(input[i]) * input[i];
        yy += static_cast<double>(output[i + lag]) * output[i + lag];
      }
      const double correlation = xy / std::sqrt(xx * yy);
      if (correlation > best) {
        best = correlation;
        best_lag = lag;
      }
    }
    std::cout << "  " << rate << " Hz pipeline delay " << best_lag << " frames (table "
              << EchoProcessor::PipelineDelayFrames(rate) << ")\n";
    // AEC3's high-pass filter adds a small, frequency-dependent group delay, so the correlation
    // peak may sit a frame or two from the framing delay depending on the test signal's spectrum.
    const std::size_t table = EchoProcessor::PipelineDelayFrames(rate);
    AEC_REQUIRE(best_lag + 2 >= table && best_lag <= table + 2);
  }
}

void NonFiniteInputFailsWithoutCorruptingAudio() {
  EchoProcessor processor;
  AEC_REQUIRE(processor.Configure(16'000, 1, 16'000, 1));
  std::vector<float> render(processor.render_frames(), 0.0F);
  std::vector<float> capture(processor.capture_frames(), std::numeric_limits<float>::quiet_NaN());
  AEC_REQUIRE(processor.ProcessRender(render.data()));
  if (!processor.ProcessCapture(capture.data())) {
    AEC_REQUIRE(std::isnan(capture[0]));  // left untouched for the caller's pass-through
    AEC_REQUIRE(!processor.ready());
  } else {
    for (const float sample : capture) {
      AEC_REQUIRE(std::isfinite(sample));
    }
  }
}

std::atomic<int> g_os_calls{0};
void* g_enter_critical_section = nullptr;
void* g_acquire_srw_exclusive = nullptr;
void* g_acquire_srw_shared = nullptr;
void* g_heap_alloc = nullptr;
void* g_wait_single = nullptr;
void* g_wait_single_ex = nullptr;

void WINAPI CountEnterCriticalSection(LPCRITICAL_SECTION section) {
  ++g_os_calls;
  reinterpret_cast<decltype(&EnterCriticalSection)>(g_enter_critical_section)(section);
}
void WINAPI CountAcquireSrwExclusive(PSRWLOCK lock) {
  ++g_os_calls;
  reinterpret_cast<decltype(&AcquireSRWLockExclusive)>(g_acquire_srw_exclusive)(lock);
}
void WINAPI CountAcquireSrwShared(PSRWLOCK lock) {
  ++g_os_calls;
  reinterpret_cast<decltype(&AcquireSRWLockShared)>(g_acquire_srw_shared)(lock);
}
LPVOID WINAPI CountHeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes) {
  ++g_os_calls;
  return reinterpret_cast<decltype(&HeapAlloc)>(g_heap_alloc)(heap, flags, bytes);
}
DWORD WINAPI CountWaitForSingleObject(HANDLE handle, DWORD timeout) {
  ++g_os_calls;
  return reinterpret_cast<decltype(&WaitForSingleObject)>(g_wait_single)(handle, timeout);
}
DWORD WINAPI CountWaitForSingleObjectEx(HANDLE handle, DWORD timeout, BOOL alertable) {
  ++g_os_calls;
  return reinterpret_cast<decltype(&WaitForSingleObjectEx)>(g_wait_single_ex)(handle, timeout, alertable);
}

// After warm-up, the real-time calls must not lock, wait or allocate. This relies on both pinned
// single-thread WebRTC patches (capture and render).
void WarmProcessingUsesNoLocksWaitsOrHeap() {
  EchoProcessor processor;
  AEC_REQUIRE(processor.Configure(48'000, 1, 48'000, 2));
  SmoothNoise far_end(7);
  EchoPath path(3'000, 0.4F);
  std::vector<float> render(processor.render_frames() * 2);
  std::vector<float> capture(processor.capture_frames());
  auto run = [&](int blocks) {
    bool ok = true;
    for (int block = 0; block < blocks; ++block) {
      for (std::size_t frame = 0; frame < processor.render_frames(); ++frame) {
        const float sample = 0.3F * far_end.Next();
        render[frame * 2] = sample;
        render[frame * 2 + 1] = 0.5F * sample;
        capture[frame] = path.Apply(sample);
      }
      ok = processor.ProcessRender(render.data()) && ok;
      ok = processor.ProcessCapture(capture.data()) && ok;
    }
    return ok;
  };
  AEC_REQUIRE(run(500));  // converge: delay found, filters adapted

  // A function this executable does not import cannot be reached through its import table, so a
  // failed install there means "never called". The critical-section and heap entry points are
  // always imported by the statically linked runtime and WebRTC, so those hooks must install.
  ncs::aec_test::ImportHook hooks[6];
  AEC_REQUIRE(hooks[0].Install("EnterCriticalSection", reinterpret_cast<void*>(&CountEnterCriticalSection), &g_enter_critical_section));
  hooks[1].Install("AcquireSRWLockExclusive", reinterpret_cast<void*>(&CountAcquireSrwExclusive), &g_acquire_srw_exclusive);
  hooks[2].Install("AcquireSRWLockShared", reinterpret_cast<void*>(&CountAcquireSrwShared), &g_acquire_srw_shared);
  AEC_REQUIRE(hooks[3].Install("HeapAlloc", reinterpret_cast<void*>(&CountHeapAlloc), &g_heap_alloc));
  hooks[4].Install("WaitForSingleObject", reinterpret_cast<void*>(&CountWaitForSingleObject), &g_wait_single);
  hooks[5].Install("WaitForSingleObjectEx", reinterpret_cast<void*>(&CountWaitForSingleObjectEx), &g_wait_single_ex);
  g_os_calls = 0;
  const bool ok = run(300);
  const int calls = g_os_calls.load();
  for (auto& hook : hooks) {
    hook.Restore();
  }
  AEC_REQUIRE(ok);
  std::cout << "  OS lock/wait/heap calls during 300 warm blocks: " << calls << '\n';
  AEC_REQUIRE(calls == 0);
}

}  // namespace

int main() {
  return ncs::aec_test::Run({
      {"configuration enables only full-band AEC3", &ConfigurationEnablesOnlyFullBandAec3},
      {"rejects unsupported formats", &RejectsUnsupportedFormats},
      {"cancels echo at 48 kHz mono microphone", &CancelsEchoAt48kMonoMicrophone},
      {"cancels echo from stereo render", &CancelsEchoFromStereoRender},
      {"cancels echo at 16 kHz microphone with 48 kHz render", &CancelsEchoAt16kMicrophoneWith48kRender},
      {"cancels echo on stereo microphone", &CancelsEchoOnStereoMicrophone},
      {"preserves near end without far end", &PreservesNearEndWithoutFarEnd},
      {"pipeline delay table matches WebRTC", &PipelineDelayTableMatchesWebRtc},
      {"non-finite input fails without corrupting audio", &NonFiniteInputFailsWithoutCorruptingAudio},
      {"warm processing uses no locks, waits or heap", &WarmProcessingUsesNoLocksWaitsOrHeap},
  });
}
