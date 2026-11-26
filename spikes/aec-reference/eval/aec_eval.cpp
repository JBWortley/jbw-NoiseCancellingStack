// SPDX-License-Identifier: MIT
//
// THROWAWAY FEASIBILITY SPIKE - NOT PRODUCT CODE.
//
// Offline AEC3 evaluation on microphone/reference pairs recorded by the spike MFX.
//
//   aec-eval <mic.wav> <ref.wav> [--ref-lead-ms X] [--mono-ref] [--rate32] [--out out.wav]
//
// Feeds 10 ms frames exactly as a real-time effect would (reverse stream first, then capture),
// and reports:
//   - echo presence: peak normalised correlation between mic and reference, and its lag
//   - per-second input vs output power and the converged power reduction (dB)
//   - AEC3's own ERL / ERLE / delay / residual-echo statistics
//   - processing cost as a fraction of real time

#include "api/audio/audio_processing.h"
#include "echo_processor.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Wav {
  uint32_t rate = 0;
  uint16_t channels = 0;
  std::vector<float> samples;  // interleaved
  size_t Frames() const { return channels == 0 ? 0 : samples.size() / channels; }
};

bool ReadFloatWav(const char* path, Wav& wav) {
  FILE* file = nullptr;
  if (fopen_s(&file, path, "rb") != 0 || file == nullptr) return false;
  char riff[12];
  if (std::fread(riff, 1, 12, file) != 12 || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
    std::fclose(file);
    return false;
  }
  bool haveFormat = false;
  for (;;) {
    char id[4];
    uint32_t size = 0;
    if (std::fread(id, 1, 4, file) != 4 || std::fread(&size, 4, 1, file) != 1) break;
    if (std::memcmp(id, "fmt ", 4) == 0) {
      std::vector<uint8_t> fmt(size);
      std::fread(fmt.data(), 1, size, file);
      uint16_t tag = 0, bits = 0;
      std::memcpy(&tag, fmt.data(), 2);
      std::memcpy(&wav.channels, fmt.data() + 2, 2);
      std::memcpy(&wav.rate, fmt.data() + 4, 4);
      std::memcpy(&bits, fmt.data() + 14, 2);
      haveFormat = tag == 3 && bits == 32;
    } else if (std::memcmp(id, "data", 4) == 0 && haveFormat) {
      wav.samples.resize(size / sizeof(float));
      const size_t read = std::fread(wav.samples.data(), sizeof(float), wav.samples.size(), file);
      wav.samples.resize(read);
      std::fclose(file);
      return true;
    } else {
      std::fseek(file, static_cast<long>(size + (size & 1)), SEEK_CUR);
    }
  }
  std::fclose(file);
  return false;
}

bool WriteFloatWav(const char* path, const std::vector<float>& samples, uint32_t rate, uint16_t channels) {
  FILE* file = nullptr;
  if (fopen_s(&file, path, "wb") != 0 || file == nullptr) return false;
  const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
  const uint16_t blockAlign = static_cast<uint16_t>(channels * sizeof(float));
  const uint32_t riffSize = 4 + (8 + 16) + (8 + dataBytes), fmtSize = 16, byteRate = rate * blockAlign;
  const uint16_t tag = 3, bits = 32;
  std::fwrite("RIFF", 1, 4, file); std::fwrite(&riffSize, 4, 1, file); std::fwrite("WAVE", 1, 4, file);
  std::fwrite("fmt ", 1, 4, file); std::fwrite(&fmtSize, 4, 1, file); std::fwrite(&tag, 2, 1, file);
  std::fwrite(&channels, 2, 1, file); std::fwrite(&rate, 4, 1, file); std::fwrite(&byteRate, 4, 1, file);
  std::fwrite(&blockAlign, 2, 1, file); std::fwrite(&bits, 2, 1, file);
  std::fwrite("data", 1, 4, file); std::fwrite(&dataBytes, 4, 1, file);
  std::fwrite(samples.data(), sizeof(float), samples.size(), file);
  std::fclose(file);
  return true;
}

double Db(double ratio) { return ratio <= 1e-30 ? -300.0 : 10.0 * std::log10(ratio); }

// Mono 8 kHz envelope-free downsample by block averaging (crude anti-aliasing is fine here).
std::vector<float> MonoAt8k(const Wav& wav, size_t skipFrames) {
  const uint32_t factor = std::max<uint32_t>(1, wav.rate / 8000);
  std::vector<float> out;
  for (size_t frame = skipFrames; frame + factor <= wav.Frames(); frame += factor) {
    double sum = 0.0;
    for (uint32_t k = 0; k < factor; ++k)
      for (uint16_t c = 0; c < wav.channels; ++c) sum += wav.samples[(frame + k) * wav.channels + c];
    out.push_back(static_cast<float>(sum / (factor * wav.channels)));
  }
  return out;
}

// Energy of `y` that is linearly predictable from `r` through a causal FIR of `taps` taps
// starting at `lag0` (least-squares projection, normal equations solved by Cholesky). Near-end
// speech and room noise are uncorrelated with the reference, so this isolates the echo.
double ProjectedEchoEnergy(const std::vector<float>& r, const std::vector<float>& y, size_t lag0, size_t taps,
                           size_t begin, size_t end) {
  std::vector<double> acf(taps, 0.0), b(taps, 0.0);
  for (size_t n = begin; n < end; ++n) {
    if (n < lag0 + taps) continue;
    for (size_t k = 0; k < taps; ++k) {
      const double rk = r[n - lag0 - k];
      b[k] += y[n] * rk;
    }
  }
  // Biased autocorrelation over one contiguous span of the reference: the resulting Toeplitz
  // matrix is positive semi-definite, so the Cholesky factorisation below is well posed.
  const size_t spanBegin = std::max(begin, lag0 + taps) - lag0 - taps;
  const size_t spanEnd = end - lag0;
  for (size_t k = 0; k < taps; ++k)
    for (size_t m = spanBegin; m + k < spanEnd; ++m) acf[k] += static_cast<double>(r[m]) * r[m + k];
  std::vector<double> a(taps * taps);
  for (size_t i = 0; i < taps; ++i)
    for (size_t j = 0; j < taps; ++j) a[i * taps + j] = acf[i > j ? i - j : j - i];
  for (size_t i = 0; i < taps; ++i) a[i * taps + i] += 1e-4 * acf[0] + 1e-12;  // diagonal loading
  // Cholesky A = L L^T in place (lower triangle).
  for (size_t j = 0; j < taps; ++j) {
    double d = a[j * taps + j];
    for (size_t k = 0; k < j; ++k) d -= a[j * taps + k] * a[j * taps + k];
    d = std::sqrt(std::max(d, 1e-30));
    a[j * taps + j] = d;
    for (size_t i = j + 1; i < taps; ++i) {
      double s = a[i * taps + j];
      for (size_t k = 0; k < j; ++k) s -= a[i * taps + k] * a[j * taps + k];
      a[i * taps + j] = s / d;
    }
  }
  // Explained energy = b^T A^-1 b = |L^-1 b|^2.
  std::vector<double> z(taps);
  double energy = 0.0;
  for (size_t i = 0; i < taps; ++i) {
    double s = b[i];
    for (size_t k = 0; k < i; ++k) s -= a[i * taps + k] * z[k];
    z[i] = s / a[i * taps + i];
    energy += z[i] * z[i];
  }
  return energy;
}

// Peak normalised cross-correlation of mic against reference for reference-leading lags.
size_t EchoPresence(const Wav& mic, const Wav& ref, size_t refSkip) {
  const std::vector<float> m = MonoAt8k(mic, 0);
  const std::vector<float> r = MonoAt8k(ref, refSkip);
  const size_t maxLag = 8000;  // 1 s at 8 kHz
  const size_t n = std::min(m.size(), r.size());
  if (n <= maxLag + 8000) {
    std::printf("  echo presence: too short to correlate\n");
    return 0;
  }
  const size_t len = n - maxLag;
  double best = 0.0;
  size_t bestLag = 0;
  for (size_t lag = 0; lag < maxLag; ++lag) {
    double xy = 0.0, xx = 0.0, yy = 0.0;
    for (size_t i = 0; i < len; ++i) {
      const double x = r[i], y = m[i + lag];
      xy += x * y; xx += x * x; yy += y * y;
    }
    const double c = (xx > 0 && yy > 0) ? std::fabs(xy) / std::sqrt(xx * yy) : 0.0;
    if (c > best) { best = c; bestLag = lag; }
  }
  std::printf("  echo presence: peak correlation %.3f at mic lag %.1f ms behind reference\n", best, bestLag / 8.0);
  return bestLag;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: aec-eval <mic.wav> <ref.wav> [--ref-lead-ms X] [--mono-ref] [--rate32] [--out out.wav]\n");
    return 2;
  }
  double refLeadMs = 0.0;
  bool monoRef = false, rate32 = false;
  const char* outPath = nullptr;
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--ref-lead-ms" && i + 1 < argc) refLeadMs = std::atof(argv[++i]);
    else if (arg == "--mono-ref") monoRef = true;
    else if (arg == "--rate32") rate32 = true;
    else if (arg == "--out" && i + 1 < argc) outPath = argv[++i];
  }

  Wav mic, ref;
  if (!ReadFloatWav(argv[1], mic) || !ReadFloatWav(argv[2], ref)) {
    std::fprintf(stderr, "could not read float WAV input\n");
    return 1;
  }
  // The reference recording started earlier than the microphone recording by refLeadMs; drop
  // that much of the reference so both files start at the same engine timestamp.
  const size_t refSkip = static_cast<size_t>(std::max(0.0, refLeadMs) * ref.rate / 1000.0);
  std::printf("mic %u Hz x%u (%.1f s), ref %u Hz x%u (%.1f s), ref lead %.1f ms%s%s\n", mic.rate, mic.channels,
              mic.Frames() / double(mic.rate), ref.rate, ref.channels, ref.Frames() / double(ref.rate), refLeadMs,
              monoRef ? ", mono reference" : "", rate32 ? ", 32 kHz internal" : ", 48 kHz internal");
  const size_t echoLag = EchoPresence(mic, ref, refSkip);

  // Production configuration (native/webrtc-apm-vst2/aec/echo_processor.cpp), with the spike's
  // comparison switches applied on top.
  webrtc::AudioProcessing::Config config = ncs::CreateEchoCancellationConfig();
  if (rate32) config.pipeline.maximum_internal_processing_rate = 32000;
  if (monoRef) config.pipeline.multi_channel_render = false;
  auto apm = webrtc::BuiltinAudioProcessingBuilder(config).Build(webrtc::CreateEnvironment());
  if (!apm) {
    std::fprintf(stderr, "APM construction failed\n");
    return 1;
  }

  const size_t micFrame = mic.rate / 100, refFrame = ref.rate / 100;
  const uint16_t refChannels = monoRef ? 1 : ref.channels;
  const webrtc::StreamConfig micConfig(static_cast<int>(mic.rate), mic.channels);
  const webrtc::StreamConfig refConfig(static_cast<int>(ref.rate), refChannels);
  std::vector<std::vector<float>> micIn(mic.channels, std::vector<float>(micFrame));
  std::vector<std::vector<float>> micOut(mic.channels, std::vector<float>(micFrame));
  std::vector<std::vector<float>> refIn(refChannels, std::vector<float>(refFrame));
  std::vector<const float*> micInPtr, refInPtr;
  std::vector<float*> micOutPtr, refOutPtr;
  for (auto& c : micIn) micInPtr.push_back(c.data());
  for (auto& c : micOut) micOutPtr.push_back(c.data());
  for (auto& c : refIn) { refInPtr.push_back(c.data()); refOutPtr.push_back(c.data()); }

  const size_t frames = std::min(mic.Frames() / micFrame, (ref.Frames() - std::min(ref.Frames(), refSkip)) / refFrame);
  std::vector<float> output;
  output.reserve(frames * micFrame * mic.channels);
  double secondIn = 0.0, secondOut = 0.0, convergedIn = 0.0, convergedOut = 0.0;
  double processSeconds = 0.0;
  const size_t convergeFrames = 300;  // ignore the first 3 s while AEC3 adapts
  std::printf("  second  mic dBFS  out dBFS  reduction | AEC3 ERL  ERLE  delay  residual-echo\n");

  for (size_t f = 0; f < frames; ++f) {
    for (size_t i = 0; i < refFrame; ++i) {
      const size_t src = (refSkip + f * refFrame + i) * ref.channels;
      if (monoRef) {
        float sum = 0.0F;
        for (uint16_t c = 0; c < ref.channels; ++c) sum += ref.samples[src + c];
        refIn[0][i] = sum / ref.channels;
      } else {
        for (uint16_t c = 0; c < ref.channels; ++c) refIn[c][i] = ref.samples[src + c];
      }
    }
    for (size_t i = 0; i < micFrame; ++i)
      for (uint16_t c = 0; c < mic.channels; ++c) micIn[c][i] = mic.samples[(f * micFrame + i) * mic.channels + c];

    const auto start = std::chrono::steady_clock::now();
    if (apm->ProcessReverseStream(refInPtr.data(), refConfig, refConfig, refOutPtr.data()) != 0
        || apm->set_stream_delay_ms(0) != 0
        || apm->ProcessStream(micInPtr.data(), micConfig, micConfig, micOutPtr.data()) != 0) {
      std::fprintf(stderr, "APM processing error at frame %zu\n", f);
      return 1;
    }
    processSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    double pin = 0.0, pout = 0.0;
    for (size_t i = 0; i < micFrame; ++i) {
      for (uint16_t c = 0; c < mic.channels; ++c) {
        pin += micIn[c][i] * micIn[c][i];
        pout += micOut[c][i] * micOut[c][i];
        output.push_back(micOut[c][i]);
      }
    }
    secondIn += pin; secondOut += pout;
    if (f >= convergeFrames) { convergedIn += pin; convergedOut += pout; }

    if ((f + 1) % 100 == 0) {
      const auto stats = apm->GetStatistics();
      const double count = 100.0 * micFrame * mic.channels;
      std::printf("  %6zu  %8.1f  %8.1f  %7.1f dB | %8.1f %5.1f %5d ms  %.2f\n", (f + 1) / 100, Db(secondIn / count),
                  Db(secondOut / count), Db(secondIn / std::max(secondOut, 1e-30)),
                  stats.echo_return_loss.value_or(NAN), stats.echo_return_loss_enhancement.value_or(NAN),
                  stats.delay_ms.value_or(-1), stats.residual_echo_likelihood.value_or(NAN));
      secondIn = secondOut = 0.0;
    }
  }

  const double audioSeconds = frames / 100.0;
  std::printf("RESULT converged reduction %.1f dB over %.1f s | processing %.2f%% of real time (%.3f ms per 10 ms frame)\n",
              Db(convergedIn / std::max(convergedOut, 1e-30)), (frames - convergeFrames) / 100.0,
              100.0 * processSeconds / audioSeconds, 1000.0 * processSeconds / frames);
  // Echo-specific measure over the last 10 s (after every run's delay search has settled):
  // energy linearly explainable by the reference, before and after AEC3.
  {
    Wav processed;
    processed.rate = mic.rate;
    processed.channels = mic.channels;
    processed.samples = output;
    Wav micUsed = mic;
    micUsed.samples.resize(output.size());
    const std::vector<float> r8 = MonoAt8k(ref, refSkip);
    const std::vector<float> in8 = MonoAt8k(micUsed, 0);
    const std::vector<float> out8 = MonoAt8k(processed, 0);
    const size_t n = std::min({r8.size(), in8.size(), out8.size()});
    const size_t begin = n > 80000 ? n - 80000 : 0;
    const size_t lag0 = echoLag > 64 ? echoLag - 64 : 0;  // 8 ms before the main echo peak
    const size_t taps = 480;                               // 60 ms of echo path at 8 kHz
    double inPower = 0.0, outPower = 0.0;
    for (size_t i = begin; i < n; ++i) { inPower += in8[i] * in8[i]; outPower += out8[i] * out8[i]; }
    const double echoIn = ProjectedEchoEnergy(r8, in8, lag0, taps, begin, n);
    const double echoOut = ProjectedEchoEnergy(r8, out8, lag0, taps, begin, n);
    const double count = static_cast<double>(n - begin);
    std::printf("ECHO last 10 s: echo %.1f dBFS (%.0f%% of mic energy) -> %.1f dBFS after AEC3 | echo removed %.1f dB | "
                "non-echo energy kept %.1f dB of %.1f dB\n",
                Db(echoIn / count), 100.0 * echoIn / std::max(inPower, 1e-30), Db(echoOut / count),
                Db(echoIn / std::max(echoOut, 1e-30)), Db((outPower - echoOut) / count), Db((inPower - echoIn) / count));
  }
  if (outPath != nullptr) {
    std::printf("wrote %s: %s\n", outPath, WriteFloatWav(outPath, output, mic.rate, mic.channels) ? "ok" : "FAILED");
  }
  return 0;
}
