# WebRTC APM 16 kHz Fallback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a hardware-vendor-agnostic, system-wide WebRTC APM noise-suppression fallback for exactly 16 kHz capture streams while retaining RNNoise at exactly 48 kHz and the existing startup, repair, and rollback guarantees.

**Architecture:** Equalizer APO remains the only runtime host. Its managed capture configuration routes 48 kHz to the existing RNNoise VST2 plug-in and 16 kHz to a new mono VST2 plug-in that statically links WebRTC APM; all other rates pass through. The manager validates both payloads and the WebRTC self-test before mutation, accepts the exact 1.0.1 configuration during upgrade, and keeps the existing SYSTEM boot-repair workflow.

**Tech Stack:** .NET 8/C# manager and xUnit tests; C++20 native VST2 plug-in; WebRTC APM `m155.8059.4.0`; FST VST2 ABI headers; Meson 1.12.1; Ninja 1.13.2; PowerShell; Inno Setup 6; Equalizer APO 1.4.2.

**Spec:** `docs/superpowers/specs/2026-10-22-webrtc-apm-16khz-fallback-design.md`

## Global Constraints

- Windows 10/11 x64 shared-mode capture only; never alter render endpoints.
- Exactly 48,000 Hz uses RNNoise 1.10; exactly 16,000 Hz uses WebRTC APM; every other rate passes through.
- Both processors remain nested under `stage == "capture"` and self-bypass at unsupported rates.
- WebRTC APM enables only noise suppression at `kHigh`; AEC, AGC, high-pass, transient suppression, and other APM features remain disabled.
- The native processor uses mono 160-sample/10 ms frames and a fixed 160-sample processing delay.
- Native callback failure is permanent-instance, delayed-dry pass-through; no exception crosses the VST ABI.
- The WebRTC plug-in is a single x64 DLL with WebRTC and the Microsoft runtime linked statically; no vendor runtime, service, virtual microphone, or secondary WebRTC DLL.
- Pin WebRTC source release `m155.8059.4.0`, commit `3a6b666a906da6cb3c6f9d474fab21c2ba739f22`, archive SHA-256 `028DF34D5ED4A1015B7349D2A099EFB27C17A68ACEFB5C1D448F833207F5DB9A`.
- Pin Meson `1.12.1` wheel SHA-256 `930BC7542CBD9F57009E182FD014EBA48CF1A0180A7B9006D2D32D8F168D8B02` and Ninja `1.13.2` Windows ZIP SHA-256 `07FC8261B42B20E71D1720B39068C2E14FFCEE6396B76FB7A795FB460B78DC65`.
- Use the FST headers from the already-pinned Noise Suppression for Voice 1.10 corresponding-source archive; license new plug-in sources as GPL-3.0-only.
- Manager/installer code remains MIT; third-party source, licenses, notices, and patent grant remain separately identified and distributed.
- Release version is `1.1.0`; state schema remains `1` because endpoint journal structure does not change.
- Installation remains offline; build-time retrieval is checksum-pinned.
- Automated verification must not capture a microphone, play audio, launch a visible application, or manipulate the user's desktop.

## Review Focus

- A VST host calls `processReplacing` before activation, with zero frames, or with block sizes of 1, 159, 160, 161, and 4097 samples: the plug-in must not crash, allocate an unbounded queue, or reorder samples; Task 2 adds these cases.
- Two VST instances process interleaved blocks on separate threads: all frame, delay, and bypass state must remain instance-local; Task 3 adds the concurrency smoke test.
- WebRTC fails after input has already been buffered: output must switch to the correctly delayed dry stream without dropping or duplicating samples; Task 2 adds a mid-frame failure test.
- An existing managed configuration resembles 1.0.1 but has any changed line or plug-in path: it must not authorize a nonempty recovery journal; Task 1 adds near-match rejection tests.
- Both files pass hash validation but the WebRTC export is absent or returns nonzero: install/repair must stop before backup, configuration, or endpoint mutation; Task 4 adds fake-probe ordering tests and Task 3 exercises the real export.

## File Structure

- `native/webrtc-apm-vst2/frame_adapter.{h,cpp}`: fixed-delay adaptation between arbitrary host blocks and 160-sample frames.
- `native/webrtc-apm-vst2/webrtc_processor.{h,cpp}`: WebRTC APM construction, configuration, warm-up, and one-frame processing.
- `native/webrtc-apm-vst2/vst_plugin.cpp`: minimal FST-based mono VST2 ABI and exported self-test.
- `native/webrtc-apm-vst2/meson.build`: native library and test targets.
- `native/webrtc-apm-vst2/tests/*.cpp`: dependency-free native unit tests and VST smoke host.
- `scripts/build-native.ps1`: pinned native-tool/source bootstrap, build, test, dependency inspection, and payload manifest generation.
- `src/NoiseCancellingStack.Manager/AudioRoutePlanner.cs`: sample-rate-to-processor classification shared by status output.
- `src/NoiseCancellingStack.Manager/PayloadValidator.cs`: checksums and injectable WebRTC plug-in probe.
- `src/NoiseCancellingStack.Manager/NativeWebRtcPluginProbe.cs`: real `NativeLibrary` self-test invocation.
- Existing manager, installer, build, documentation, and test files change only where named below.

---

### Task 1: Managed rate routing, status classification, and trusted upgrade

**Files:**
- Create: `src/NoiseCancellingStack.Manager/AudioRoutePlanner.cs`
- Create: `tests/NoiseCancellingStack.Tests/AudioRoutePlannerTests.cs`
- Modify: `src/NoiseCancellingStack.Manager/ConfigManager.cs`
- Modify: `src/NoiseCancellingStack.Manager/ProductConstants.cs`
- Modify: `src/NoiseCancellingStack.Manager/InstallerEngine.cs`
- Modify: `src/NoiseCancellingStack.Manager/Program.cs`
- Modify: `tests/NoiseCancellingStack.Tests/ConfigManagerTests.cs`

**Interfaces:**
- Consumes: Existing marked-include, atomic-write, exact-installation, state-journal trust, and endpoint status behavior.
- Produces: `CaptureProcessor { PassThrough, WebRtcApm, RnNoise }`; `AudioRoutePlanner.Select(uint sampleRate) -> CaptureProcessor`; `ConfigManager.BuildManagedConfig(string rnNoisePluginPath, string webRtcPluginPath) -> string`; `ConfigManager.IsManagedInstallation(string configDirectory, string rnNoisePluginPath, string webRtcPluginPath) -> bool`; `ConfigManager.IsTrustedManagedInstallation(string configDirectory, string rnNoisePluginPath, string webRtcPluginPath) -> bool`; dual-path `Install` and `Uninstall` overload replacements.

- [ ] **Step 1: Update configuration tests first**

Add tests named `BuildManagedConfig_RoutesCaptureRatesToExclusiveProcessors`, `BuildManagedConfig_RejectsInjectionInEitherPath`, `IsManagedInstallation_RequiresExactDualEngineFiles`, `IsTrustedManagedInstallation_AcceptsExactVersion101Config`, `IsTrustedManagedInstallation_RejectsLegacyNearMatches`, and `Install_IsIdempotentForDualEngineConfig`. Assert exact 48 kHz/RNNoise and 16 kHz/WebRTC blocks, capture nesting, pass-through at other rates by absence of unguarded plug-in lines, exact legacy acceptance, and rejection after changing one legacy comment, rate, or path.

- [ ] **Step 2: Run the configuration tests and confirm RED**

Run: `dotnet test .\tests\NoiseCancellingStack.Tests\NoiseCancellingStack.Tests.csproj --configuration Release --filter FullyQualifiedName~ConfigManagerTests --nologo`

Expected: FAIL to compile because the dual-path signatures and trusted-legacy method do not exist.

- [ ] **Step 3: Implement the dual-engine configuration and migration boundary**

Change the named `ConfigManager` methods to take both paths, validate each path, generate two mutually exclusive nested rate blocks, and recognize only the exact current configuration or the exact version-1.0.1 generated configuration for journal trust. Update `InstallerEngine` calls and add `ProductConstants.WebRtcPluginPath` for `plugins\webrtc_apm_16k.dll`.

- [ ] **Step 4: Run configuration tests and confirm GREEN**

Run the Step 2 command.

Expected: all `ConfigManagerTests` pass with zero failures.

- [ ] **Step 5: Add failing route-classification and status-label tests**

Assert `16_000 -> WebRtcApm`, `48_000 -> RnNoise`, and `0`, `8_000`, `44_100`, `96_000 -> PassThrough`. Add a focused test for `AudioRoutePlanner.Label` returning exactly `WEBRTC`, `RNNOISE`, or `PASSTHROUGH`.

- [ ] **Step 6: Run route tests and confirm RED**

Run: `dotnet test .\tests\NoiseCancellingStack.Tests\NoiseCancellingStack.Tests.csproj --configuration Release --filter FullyQualifiedName~AudioRoutePlannerTests --nologo`

Expected: FAIL to compile because `AudioRoutePlanner` does not exist.

- [ ] **Step 7: Implement route selection and use it in status output**

Add `AudioRoutePlanner.Select(uint)` and `AudioRoutePlanner.Label(CaptureProcessor)`. Update `Program.PrintStatus` so each endpoint line includes the classification without changing attachment detection.

- [ ] **Step 8: Run managed tests**

Run: `dotnet test .\NoiseCancellingStack.slnx --configuration Release --nologo`

Expected: all tests pass with zero failures.

- [ ] **Step 9: Commit Task 1**

```powershell
git add src/NoiseCancellingStack.Manager tests/NoiseCancellingStack.Tests
git commit -m "Add dual-rate capture routing"
```

### Task 2: Real-time 160-sample frame adapter

**Files:**
- Create: `native/webrtc-apm-vst2/meson.build`
- Create: `native/webrtc-apm-vst2/frame_processor.h`
- Create: `native/webrtc-apm-vst2/frame_adapter.h`
- Create: `native/webrtc-apm-vst2/frame_adapter.cpp`
- Create: `native/webrtc-apm-vst2/tests/test_support.h`
- Create: `native/webrtc-apm-vst2/tests/frame_adapter_tests.cpp`
- Create: `scripts/build-native.ps1`

**Interfaces:**
- Consumes: C++20, MSVC x64, Meson 1.12.1, and Ninja 1.13.2.
- Produces: `FrameProcessor::ProcessFrame(const std::array<float, 160>& input, std::array<float, 160>& output) noexcept -> bool`; `FrameAdapter::ProcessBlock(const float* input, float* output, std::size_t sampleCount) noexcept`; `FrameAdapter::Reset() noexcept`; `FrameAdapter::bypassed() const noexcept -> bool`; fixed `FrameAdapter::kFrameSamples == 160` and `FrameAdapter::kLatencySamples == 160`.

- [ ] **Step 1: Create the native test harness and failing frame tests**

Use dependency-free assertions with fake identity, scaled-output, non-finite-output, and fail-on-selected-frame processors. Cover exact 160-sample delay, block sizes `0, 1, 159, 160, 161, 4097`, reset, multi-block ordering, queue bounds, permanent bypass, non-finite rejection, and failure after a partially buffered frame. The expected fallback is the original dry input delayed by exactly 160 samples with no loss or duplication.

- [ ] **Step 2: Run native core tests and confirm RED**

Run: `powershell -ExecutionPolicy Bypass -File .\scripts\build-native.ps1 -CoreOnly`

Expected: compilation fails because `FrameAdapter` and `FrameProcessor` are not implemented.

- [ ] **Step 3: Implement `FrameProcessor` and `FrameAdapter`**

Use fixed-size input, processed, and delayed-dry arrays. Consume and emit one sample at a time so arbitrary block sizes cannot affect ordering. Preallocate all plug-in-owned storage, validate processed samples with `std::isfinite`, and permanently enqueue delayed dry frames after the first processor failure.

- [ ] **Step 4: Run native core tests and confirm GREEN**

Run the Step 2 command.

Expected: all frame-adapter tests pass; the script exits `0`.

- [ ] **Step 5: Commit Task 2**

```powershell
git add native/webrtc-apm-vst2 scripts/build-native.ps1
git commit -m "Add real-time 16 kHz frame adapter"
```

### Task 3: Statically linked WebRTC APM VST2 processor

**Files:**
- Create: `native/webrtc-apm-vst2/webrtc_processor.h`
- Create: `native/webrtc-apm-vst2/webrtc_processor.cpp`
- Create: `native/webrtc-apm-vst2/vst_plugin.cpp`
- Create: `native/webrtc-apm-vst2/tests/webrtc_processor_tests.cpp`
- Create: `native/webrtc-apm-vst2/tests/vst_smoke_host.cpp`
- Modify: `native/webrtc-apm-vst2/meson.build`
- Modify: `scripts/build-native.ps1`

**Interfaces:**
- Consumes: Task 2 `FrameProcessor` and `FrameAdapter`; pinned WebRTC source; `aeffect.h`/`aeffectx.h`/`fst.h` from pinned FST source.
- Produces: `CreateWebRtcNoiseSuppressionConfig() -> webrtc::AudioProcessing::Config`; `WebRtcProcessor final : FrameProcessor`; exported `VSTPluginMain(audioMasterCallback) -> AEffect*`; exported `main(audioMasterCallback) -> AEffect*` alias where required by VST2 hosts; exported `NcsWebRtcApmSelfTest() noexcept -> int32_t`; `artifacts/payload/webrtc_apm_16k.dll`; `artifacts/native/webrtc-apm.props` containing `WebRtcApmPluginSha256`.

- [ ] **Step 1: Add failing WebRTC processor tests**

Test construction and warm-up, silence stability, finite output, and exact frame size. Assert the returned config enables noise suppression at `kHigh` while AEC, AGC1, AGC2, high-pass, and transient suppression are false. For effectiveness, feed six seconds of deterministic zero-mean white noise at approximately `0.05` RMS and require the final two seconds of output to measure at least `3.0 dB` below the corresponding input. Do not access audio devices.

- [ ] **Step 2: Run the WebRTC processor tests and confirm RED**

Run: `powershell -ExecutionPolicy Bypass -File .\scripts\build-native.ps1 -TestFilter webrtc-processor`

Expected: compilation fails because `WebRtcProcessor` does not exist.

- [ ] **Step 3: Pin and build WebRTC, then implement `WebRtcProcessor`**

Extend the script to hash-verify and extract the WebRTC archive, Meson wheel, Ninja ZIP, and FST headers into `artifacts`/`vendor` ignored paths. Configure the native build with C++20, Release optimization, x64 MSVC, `default_library=static`, and static CRT. Construct and warm APM outside `ProcessFrame`; use 16 kHz mono `StreamConfig` and return `false` on every APM error or invalid sample.

- [ ] **Step 4: Run WebRTC processor tests and confirm GREEN**

Run the Step 2 command.

Expected: all WebRTC processor tests pass; stationary-noise RMS is lower after adaptation; no sample is non-finite.

- [ ] **Step 5: Add the failing VST smoke host**

Load the produced DLL by full path and assert the VST entry point, unique plug-in ID, one input/output, replacing support, zero parameters, 160-sample initial delay, 16 kHz processing, bit-for-bit non-16-kHz pass-through, zero-frame safety, process-before-activation safety, two-instance isolation under interleaved threads, and `NcsWebRtcApmSelfTest() == 0`.

- [ ] **Step 6: Run the smoke host and confirm RED**

Run: `powershell -ExecutionPolicy Bypass -File .\scripts\build-native.ps1 -TestFilter vst-smoke`

Expected: link or load failure because the VST entry points are not implemented.

- [ ] **Step 7: Implement the minimal mono VST2 ABI and self-test**

Store all processing state in the `AEffect` instance object. Handle open/close, sample rate, block size, mains change, product/vendor/effect names, VST version, and replacing callbacks. Catch every exception at exported and callback boundaries. Unsupported rates copy directly; 16 kHz uses Task 2's fixed-delay adapter. The self-test constructs WebRTC and processes synthetic silence without accessing peripherals.

- [ ] **Step 8: Run all native tests and inspect dependencies**

Run: `powershell -ExecutionPolicy Bypass -File .\scripts\build-native.ps1`

Expected: all native tests pass; `dumpbin /dependents` reports only Windows system libraries and no `MSVCP*.dll`, `VCRUNTIME*.dll`, or secondary WebRTC DLL; the payload DLL and props manifest exist.

- [ ] **Step 9: Commit Task 3**

```powershell
git add native/webrtc-apm-vst2 scripts/build-native.ps1
git commit -m "Add WebRTC APM 16 kHz VST processor"
```

### Task 4: Payload preflight, startup repair, and failure recovery

**Files:**
- Create: `src/NoiseCancellingStack.Manager/PayloadValidator.cs`
- Create: `src/NoiseCancellingStack.Manager/NativeWebRtcPluginProbe.cs`
- Create: `src/NoiseCancellingStack.Manager/AssemblyInfo.cs`
- Create: `tests/NoiseCancellingStack.Tests/PayloadValidatorTests.cs`
- Create: `tests/NoiseCancellingStack.Tests/MaintenanceTaskTests.cs`
- Create: `tests/NoiseCancellingStack.Tests/InstallerEngineOrderingTests.cs`
- Modify: `src/NoiseCancellingStack.Manager/InstallerEngine.cs`
- Modify: `src/NoiseCancellingStack.Manager/MaintenanceTask.cs`
- Modify: `src/NoiseCancellingStack.Manager/ProductConstants.cs`
- Modify: `src/NoiseCancellingStack.Manager/NoiseCancellingStack.Manager.csproj`
- Modify: `scripts/verify-local-install.ps1`

**Interfaces:**
- Consumes: Task 1 dual paths and trusted migration; Task 3 `NcsWebRtcApmSelfTest`; Task 3 generated `WebRtcApmPluginSha256` MSBuild property.
- Produces: `INativePluginProbe.Verify(string pluginPath) -> void`; `NativeWebRtcPluginProbe`; `PayloadValidator.Validate(string rnNoisePath, string expectedRnNoiseSha256, string webRtcPath, string expectedWebRtcSha256) -> void`; generated compile-time `GeneratedPayloadHashes.WebRtcApmPluginSha256`; internal testable `MaintenanceTask.BuildTaskXml(string managerPath) -> string`.

- [ ] **Step 1: Add failing payload validation tests**

Use temporary files and a fake probe. Cover missing RNNoise, missing WebRTC, wrong hash for each, success calling the probe exactly once, probe exception propagation, and a nonzero/absent-export equivalent from the fake. Assert no probe runs after any file/hash failure.

- [ ] **Step 2: Run payload tests and confirm RED**

Run: `dotnet test .\tests\NoiseCancellingStack.Tests\NoiseCancellingStack.Tests.csproj --configuration Release --filter FullyQualifiedName~PayloadValidatorTests --nologo`

Expected: FAIL to compile because the validator and probe interfaces do not exist.

- [ ] **Step 3: Implement hash validation and the native self-test probe**

Keep SHA-256 comparison ordinal-ignore-case and stream-based. `NativeWebRtcPluginProbe` loads by absolute path with `NativeLibrary`, resolves only `NcsWebRtcApmSelfTest`, requires return `0`, unloads in `finally`, and translates load/export/result failures to actionable `InvalidDataException` messages.

- [ ] **Step 4: Run payload tests and confirm GREEN**

Run the Step 2 command.

Expected: all payload tests pass with zero failures.

- [ ] **Step 5: Add failing startup-task and repair-contract tests**

Assert SYSTEM identity, boot trigger with `PT1M`, `StartWhenAvailable=true`, battery execution, `IgnoreNew`, `PT15M` limit, hidden `repair --quiet --scheduled`, XML escaping, and processor-neutral description. Add `InstallOrRepair_ValidatesPayloadsBeforeAnyMutation`, which reads `InstallerEngine.cs`, isolates the `InstallOrRepair` method, and asserts the payload-validation call appears before `SafetyBackup.Create`, `ConfigManager.Install`, the first `stateStore.Save`, `AttachAll`, and `TryRestartAudio` call.

- [ ] **Step 6: Run startup/recovery tests and confirm RED**

Run: `dotnet test .\tests\NoiseCancellingStack.Tests\NoiseCancellingStack.Tests.csproj --configuration Release --filter "FullyQualifiedName~MaintenanceTaskTests|FullyQualifiedName~InstallerEngineOrderingTests" --nologo`

Expected: at least the task XML visibility/description and preflight-order assertions fail.

- [ ] **Step 7: Integrate preflight and make task language processor-neutral**

Generate the WebRTC hash constant in the manager intermediate directory from the MSBuild property, with a non-release development sentinel that cannot validate an installed payload. Call `PayloadValidator` and the real probe before backup or mutation. Preserve the current task trigger/settings and scheduled-repair behavior; update only product-neutral descriptions/messages. Extend local-install verification to check both payloads, both exact rate guards, the scheduled task, and Windows Audio without accessing a microphone.

- [ ] **Step 8: Run all managed tests**

Run: `dotnet test .\NoiseCancellingStack.slnx --configuration Release --nologo -p:WebRtcApmPluginSha256=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA`

Expected: all tests pass with zero failures and zero warnings.

- [ ] **Step 9: Commit Task 4**

```powershell
git add src/NoiseCancellingStack.Manager tests/NoiseCancellingStack.Tests scripts/verify-local-install.ps1
git commit -m "Validate and repair the WebRTC capture payload"
```

### Task 5: Offline installer, release provenance, CI, and user documentation

**Files:**
- Create: `tests/NoiseCancellingStack.Tests/ReleaseContractTests.cs`
- Modify: `scripts/build.ps1`
- Modify: `installer/NoiseCancellingStack.iss`
- Modify: `.github/workflows/ci.yml`
- Modify: `.github/PULL_REQUEST_TEMPLATE.md`
- Modify: `.github/ISSUE_TEMPLATE/bug_report.yml`
- Modify: `src/NoiseCancellingStack.Manager/ProductConstants.cs`
- Modify: `src/NoiseCancellingStack.Manager/NoiseCancellingStack.Manager.csproj`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `docs/ARCHITECTURE.md`
- Modify: `docs/SAFETY.md`
- Modify: `docs/PUBLIC-RELEASE-CHECKLIST.md`
- Modify: `LICENSING.md`
- Modify: `THIRD-PARTY-NOTICES.md`

**Interfaces:**
- Consumes: Task 3 payload/props, Task 4 compiled hash property, existing offline Inno installer and source-bundle pipeline.
- Produces: version `1.1.0` installer containing `plugins\rnnoise_mono.dll` and `plugins\webrtc_apm_16k.dll`; WebRTC/FST license files; WebRTC corresponding-source release archive; updated `SHA256SUMS.txt`; CI native and managed verification.

- [ ] **Step 1: Add failing release-contract tests**

Assert version `1.1.0` is consistent across manager, product constants, installer, and build script; Inno packages both plug-ins and required notices; the build invokes native build before manager publish and passes the generated hash property; source output includes the exact WebRTC archive; README/architecture/safety documents state the exclusive 16/48 kHz routing and unsupported-rate pass-through; release notes disclose the 10 ms latency and double-suppression risk.

- [ ] **Step 2: Run release-contract tests and confirm RED**

Run: `dotnet test .\tests\NoiseCancellingStack.Tests\NoiseCancellingStack.Tests.csproj --configuration Release --filter FullyQualifiedName~ReleaseContractTests --nologo`

Expected: assertions fail because packaging, versions, and documentation still describe RNNoise-only 1.0.1.

- [ ] **Step 3: Integrate the native build and payload into the release pipeline**

Invoke `build-native.ps1` before managed publish, read its generated digest, pass `WebRtcApmPluginSha256` to `dotnet test` and `dotnet publish`, copy the plug-in and extracted upstream licenses into installer payloads, add the exact WebRTC source archive to `dist`, and preserve safe build-directory guards and checksum generation.

- [ ] **Step 4: Update installer and version metadata**

Set version `1.1.0`, add the new DLL/licenses, change RNNoise-only UI wording to offline noise suppression, and leave privilege, hidden execution, safe-partial exit handling, rollback cancellation, and fixed install directory unchanged.

- [ ] **Step 5: Update CI, notices, and user-facing documentation**

Run the native build/tests on `windows-latest` before the managed suite. Document WebRTC/FST provenance and licensing, the C920/Bluetooth benefit and limitations, 16/48 kHz routing, 10 ms added latency, application double suppression, shared-mode/APO limitations, startup repair, and the new release artifact set.

- [ ] **Step 6: Run release-contract and full test suites**

Run: `dotnet test .\NoiseCancellingStack.slnx --configuration Release --nologo -p:WebRtcApmPluginSha256=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA`

Expected: all managed tests pass with zero failures and zero warnings.

Run: `powershell -ExecutionPolicy Bypass -File .\scripts\build-native.ps1`

Expected: all native tests and dependency checks pass; payload manifest is generated.

- [ ] **Step 7: Build the complete offline release without installing it**

Run: `powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1`

Expected: exit `0`; `dist` contains the 1.1.0 installer, project source, Equalizer APO source, Noise Suppression for Voice source, WebRTC source, and checksum manifest; every manifest hash verifies. Do not launch the installer or access an audio device.

- [ ] **Step 8: Commit Task 5**

```powershell
git add .github installer scripts src tests README.md CHANGELOG.md docs LICENSING.md THIRD-PARTY-NOTICES.md
git commit -m "Package the WebRTC 16 kHz fallback"
```

## Final Verification and Independent Review

- [ ] Run `git diff --check 67930bc..HEAD` and require no whitespace errors.
- [ ] Run `dotnet test .\NoiseCancellingStack.slnx --configuration Release --nologo` with the generated payload hash and require zero failures/warnings.
- [ ] Run `powershell -ExecutionPolicy Bypass -File .\scripts\build-native.ps1` and require all native tests plus the dependency audit to pass.
- [ ] Run `powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1` and independently verify every file listed by `dist\SHA256SUMS.txt`.
- [ ] Review the specification requirement-by-requirement against the final diff and record any deliberate deviation as a ruling.
- [ ] Generate a whole-change review package from base `67930bc` through `HEAD`.
- [ ] Dispatch an independent worker with the specification, this plan, review package, Review Focus section, and commit range.
- [ ] Re-grade every finding by user impact; fix all Critical and Important findings test-first in one pass, report deferred Minor findings, and rerun the complete managed/native/release verification.
- [ ] Do not perform a live install, microphone capture, playback, UI automation, or desktop interaction without separate explicit permission.
