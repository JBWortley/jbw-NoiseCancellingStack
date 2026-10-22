# Noise Cancelling Stack

**System-wide, offline microphone noise suppression for Windows—without a GPU, cloud service, virtual microphone, or vendor-specific runtime.**

Noise Cancelling Stack attaches capture-only filtering to Windows microphone endpoints. Compatible shared-mode applications—including Teams, Discord, OBS, browsers, games, and WD Studio—receive the filtered signal from the microphone they already use.

The stack combines [Equalizer APO](https://sourceforge.net/projects/equalizerapo/), the [Noise Suppression for Voice](https://github.com/werman/noise-suppression-for-voice) RNNoise plug-in, and a statically linked [WebRTC Audio Processing](https://github.com/Sqhh99/webrtc-audio-processing) plug-in in one safety-focused installer. Processing stays on the local CPU. There is no account, telemetry, network service, or audio recording.

This is an independent community project. It is not affiliated with or endorsed by NVIDIA, Equalizer APO, Noise Suppression for Voice, Google, or the WebRTC project.

## At a glance

- Windows 10 and 11 x64; local CPU processing on Intel, AMD, or other x64 hardware.
- Automatic filtering for attached microphones in the Windows shared-mode capture path.
- RNNoise for studio-quality 48 kHz streams and WebRTC APM for 16 kHz fallback devices.
- Safe unchanged pass-through when a stream uses any other sample rate or a processor fails.
- A SYSTEM startup-repair task that discovers and attaches newly registered microphones.
- Transactional registry changes, safety backups, idempotent repair, and conflict-aware uninstall.
- One offline installer with pinned dependencies, corresponding source, notices, and SHA-256 manifests.

It is intentionally narrower than NVIDIA Broadcast: this project handles microphone noise suppression only. It does not provide camera effects, acoustic echo cancellation, room-echo removal, speaker processing, or a tuning interface.

## Which filter does my microphone use?

Routing is automatic and is based on the sample rate of the capture stream Windows opens:

| Active capture rate | Processing path | Typical examples |
| --- | --- | --- |
| Exactly 48 kHz | **RNNoise** | USB studio microphones, PC audio inputs, many USB headsets and glasses |
| Exactly 16 kHz | **WebRTC APM** | Logitech C920 microphone, Bluetooth hands-free profiles |
| Any other rate | **Unchanged pass-through** | 8 kHz, 44.1 kHz, 96 kHz, and uncommon device formats |

Switching devices requires no filter selection. A 16 kHz Bluetooth microphone uses WebRTC while it is active; switching back to a 48 kHz studio microphone loads RNNoise again. Equalizer APO enforces both rate branches, and the WebRTC plug-in additionally self-bypasses outside 16 kHz.

```text
Microphone
    -> Windows shared-mode capture pipeline
    -> Equalizer APO capture effect
       -> 48 kHz: RNNoise VST2
       -> 16 kHz: WebRTC APM VST2
       -> every other rate: unchanged pass-through
    -> application
```

Equalizer APO 1.4.2 hosts the unmodified `rnnoise_mono.dll` from Noise Suppression for Voice 1.10 and the project-built `webrtc_apm_16k.dll`. The generated configuration keeps the mutually exclusive rate branches inside `stage == "capture"`; speakers and playback streams are never processed.

Existing vendor audio effects are retained as Equalizer APO child effects rather than discarded. Endpoint attachment follows Equalizer APO's legacy, modern, and combined-device selection rules.

## Is it always running?

There is no persistent filtering application. Windows loads the configured APO and the matching processor when an attached microphone's shared-mode capture graph becomes active, then unloads it when no longer needed.

A hidden SYSTEM task runs an idempotent repair scan one minute after startup, including on battery. It uses `StartWhenAvailable`, permits only one instance, and has a bounded execution time. The scan repairs the managed configuration, discovers newly registered microphones, and restarts Windows Audio only when an attachment or configuration actually changed.

This makes the stack ready when Windows or an application switches microphones. To inspect the current state at any time, use **Microphone status** from the Start menu or run:

```text
NoiseCancellingStack.Manager.exe status
```

Each registered endpoint is reported as `RNNOISE`, `WEBRTC`, or `PASSTHROUGH`, together with its Equalizer APO attachment state.

## Application compatibility

| Filtered | Bypasses this stack |
| --- | --- |
| Windows shared-mode capture | ASIO and WASAPI exclusive mode |
| Applications using the attached Windows endpoint | Applications that explicitly disable system effects/enhancements |
| WD Studio and Electron applications using normal Windows capture | Driver or hardware paths that bypass Windows APOs |
| Exactly 16 kHz and 48 kHz streams | Unsupported rates, which pass through unchanged |

An application's name does not determine coverage—the audio path does. WD Studio receives filtered audio when it opens an attached endpoint through the normal Windows shared-mode path. If an application deliberately bypasses system effects, this project cannot intercept it.

Turn off application-level noise suppression when evaluating the stack. Running Teams, Discord, OBS, or another application's suppression at the same time can cause double suppression, metallic speech, pumping, or missing quiet syllables.

The WebRTC adapter adds a fixed 10 ms delay at 16 kHz. It suppresses stationary background noise but cannot restore bandwidth lost by a hands-free codec or repair clipping, microphone distance, reverberation, or a poor radio link. Noise suppression is not voice restoration.

## Install

1. Download the complete v1.1.0 release set from [GitHub Releases](https://github.com/JBWortley/jbw-NoiseCancellingStack/releases), keeping the installer and checksum manifest together.
2. Verify `NoiseCancellingStack-Setup-1.1.0.exe` against `SHA256SUMS.txt` obtained through a trusted channel.
3. End active calls and recordings; setup briefly restarts Windows Audio when required.
4. Run the installer and approve the administrator prompt.
5. Open **Microphone status** and confirm the intended endpoint reports `RNNOISE`, `WEBRTC`, or `PASSTHROUGH` for its current rate.

The installer is currently unsigned, so Windows SmartScreen may display **Unknown publisher**. Do not bypass that warning unless the SHA-256 hash matches the release manifest you obtained through a trusted channel.

The package is fully offline. If Equalizer APO is absent, setup installs the bundled, hash-pinned 1.4.2 x64 package. If another Equalizer APO version is already present, setup stops instead of silently replacing or downgrading it.

## Maintenance and recovery

The Start menu provides **Repair all microphones**, **Microphone status**, and **Logs and backups** shortcuts. The equivalent commands from an elevated terminal in the installation directory are:

```text
NoiseCancellingStack.Manager.exe status
NoiseCancellingStack.Manager.exe repair
NoiseCancellingStack.Manager.exe uninstall
```

Exit code `0` means success, `2` means a safe partial result or preserved conflict, and `1` means failure. Repair validates both plug-in hashes and the WebRTC self-test before changing configuration or endpoints. Re-running the offline installer restores missing or damaged release payloads; a subsequent repair safely converges on the intended state.

## Safety and rollback

Before every mutating operation, the manager exports the capture-endpoint and Equalizer APO registry areas and copies the active `config.txt`. State, logs, and backups are stored under:

```text
C:\ProgramData\NoiseCancellingStack
```

That directory permits writes only by SYSTEM and Administrators. State is recorded before the first endpoint mutation. During uninstall, a registry value is restored only if it still matches either the project-installed value or its recorded original value. Later changes made by drivers or other audio tools are preserved and reported as conflicts.

The existing Equalizer APO configuration is retained. This project owns only a marked include block and `NoiseCancellingStack.txt`. Equalizer APO itself is deliberately left installed during uninstall because another application may depend on it.

The design invariants are documented in [docs/SAFETY.md](docs/SAFETY.md), and the adversarial review record is in [docs/REVIEW.md](docs/REVIEW.md).

## Privacy

Audio remains inside the local Windows audio pipeline. This software does not save, transmit, or inspect recordings outside the in-process RNNoise or WebRTC callbacks. It has no listener, web server, analytics client, account, or update checker.

Local logs contain endpoint names, sample rates, endpoint GUIDs, and operation results. Registry backups may reveal installed audio hardware and should be treated as machine-specific diagnostic data. Neither is included in this repository.

## Build from source

Requirements:

- Windows 10 or Windows 11 x64
- .NET SDK 9.0.304, or a compatible patch selected by `global.json`
- Visual Studio 2022 C++ build tools, Python 3, and Git for Windows
- Inno Setup 6

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

The build retrieves missing dependencies only from pinned upstream URLs, verifies every SHA-256 digest, runs the managed and native release tests, publishes the self-contained manager, and creates the installer plus corresponding-source archives in `dist`.

The complete v1.1.0 release set is:

```text
NoiseCancellingStack-Setup-1.1.0.exe
NoiseCancellingStack-source-1.1.0.zip
EqualizerAPO-src-1.4.2.zip
noise-suppression-for-voice-v1.10.zip
webrtc-audio-processing-3a6b666a906da6cb3c6f9d474fab21c2ba739f22.zip
abseil-cpp-20250814.1.tar.gz
abseil-cpp_20250814.1-1_patch.zip
SHA256SUMS.txt
```

Do not redistribute the installer alone. Keep the notices, checksum manifest, this project's source, and every upstream corresponding-source archive available to recipients.

## Licensing

Code written for Noise Cancelling Stack is available under the [MIT License](LICENSE): you may use, modify, and redistribute it, including commercially, provided the copyright and license notice remain with substantial copies.

That MIT grant does **not** relicense bundled projects:

- Equalizer APO 1.4.2 is distributed under GPL-2.0.
- Noise Suppression for Voice 1.10 and the project-authored FST-based WebRTC VST2 are distributed under GPL-3.0-only.
- WebRTC Audio Processing and Abseil retain their BSD-3-Clause, patent-grant, and Apache-2.0 notices.
- Inno Setup is a separately licensed build tool whose generated setup engine is present in the installer.

See [LICENSING.md](LICENSING.md) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) before redistributing a compiled installer.

## Contributing and security

Contributions are welcome under the project's MIT license. Start with [CONTRIBUTING.md](CONTRIBUTING.md). Report security-sensitive issues privately as described in [SECURITY.md](SECURITY.md), particularly anything involving elevated registry writes, scheduled tasks, installer trust, or rollback behavior.

## Project status

Version 1.1.0 adds the vendor-agnostic 16 kHz WebRTC fallback while preserving the 48 kHz RNNoise path, fail-open behavior, and startup repair. Automated tests use synthetic audio only; no microphone or playback device is accessed during the release build.
