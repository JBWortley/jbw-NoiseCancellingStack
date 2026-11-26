// SPDX-License-Identifier: MIT
//
// Loads ncs_aec_apo.dll in-process (no COM or APO registration) and drives it through the same
// interface sequence the Windows audio engine uses: Initialize, format negotiation,
// AddAuxiliaryInput, LockForProcess, interleaved AcceptInput/APOProcess, UnlockForProcess.
#include "aec_test_support.h"

#include <objbase.h>
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h>
#include <audiomediatype.h>

namespace {

// {6F0DE50F-5FE3-47EA-BA01-D9AAB0788EF7}
constexpr CLSID kClsid = {0x6F0DE50F, 0x5FE3, 0x47EA, {0xBA, 0x01, 0xD9, 0xAA, 0xB0, 0x78, 0x8E, 0xF7}};
constexpr UINT32 kRate = 48'000;
constexpr UINT32 kFrames = 480;

using GetClassObjectFunction = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
using CanUnloadFunction = HRESULT(STDAPICALLTYPE*)();
using SelfTestFunction = std::int32_t (*)();

HMODULE g_dll = nullptr;

template <typename T>
struct Com {
  T* p = nullptr;
  Com() = default;
  Com(Com&& other) noexcept : p(other.p) { other.p = nullptr; }
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  Com& operator=(Com&&) = delete;
  ~Com() { if (p != nullptr) p->Release(); }
  T** put() { return &p; }
  T* operator->() const { return p; }
};

Com<IAudioMediaType> MakeFormat(UINT32 rate, UINT32 channels) {
  UNCOMPRESSEDAUDIOFORMAT format = {KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, channels, 4, 32, static_cast<FLOAT32>(rate),
                                    channels == 1 ? static_cast<DWORD>(KSAUDIO_SPEAKER_MONO)
                                                  : static_cast<DWORD>(KSAUDIO_SPEAKER_STEREO)};
  Com<IAudioMediaType> type;
  AEC_REQUIRE(SUCCEEDED(CreateAudioMediaTypeFromUncompressedAudioFormat(&format, type.put())));
  return type;
}

struct Apo {
  Com<IUnknown> unknown;
  Com<IAudioProcessingObject> object;
  Com<IAudioProcessingObjectRT> realtime;
  Com<IAudioProcessingObjectConfiguration> configuration;
  Com<IApoAuxiliaryInputConfiguration> auxiliary;
  Com<IApoAuxiliaryInputRT> auxiliary_realtime;

  explicit Apo(const GUID& mode) {
    auto get_class_object = reinterpret_cast<GetClassObjectFunction>(GetProcAddress(g_dll, "DllGetClassObject"));
    AEC_REQUIRE(get_class_object != nullptr);
    Com<IClassFactory> factory;
    AEC_REQUIRE(SUCCEEDED(get_class_object(kClsid, __uuidof(IClassFactory), reinterpret_cast<void**>(factory.put()))));
    AEC_REQUIRE(SUCCEEDED(factory->CreateInstance(nullptr, __uuidof(IUnknown), reinterpret_cast<void**>(unknown.put()))));
    AEC_REQUIRE(SUCCEEDED(unknown->QueryInterface(__uuidof(IAudioProcessingObject), reinterpret_cast<void**>(object.put()))));
    AEC_REQUIRE(SUCCEEDED(unknown->QueryInterface(__uuidof(IAudioProcessingObjectRT), reinterpret_cast<void**>(realtime.put()))));
    AEC_REQUIRE(SUCCEEDED(unknown->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration), reinterpret_cast<void**>(configuration.put()))));
    AEC_REQUIRE(SUCCEEDED(unknown->QueryInterface(__uuidof(IApoAuxiliaryInputConfiguration), reinterpret_cast<void**>(auxiliary.put()))));
    AEC_REQUIRE(SUCCEEDED(unknown->QueryInterface(__uuidof(IApoAuxiliaryInputRT), reinterpret_cast<void**>(auxiliary_realtime.put()))));

    APOInitSystemEffects2 init = {};
    init.APOInit.cbSize = sizeof(init);
    init.APOInit.clsid = kClsid;
    init.AudioProcessingMode = mode;
    AEC_REQUIRE(SUCCEEDED(object->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init))));
  }
};

// Mono 48 kHz microphone hearing a stereo far end through two delayed paths.
struct Signals {
  ncs::aec_test::Noise left{11};
  ncs::aec_test::Noise right{22};
  std::vector<float> history_left = std::vector<float>(4096, 0.0F);
  std::vector<float> history_right = std::vector<float>(4096, 0.0F);
  std::size_t position = 0;

  void Next(float* reference_stereo, float* microphone, UINT32 frames) {
    for (UINT32 i = 0; i < frames; ++i) {
      position = (position + 1) % history_left.size();
      const float l = 0.25F * left.Next();
      const float r = 0.25F * right.Next();
      history_left[position] = l;
      history_right[position] = r;
      reference_stereo[2 * i] = l;
      reference_stereo[2 * i + 1] = r;
      const auto at = [this](const std::vector<float>& h, std::size_t age) { return h[(position + h.size() - age) % h.size()]; };
      microphone[i] = 0.4F * at(history_left, 2880) - 0.1F * at(history_left, 2900) + 0.25F * at(history_right, 3100);
    }
  }
};

APO_CONNECTION_DESCRIPTOR Descriptor(IAudioMediaType* format, float* buffer) {
  return {APO_CONNECTION_BUFFER_TYPE_EXTERNAL, reinterpret_cast<UINT_PTR>(buffer), kFrames, format,
          APO_CONNECTION_DESCRIPTOR_SIGNATURE};
}

APO_CONNECTION_PROPERTY_V2 Property(float* buffer, UINT32 frames, APO_BUFFER_FLAGS flags) {
  APO_CONNECTION_PROPERTY_V2 property = {};
  property.property.pBuffer = reinterpret_cast<UINT_PTR>(buffer);
  property.property.u32ValidFrameCount = frames;
  property.property.u32BufferFlags = flags;
  property.property.u32Signature = APO_CONNECTION_PROPERTY_V2_SIGNATURE;
  return property;
}

struct Session {
  Apo& apo;
  Com<IAudioMediaType> microphone_format = MakeFormat(kRate, 1);
  Com<IAudioMediaType> reference_format = MakeFormat(kRate, 2);
  std::vector<float> input = std::vector<float>(kFrames, 0.0F);
  std::vector<float> output = std::vector<float>(kFrames, 0.0F);
  std::vector<float> reference = std::vector<float>(kFrames * 2, 0.0F);
  bool with_reference;

  Session(Apo& owner, bool attach_reference) : apo(owner), with_reference(attach_reference) {
    Com<IAudioMediaType> supported;
    AEC_REQUIRE(apo.object->IsInputFormatSupported(microphone_format.p, microphone_format.p, supported.put()) == S_OK);
    if (with_reference) {
      APO_CONNECTION_DESCRIPTOR descriptor = Descriptor(reference_format.p, reference.data());
      AEC_REQUIRE(SUCCEEDED(apo.auxiliary->AddAuxiliaryInput(1, 0, nullptr, &descriptor)));
    }
    APO_CONNECTION_DESCRIPTOR in = Descriptor(microphone_format.p, input.data());
    APO_CONNECTION_DESCRIPTOR out = Descriptor(microphone_format.p, output.data());
    APO_CONNECTION_DESCRIPTOR* ins[] = {&in};
    APO_CONNECTION_DESCRIPTOR* outs[] = {&out};
    AEC_REQUIRE(SUCCEEDED(apo.configuration->LockForProcess(1, ins, 1, outs)));
  }

  ~Session() {
    apo.configuration->UnlockForProcess();
    if (with_reference) {
      apo.auxiliary->RemoveAuxiliaryInput(1);
    }
  }

  void Process(UINT32 frames, APO_BUFFER_FLAGS flags = BUFFER_VALID) {
    if (with_reference) {
      APO_CONNECTION_PROPERTY_V2 aux = Property(reference.data(), frames, BUFFER_VALID);
      apo.auxiliary_realtime->AcceptInput(1, &aux.property);
    }
    APO_CONNECTION_PROPERTY_V2 in = Property(input.data(), frames, flags);
    APO_CONNECTION_PROPERTY_V2 out = Property(output.data(), frames, BUFFER_INVALID);
    APO_CONNECTION_PROPERTY* ins[] = {&in.property};
    APO_CONNECTION_PROPERTY* outs[] = {&out.property};
    apo.realtime->APOProcess(1, ins, 1, outs);
    AEC_REQUIRE(out.property.u32ValidFrameCount == frames);
    AEC_REQUIRE(out.property.u32BufferFlags == flags);
  }
};

void FactoryExposesEchoCancellerInterfaces() {
  Apo apo(AUDIO_SIGNALPROCESSINGMODE_DEFAULT);
  Com<IAudioSystemEffects2> effects;
  AEC_REQUIRE(SUCCEEDED(apo.unknown->QueryInterface(__uuidof(IAudioSystemEffects2), reinterpret_cast<void**>(effects.put()))));
  LPGUID list = nullptr;
  UINT count = 99;
  AEC_REQUIRE(SUCCEEDED(effects->GetEffectsList(&list, &count, nullptr)));
  AEC_REQUIRE(count == 0 && list == nullptr);  // not advertised to applications
  Com<IApoAcousticEchoCancellation2> aec;
  AEC_REQUIRE(SUCCEEDED(apo.unknown->QueryInterface(__uuidof(IApoAcousticEchoCancellation2), reinterpret_cast<void**>(aec.put()))));
  APO_REFERENCE_STREAM_PROPERTIES properties = APO_REFERENCE_STREAM_PROPERTIES_NONE;
  AEC_REQUIRE(SUCCEEDED(aec->GetDesiredReferenceStreamProperties(&properties)));
  AEC_REQUIRE(properties == APO_REFERENCE_STREAM_PROPERTIES_POST_VOLUME_LOOPBACK);
  HNSTIME latency = -1;
  AEC_REQUIRE(SUCCEEDED(apo.object->GetLatency(&latency)) && latency == 0);
}

void ReferenceFormatNegotiationRequestsFloatStereoAtNativeRate() {
  Apo apo(AUDIO_SIGNALPROCESSINGMODE_DEFAULT);
  Com<IAudioMediaType> accepted;
  Com<IAudioMediaType> stereo = MakeFormat(48'000, 2);
  AEC_REQUIRE(apo.auxiliary->IsInputFormatSupported(stereo.p, accepted.put()) == S_OK);

  UNCOMPRESSEDAUDIOFORMAT surround = {KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 6, 4, 32, 44'100.0F, KSAUDIO_SPEAKER_5POINT1};
  Com<IAudioMediaType> offered;
  AEC_REQUIRE(SUCCEEDED(CreateAudioMediaTypeFromUncompressedAudioFormat(&surround, offered.put())));
  Com<IAudioMediaType> suggested;
  AEC_REQUIRE(apo.auxiliary->IsInputFormatSupported(offered.p, suggested.put()) == S_FALSE);
  UNCOMPRESSEDAUDIOFORMAT result = {};
  AEC_REQUIRE(SUCCEEDED(suggested->GetUncompressedAudioFormat(&result)));
  AEC_REQUIRE(result.dwSamplesPerFrame == 2 && result.fFramesPerSecond == 48'000.0F);
  AEC_REQUIRE(result.guidFormatType == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

  APO_CONNECTION_DESCRIPTOR descriptor = {APO_CONNECTION_BUFFER_TYPE_EXTERNAL, 0, kFrames, offered.p,
                                          APO_CONNECTION_DESCRIPTOR_SIGNATURE};
  AEC_REQUIRE(apo.auxiliary->AddAuxiliaryInput(7, 0, nullptr, &descriptor) == APOERR_FORMAT_NOT_SUPPORTED);
}

void CancelsEchoThroughTheComInterfaces() {
  Apo apo(AUDIO_SIGNALPROCESSINGMODE_DEFAULT);
  Session session(apo, true);
  Signals signals;
  double echo_in = 0.0, echo_out = 0.0;
  for (int block = 0; block < 800; ++block) {  // 8 s
    signals.Next(session.reference.data(), session.input.data(), kFrames);
    session.Process(kFrames);
    if (block >= 700) {
      for (UINT32 i = 0; i < kFrames; ++i) {
        AEC_REQUIRE(std::isfinite(session.output[i]));
        echo_in += static_cast<double>(session.input[i]) * session.input[i];
        echo_out += static_cast<double>(session.output[i]) * session.output[i];
      }
    }
  }
  const double reduction = ncs::aec_test::Db(echo_in / std::max(echo_out, 1e-30));
  std::cout << "  echo reduction through the APO " << reduction << " dB\n";
  AEC_REQUIRE(reduction >= 20.0);
}

void NonDefaultModePassesAudioThroughUnchanged() {
  Apo apo(AUDIO_SIGNALPROCESSINGMODE_COMMUNICATIONS);
  Session session(apo, true);
  Signals signals;
  for (int block = 0; block < 50; ++block) {
    signals.Next(session.reference.data(), session.input.data(), kFrames);
    session.Process(kFrames);
    AEC_REQUIRE(std::memcmp(session.input.data(), session.output.data(), kFrames * sizeof(float)) == 0);
  }
}

// While active, the reported latency is AEC3's fixed 430-frame delay at 48 kHz; a block that is not
// exactly 10 ms is emitted as dry audio delayed by that same amount, never undelayed.
void LatencyIsReportedAndIrregularBlocksStayDelayed() {
  Apo apo(AUDIO_SIGNALPROCESSINGMODE_DEFAULT);
  HNSTIME latency = -1;
  AEC_REQUIRE(SUCCEEDED(apo.object->GetLatency(&latency)) && latency == 0);
  Session session(apo, true);
  AEC_REQUIRE(SUCCEEDED(apo.object->GetLatency(&latency)));
  AEC_REQUIRE(latency == 430LL * 10'000'000 / 48'000);

  constexpr UINT32 kIrregular = 256;
  constexpr std::size_t kDelay = 430;
  std::vector<float> input_history, output_history;
  float value = 0.0F;
  for (int block = 0; block < 20; ++block) {
    for (UINT32 i = 0; i < kIrregular; ++i) {
      value += 1.0F / 65'536.0F;
      session.input[i] = value;
    }
    session.Process(kIrregular);
    input_history.insert(input_history.end(), session.input.begin(), session.input.begin() + kIrregular);
    output_history.insert(output_history.end(), session.output.begin(), session.output.begin() + kIrregular);
  }
  for (std::size_t i = 0; i < kDelay; ++i) {
    AEC_REQUIRE(output_history[i] == 0.0F);
  }
  for (std::size_t i = kDelay; i < output_history.size(); ++i) {
    AEC_REQUIRE(output_history[i] == input_history[i - kDelay]);
  }
}

void NonDefaultModeReportsNoLatency() {
  Apo apo(AUDIO_SIGNALPROCESSINGMODE_COMMUNICATIONS);
  Session session(apo, true);
  HNSTIME latency = -1;
  AEC_REQUIRE(SUCCEEDED(apo.object->GetLatency(&latency)) && latency == 0);
}

void MissingReferenceKeepsMicrophoneLevel() {
  Apo apo(AUDIO_SIGNALPROCESSINGMODE_DEFAULT);
  Session session(apo, false);
  ncs::aec_test::Noise near_end(5);
  float smoothed = 0.0F;
  double in = 0.0, out = 0.0;
  for (int block = 0; block < 200; ++block) {
    for (float& sample : session.input) {
      smoothed += 0.35F * (near_end.Next() - smoothed);  // speech-band noise
      sample = 0.4F * smoothed;
    }
    session.Process(kFrames);
    if (block >= 50) {
      for (UINT32 i = 0; i < kFrames; ++i) {
        in += static_cast<double>(session.input[i]) * session.input[i];
        out += static_cast<double>(session.output[i]) * session.output[i];
      }
    }
  }
  AEC_REQUIRE(std::fabs(ncs::aec_test::Db(out / in)) <= 1.0);
}

void SilentBuffersStaySilentAndFlagged() {
  Apo apo(AUDIO_SIGNALPROCESSINGMODE_DEFAULT);
  Session session(apo, true);
  for (int block = 0; block < 20; ++block) {
    std::fill(session.input.begin(), session.input.end(), 0.0F);
    session.Process(kFrames, BUFFER_SILENT);
    for (const float sample : session.output) {
      AEC_REQUIRE(sample == 0.0F);
    }
  }
}

void ExportedSelfTestPasses() {
  auto self_test = reinterpret_cast<SelfTestFunction>(GetProcAddress(g_dll, "NcsAecApoSelfTest"));
  AEC_REQUIRE(self_test != nullptr);
  AEC_REQUIRE(self_test() == 0);
}

void DllUnloadsOnceObjectsAreReleased() {
  auto can_unload = reinterpret_cast<CanUnloadFunction>(GetProcAddress(g_dll, "DllCanUnloadNow"));
  AEC_REQUIRE(can_unload != nullptr && can_unload() == S_OK);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: aec-apo-smoke-host <ncs_aec_apo.dll>\n";
    return 2;
  }
  g_dll = LoadLibraryA(argv[1]);
  if (g_dll == nullptr) {
    std::cerr << "could not load " << argv[1] << '\n';
    return 1;
  }
  const int result = ncs::aec_test::Run({
      {"factory exposes echo canceller interfaces", &FactoryExposesEchoCancellerInterfaces},
      {"reference format negotiation requests float stereo at a native rate", &ReferenceFormatNegotiationRequestsFloatStereoAtNativeRate},
      {"cancels echo through the COM interfaces", &CancelsEchoThroughTheComInterfaces},
      {"non-default mode passes audio through unchanged", &NonDefaultModePassesAudioThroughUnchanged},
      {"latency is reported and irregular blocks stay delayed", &LatencyIsReportedAndIrregularBlocksStayDelayed},
      {"non-default mode reports no latency", &NonDefaultModeReportsNoLatency},
      {"missing reference keeps microphone level", &MissingReferenceKeepsMicrophoneLevel},
      {"silent buffers stay silent and flagged", &SilentBuffersStaySilentAndFlagged},
      {"exported self-test passes", &ExportedSelfTestPasses},
      {"DLL unloads once objects are released", &DllUnloadsOnceObjectsAreReleased},
  });
  FreeLibrary(g_dll);
  return result;
}
