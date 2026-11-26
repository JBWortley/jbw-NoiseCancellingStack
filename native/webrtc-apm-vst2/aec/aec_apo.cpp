// SPDX-License-Identifier: MIT
//
// Noise Cancelling Stack acoustic echo cancellation APO.
//
// A Windows 11 capture mode effect (MFX) that identifies itself as an echo canceller so the audio
// engine supplies a loopback reference of the active render endpoint. Each 10 ms microphone block
// is processed by WebRTC AEC3 before the Equalizer APO stream effect (SFX) applies noise
// suppression. Microphone availability outranks echo cancellation: an unsupported format or mode
// passes audio through unchanged, and once processing is active any failure switches to the
// same audio delayed by the fixed pipeline latency, so the reported latency never changes.
//
// Threading: IApoAuxiliaryInputRT::AcceptInput (engine loopback thread) only writes into a
// lock-free ring. APOProcess (capture thread) drains the ring and is the only caller of WebRTC.

#define NOMINMAX
#include "delay_line.h"
#include "echo_processor.h"
#include "reference_ring.h"

#include <windows.h>
#include <objbase.h>
#include <olectl.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h>
#include <baseaudioprocessingobject.h>
#include <audiomediatype.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

// {6F0DE50F-5FE3-47EA-BA01-D9AAB0788EF7}
constexpr CLSID kEchoCancellerClsid = {
    0x6F0DE50F, 0x5FE3, 0x47EA, {0xBA, 0x01, 0xD9, 0xAA, 0xB0, 0x78, 0x8E, 0xF7}};
constexpr wchar_t kEchoCancellerClsidString[] = L"{6F0DE50F-5FE3-47EA-BA01-D9AAB0788EF7}";
constexpr wchar_t kSettingsKey[] = L"SOFTWARE\\NoiseCancellingStack";
constexpr wchar_t kDisableValue[] = L"DisableEchoCancellation";
constexpr int kPreferredReferenceRate = 48'000;
constexpr std::size_t kRingSeconds = 1;                // reference ring capacity
constexpr std::size_t kMaxReferenceBacklogBlocks = 25;  // 250 ms; older reference is dropped

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

void Trace(const wchar_t* message) noexcept { OutputDebugStringW(message); }

bool IsDisabledByPolicy() noexcept {
  DWORD value = 0;
  DWORD size = sizeof(value);
  return RegGetValueW(HKEY_LOCAL_MACHINE, kSettingsKey, kDisableValue, RRF_RT_REG_DWORD, nullptr,
                      &value, &size)
             == ERROR_SUCCESS
      && value != 0;
}

struct Format {
  bool valid = false;
  bool is_float32 = false;
  int rate = 0;
  UINT32 channels = 0;
};

Format ReadFormat(IAudioMediaType* type) noexcept {
  Format format;
  UNCOMPRESSEDAUDIOFORMAT uncompressed = {};
  if (type == nullptr || FAILED(type->GetUncompressedAudioFormat(&uncompressed))) {
    return format;
  }
  format.valid = true;
  format.is_float32 = uncompressed.guidFormatType == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
      && uncompressed.dwBytesPerSampleContainer == 4 && uncompressed.dwValidBitsPerSample == 32;
  format.rate = static_cast<int>(std::lround(uncompressed.fFramesPerSecond));
  format.channels = uncompressed.dwSamplesPerFrame;
  return format;
}

bool IsAcceptableReference(const Format& format) noexcept {
  return format.valid && format.is_float32 && ncs::EchoProcessor::IsSupportedRate(format.rate)
      && format.channels >= 1 && format.channels <= ncs::EchoProcessor::kMaxRenderChannels;
}

class INonDelegatingUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE NonDelegatingQueryInterface(REFIID iid, void** object) = 0;
  virtual ULONG STDMETHODCALLTYPE NonDelegatingAddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE NonDelegatingRelease() = 0;

 protected:
  ~INonDelegatingUnknown() = default;
};

#pragma warning(suppress : 4815)  // CRegAPOProperties<1> carries a zero-length IID tail.
const CRegAPOProperties<1> kRegistration(
    kEchoCancellerClsid, L"Noise Cancelling Stack Echo Canceller", L"Copyright (c) JBWortley (MIT)",
    1, 0, __uuidof(IAudioProcessingObject),
    static_cast<APO_FLAG>(APO_FLAG_DEFAULT | APO_FLAG_INPLACE));

class EchoCancellerApo final : public CBaseAudioProcessingObject,
                               public IAudioSystemEffects2,
                               public IApoAcousticEchoCancellation2,
                               public IApoAuxiliaryInputConfiguration,
                               public IApoAuxiliaryInputRT,
                               public INonDelegatingUnknown {
 public:
  explicit EchoCancellerApo(IUnknown* outer)
      : CBaseAudioProcessingObject(kRegistration), disabled_by_policy_(IsDisabledByPolicy()) {
    outer_ = outer != nullptr ? outer : reinterpret_cast<IUnknown*>(static_cast<INonDelegatingUnknown*>(this));
    ring_.Allocate(kRingSeconds * kPreferredReferenceRate * ncs::EchoProcessor::kMaxRenderChannels);
    ++g_objects;
  }

  ~EchoCancellerApo() override { --g_objects; }

  EchoCancellerApo(const EchoCancellerApo&) = delete;
  EchoCancellerApo& operator=(const EchoCancellerApo&) = delete;

  // ----- IUnknown: delegates to the controlling object when aggregated -----
  STDMETHODIMP QueryInterface(REFIID iid, void** object) override { return outer_->QueryInterface(iid, object); }
  STDMETHODIMP_(ULONG) AddRef() override { return outer_->AddRef(); }
  STDMETHODIMP_(ULONG) Release() override { return outer_->Release(); }

  HRESULT STDMETHODCALLTYPE NonDelegatingQueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (iid == __uuidof(IUnknown)) {
      *object = static_cast<INonDelegatingUnknown*>(this);
    } else if (iid == __uuidof(IAudioProcessingObject)) {
      *object = static_cast<IAudioProcessingObject*>(this);
    } else if (iid == __uuidof(IAudioProcessingObjectRT)) {
      *object = static_cast<IAudioProcessingObjectRT*>(this);
    } else if (iid == __uuidof(IAudioProcessingObjectConfiguration)) {
      *object = static_cast<IAudioProcessingObjectConfiguration*>(this);
    } else if (iid == __uuidof(IAudioSystemEffects) || iid == __uuidof(IAudioSystemEffects2)) {
      *object = static_cast<IAudioSystemEffects2*>(this);
    } else if (iid == __uuidof(IApoAcousticEchoCancellation)
               || iid == __uuidof(IApoAcousticEchoCancellation2)) {
      *object = static_cast<IApoAcousticEchoCancellation2*>(this);
    } else if (iid == __uuidof(IApoAuxiliaryInputConfiguration)) {
      *object = static_cast<IApoAuxiliaryInputConfiguration*>(this);
    } else if (iid == __uuidof(IApoAuxiliaryInputRT)) {
      *object = static_cast<IApoAuxiliaryInputRT*>(this);
    } else {
      return E_NOINTERFACE;
    }
    reinterpret_cast<IUnknown*>(*object)->AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE NonDelegatingAddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }

  ULONG STDMETHODCALLTYPE NonDelegatingRelease() override {
    const LONG remaining = InterlockedDecrement(&references_);
    if (remaining == 0) {
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }

  // ----- IAudioProcessingObject -----
  STDMETHODIMP Initialize(UINT32 size, BYTE* data) override {
    if ((data == nullptr) != (size == 0)) {
      return E_INVALIDARG;
    }
    GUID mode = AUDIO_SIGNALPROCESSINGMODE_DEFAULT;
    if (size == sizeof(APOInitSystemEffects3)) {
      mode = reinterpret_cast<const APOInitSystemEffects3*>(data)->AudioProcessingMode;
    } else if (size == sizeof(APOInitSystemEffects2)) {
      mode = reinterpret_cast<const APOInitSystemEffects2*>(data)->AudioProcessingMode;
    }
    // The product registers this effect for the default mode only. Any other mode passes audio
    // through rather than guessing at that mode's processing contract.
    mode_supported_ = mode == AUDIO_SIGNALPROCESSINGMODE_DEFAULT;
    m_bIsInitialized = true;
    return S_OK;
  }

  STDMETHODIMP GetLatency(HNSTIME* time) override {
    if (time == nullptr) {
      return E_POINTER;
    }
    *time = latency_;  // AEC3's fixed capture-path delay while active; zero when passing through
    return S_OK;
  }

  // ----- IAudioProcessingObjectConfiguration -----
  STDMETHODIMP LockForProcess(UINT32 input_count, APO_CONNECTION_DESCRIPTOR** inputs,
                              UINT32 output_count, APO_CONNECTION_DESCRIPTOR** outputs) override {
    const HRESULT result = CBaseAudioProcessingObject::LockForProcess(input_count, inputs, output_count, outputs);
    if (FAILED(result)) {
      return result;
    }
    // Never fail beyond the base class: a format the echo canceller cannot use is passed through.
    active_ = false;
    failed_ = false;
    latency_ = 0;
    try {
      const Format microphone = ReadFormat(inputs[0]->pFormat);
      const UINT32 max_frames = inputs[0]->u32MaxFrameCount;
      const bool reference_attached = aux_input_id_.load(std::memory_order_acquire) != 0;
      locked_reference_rate_ = reference_attached ? reference_rate_.load() : kPreferredReferenceRate;
      locked_reference_channels_ = reference_attached ? reference_channels_.load() : 2U;
      const bool usable = !disabled_by_policy_ && mode_supported_ && microphone.is_float32
          && ncs::EchoProcessor::IsSupportedRate(microphone.rate) && microphone.channels == m_u32SamplesPerFrame
          && max_frames >= static_cast<UINT32>(microphone.rate / 100);
      if (usable
          && processor_.Configure(microphone.rate, microphone.channels, locked_reference_rate_,
                                  locked_reference_channels_)) {
        const std::size_t delay = ncs::EchoProcessor::PipelineDelayFrames(microphone.rate);
        render_block_.assign(processor_.render_frames() * processor_.render_channels(), 0.0F);
        silent_block_.assign(processor_.capture_frames() * processor_.capture_channels(), 0.0F);
        dry_block_.assign(silent_block_.size(), 0.0F);
        dry_delay_.Configure(delay, microphone.channels);
        latency_ = static_cast<HNSTIME>(delay) * 10'000'000 / microphone.rate;
        ring_.DiscardAll();
        active_ = true;
      } else if (usable) {
        Trace(L"NCS echo canceller: WebRTC configuration failed; passing audio through.\n");
      }
    } catch (...) {
      processor_.Reset();
      active_ = false;
      latency_ = 0;
    }
    return result;
  }

  STDMETHODIMP UnlockForProcess() override {
    active_ = false;
    latency_ = 0;
    processor_.Reset();
    return CBaseAudioProcessingObject::UnlockForProcess();
  }

  // ----- IAudioProcessingObjectRT -----
  STDMETHODIMP_(void) APOProcess(UINT32 input_count, APO_CONNECTION_PROPERTY** inputs,
                                 UINT32 output_count, APO_CONNECTION_PROPERTY** outputs) override {
    if (input_count < 1 || output_count < 1 || inputs[0] == nullptr || outputs[0] == nullptr) {
      return;
    }
    APO_CONNECTION_PROPERTY* source = inputs[0];
    APO_CONNECTION_PROPERTY* destination = outputs[0];
    const UINT32 frames = source->u32ValidFrameCount;
    auto* output = reinterpret_cast<float*>(destination->pBuffer);
    if ((source->u32BufferFlags == BUFFER_VALID || source->u32BufferFlags == BUFFER_SILENT)
        && destination->pBuffer != source->pBuffer) {
      std::memcpy(output, reinterpret_cast<const float*>(source->pBuffer),
                  static_cast<std::size_t>(frames) * m_u32SamplesPerFrame * sizeof(float));
    }
    destination->u32ValidFrameCount = frames;
    destination->u32BufferFlags = source->u32BufferFlags;

    if (!active_ || source->u32BufferFlags == BUFFER_INVALID) {
      return;  // inactive: unchanged microphone audio with zero reported latency
    }

    // Active: every output from here on carries the fixed pipeline delay.
    const bool silent = source->u32BufferFlags == BUFFER_SILENT;
    const std::size_t samples = static_cast<std::size_t>(frames) * m_u32SamplesPerFrame;
    if (silent) {
      std::memset(output, 0, samples * sizeof(float));  // silent buffers carry no defined data
    }
    // A block that is not exactly 10 ms is passed through (delayed) without ending processing; a
    // WebRTC failure ends processing for this instance.
    const bool irregular = frames != processor_.capture_frames();
    if (!failed_ && !irregular && !FeedReference()) {
      failed_ = true;
    }
    if (failed_ || irregular) {
      dry_delay_.Process(output, output, samples);  // delayed dry audio
      return;
    }

    dry_delay_.Process(output, dry_block_.data(), samples);  // keep the fallback path in step
    if (silent) {
      // Keep AEC3's capture timeline continuous; the silent output flag is left untouched.
      std::fill(silent_block_.begin(), silent_block_.end(), 0.0F);
      failed_ = !processor_.ProcessCapture(silent_block_.data());
      return;
    }
    // ProcessCapture writes the result only on success; on failure emit the delayed dry block.
    if (!processor_.ProcessCapture(output)) {
      failed_ = true;
      std::memcpy(output, dry_block_.data(), samples * sizeof(float));
    }
  }

  // ----- IAudioSystemEffects2 -----
  STDMETHODIMP GetEffectsList(LPGUID* effects, UINT* count, HANDLE) override {
    if (effects == nullptr || count == nullptr) {
      return E_POINTER;
    }
    // Not advertised: applications keep their own echo cancellation unless the user turns it off.
    *effects = nullptr;
    *count = 0;
    return S_OK;
  }

  // ----- IApoAcousticEchoCancellation2 -----
  STDMETHODIMP GetDesiredReferenceStreamProperties(APO_REFERENCE_STREAM_PROPERTIES* properties) override {
    if (properties == nullptr) {
      return E_POINTER;
    }
    // Post-volume loopback tracks the user's output volume and mute; Windows falls back to
    // pre-volume loopback on endpoints that cannot provide it.
    *properties = APO_REFERENCE_STREAM_PROPERTIES_POST_VOLUME_LOOPBACK;
    return S_OK;
  }

  // ----- IApoAuxiliaryInputConfiguration -----
  STDMETHODIMP AddAuxiliaryInput(DWORD input_id, UINT32, BYTE*, APO_CONNECTION_DESCRIPTOR* connection) override {
    if (connection == nullptr || input_id == 0) {
      return E_INVALIDARG;
    }
    const Format format = ReadFormat(connection->pFormat);
    if (!IsAcceptableReference(format)) {
      return APOERR_FORMAT_NOT_SUPPORTED;
    }
    if (aux_input_id_.load(std::memory_order_acquire) != 0) {
      return APOERR_NUM_CONNECTIONS_INVALID;
    }
    reference_rate_.store(format.rate);
    reference_channels_.store(format.channels);
    aux_input_id_.store(input_id, std::memory_order_release);
    return S_OK;
  }

  STDMETHODIMP RemoveAuxiliaryInput(DWORD input_id) override {
    if (aux_input_id_.load(std::memory_order_acquire) != input_id) {
      return APOERR_INVALID_INPUTID;
    }
    aux_input_id_.store(0, std::memory_order_release);
    return S_OK;
  }

  STDMETHODIMP IsInputFormatSupported(IAudioMediaType* requested, IAudioMediaType** supported) override {
    if (requested == nullptr || supported == nullptr) {
      return E_POINTER;
    }
    *supported = nullptr;
    const Format format = ReadFormat(requested);
    if (IsAcceptableReference(format)) {
      requested->AddRef();
      *supported = requested;
      return S_OK;
    }
    // Ask the engine to convert the reference: 32-bit float, a WebRTC-native rate, at most stereo.
    const int rate = ncs::EchoProcessor::IsSupportedRate(format.rate) ? format.rate : kPreferredReferenceRate;
    const UINT32 channels = format.channels == 1 ? 1U : 2U;
    UNCOMPRESSEDAUDIOFORMAT preferred = {KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, channels, 4, 32,
                                         static_cast<FLOAT32>(rate),
                                         channels == 1 ? static_cast<DWORD>(KSAUDIO_SPEAKER_MONO)
                                                       : static_cast<DWORD>(KSAUDIO_SPEAKER_STEREO)};
    const HRESULT result = CreateAudioMediaTypeFromUncompressedAudioFormat(&preferred, supported);
    return FAILED(result) ? result : S_FALSE;
  }

  // The engine also uses the IAudioProcessingObject overload for the microphone connection.
  using CBaseAudioProcessingObject::IsInputFormatSupported;

  // ----- IApoAuxiliaryInputRT -----
  STDMETHODIMP_(void) AcceptInput(DWORD input_id, const APO_CONNECTION_PROPERTY* connection) override {
    if (connection == nullptr || input_id != aux_input_id_.load(std::memory_order_acquire)) {
      return;
    }
    const std::size_t samples = static_cast<std::size_t>(connection->u32ValidFrameCount)
        * reference_channels_.load(std::memory_order_relaxed);
    if (connection->u32BufferFlags == BUFFER_VALID) {
      ring_.Write(reinterpret_cast<const float*>(connection->pBuffer), samples);
    } else if (connection->u32BufferFlags == BUFFER_SILENT) {
      ring_.WriteSilence(samples);
    }
  }

 private:
  // Real-time. Feeds every complete 10 ms reference block received so far, after dropping any
  // backlog beyond kMaxReferenceBacklogBlocks (whole blocks, so frame alignment is preserved).
  bool FeedReference() noexcept {
    if (aux_input_id_.load(std::memory_order_acquire) == 0
        || reference_rate_.load(std::memory_order_relaxed) != locked_reference_rate_
        || reference_channels_.load(std::memory_order_relaxed) != locked_reference_channels_) {
      return true;  // no usable reference: AEC3 sees no far end and leaves the microphone alone
    }
    const std::size_t block = render_block_.size();
    const std::size_t available = ring_.Available();
    const std::size_t limit = block * kMaxReferenceBacklogBlocks;
    if (available > limit) {
      ring_.Discard(((available - limit + block - 1) / block) * block);
    }
    while (ring_.Read(render_block_.data(), block)) {
      if (!processor_.ProcessRender(render_block_.data())) {
        return false;
      }
    }
    return true;
  }

  IUnknown* outer_ = nullptr;
  volatile LONG references_ = 1;
  const bool disabled_by_policy_;
  bool mode_supported_ = true;

  std::atomic<DWORD> aux_input_id_{0};
  std::atomic<int> reference_rate_{0};
  std::atomic<UINT32> reference_channels_{0};
  int locked_reference_rate_ = kPreferredReferenceRate;
  UINT32 locked_reference_channels_ = 2;

  ncs::ReferenceRing ring_;
  ncs::EchoProcessor processor_;
  ncs::DelayLine dry_delay_;
  std::vector<float> render_block_;
  std::vector<float> silent_block_;
  std::vector<float> dry_block_;
  HNSTIME latency_ = 0;
  bool active_ = false;
  bool failed_ = false;
};

class ClassFactory final : public IClassFactory {
 public:
  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IClassFactory)) {
      *object = static_cast<IClassFactory*>(this);
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 2; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }

  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (outer != nullptr && iid != __uuidof(IUnknown)) {
      return E_NOINTERFACE;
    }
    try {
      auto* apo = new (std::nothrow) EchoCancellerApo(outer);
      if (apo == nullptr) {
        return E_OUTOFMEMORY;
      }
      const HRESULT result = apo->NonDelegatingQueryInterface(iid, object);
      apo->NonDelegatingRelease();
      return result;
    } catch (...) {
      return E_OUTOFMEMORY;
    }
  }

  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock) {
      ++g_locks;
    } else {
      --g_locks;
    }
    return S_OK;
  }
};

ClassFactory g_factory;

LSTATUS SetString(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
  return RegSetKeyValueW(HKEY_LOCAL_MACHINE, path.c_str(), name, REG_SZ, value.c_str(),
                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

// Synthetic far-end noise with a delayed, attenuated echo in the near-end signal.
std::int32_t RunSyntheticEchoTest() noexcept {
  ncs::EchoProcessor processor;
  if (!processor.Configure(16'000, 1, 16'000, 1)) {
    return 10;
  }
  std::uint32_t seed = 0x12345678U;
  auto noise = [&seed]() {
    seed = seed * 1664525U + 1013904223U;
    return static_cast<float>(static_cast<std::int32_t>(seed >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
  };
  constexpr std::size_t kFrames = 160;
  constexpr std::size_t kDelay = 640;  // 40 ms
  std::vector<float> history(kDelay + kFrames, 0.0F);
  std::vector<float> render(kFrames), capture(kFrames);
  double echo_in = 0.0, echo_out = 0.0;
  for (int block = 0; block < 400; ++block) {  // 4 s
    for (std::size_t i = 0; i < kFrames; ++i) {
      render[i] = 0.25F * noise();
      history[kDelay + i] = render[i];
      capture[i] = 0.3F * history[i];
    }
    std::memmove(history.data(), history.data() + kFrames, kDelay * sizeof(float));
    if (!processor.ProcessRender(render.data())) {
      return 11;
    }
    const std::vector<float> original = capture;
    if (!processor.ProcessCapture(capture.data())) {
      return 12;
    }
    if (block >= 300) {
      for (std::size_t i = 0; i < kFrames; ++i) {
        if (!std::isfinite(capture[i])) {
          return 13;
        }
        echo_in += static_cast<double>(original[i]) * original[i];
        echo_out += static_cast<double>(capture[i]) * capture[i];
      }
    }
  }
  return echo_out * 100.0 < echo_in ? 0 : 14;  // at least 20 dB reduction once converged
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (clsid != kEchoCancellerClsid) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  return g_factory.QueryInterface(iid, object);
}

STDAPI DllCanUnloadNow() { return g_objects.load() == 0 && g_locks.load() == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
  wchar_t module_path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(g_module, module_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return SELFREG_E_CLASS;
  }
  const std::wstring clsid_key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kEchoCancellerClsidString;
  if (SetString(clsid_key, nullptr, L"Noise Cancelling Stack Echo Canceller") != ERROR_SUCCESS
      || SetString(clsid_key + L"\\InprocServer32", nullptr, module_path) != ERROR_SUCCESS
      || SetString(clsid_key + L"\\InprocServer32", L"ThreadingModel", L"Both") != ERROR_SUCCESS) {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsid_key.c_str());
    return SELFREG_E_CLASS;
  }
  const HRESULT result = RegisterAPO(kRegistration);
  if (FAILED(result)) {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsid_key.c_str());
  }
  return result;
}

STDAPI DllUnregisterServer() {
  const HRESULT result = UnregisterAPO(kEchoCancellerClsid);
  const LSTATUS deleted = RegDeleteTreeW(
      HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kEchoCancellerClsidString).c_str());
  return FAILED(result) ? result : (deleted == ERROR_SUCCESS || deleted == ERROR_FILE_NOT_FOUND ? S_OK : SELFREG_E_CLASS);
}

// Pre-mutation payload check used by the manager: the class factory must produce an object that
// identifies itself as an echo-cancelling APO, and AEC3 must remove a synthetic echo.
extern "C" std::int32_t NcsAecApoSelfTest() noexcept {
  try {
    IClassFactory* factory = nullptr;
    if (FAILED(DllGetClassObject(kEchoCancellerClsid, __uuidof(IClassFactory), reinterpret_cast<void**>(&factory)))
        || factory == nullptr) {
      return 1;
    }
    IUnknown* object = nullptr;
    const HRESULT created = factory->CreateInstance(nullptr, __uuidof(IUnknown), reinterpret_cast<void**>(&object));
    factory->Release();
    if (FAILED(created) || object == nullptr) {
      return 2;
    }
    const IID required[] = {__uuidof(IAudioProcessingObject), __uuidof(IAudioProcessingObjectRT),
                            __uuidof(IAudioProcessingObjectConfiguration), __uuidof(IAudioSystemEffects),
                            __uuidof(IApoAcousticEchoCancellation), __uuidof(IApoAuxiliaryInputConfiguration),
                            __uuidof(IApoAuxiliaryInputRT)};
    std::int32_t status = 0;
    for (const IID& iid : required) {
      IUnknown* item = nullptr;
      if (FAILED(object->QueryInterface(iid, reinterpret_cast<void**>(&item))) || item == nullptr) {
        status = 3;
        break;
      }
      item->Release();
    }
    object->Release();
    return status != 0 ? status : RunSyntheticEchoTest();
  } catch (...) {
    return 4;
  }
}
