# WebRTC APM 16 kHz Fallback Design

**Date:** 2026-10-22

**Status:** Approved direction; implementation pending

**Scope:** Windows 10/11 x64 shared-mode capture through Equalizer APO

## Intent

Add a hardware-vendor-agnostic noise-suppression path for microphones whose Windows capture stream runs at exactly 16,000 Hz. Preserve the existing RNNoise path for exactly 48,000 Hz, keep all other rates as safe pass-through, and retain the product's existing unattended startup repair, transactional configuration, rollback, privacy, and offline-install guarantees.

The feature is primarily for the Logitech C920 microphone and Bluetooth hands-free microphones that currently attach to Equalizer APO but bypass RNNoise because their active capture format is 16 kHz.

## Goals

- Process exactly 16 kHz capture streams with WebRTC Audio Processing Module noise suppression.
- Remain independent of AMD, Intel, NVIDIA, or other vendor-specific hardware and software.
- Keep exactly 48 kHz capture streams on the existing RNNoise 1.10 processor.
- Never load either processor outside the capture stage or at an unsupported sample rate.
- Add no tray application, persistent user process, cloud service, account, telemetry, or recording.
- Load processing on demand with the Windows audio graph, as the current RNNoise path does.
- Preserve idempotent install and repair, startup endpoint discovery, write-ahead endpoint state, atomic Equalizer APO configuration, conflict-aware uninstall, and fail-closed dependency validation.
- Support safe upgrade from the existing RNNoise-only 1.0.1 installation.

## Non-goals

- Acoustic echo cancellation, automatic gain control, voice activity gating, dereverberation, or output/speaker processing.
- Processing 8 kHz, 44.1 kHz, or arbitrary sample rates in this release.
- Restoring bandwidth lost by a 16 kHz device or repairing Bluetooth codec artifacts.
- Bypassing WASAPI exclusive mode, ASIO, or applications that explicitly disable Windows system effects.
- Replacing application-specific suppression automatically. Applications such as WD Studio may still apply their own suppression and should be tested for double-processing artifacts.

## Selected Architecture

The managed Equalizer APO configuration will route by capture-stream sample rate:

```text
Windows shared-mode capture
    -> Equalizer APO pre-mix capture effect
       -> 48,000 Hz: RNNoise VST2 mono processor
       -> 16,000 Hz: WebRTC APM VST2 mono processor
       -> every other rate: unchanged pass-through
    -> application
```

Both conditions remain nested under `stage == "capture"`. They are separate, mutually exclusive sample-rate blocks. Each block uses `Channel: all`, allowing Equalizer APO to instantiate the corresponding mono processor for each selected capture channel.

This design uses no virtual microphone and requires no application-specific device selection. It inherits the same system-effect coverage and limitations as the current RNNoise path.

## WebRTC Processor

### Dependency

Pin the WebRTC Audio Processing standalone source at release `m155.8059.4.0`, commit `3a6b666a906da6cb3c6f9d474fab21c2ba739f22`. The build retrieves the immutable commit archive through a pinned URL and SHA-256 digest. WebRTC and its statically linked dependencies retain their upstream BSD, Apache-2.0, and compatible notices.

The processor is compiled from source as part of the release build. WebRTC APM is linked statically into one x64 VST2 DLL with the static Microsoft C/C++ runtime. The installed audio path therefore has no secondary WebRTC DLL, GPU runtime, vendor driver, PATH dependency, or background service that can fail independently at startup.

The VST2 ABI uses the reverse-engineered FST headers already present in the pinned Noise Suppression for Voice corresponding-source archive. Because FST is GPL-3.0, the new plug-in source files are GPL-3.0 and are kept legally separate from the MIT-licensed manager and installer code. Release artifacts include all corresponding source and notices required to rebuild the plug-in.

### DSP Configuration

- Mono input and mono output.
- Exactly 16,000 Hz; every other rate bypasses inside the plug-in even if the Equalizer APO guard is accidentally changed.
- WebRTC noise suppression enabled at `kHigh`.
- Echo cancellation, gain control, high-pass filtering, transient suppression, and other APM features disabled.
- WebRTC receives 10 ms frames: 160 floating-point samples at 16 kHz.
- An input/output ring buffer adapts arbitrary VST host block sizes to 160-sample APM frames.
- The plug-in advertises and maintains a fixed 160-sample (10 ms) processing delay so output timing never varies with the host block size.
- Plug-in-owned buffers are allocated before audio processing starts. No filesystem access, logging, waiting, or locks occur in the audio callback.

### Fail-open Contract

Microphone availability is more important than suppression. The plug-in copies input to output unchanged when:

- the host rate is not exactly 16 kHz;
- APM construction or warm-up fails;
- WebRTC reports a processing error; or
- an unexpected exception reaches the plug-in boundary.

After a processing fault, that plug-in instance remains bypassed rather than retrying repeatedly on the real-time audio thread. Samples must remain finite and bounded; malformed processing output causes the same permanent-instance bypass.

## Build and Supply-chain Integration

The release build will:

1. Retrieve checksum-pinned WebRTC, Meson, Ninja, Equalizer APO, and RNNoise inputs.
2. Extract WebRTC and the FST ABI headers into an artifact-only native build tree.
3. Build WebRTC and the new VST2 plug-in for x64 in Release mode with static runtime linkage.
4. Run native unit tests and a VST ABI smoke host against the produced DLL.
5. Compute the plug-in SHA-256 digest and compile that expected digest into the manager release build.
6. Run the managed test suite and publish the self-contained manager.
7. Package the plug-in, notices, licenses, project source, and exact upstream corresponding-source archives into the offline installer release set.

Downloaded or generated binaries never enter source archives. All release inputs are version-pinned and hash-verified before use.

## Manager and Configuration Changes

`ConfigManager` will accept both the RNNoise and WebRTC plug-in paths and generate both guarded branches. Path validation continues to reject line injection, and writes remain same-directory atomic replacements.

`InstallerEngine` will validate before any endpoint mutation:

- both plug-in files exist;
- both files match their compiled-in SHA-256 digests; and
- the WebRTC plug-in exports and passes a lightweight self-test that constructs APM and processes a synthetic 16 kHz frame.

The self-test does not access a microphone or any other peripheral.

Upgrade trust validation will recognize both:

- the new exact two-engine managed configuration; and
- the exact legacy RNNoise-only configuration generated by version 1.0.1.

This prevents a legitimate upgrade with an existing state journal from being mistaken for pre-staged malicious recovery data. Repair then atomically replaces the old managed configuration with the new one.

Status output will classify each registered endpoint as `RNNOISE`, `WEBRTC`, or `PASSTHROUGH` from its current shared-mode sample rate while continuing to report whether Equalizer APO is attached.

## Startup, Repair, and Recovery

The existing SYSTEM scheduled task remains the startup mechanism. One minute after boot, including on battery, it runs hidden with `StartWhenAvailable`, a single-instance policy, and a bounded execution time.

Scheduled and manual repair use the same idempotent sequence:

1. Validate the platform, Equalizer APO version, both plug-in hashes, and WebRTC self-test.
2. Validate the existing state journal against a trusted current or legacy managed configuration.
3. Take the existing safety backups.
4. Atomically repair the managed Equalizer APO include and two-engine configuration.
5. Discover and transactionally attach newly registered capture endpoints.
6. Restart Windows Audio only when configuration or endpoint attachment changed.

If a plug-in is missing, corrupt, or fails self-test, repair stops before configuration or endpoint mutation and records an actionable error. Re-running the offline installer restores release payloads; the next repair is idempotent. Existing endpoint rollback and uninstall behavior remains conflict-aware and never overwrites later third-party changes.

The scheduled task description and log messages become processor-neutral rather than RNNoise-specific. No separate task, service, or logon application is introduced for WebRTC.

## Installer and Uninstall

The Inno Setup package installs the new VST2 DLL beside the existing RNNoise DLL under the protected fixed Program Files directory. Post-install continues to run the manager hidden and accept success or safe-partial exit codes.

Uninstall removes the managed file only when it exactly matches the generated two-engine configuration. User-modified configuration remains preserved and reported. Endpoint rollback, task removal, incomplete-rollback cancellation, and retention of recovery state continue unchanged.

## Security and Privacy

- Processing remains entirely local inside the Windows shared-mode capture pipeline.
- The plug-in has no network code, updater, UI, telemetry, or audio-recording path.
- The SYSTEM task continues to execute only the manager at its fixed protected Program Files path.
- Native source and binary inputs are pinned by SHA-256.
- No exception crosses the VST ABI or real-time callback boundary.
- The plug-in never attempts to load arbitrary models or configuration from user-writable locations.

## Testing and Acceptance

### Managed tests

- Exact capture-only 48 kHz RNNoise and 16 kHz WebRTC branches.
- No WebRTC branch at 8 kHz, 44.1 kHz, or other rates.
- Path injection rejection for both plug-in paths.
- Exact current-install recognition.
- Exact 1.0.1 legacy-install recognition for upgrade.
- Rejection of modified or untrusted managed configurations.
- Correct endpoint engine classification.
- Startup-task XML retains boot delay, battery support, `StartWhenAvailable`, single-instance policy, SYSTEM identity, and bounded execution.
- Installer dependency validation fails before mutation when either plug-in is absent, corrupt, or fails self-test.

### Native tests

- VST entry point and expected mono capabilities are exported.
- Non-16 kHz input is bit-for-bit pass-through.
- Arbitrary host block sizes preserve ordering with a fixed 160-sample delay.
- Silence remains finite and stable.
- A deterministic stationary-noise fixture produces measurable attenuation after APM adaptation.
- Processing errors force permanent-instance pass-through.
- Long synthetic runs are free of buffer overruns, invalid floating-point output, and unbounded allocation.

### Build and packaging checks

- Full managed and native test suites pass in Release mode.
- The native DLL has no non-system runtime dependency.
- The built DLL digest equals the value embedded in the manager.
- Installer contents, licenses, notices, source archives, and `SHA256SUMS.txt` are complete.
- The local-install verification script checks both guarded branches, both payload files, the startup task, and Windows Audio service state.

No live microphone capture or audible playback is part of automated verification. Any later perceptual A/B test requires the user's separate explicit permission because it accesses an audio peripheral.

## Independent Review Focus

After implementation and verification, an independent worker reviews the complete change with particular attention to:

- VST lifetime and callback safety inside `audiodg.exe`;
- ring-buffer ordering, fixed latency, and arbitrary block sizes;
- fail-open behavior and exception containment;
- exact rate/stage routing and double-processing prevention;
- trusted migration from the 1.0.1 configuration;
- validation-before-mutation ordering;
- scheduled-task reliability and idempotent recovery;
- dependency pinning, reproducibility, licensing, and corresponding source; and
- installer/uninstall rollback behavior.

Critical and important findings are fixed and re-verified before completion. Minor findings are reported explicitly if deferred.
