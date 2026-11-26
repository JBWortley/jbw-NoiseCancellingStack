// SPDX-License-Identifier: MIT
//
// THROWAWAY FEASIBILITY SPIKE - NOT PRODUCT CODE.
//
// A capture mode effect (MFX) that identifies itself to Windows 11 as an acoustic echo
// cancellation APO and records whether the audio engine supplies a loopback reference stream.
// Microphone audio is always passed through unchanged; the spike never cancels anything.
//
// What it records (to %ProgramData%\NcsAecSpike\apo.log):
//   - Initialize: processing mode (DEFAULT / COMMUNICATIONS / ...), endpoint, struct version.
//   - AddAuxiliaryInput: whether Windows attaches a reference, from which render endpoint,
//     in which format, and whether post-volume loopback was granted.
//   - Every 2 s while locked: microphone and reference call/frame counts, peaks, thread IDs,
//     and whether the reference callback ever overlapped APOProcess.
// Optional (HKLM\SOFTWARE\NcsAecSpike RecordSeconds > 0): the first N seconds of microphone
// and reference audio are written as float WAV files for offline AEC evaluation.

#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <olectl.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h>
#include <baseaudioprocessingobject.h>
#include <audiomediatype.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

// {0B3A9C87-CF54-45CD-9E30-942C93D8EDB6}
constexpr CLSID kSpikeClsid = {
    0x0B3A9C87, 0xCF54, 0x45CD, {0x9E, 0x30, 0x94, 0x2C, 0x93, 0xD8, 0xED, 0xB6}};
constexpr wchar_t kSpikeClsidString[] = L"{0B3A9C87-CF54-45CD-9E30-942C93D8EDB6}";
constexpr wchar_t kConfigKey[] = L"SOFTWARE\\NcsAecSpike";
constexpr DWORD kMaxRecordSeconds = 120;

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};
std::atomic<long> g_nextInstance{0};
SRWLOCK g_logLock = SRWLOCK_INIT;

// ---------------------------------------------------------------------------------------------
// Non-real-time helpers. None of these may be called from APOProcess or AcceptInput.
// ---------------------------------------------------------------------------------------------

std::wstring DataDirectory() {
  wchar_t buffer[MAX_PATH];
  const DWORD length = ExpandEnvironmentStringsW(L"%ProgramData%\\NcsAecSpike", buffer, MAX_PATH);
  if (length == 0 || length > MAX_PATH || buffer[0] == L'%') {
    return L"C:\\ProgramData\\NcsAecSpike";
  }
  return buffer;
}

std::string Utf8(const wchar_t* text) {
  if (text == nullptr) {
    return "(null)";
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string result(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
  return result;
}

std::string GuidText(const GUID& guid) {
  wchar_t buffer[64] = {};
  StringFromGUID2(guid, buffer, 64);
  return Utf8(buffer);
}

void AppendToLog(const std::string& line) {
  AcquireSRWLockExclusive(&g_logLock);
  std::wstring path = DataDirectory() + L"\\apo.log";
  HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    path = std::wstring(temp) + L"NcsAecSpike-apo.log";
    file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
  }
  OutputDebugStringA(line.c_str());
  ReleaseSRWLockExclusive(&g_logLock);
}

void Log(long instance, const char* format, ...) {
  char body[1536];
  va_list args;
  va_start(args, format);
  vsnprintf(body, sizeof(body), format, args);
  va_end(args);

  SYSTEMTIME now;
  GetLocalTime(&now);
  char line[1800];
  snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu apo#%ld %s\r\n",
           now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
           now.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), instance, body);
  AppendToLog(line);
}

DWORD ReadConfig(const wchar_t* name, DWORD fallback) {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, kConfigKey, name, RRF_RT_REG_DWORD, nullptr, &value,
                   &size) == ERROR_SUCCESS) {
    return value;
  }
  return fallback;
}

std::string ModeName(const GUID& mode) {
  if (mode == AUDIO_SIGNALPROCESSINGMODE_DEFAULT) return "DEFAULT";
  if (mode == AUDIO_SIGNALPROCESSINGMODE_COMMUNICATIONS) return "COMMUNICATIONS";
  if (mode == AUDIO_SIGNALPROCESSINGMODE_RAW) return "RAW";
  if (mode == AUDIO_SIGNALPROCESSINGMODE_SPEECH) return "SPEECH";
  if (mode == AUDIO_SIGNALPROCESSINGMODE_MEDIA) return "MEDIA";
  if (mode == AUDIO_SIGNALPROCESSINGMODE_MOVIE) return "MOVIE";
  if (mode == AUDIO_SIGNALPROCESSINGMODE_NOTIFICATION) return "NOTIFICATION";
  if (mode == GUID_NULL) return "NULL";
  return GuidText(mode);
}

struct FormatInfo {
  bool valid = false;
  bool isFloat = false;
  float rate = 0.0F;
  UINT32 channels = 0;
  UINT32 container = 0;
  UINT32 bits = 0;
  DWORD mask = 0;
};

FormatInfo ReadFormat(IAudioMediaType* type) {
  FormatInfo info;
  if (type == nullptr) {
    return info;
  }
  UNCOMPRESSEDAUDIOFORMAT format = {};
  if (FAILED(type->GetUncompressedAudioFormat(&format))) {
    return info;
  }
  info.valid = true;
  info.isFloat = format.guidFormatType == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  info.rate = format.fFramesPerSecond;
  info.channels = format.dwSamplesPerFrame;
  info.container = format.dwBytesPerSampleContainer;
  info.bits = format.dwValidBitsPerSample;
  info.mask = format.dwChannelMask;
  return info;
}

std::string Describe(IAudioMediaType* type) {
  if (type == nullptr) {
    return "(null format)";
  }
  const FormatInfo info = ReadFormat(type);
  if (!info.valid) {
    return "(not an uncompressed format)";
  }
  char text[160];
  snprintf(text, sizeof(text), "%s %.0f Hz %u ch %u/%u-bit mask=0x%lx",
           info.isFloat ? "float" : "PCM", info.rate, info.channels, info.bits,
           info.container * 8, info.mask);
  return text;
}

std::string DescribeDevices(IMMDeviceCollection* devices) {
  if (devices == nullptr) {
    return "(no device collection)";
  }
  UINT count = 0;
  if (FAILED(devices->GetCount(&count))) {
    return "(device collection GetCount failed)";
  }
  std::string result = "count=" + std::to_string(count);
  for (UINT index = 0; index < count; ++index) {
    IMMDevice* device = nullptr;
    if (FAILED(devices->Item(index, &device)) || device == nullptr) {
      continue;
    }
    LPWSTR id = nullptr;
    device->GetId(&id);
    std::string name = "?";
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store != nullptr) {
      PROPVARIANT value;
      PropVariantInit(&value);
      if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        name = Utf8(value.pwszVal);
      }
      PropVariantClear(&value);
      store->Release();
    }
    result += " [" + std::to_string(index) + "] " + name + " " + Utf8(id);
    CoTaskMemFree(id);
    device->Release();
  }
  return result;
}

// The endpoint that owns an APO is the last device in its initialization collection.
std::string LastDeviceId(IMMDeviceCollection* devices) {
  UINT count = 0;
  if (devices == nullptr || FAILED(devices->GetCount(&count)) || count == 0) {
    return {};
  }
  IMMDevice* device = nullptr;
  if (FAILED(devices->Item(count - 1, &device)) || device == nullptr) {
    return {};
  }
  LPWSTR id = nullptr;
  std::string result;
  if (SUCCEEDED(device->GetId(&id))) {
    result = Utf8(id);
    CoTaskMemFree(id);
  }
  device->Release();
  return result;
}

// "{0.0.1.00000000}.{1234abcd-...}" -> "1234abcd"
std::string ShortTag(const std::string& endpointId) {
  const size_t brace = endpointId.rfind('{');
  if (brace == std::string::npos || brace + 9 > endpointId.size()) {
    return "unknown";
  }
  return endpointId.substr(brace + 1, 8);
}

bool WriteFloatWav(const std::wstring& path, const float* samples, size_t sampleCount,
                   UINT32 rate, UINT32 channels) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  const uint32_t dataBytes = static_cast<uint32_t>(sampleCount * sizeof(float));
  const uint16_t blockAlign = static_cast<uint16_t>(channels * sizeof(float));
  const uint32_t frames = channels == 0 ? 0 : static_cast<uint32_t>(sampleCount / channels);
#pragma pack(push, 1)
  struct Header {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t riffSize = 0;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmtSize = 16;
    uint16_t formatTag = 3;  // WAVE_FORMAT_IEEE_FLOAT
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint32_t byteRate = 0;
    uint16_t blockAlign = 0;
    uint16_t bits = 32;
    char fact[4] = {'f', 'a', 'c', 't'};
    uint32_t factSize = 4;
    uint32_t frames = 0;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize = 0;
  } header;
#pragma pack(pop)
  header.riffSize = static_cast<uint32_t>(sizeof(Header) - 8 + dataBytes);
  header.channels = static_cast<uint16_t>(channels);
  header.rate = rate;
  header.byteRate = rate * blockAlign;
  header.blockAlign = blockAlign;
  header.frames = frames;
  header.dataSize = dataBytes;
  DWORD written = 0;
  bool ok = WriteFile(file, &header, sizeof(header), &written, nullptr) != FALSE;
  ok = ok && WriteFile(file, samples, dataBytes, &written, nullptr) != FALSE;
  CloseHandle(file);
  return ok;
}

// ---------------------------------------------------------------------------------------------
// Real-time-safe statistics shared between the RT callbacks and the reporting thread.
// ---------------------------------------------------------------------------------------------

struct StreamStats {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> silentCalls{0};
  std::atomic<uint64_t> v2Calls{0};
  std::atomic<float> peak{0.0F};
  std::atomic<DWORD> lastThread{0};
  std::atomic<uint64_t> lastBufferQpc{0};
  std::atomic<int64_t> lastCallQpc{0};
};

float BlockPeak(const float* samples, size_t count) {
  float peak = 0.0F;
  for (size_t index = 0; index < count; ++index) {
    const float magnitude = std::fabs(samples[index]);
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  return peak;
}

struct Recorder {
  std::vector<float> samples;
  std::atomic<size_t> position{0};
  UINT32 rate = 0;
  UINT32 channels = 0;
  std::atomic<uint64_t> firstBufferQpc{0};
  std::atomic<int64_t> firstCallQpc{0};

  bool Full() const { return !samples.empty() && position.load(std::memory_order_acquire) >= samples.size(); }

  // Real-time: copies without allocation and publishes the new position with release order.
  void Append(const float* source, size_t count, uint64_t bufferQpc, int64_t callQpc) {
    const size_t start = position.load(std::memory_order_relaxed);
    if (start >= samples.size()) {
      return;
    }
    if (start == 0) {
      firstBufferQpc.store(bufferQpc, std::memory_order_relaxed);
      firstCallQpc.store(callQpc, std::memory_order_relaxed);
    }
    const size_t n = std::min(count, samples.size() - start);
    std::memcpy(samples.data() + start, source, n * sizeof(float));
    position.store(start + n, std::memory_order_release);
  }
};

class INonDelegatingUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE NonDelegatingQueryInterface(REFIID iid, void** object) = 0;
  virtual ULONG STDMETHODCALLTYPE NonDelegatingAddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE NonDelegatingRelease() = 0;
};

#pragma warning(suppress : 4815)  // CRegAPOProperties<1> carries a zero-length IID tail.
const CRegAPOProperties<1> kRegistration(
    kSpikeClsid, L"NCS AEC reference spike (pass-through)", L"MIT - Noise Cancelling Stack spike",
    1, 0, __uuidof(IAudioProcessingObject),
    static_cast<APO_FLAG>(APO_FLAG_DEFAULT | APO_FLAG_INPLACE));

class SpikeApo final : public CBaseAudioProcessingObject,
                       public IAudioSystemEffects2,
                       public IApoAcousticEchoCancellation2,
                       public IApoAuxiliaryInputConfiguration,
                       public IApoAuxiliaryInputRT,
                       public INonDelegatingUnknown {
 public:
  explicit SpikeApo(IUnknown* outer)
      : CBaseAudioProcessingObject(kRegistration),
        instance_(++g_nextInstance),
        advertiseAec_(ReadConfig(L"AdvertiseAec", 0) != 0),
        postVolume_(ReadConfig(L"PostVolumeLoopback", 0) != 0),
        recordSeconds_(std::min(ReadConfig(L"RecordSeconds", 0), kMaxRecordSeconds)) {
    outer_ = outer != nullptr ? outer : reinterpret_cast<IUnknown*>(static_cast<INonDelegatingUnknown*>(this));
    ++g_objects;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    qpcFrequency_ = frequency.QuadPart;
    Log(instance_, "created aggregated=%d advertiseAec=%d postVolume=%d recordSeconds=%lu",
        outer != nullptr, advertiseAec_, postVolume_, recordSeconds_);
  }

  ~SpikeApo() override {
    StopReporter();
    Log(instance_, "destroyed");
    --g_objects;
  }

  // ----- IUnknown (delegating to the outer object when aggregated) -----
  STDMETHODIMP QueryInterface(REFIID iid, void** object) override { return outer_->QueryInterface(iid, object); }
  STDMETHODIMP_(ULONG) AddRef() override { return outer_->AddRef(); }
  STDMETHODIMP_(ULONG) Release() override { return outer_->Release(); }

  // ----- INonDelegatingUnknown -----
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
      Log(instance_, "QueryInterface: not implemented %s", GuidText(iid).c_str());
      return E_NOINTERFACE;
    }
    reinterpret_cast<IUnknown*>(*object)->AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE NonDelegatingAddRef() override { return InterlockedIncrement(&references_); }

  ULONG STDMETHODCALLTYPE NonDelegatingRelease() override {
    const ULONG remaining = InterlockedDecrement(&references_);
    if (remaining == 0) {
      delete this;
    }
    return remaining;
  }

  // ----- IAudioProcessingObject -----
  STDMETHODIMP Initialize(UINT32 size, BYTE* data) override {
    if ((data == nullptr) != (size == 0)) {
      return E_INVALIDARG;
    }
    std::string detail = "unrecognised init struct";
    if (size == sizeof(APOInitSystemEffects3)) {
      const auto* init = reinterpret_cast<APOInitSystemEffects3*>(data);
      mode_ = init->AudioProcessingMode;
      endpointTag_ = ShortTag(LastDeviceId(init->pDeviceCollection));
      detail = "APOInitSystemEffects3 mode=" + ModeName(init->AudioProcessingMode)
          + " discoveryOnly=" + std::to_string(init->InitializeForDiscoveryOnly)
          + " serviceProvider=" + (init->pServiceProvider != nullptr ? "yes" : "no")
          + " devices: " + DescribeDevices(init->pDeviceCollection);
    } else if (size == sizeof(APOInitSystemEffects2)) {
      const auto* init = reinterpret_cast<APOInitSystemEffects2*>(data);
      mode_ = init->AudioProcessingMode;
      endpointTag_ = ShortTag(LastDeviceId(init->pDeviceCollection));
      detail = "APOInitSystemEffects2 mode=" + ModeName(init->AudioProcessingMode)
          + " discoveryOnly=" + std::to_string(init->InitializeForDiscoveryOnly)
          + " devices: " + DescribeDevices(init->pDeviceCollection);
    } else if (size == sizeof(APOInitSystemEffects)) {
      detail = "APOInitSystemEffects (v1, no mode)";
    }
    Log(instance_, "Initialize size=%u %s", size, detail.c_str());
    m_bIsInitialized = true;
    return S_OK;
  }

  STDMETHODIMP GetLatency(HNSTIME* time) override {
    if (time == nullptr) {
      return E_POINTER;
    }
    *time = 0;
    return S_OK;
  }

  STDMETHODIMP IsInputFormatSupported(IAudioMediaType* output, IAudioMediaType* requested,
                                      IAudioMediaType** supported) override {
    const HRESULT result = CBaseAudioProcessingObject::IsInputFormatSupported(output, requested, supported);
    Log(instance_, "IsInputFormatSupported(mic) requested=%s output=%s -> 0x%08lx",
        Describe(requested).c_str(), Describe(output).c_str(), result);
    return result;
  }

  // ----- IAudioProcessingObjectConfiguration -----
  STDMETHODIMP LockForProcess(UINT32 inputCount, APO_CONNECTION_DESCRIPTOR** inputs,
                              UINT32 outputCount, APO_CONNECTION_DESCRIPTOR** outputs) override {
    const HRESULT result =
        CBaseAudioProcessingObject::LockForProcess(inputCount, inputs, outputCount, outputs);
    const FormatInfo micFormat = inputCount > 0 ? ReadFormat(inputs[0]->pFormat) : FormatInfo{};
    Log(instance_,
        "LockForProcess -> 0x%08lx inputs=%u outputs=%u mic=%s out=%s maxFrames=%u reference=%s",
        result, inputCount, outputCount,
        inputCount > 0 ? Describe(inputs[0]->pFormat).c_str() : "-",
        outputCount > 0 ? Describe(outputs[0]->pFormat).c_str() : "-",
        inputCount > 0 ? inputs[0]->u32MaxFrameCount : 0,
        auxInputId_.load() != 0 ? "attached" : "NOT attached");
    if (FAILED(result)) {
      return result;
    }

    micChannels_ = m_u32SamplesPerFrame;
    micStats_.~StreamStats();
    new (&micStats_) StreamStats();
    refStats_.~StreamStats();
    new (&refStats_) StreamStats();
    overlaps_ = 0;

    if (recordSeconds_ > 0 && !recordingArmed_.load() && !recordingAllocated_) {
      const auto micRate = static_cast<UINT32>(micFormat.rate);
      micRecorder_.rate = micRate;
      micRecorder_.channels = micChannels_;
      micRecorder_.samples.assign(static_cast<size_t>(recordSeconds_) * micRate * micChannels_, 0.0F);
      refRecorder_.rate = refRate_.load();
      refRecorder_.channels = refChannels_.load();
      refRecorder_.samples.assign(
          static_cast<size_t>(recordSeconds_) * refRecorder_.rate * refRecorder_.channels, 0.0F);
      recordingAllocated_ = true;
      recordingArmed_.store(refRecorder_.channels > 0, std::memory_order_release);
      Log(instance_, "recording armed=%d mic=%u Hz x%u ref=%u Hz x%u for %lu s",
          recordingArmed_.load(), micRecorder_.rate, micRecorder_.channels, refRecorder_.rate,
          refRecorder_.channels, recordSeconds_);
    }

    StartReporter();
    return result;
  }

  STDMETHODIMP UnlockForProcess() override {
    StopReporter();
    Report("UnlockForProcess (final)");
    recordingArmed_.store(false, std::memory_order_release);
    SaveRecordings();
    return CBaseAudioProcessingObject::UnlockForProcess();
  }

  // ----- IAudioProcessingObjectRT -----
  STDMETHODIMP_(void) APOProcess(UINT32 inputCount, APO_CONNECTION_PROPERTY** inputs,
                                 UINT32 outputCount, APO_CONNECTION_PROPERTY** outputs) override {
    if (inputCount < 1 || outputCount < 1 || inputs[0] == nullptr || outputs[0] == nullptr) {
      return;
    }
    APO_CONNECTION_PROPERTY* source = inputs[0];
    APO_CONNECTION_PROPERTY* destination = outputs[0];
    const UINT32 frames = source->u32ValidFrameCount;
    const size_t samples = static_cast<size_t>(frames) * micChannels_;
    const auto* input = reinterpret_cast<const float*>(source->pBuffer);

    if ((source->u32BufferFlags == BUFFER_VALID || source->u32BufferFlags == BUFFER_SILENT)
        && destination->pBuffer != source->pBuffer) {
      std::memcpy(reinterpret_cast<void*>(destination->pBuffer), input, samples * sizeof(float));
    }
    destination->u32ValidFrameCount = frames;
    destination->u32BufferFlags = source->u32BufferFlags;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t bufferQpc = 0;
    if (source->u32Signature == APO_CONNECTION_PROPERTY_V2_SIGNATURE) {
      bufferQpc = reinterpret_cast<const APO_CONNECTION_PROPERTY_V2*>(source)->u64QPCTime;
      micStats_.v2Calls.fetch_add(1, std::memory_order_relaxed);
    }
    Record(micStats_, source->u32BufferFlags, frames, input, samples, bufferQpc, now.QuadPart);
    if (inAcceptInput_.load(std::memory_order_acquire) != 0) {
      overlaps_.fetch_add(1, std::memory_order_relaxed);
    }
    if (recordingArmed_.load(std::memory_order_acquire) && referenceSeen_.load(std::memory_order_acquire)
        && source->u32BufferFlags == BUFFER_VALID) {
      micRecorder_.Append(input, samples, bufferQpc, now.QuadPart);
    }
  }

  // ----- IAudioSystemEffects2 -----
  STDMETHODIMP GetEffectsList(LPGUID* effects, UINT* count, HANDLE) override {
    if (effects == nullptr || count == nullptr) {
      return E_POINTER;
    }
    *effects = nullptr;
    *count = 0;
    if (advertiseAec_) {
      *effects = static_cast<LPGUID>(CoTaskMemAlloc(sizeof(GUID)));
      if (*effects == nullptr) {
        return E_OUTOFMEMORY;
      }
      (*effects)[0] = AUDIO_EFFECT_TYPE_ACOUSTIC_ECHO_CANCELLATION;
      *count = 1;
    }
    Log(instance_, "GetEffectsList -> %u effect(s)", *count);
    return S_OK;
  }

  // ----- IApoAcousticEchoCancellation2 -----
  STDMETHODIMP GetDesiredReferenceStreamProperties(APO_REFERENCE_STREAM_PROPERTIES* properties) override {
    if (properties == nullptr) {
      return E_POINTER;
    }
    *properties = postVolume_ ? APO_REFERENCE_STREAM_PROPERTIES_POST_VOLUME_LOOPBACK
                              : APO_REFERENCE_STREAM_PROPERTIES_NONE;
    Log(instance_, "GetDesiredReferenceStreamProperties -> %s", postVolume_ ? "POST_VOLUME" : "NONE");
    return S_OK;
  }

  // ----- IApoAuxiliaryInputConfiguration -----
  STDMETHODIMP AddAuxiliaryInput(DWORD inputId, UINT32 size, BYTE* data,
                                 APO_CONNECTION_DESCRIPTOR* connection) override {
    std::string detail = "unrecognised payload";
    if (data != nullptr && size == sizeof(AcousticEchoCanceller_Reference_Input)) {
      const auto* reference = reinterpret_cast<AcousticEchoCanceller_Reference_Input*>(data);
      detail = std::string("AcousticEchoCanceller_Reference_Input streamProperties=")
          + ((reference->streamProperties & APO_REFERENCE_STREAM_PROPERTIES_POST_VOLUME_LOOPBACK) != 0
                 ? "POST_VOLUME"
                 : "PRE_VOLUME")
          + " mode=" + ModeName(reference->apoInitSystemEffects.AudioProcessingMode)
          + " render devices: " + DescribeDevices(reference->apoInitSystemEffects.pDeviceCollection);
    } else if (data != nullptr && size == sizeof(APOInitSystemEffects3)) {
      const auto* init = reinterpret_cast<APOInitSystemEffects3*>(data);
      detail = "APOInitSystemEffects3 render devices: " + DescribeDevices(init->pDeviceCollection);
    } else if (data != nullptr && size == sizeof(APOInitSystemEffects2)) {
      const auto* init = reinterpret_cast<APOInitSystemEffects2*>(data);
      detail = "APOInitSystemEffects2 render devices: " + DescribeDevices(init->pDeviceCollection);
    }

    IAudioMediaType* format = connection != nullptr ? connection->pFormat : nullptr;
    const FormatInfo info = ReadFormat(format);
    Log(instance_, "AddAuxiliaryInput id=%lu size=%u locked=%d format=%s maxFrames=%u %s", inputId,
        size, m_bIsLocked, Describe(format).c_str(),
        connection != nullptr ? connection->u32MaxFrameCount : 0, detail.c_str());

    if (!info.valid || !info.isFloat || info.container != 4) {
      Log(instance_, "AddAuxiliaryInput rejected: reference is not 32-bit float");
      return APOERR_FORMAT_NOT_SUPPORTED;
    }
    if (auxInputId_.load() != 0) {
      Log(instance_, "AddAuxiliaryInput rejected: already have input %lu", auxInputId_.load());
      return APOERR_NUM_CONNECTIONS_INVALID;
    }
    refChannels_.store(info.channels);
    refRate_.store(static_cast<UINT32>(info.rate));
    auxInputId_.store(inputId);
    return S_OK;
  }

  STDMETHODIMP RemoveAuxiliaryInput(DWORD inputId) override {
    Log(instance_, "RemoveAuxiliaryInput id=%lu locked=%d (had %lu)", inputId, m_bIsLocked, auxInputId_.load());
    if (auxInputId_.load() != inputId) {
      return APOERR_INVALID_INPUTID;
    }
    auxInputId_.store(0);
    return S_OK;
  }

  STDMETHODIMP IsInputFormatSupported(IAudioMediaType* requested, IAudioMediaType** supported) override {
    if (requested == nullptr || supported == nullptr) {
      return E_POINTER;
    }
    *supported = nullptr;
    const FormatInfo info = ReadFormat(requested);
    if (info.valid && info.isFloat && info.container == 4) {
      requested->AddRef();
      *supported = requested;
      Log(instance_, "IsInputFormatSupported(reference) %s -> S_OK", Describe(requested).c_str());
      return S_OK;
    }
    // Ask for 32-bit float at the offered rate and channel count; the engine converts.
    UNCOMPRESSEDAUDIOFORMAT preferred = {KSDATAFORMAT_SUBTYPE_IEEE_FLOAT,
                                         info.channels != 0 ? info.channels : 2, 4, 32,
                                         info.rate != 0.0F ? info.rate : 48000.0F,
                                         KSAUDIO_SPEAKER_DIRECTOUT};
    const HRESULT result = CreateAudioMediaTypeFromUncompressedAudioFormat(&preferred, supported);
    Log(instance_, "IsInputFormatSupported(reference) %s -> suggest float (0x%08lx)",
        Describe(requested).c_str(), result);
    return FAILED(result) ? result : S_FALSE;
  }

  // ----- IApoAuxiliaryInputRT -----
  STDMETHODIMP_(void) AcceptInput(DWORD, const APO_CONNECTION_PROPERTY* connection) override {
    if (connection == nullptr) {
      return;
    }
    inAcceptInput_.fetch_add(1, std::memory_order_acq_rel);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const UINT32 frames = connection->u32ValidFrameCount;
    const size_t samples = static_cast<size_t>(frames) * refChannels_.load(std::memory_order_relaxed);
    const auto* input = reinterpret_cast<const float*>(connection->pBuffer);
    uint64_t bufferQpc = 0;
    if (connection->u32Signature == APO_CONNECTION_PROPERTY_V2_SIGNATURE) {
      bufferQpc = reinterpret_cast<const APO_CONNECTION_PROPERTY_V2*>(connection)->u64QPCTime;
      refStats_.v2Calls.fetch_add(1, std::memory_order_relaxed);
    }
    Record(refStats_, connection->u32BufferFlags, frames, input, samples, bufferQpc, now.QuadPart);
    if (recordingArmed_.load(std::memory_order_acquire)) {
      referenceSeen_.store(true, std::memory_order_release);
      if (connection->u32BufferFlags == BUFFER_VALID) {
        refRecorder_.Append(input, samples, bufferQpc, now.QuadPart);
      } else {
        // Keep the reference timeline continuous for offline alignment.
        AppendSilence(refRecorder_, samples, bufferQpc, now.QuadPart);
      }
    }
    inAcceptInput_.fetch_sub(1, std::memory_order_acq_rel);
  }

 private:
  static void Record(StreamStats& stats, UINT32 flags, UINT32 frames, const float* samples,
                     size_t sampleCount, uint64_t bufferQpc, int64_t callQpc) {
    stats.calls.fetch_add(1, std::memory_order_relaxed);
    stats.frames.fetch_add(frames, std::memory_order_relaxed);
    stats.lastThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    stats.lastBufferQpc.store(bufferQpc, std::memory_order_relaxed);
    stats.lastCallQpc.store(callQpc, std::memory_order_relaxed);
    if (flags == BUFFER_SILENT) {
      stats.silentCalls.fetch_add(1, std::memory_order_relaxed);
    } else if (flags == BUFFER_VALID && samples != nullptr) {
      const float peak = BlockPeak(samples, sampleCount);
      if (peak > stats.peak.load(std::memory_order_relaxed)) {
        stats.peak.store(peak, std::memory_order_relaxed);
      }
    }
  }

  static void AppendSilence(Recorder& recorder, size_t count, uint64_t bufferQpc, int64_t callQpc) {
    static const float zeros[4096] = {};
    while (count > 0) {
      const size_t n = std::min<size_t>(count, 4096);
      recorder.Append(zeros, n, bufferQpc, callQpc);
      count -= n;
    }
  }

  double QpcToMs(int64_t ticks) const {
    return qpcFrequency_ == 0 ? 0.0 : static_cast<double>(ticks) * 1000.0 / static_cast<double>(qpcFrequency_);
  }

  void Report(const char* label) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float micPeak = micStats_.peak.exchange(0.0F);
    const float refPeak = refStats_.peak.exchange(0.0F);
    const int64_t micCall = micStats_.lastCallQpc.load();
    const int64_t refCall = refStats_.lastCallQpc.load();
    Log(instance_,
        "%s endpoint=%s mode=%s | mic calls=%llu frames=%llu silent=%llu v2=%llu peak=%.4f tid=%lu age=%.1fms "
        "| ref calls=%llu frames=%llu silent=%llu v2=%llu peak=%.4f tid=%lu age=%.1fms "
        "| overlaps=%llu bufQpc mic=%llu ref=%llu qpcNow=%lld freq=%lld | rec mic=%zu/%zu ref=%zu/%zu",
        label, endpointTag_.c_str(), ModeName(mode_).c_str(), micStats_.calls.load(), micStats_.frames.load(),
        micStats_.silentCalls.load(), micStats_.v2Calls.load(), micPeak, micStats_.lastThread.load(),
        micCall == 0 ? -1.0 : QpcToMs(now.QuadPart - micCall), refStats_.calls.load(),
        refStats_.frames.load(), refStats_.silentCalls.load(), refStats_.v2Calls.load(), refPeak,
        refStats_.lastThread.load(), refCall == 0 ? -1.0 : QpcToMs(now.QuadPart - refCall),
        overlaps_.load(), micStats_.lastBufferQpc.load(), refStats_.lastBufferQpc.load(),
        now.QuadPart, qpcFrequency_, micRecorder_.position.load(), micRecorder_.samples.size(),
        refRecorder_.position.load(), refRecorder_.samples.size());
  }

  void SaveRecordings() {
    if (!recordingAllocated_ || recordingsSaved_) {
      return;
    }
    const size_t micCount = micRecorder_.position.load(std::memory_order_acquire);
    const size_t refCount = refRecorder_.position.load(std::memory_order_acquire);
    if (micCount == 0 && refCount == 0) {
      return;
    }
    const std::wstring directory = DataDirectory() + L"\\recordings";
    CreateDirectoryW(directory.c_str(), nullptr);
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stamp[128];
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u-%hs-%hs-apo%ld", now.wYear, now.wMonth, now.wDay,
               now.wHour, now.wMinute, now.wSecond, endpointTag_.c_str(), ModeName(mode_).c_str(),
               instance_);
    const std::wstring micPath = directory + L"\\" + stamp + L"-mic.wav";
    const std::wstring refPath = directory + L"\\" + stamp + L"-ref.wav";
    const bool micOk = WriteFloatWav(micPath, micRecorder_.samples.data(), micCount,
                                     micRecorder_.rate, micRecorder_.channels);
    const bool refOk = WriteFloatWav(refPath, refRecorder_.samples.data(), refCount,
                                     refRecorder_.rate, refRecorder_.channels);
    recordingsSaved_ = true;
    Log(instance_,
        "saved recordings mic=%s (%d, %zu samples, firstBufQpc=%llu firstCallQpc=%lld) "
        "ref=%s (%d, %zu samples, firstBufQpc=%llu firstCallQpc=%lld)",
        Utf8(micPath.c_str()).c_str(), micOk, micCount, micRecorder_.firstBufferQpc.load(),
        micRecorder_.firstCallQpc.load(), Utf8(refPath.c_str()).c_str(), refOk, refCount,
        refRecorder_.firstBufferQpc.load(), refRecorder_.firstCallQpc.load());
  }

  void StartReporter() {
    if (reporter_ != nullptr) {
      return;
    }
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    reporter_ = CreateThread(nullptr, 0, &SpikeApo::ReporterMain, this, 0, nullptr);
  }

  void StopReporter() {
    if (reporter_ == nullptr) {
      return;
    }
    SetEvent(stopEvent_);
    WaitForSingleObject(reporter_, 5000);
    CloseHandle(reporter_);
    CloseHandle(stopEvent_);
    reporter_ = nullptr;
    stopEvent_ = nullptr;
  }

  static DWORD WINAPI ReporterMain(void* parameter) {
    auto* self = static_cast<SpikeApo*>(parameter);
    while (WaitForSingleObject(self->stopEvent_, 2000) == WAIT_TIMEOUT) {
      self->Report("progress");
      if (self->micRecorder_.Full() && !self->recordingsSaved_) {
        self->recordingArmed_.store(false, std::memory_order_release);
        self->SaveRecordings();
      }
    }
    return 0;
  }

  IUnknown* outer_ = nullptr;
  volatile LONG references_ = 1;
  const long instance_;
  const bool advertiseAec_;
  const bool postVolume_;
  const DWORD recordSeconds_;
  int64_t qpcFrequency_ = 0;
  GUID mode_ = GUID_NULL;
  std::string endpointTag_ = "unknown";

  UINT32 micChannels_ = 0;
  std::atomic<DWORD> auxInputId_{0};
  std::atomic<UINT32> refChannels_{0};
  std::atomic<UINT32> refRate_{0};

  StreamStats micStats_;
  StreamStats refStats_;
  std::atomic<int> inAcceptInput_{0};
  std::atomic<uint64_t> overlaps_{0};

  Recorder micRecorder_;
  Recorder refRecorder_;
  std::atomic<bool> recordingArmed_{false};
  std::atomic<bool> referenceSeen_{false};
  bool recordingAllocated_ = false;
  bool recordingsSaved_ = false;

  HANDLE reporter_ = nullptr;
  HANDLE stopEvent_ = nullptr;
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
    auto* apo = new (std::nothrow) SpikeApo(outer);
    if (apo == nullptr) {
      return E_OUTOFMEMORY;
    }
    const HRESULT result = apo->NonDelegatingQueryInterface(iid, object);
    apo->NonDelegatingRelease();
    return result;
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

LSTATUS SetString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
  return RegSetKeyValueW(root, path.c_str(), name, REG_SZ, value.c_str(),
                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
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
  if (clsid != kSpikeClsid) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  return g_factory.QueryInterface(iid, object);
}

STDAPI DllCanUnloadNow() { return g_objects.load() == 0 && g_locks.load() == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
  wchar_t modulePath[MAX_PATH] = {};
  GetModuleFileNameW(g_module, modulePath, MAX_PATH);
  const std::wstring clsidKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kSpikeClsidString;
  if (SetString(HKEY_LOCAL_MACHINE, clsidKey, nullptr, L"NCS AEC reference spike") != ERROR_SUCCESS
      || SetString(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", nullptr, modulePath) != ERROR_SUCCESS
      || SetString(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Both")
             != ERROR_SUCCESS) {
    return SELFREG_E_CLASS;
  }
  return RegisterAPO(kRegistration);
}

STDAPI DllUnregisterServer() {
  const HRESULT result = UnregisterAPO(kSpikeClsid);
  RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kSpikeClsidString).c_str());
  return result;
}
