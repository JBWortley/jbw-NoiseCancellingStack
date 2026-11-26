// SPDX-License-Identifier: MIT
//
// THROWAWAY FEASIBILITY SPIKE - NOT PRODUCT CODE.
//
// ncs-aec-probe
//   list
//       Print active capture/render endpoints, their mix formats, and the defaults.
//   run <capture-endpoint-guid> [--seconds N] [--category default|communications]
//       [--play | --play-wav file] [--level-db -30] [--wav path] [--loopback-wav path]
//       Open one shared-mode capture stream on the endpoint in the chosen audio category, so the
//       audio engine builds the capture graph (and the spike MFX) in DEFAULT or COMMUNICATIONS
//       mode. With --play, a noise-burst test signal is rendered to the default console render
//       endpoint at the same time so a loopback reference exists; --play-wav loops a WAV file
//       instead (its active level normalized to --level-db). --loopback-wav records what the
//       default render endpoint played. Both recordings are placed by device position and a
//       <wav>.json sidecar holds their QPC start times for offline alignment. Runs as a normal user.

#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <propkeydef.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

template <typename T>
struct ComPtr {
  T* p = nullptr;
  ~ComPtr() { if (p != nullptr) p->Release(); }
  T** operator&() { return &p; }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

std::string Utf8(const wchar_t* text) {
  if (text == nullptr) return "(null)";
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string result(size > 0 ? size - 1 : 0, '\0');
  if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
  return result;
}

bool Check(HRESULT result, const char* what) {
  if (FAILED(result)) {
    std::fprintf(stderr, "error: %s failed (0x%08lx)\n", what, result);
    return false;
  }
  return true;
}

std::string DeviceName(IMMDevice* device) {
  ComPtr<IPropertyStore> store;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return "?";
  PROPVARIANT value;
  PropVariantInit(&value);
  std::string name = "?";
  if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
    name = Utf8(value.pwszVal);
  }
  PropVariantClear(&value);
  return name;
}

std::string DeviceId(IMMDevice* device) {
  LPWSTR id = nullptr;
  if (FAILED(device->GetId(&id))) return "?";
  std::string text = Utf8(id);
  CoTaskMemFree(id);
  return text;
}

bool IsFloat(const WAVEFORMATEX* format) {
  if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
  if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  }
  return false;
}

std::string DescribeMixFormat(IMMDevice* device) {
  ComPtr<IAudioClient> client;
  if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) {
    return "(activate failed)";
  }
  WAVEFORMATEX* format = nullptr;
  if (FAILED(client->GetMixFormat(&format))) return "(no mix format)";
  char text[96];
  std::snprintf(text, sizeof(text), "%s %lu Hz %u ch", IsFloat(format) ? "float" : "PCM",
                format->nSamplesPerSec, format->nChannels);
  CoTaskMemFree(format);
  return text;
}

// PKEY_Devices_AudioDevice_RawProcessingSupported, which applications (Chromium included) read to
// decide whether to request AUDCLNT_STREAMOPTIONS_RAW.
const PROPERTYKEY kRawProcessingSupported = {{0x8943b373, 0x388c, 0x4395, {0xb5, 0x57, 0xbc, 0x6d, 0xba, 0xff, 0xaf, 0xdb}}, 2};

std::string RawSupport(IMMDevice* device) {
  ComPtr<IPropertyStore> store;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return "?";
  PROPVARIANT value;
  PropVariantInit(&value);
  std::string text = "absent";
  if (SUCCEEDED(store->GetValue(kRawProcessingSupported, &value)) && value.vt == VT_BOOL) {
    text = value.boolVal != VARIANT_FALSE ? "yes" : "no";
  } else if (value.vt != VT_EMPTY) {
    text = "vt=" + std::to_string(value.vt);
  }
  PropVariantClear(&value);
  return text;
}

int List(IMMDeviceEnumerator* enumerator) {
  const struct { EDataFlow flow; const char* label; } flows[] = {{eCapture, "Capture"}, {eRender, "Render"}};
  for (const auto& entry : flows) {
    std::printf("%s endpoints:\n", entry.label);
    ComPtr<IMMDeviceCollection> devices;
    if (!Check(enumerator->EnumAudioEndpoints(entry.flow, DEVICE_STATE_ACTIVE, &devices), "EnumAudioEndpoints")) return 1;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
      ComPtr<IMMDevice> device;
      if (FAILED(devices->Item(index, &device))) continue;
      std::printf("  %s\n    %s | mix %s | raw mode advertised: %s\n", DeviceName(device.p).c_str(), DeviceId(device.p).c_str(),
                  DescribeMixFormat(device.p).c_str(), RawSupport(device.p).c_str());
    }
    const struct { ERole role; const char* label; } roles[] = {{eConsole, "console"}, {eCommunications, "communications"}};
    for (const auto& role : roles) {
      ComPtr<IMMDevice> device;
      if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(entry.flow, role.role, &device))) {
        std::printf("  default %s: %s\n", role.label, DeviceName(device.p).c_str());
      }
    }
  }
  return 0;
}

bool WriteFloatWav(const std::string& path, const std::vector<float>& samples, uint32_t rate, uint16_t channels) {
  FILE* file = nullptr;
  if (fopen_s(&file, path.c_str(), "wb") != 0 || file == nullptr) return false;
  const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
  const uint16_t blockAlign = static_cast<uint16_t>(channels * sizeof(float));
  const uint32_t riffSize = 4 + (8 + 16) + (8 + dataBytes);
  const uint32_t fmtSize = 16, byteRate = rate * blockAlign;
  const uint16_t formatTag = 3, bits = 32;
  std::fwrite("RIFF", 1, 4, file); std::fwrite(&riffSize, 4, 1, file); std::fwrite("WAVE", 1, 4, file);
  std::fwrite("fmt ", 1, 4, file); std::fwrite(&fmtSize, 4, 1, file); std::fwrite(&formatTag, 2, 1, file);
  std::fwrite(&channels, 2, 1, file); std::fwrite(&rate, 4, 1, file); std::fwrite(&byteRate, 4, 1, file);
  std::fwrite(&blockAlign, 2, 1, file); std::fwrite(&bits, 2, 1, file);
  std::fwrite("data", 1, 4, file); std::fwrite(&dataBytes, 4, 1, file);
  std::fwrite(samples.data(), sizeof(float), samples.size(), file);
  std::fclose(file);
  return true;
}

double ToDb(double power) { return power <= 1e-20 ? -200.0 : 10.0 * std::log10(power); }

// Reads a PCM16 or float32 WAV (plain or extensible) and mixes it to mono.
bool ReadMonoWav(const std::string& path, std::vector<float>& mono, uint32_t& rate) {
  FILE* file = nullptr;
  if (fopen_s(&file, path.c_str(), "rb") != 0 || file == nullptr) return false;
  std::vector<uint8_t> bytes;
  uint8_t chunk[65536];
  size_t got = 0;
  while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) bytes.insert(bytes.end(), chunk, chunk + got);
  std::fclose(file);
  if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return false;
  uint16_t tag = 0, channels = 0, bits = 0;
  const uint8_t* data = nullptr;
  uint32_t dataBytes = 0;
  for (size_t offset = 12; offset + 8 <= bytes.size();) {
    uint32_t size = 0;
    std::memcpy(&size, bytes.data() + offset + 4, 4);
    const uint8_t* body = bytes.data() + offset + 8;
    if (offset + 8 + size > bytes.size()) size = static_cast<uint32_t>(bytes.size() - offset - 8);
    if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && size >= 16) {
      std::memcpy(&tag, body, 2); std::memcpy(&channels, body + 2, 2);
      std::memcpy(&rate, body + 4, 4); std::memcpy(&bits, body + 14, 2);
      if (tag == WAVE_FORMAT_EXTENSIBLE && size >= 40) {
        GUID sub;
        std::memcpy(&sub, body + 24, sizeof(GUID));
        tag = sub == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ? WAVE_FORMAT_IEEE_FLOAT : sub == KSDATAFORMAT_SUBTYPE_PCM ? WAVE_FORMAT_PCM : 0;
      }
    } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
      data = body; dataBytes = size;
    }
    offset += 8 + size + (size & 1);
  }
  const bool pcm16 = tag == WAVE_FORMAT_PCM && bits == 16;
  const bool float32 = tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32;
  if (data == nullptr || channels == 0 || rate == 0 || (!pcm16 && !float32)) return false;
  const size_t frames = dataBytes / (channels * (bits / 8));
  mono.assign(frames, 0.0F);
  for (size_t frame = 0; frame < frames; ++frame) {
    float sum = 0.0F;
    for (uint16_t channel = 0; channel < channels; ++channel) {
      const size_t index = frame * channels + channel;
      if (pcm16) { int16_t value; std::memcpy(&value, data + index * 2, 2); sum += value / 32768.0F; }
      else { float value; std::memcpy(&value, data + index * 4, 4); sum += value; }
    }
    mono[frame] = sum / channels;
  }
  return true;
}

// Linear resampling to the render rate, then scaling so 20 ms frames within 30 dB of the loudest
// frame (the active speech) have an RMS of levelDb.
std::vector<float> PrepareWav(const std::vector<float>& source, uint32_t sourceRate, uint32_t targetRate, double levelDb) {
  const size_t frames = static_cast<size_t>(static_cast<double>(source.size()) * targetRate / sourceRate);
  std::vector<float> out(frames);
  for (size_t index = 0; index < frames; ++index) {
    const double position = static_cast<double>(index) * sourceRate / targetRate;
    const size_t left = static_cast<size_t>(position);
    const size_t right = std::min(left + 1, source.size() - 1);
    const double fraction = position - left;
    out[index] = static_cast<float>(source[left] * (1.0 - fraction) + source[right] * fraction);
  }
  const size_t block = targetRate / 50;
  std::vector<double> powers;
  for (size_t start = 0; start + block <= out.size(); start += block) {
    double power = 0.0;
    for (size_t index = start; index < start + block; ++index) power += static_cast<double>(out[index]) * out[index];
    powers.push_back(power / block);
  }
  double loudest = 0.0;
  for (double power : powers) loudest = std::max(loudest, power);
  double activePower = 0.0;
  size_t activeBlocks = 0;
  for (double power : powers) {
    if (power > loudest * 1e-3) { activePower += power; ++activeBlocks; }
  }
  if (activeBlocks > 0 && activePower > 0.0) {
    const double gain = std::pow(10.0, levelDb / 20.0) / std::sqrt(activePower / activeBlocks);
    for (float& sample : out) sample = static_cast<float>(std::clamp(sample * gain, -1.0, 1.0));
  }
  return out;
}

// Accumulates a capture stream by device position so dropped or discontinuous packets leave
// silence in the right place instead of shifting the rest of the recording.
struct PositionedRecording {
  uint32_t channels = 0;
  bool started = false;
  uint64_t firstPosition = 0;
  uint64_t firstQpc = 0;
  uint64_t discontinuities = 0;
  std::vector<float> samples;

  void Add(const float* data, UINT32 frames, DWORD flags, UINT64 position, UINT64 qpc) {
    if (!started) { started = true; firstPosition = position; firstQpc = qpc; }
    if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) ++discontinuities;
    if (position < firstPosition) return;
    const size_t base = static_cast<size_t>(position - firstPosition) * channels;
    if (samples.size() < base + static_cast<size_t>(frames) * channels) samples.resize(base + static_cast<size_t>(frames) * channels, 0.0F);
    for (size_t index = 0; index < static_cast<size_t>(frames) * channels; ++index) {
      samples[base + index] = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 ? 0.0F : data[index];
    }
  }
};

struct Options {
  std::wstring endpoint;
  int seconds = 10;
  bool communications = false;
  bool raw = false;
  bool play = false;
  double levelDb = -30.0;
  std::string wavPath;
  std::string playWavPath;
  std::string loopbackWavPath;
};

// Test signal: 1.5 s of band-limited noise, then 1.0 s of silence, repeating.
constexpr double kBurstSeconds = 1.5;
constexpr double kGapSeconds = 1.0;
bool BurstActive(double t) { return std::fmod(t, kBurstSeconds + kGapSeconds) < kBurstSeconds; }

int Run(IMMDeviceEnumerator* enumerator, const Options& options) {
  std::wstring id = options.endpoint;
  if (id.find(L"{0.0.") != 0) id = L"{0.0.1.00000000}." + id;
  ComPtr<IMMDevice> capture;
  if (!Check(enumerator->GetDevice(id.c_str(), &capture), "GetDevice(capture)")) return 1;

  ComPtr<IAudioClient2> captureClient;
  if (!Check(capture->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&captureClient)), "Activate(capture)")) return 1;
  AudioClientProperties properties = {};
  properties.cbSize = sizeof(properties);
  properties.bIsOffload = FALSE;
  properties.eCategory = options.communications ? AudioCategory_Communications : AudioCategory_Other;
  properties.Options = options.raw ? AUDCLNT_STREAMOPTIONS_RAW : AUDCLNT_STREAMOPTIONS_NONE;
  if (!Check(captureClient->SetClientProperties(&properties), "SetClientProperties")) return 1;

  WAVEFORMATEX* captureFormat = nullptr;
  if (!Check(captureClient->GetMixFormat(&captureFormat), "GetMixFormat(capture)")) return 1;
  if (!IsFloat(captureFormat)) {
    std::fprintf(stderr, "error: capture mix format is not float\n");
    return 1;
  }
  const UINT32 captureRate = captureFormat->nSamplesPerSec;
  const UINT32 captureChannels = captureFormat->nChannels;
  if (!Check(captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2'000'000, 0, captureFormat, nullptr), "Initialize(capture)")) return 1;
  ComPtr<IAudioCaptureClient> captureService;
  if (!Check(captureClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&captureService)), "GetService(capture)")) return 1;

  ComPtr<IMMDevice> render;
  ComPtr<IAudioClient> renderClient;
  ComPtr<IAudioRenderClient> renderService;
  ComPtr<IAudioClient> loopbackClient;
  ComPtr<IAudioCaptureClient> loopbackService;
  WAVEFORMATEX* renderFormat = nullptr;
  UINT32 renderBufferFrames = 0;
  const bool loopback = !options.loopbackWavPath.empty();
  if (options.play || loopback) {
    if (!Check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &render), "GetDefaultAudioEndpoint(render)")) return 1;
    if (!Check(render->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&renderClient)), "Activate(render)")) return 1;
    if (!Check(renderClient->GetMixFormat(&renderFormat), "GetMixFormat(render)")) return 1;
    if (!IsFloat(renderFormat)) {
      std::fprintf(stderr, "error: render mix format is not float\n");
      return 1;
    }
  }
  if (options.play) {
    if (!Check(renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2'000'000, 0, renderFormat, nullptr), "Initialize(render)")) return 1;
    if (!Check(renderClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderService)), "GetService(render)")) return 1;
    renderClient->GetBufferSize(&renderBufferFrames);
  }
  if (loopback) {
    if (!Check(render->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&loopbackClient)), "Activate(loopback)")) return 1;
    if (!Check(loopbackClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2'000'000, 0, renderFormat, nullptr), "Initialize(loopback)")) return 1;
    if (!Check(loopbackClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&loopbackService)), "GetService(loopback)")) return 1;
  }

  std::vector<float> program;
  if (!options.playWavPath.empty()) {
    std::vector<float> source;
    uint32_t sourceRate = 0;
    if (!ReadMonoWav(options.playWavPath, source, sourceRate) || source.size() < 2) {
      std::fprintf(stderr, "error: could not read %s (PCM16 or float32 WAV)\n", options.playWavPath.c_str());
      return 1;
    }
    program = PrepareWav(source, sourceRate, renderFormat->nSamplesPerSec, options.levelDb);
  }

  REFERENCE_TIME latency = 0;
  captureClient->GetStreamLatency(&latency);
  std::printf("capture: %s (%u Hz, %u ch) category=%s, stream latency %.2f ms\n", DeviceName(capture.p).c_str(), captureRate,
              captureChannels, options.communications ? "Communications" : "Other (default mode)", latency / 10000.0);
  if (options.raw) std::printf("raw mode: system effects bypassed\n");
  if (options.play) {
    std::printf("playing: %s at %.0f dBFS on %s (%lu Hz, %u ch)\n",
                program.empty() ? "noise bursts" : options.playWavPath.c_str(), options.levelDb,
                DeviceName(render.p).c_str(), renderFormat->nSamplesPerSec, renderFormat->nChannels);
  }

  PositionedRecording recorded;
  recorded.channels = captureChannels;
  PositionedRecording loopbackRecorded;
  if (loopback) loopbackRecorded.channels = renderFormat->nChannels;
  std::mt19937 random(1234);
  std::normal_distribution<float> noise(0.0F, 1.0F);
  const float amplitude = static_cast<float>(std::pow(10.0, options.levelDb / 20.0));
  float lowpass = 0.0F, highpass = 0.0F, previous = 0.0F;
  uint64_t renderFramesWritten = 0;
  uint64_t captureFrames = 0;
  double powerOn = 0.0, powerOff = 0.0;
  uint64_t samplesOn = 0, samplesOff = 0;
  float peak = 0.0F;

  if (loopback && !Check(loopbackClient->Start(), "Start(loopback)")) return 1;
  if (options.play && !Check(renderClient->Start(), "Start(render)")) return 1;
  if (!Check(captureClient->Start(), "Start(capture)")) return 1;
  const auto start = std::chrono::steady_clock::now();

  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(options.seconds)) {
    if (options.play) {
      UINT32 padding = 0;
      renderClient->GetCurrentPadding(&padding);
      const UINT32 target = renderFormat->nSamplesPerSec / 10;  // keep ~100 ms queued
      if (padding < target) {
        const UINT32 frames = std::min(target - padding, renderBufferFrames - padding);
        BYTE* buffer = nullptr;
        if (SUCCEEDED(renderService->GetBuffer(frames, &buffer))) {
          auto* out = reinterpret_cast<float*>(buffer);
          for (UINT32 frame = 0; frame < frames; ++frame) {
            const double t = static_cast<double>(renderFramesWritten + frame) / renderFormat->nSamplesPerSec;
            float sample = 0.0F;
            if (!program.empty()) {
              sample = program[static_cast<size_t>((renderFramesWritten + frame) % program.size())];
            } else {
              // ~150 Hz - 6 kHz noise: one-pole low-pass, then a one-pole high-pass.
              lowpass += 0.55F * (noise(random) - lowpass);
              highpass = 0.98F * (highpass + lowpass - previous);
              previous = lowpass;
              sample = BurstActive(t) ? amplitude * highpass : 0.0F;
            }
            for (UINT32 channel = 0; channel < renderFormat->nChannels; ++channel) {
              out[frame * renderFormat->nChannels + channel] = sample;
            }
          }
          renderService->ReleaseBuffer(frames, 0);
          renderFramesWritten += frames;
        }
      }
    }

    UINT32 packet = 0;
    while (SUCCEEDED(captureService->GetNextPacketSize(&packet)) && packet > 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      UINT64 devicePosition = 0, qpcPosition = 0;
      if (FAILED(captureService->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition))) break;
      const auto* in = reinterpret_cast<const float*>(data);
      if (!options.wavPath.empty()) recorded.Add(in, frames, flags, devicePosition, qpcPosition);
      for (UINT32 frame = 0; frame < frames; ++frame) {
        const double t = static_cast<double>(captureFrames + frame) / captureRate;
        const double phase = std::fmod(t, kBurstSeconds + kGapSeconds);
        // Skip 250 ms after each transition to allow for output + input latency.
        const bool settledOn = phase > 0.25 && phase < kBurstSeconds;
        const bool settledOff = phase > kBurstSeconds + 0.25;
        for (UINT32 channel = 0; channel < captureChannels; ++channel) {
          const float sample = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 ? 0.0F : in[frame * captureChannels + channel];
          peak = std::max(peak, std::fabs(sample));
          if (settledOn) { powerOn += sample * sample; ++samplesOn; }
          if (settledOff) { powerOff += sample * sample; ++samplesOff; }
        }
      }
      captureFrames += frames;
      captureService->ReleaseBuffer(frames);
    }
    while (loopback && SUCCEEDED(loopbackService->GetNextPacketSize(&packet)) && packet > 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      UINT64 devicePosition = 0, qpcPosition = 0;
      if (FAILED(loopbackService->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition))) break;
      loopbackRecorded.Add(reinterpret_cast<const float*>(data), frames, flags, devicePosition, qpcPosition);
      loopbackService->ReleaseBuffer(frames);
    }
    Sleep(5);
  }

  captureClient->Stop();
  if (options.play) renderClient->Stop();
  if (loopback) loopbackClient->Stop();

  std::printf("captured %llu frames, peak %.4f (%.1f dBFS), %llu discontinuities\n", captureFrames, peak,
              ToDb(static_cast<double>(peak) * peak), recorded.discontinuities);
  if (options.play && program.empty() && samplesOn > 0 && samplesOff > 0) {
    const double on = ToDb(powerOn / samplesOn), off = ToDb(powerOff / samplesOff);
    std::printf("mic level while speakers play: %.1f dBFS, while silent: %.1f dBFS, difference %.1f dB\n", on, off, on - off);
    std::printf("(this is after the whole capture chain, including the existing noise suppression)\n");
  }
  if (!options.wavPath.empty()) {
    std::printf("wrote %s: %s\n", options.wavPath.c_str(),
                WriteFloatWav(options.wavPath, recorded.samples, captureRate, static_cast<uint16_t>(captureChannels)) ? "ok" : "FAILED");
  }
  if (loopback) {
    std::printf("wrote %s: %s (%llu discontinuities)\n", options.loopbackWavPath.c_str(),
                WriteFloatWav(options.loopbackWavPath, loopbackRecorded.samples, renderFormat->nSamplesPerSec,
                              renderFormat->nChannels) ? "ok" : "FAILED",
                loopbackRecorded.discontinuities);
  }
  if (!options.wavPath.empty()) {
    FILE* sidecar = nullptr;
    const std::string sidecarPath = options.wavPath + ".json";
    if (fopen_s(&sidecar, sidecarPath.c_str(), "wb") == 0 && sidecar != nullptr) {
      // GetBuffer's QPC position is already in 100 ns units.
      std::fprintf(sidecar,
                   "{\"mic_rate\": %u, \"mic_channels\": %u, \"mic_first_qpc_hns\": %llu, "
                   "\"loopback_rate\": %lu, \"loopback_channels\": %u, \"loopback_first_qpc_hns\": %llu, "
                   "\"raw\": %s, \"category\": \"%s\", \"stream_latency_hns\": %lld, \"level_db\": %.1f}\n",
                   captureRate, captureChannels, recorded.firstQpc,
                   loopback ? renderFormat->nSamplesPerSec : 0UL, loopback ? renderFormat->nChannels : 0U,
                   loopbackRecorded.firstQpc, options.raw ? "true" : "false", options.communications ? "communications" : "default",
                   static_cast<long long>(latency), options.levelDb);
      std::fclose(sidecar);
    }
  }
  CoTaskMemFree(captureFormat);
  if (renderFormat != nullptr) CoTaskMemFree(renderFormat);
  return 0;
}

int Usage() {
  std::fprintf(stderr,
               "usage:\n  ncs-aec-probe list\n  ncs-aec-probe run <capture-endpoint-guid> [--seconds N] "
               "[--category default|communications] [--play | --play-wav file] [--level-db -30] [--wav path] "
               "[--loopback-wav path]\n");
  return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) return Usage();
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
  int exitCode = 2;
  {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (!Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator)), "CoCreateInstance(MMDeviceEnumerator)")) {
      CoUninitialize();
      return 1;
    }
    const std::wstring command = argv[1];
    if (command == L"list") {
      exitCode = List(enumerator.p);
    } else if (command == L"run" && argc >= 3) {
      Options options;
      options.endpoint = argv[2];
      bool valid = true;
      for (int index = 3; index < argc && valid; ++index) {
        const std::wstring arg = argv[index];
        const bool hasValue = index + 1 < argc;
        if (arg == L"--seconds" && hasValue) options.seconds = std::max(1, _wtoi(argv[++index]));
        else if (arg == L"--category" && hasValue) {
          const std::wstring value = argv[++index];
          if (value == L"communications") options.communications = true;
          else if (value != L"default") valid = false;
        }
        else if (arg == L"--play") options.play = true;
        else if (arg == L"--raw") options.raw = true;
        else if (arg == L"--level-db" && hasValue) options.levelDb = std::min(-6.0, _wtof(argv[++index]));
        else if (arg == L"--wav" && hasValue) options.wavPath = Utf8(argv[++index]);
        else if (arg == L"--play-wav" && hasValue) { options.playWavPath = Utf8(argv[++index]); options.play = true; }
        else if (arg == L"--loopback-wav" && hasValue) options.loopbackWavPath = Utf8(argv[++index]);
        else valid = false;
      }
      exitCode = valid ? Run(enumerator.p, options) : Usage();
    } else {
      exitCode = Usage();
    }
  }
  CoUninitialize();
  return exitCode;
}
