# Noise Cancelling Stack

System-wide, offline microphone noise suppression for Windows PCs that cannot—or do not want to—use vendor-specific broadcast software.

Noise Cancelling Stack packages [Equalizer APO](https://sourceforge.net/projects/equalizerapo/), the [Noise Suppression for Voice](https://github.com/werman/noise-suppression-for-voice) RNNoise plug-in, and a statically linked [WebRTC Audio Processing](https://github.com/Sqhh99/webrtc-audio-processing) plug-in into one safety-focused installer. It attaches capture-only filtering to Windows endpoints, so compatible microphones are cleaned before their audio reaches Teams, Discord, OBS, a browser, a game, WD Studio, or another shared-mode application.

The project is CPU-based and hardware-vendor agnostic. RNNoise handles exactly 48 kHz capture streams, while WebRTC APM provides the fallback for exactly 16 kHz devices such as the Logitech C920 microphone and Bluetooth hands-free profiles. There is no cloud processing, account, telemetry, or audio recording.

This is an independent community project. It is not affiliated with or endorsed by NVIDIA, Equalizer APO, Noise Suppression for Voice, Google, or the WebRTC project.

## What it provides

- One offline installer for Windows 10 and 11 x64.
- Microphone filtering at the Windows system-effect layer rather than inside one application.
- Automatic registration of every currently known capture endpoint.
- A startup repair task that discovers newly connected microphones without user intervention.
- Local CPU processing that does not require Intel, AMD, NVIDIA, or another vendor runtime.
- Transactional registry changes, full safety backups, idempotent repair, and conflict-aware uninstall.
- Pinned upstream versions and SHA-256 checks for every bundled binary.

It is intentionally narrower than NVIDIA Broadcast: this project handles microphone noise suppression only. It does not provide camera effects, room-echo removal, speaker processing, or a tuning interface.

## How it works

```text
Microphone
    -> Windows shared-mode capture pipeline
    -> Equalizer APO capture effect
       -> 48 kHz: RNNoise VST2
       -> 16 kHz: WebRTC APM VST2
       -> every other rate: unchanged pass-through
    -> Teams / Discord / OBS / browser / game / other application
```

Equalizer APO 1.4.2 hosts the unmodified `rnnoise_mono.dll` from Noise Suppression for Voice 1.10 and the project-built `webrtc_apm_16k.dll`. The generated configuration nests mutually exclusive `sampleRate == 48000` and `sampleRate == 16000` branches inside `stage == "capture"`. The WebRTC plug-in also self-bypasses at any non-16 kHz rate.

Existing vendor audio effects are retained as Equalizer APO child effects rather than discarded. The endpoint attachment mode follows Equalizer APO's own legacy, modern, and combined-device selection rules.

## Compatibility and limits

| Works | Does not work or passes through |
| --- | --- |
| Windows 10/11 x64 | Windows on Arm or 32-bit Windows |
| Shared-mode Windows capture | ASIO and WASAPI exclusive mode |
| Applications using Windows system effects | Applications that explicitly disable enhancements |
| Exactly 16 kHz or 48 kHz capture streams | 8 kHz, 44.1 kHz, 96 kHz, and other rates (pass-through) |
| CPU-based local processing | Driver or hardware paths that bypass Windows APOs |

All registered microphones can be attached. Exactly 16 kHz uses WebRTC APM, exactly 48 kHz uses RNNoise, and unsupported rates use unchanged pass-through. The WebRTC frame adapter adds a fixed 10 ms latency at 16 kHz. It suppresses stationary background noise but cannot restore bandwidth lost by a hands-free codec or fix clipping, distance, reverberation, or a poor radio link.

Turn off application-level suppression first when evaluating this stack. Running WD Studio, Teams, Discord, OBS, or another application's noise suppression at the same time can cause double suppression, metallic speech, pumping, or missing quiet syllables.

Noise suppression is not voice restoration. A poor, clipping, distant, or heavily reverberant microphone can still sound poor, and aggressive suppression may occasionally mistake quiet speech for noise.

## Install

1. Download the v1.1.0 files from [Releases](../../releases), keeping them together.
2. Verify `NoiseCancellingStack-Setup-1.1.0.exe` against `SHA256SUMS.txt` through a trusted channel.
3. End active calls and recordings; setup briefly restarts Windows Audio.
4. Run the installer and approve the administrator prompt.
5. Use status output to confirm the endpoint is classified `WEBRTC` at 16 kHz, `RNNOISE` at 48 kHz, or `PASSTHROUGH` at another rate.

The installer is currently unsigned, so Windows SmartScreen may display **Unknown publisher**. Do not bypass that warning unless the SHA-256 hash matches the release manifest you obtained through a trusted channel.

The installer is fully offline. If Equalizer APO is absent, setup installs the bundled, hash-pinned 1.4.2 x64 package. If another Equalizer APO version is already present, setup stops rather than silently replacing or downgrading it.

## New microphones and maintenance

A SYSTEM task performs an idempotent scan one minute after startup, including while a laptop is on battery. It restarts Windows Audio only when a newly discovered endpoint was actually attached.

The Start menu also contains **Repair all microphones**, **Microphone status**, and **Logs and backups** shortcuts. From an elevated terminal in the installation directory:

```text
NoiseCancellingStack.Manager.exe status
NoiseCancellingStack.Manager.exe repair
NoiseCancellingStack.Manager.exe uninstall
```

Exit code `0` means success, `2` means a safe partial result or preserved conflict, and `1` means failure.

## Safety and rollback

Before every mutating operation, the manager exports the capture-endpoint registry and Equalizer APO registry and copies the active `config.txt`. State, logs, and backups are stored under:

```text
C:\ProgramData\NoiseCancellingStack
```

That directory is restricted to SYSTEM and Administrators for writes. State is recorded before the first endpoint mutation. On uninstall, a registry value is restored only if it still matches either the value installed by this project or its original value. Later changes by drivers or other audio tools are preserved and reported as conflicts.

The existing Equalizer APO configuration is retained. The project owns only a marked include block and `NoiseCancellingStack.txt`. Equalizer APO itself is deliberately left installed during uninstall because another application may depend on it.

The design invariants are documented in [docs/SAFETY.md](docs/SAFETY.md), and the pre-release adversarial review is recorded in [docs/REVIEW.md](docs/REVIEW.md).

## Privacy

Audio remains in the local Windows audio pipeline. This software does not save, transmit, or inspect recordings outside the in-process RNNoise or WebRTC callbacks. There is no listener, web server, analytics client, account, or update checker.

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

The build retrieves missing dependencies only from pinned upstream URLs, verifies every SHA-256 digest, runs the release test suite, publishes the self-contained manager, and creates the installer plus corresponding-source archives in `dist`.

The resulting release set is:

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

Do not redistribute the installer by itself. Keep the notices, checksum manifest, this project's source, and all upstream corresponding-source archives available to recipients.

## Licensing

Code written for Noise Cancelling Stack is available under the [MIT License](LICENSE): you may use, modify, and redistribute it, including commercially, provided the copyright and license notice remain with substantial copies.

That MIT grant does **not** relicense the bundled projects:

- Equalizer APO 1.4.2 is distributed under GPL-2.0.
- Noise Suppression for Voice 1.10 and the project-authored FST-based WebRTC VST2 are distributed under GPL-3.0-only.
- WebRTC Audio Processing and Abseil retain their BSD-3-Clause, patent-grant, and Apache-2.0 notices.
- Inno Setup is a separately licensed build tool whose generated setup engine is present in the installer.

See [LICENSING.md](LICENSING.md) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) before redistributing a compiled installer.

## Contributing and security

Contributions are welcome under the project's MIT license. Start with [CONTRIBUTING.md](CONTRIBUTING.md). Please report security-sensitive issues privately as described in [SECURITY.md](SECURITY.md), particularly anything involving elevated registry writes, scheduled tasks, installer trust, or rollback behavior.

## Project status

Version 1.1.0 adds the vendor-agnostic 16 kHz WebRTC fallback while preserving the 48 kHz RNNoise path and startup repair. Automated tests use synthetic audio only; no microphone or playback device is accessed. Canary installation and perceptual A/B testing on representative Windows 10 and Windows 11 systems remain release-owner tasks.
