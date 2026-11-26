# Noise Cancelling Stack

**System-wide, offline microphone noise suppression for Windows—without a GPU, cloud service, virtual microphone, or vendor-specific runtime.**

Noise Cancelling Stack attaches capture-only filtering to Windows microphone endpoints. Compatible shared-mode applications—including Teams, Discord, OBS, browsers, games, and WD Studio—receive the filtered signal from the microphone they already use. On Windows 11 it also removes acoustic echo: sound from your speakers that the microphone picks up.

The stack combines [Equalizer APO](https://sourceforge.net/projects/equalizerapo/), the [Noise Suppression for Voice](https://github.com/werman/noise-suppression-for-voice) RNNoise plug-in, a statically linked [WebRTC Audio Processing](https://github.com/Sqhh99/webrtc-audio-processing) plug-in, and a project-built WebRTC echo canceller in one safety-focused installer. Processing stays on the local CPU. There is no account, telemetry, network service, or audio recording.

This is an independent community project. It is not affiliated with or endorsed by NVIDIA, Equalizer APO, Noise Suppression for Voice, Google, or the WebRTC project.

## At a glance

- Windows 10 and 11 x64; local CPU processing on Intel, AMD, or other x64 hardware.
- Automatic filtering for attached microphones in the Windows shared-mode capture path.
- RNNoise for studio-quality 48 kHz streams and WebRTC APM for 16 kHz fallback devices.
- On Windows 11, acoustic echo cancellation that uses Windows' own copy of the speaker audio as its reference.
- Safe unchanged pass-through when a stream uses any other sample rate or a processor fails.
- A SYSTEM startup-repair task that discovers and attaches newly registered microphones.
- Filtered microphones are named `… FILTERED` in every application's device list and stop advertising raw capture, so browser and Electron call apps (Teams, Discord, Odoo in WD Studio) receive the filtered signal too. A small SYSTEM watcher keeps raw capture off when Windows rebuilds a microphone after a replug or boot.
- Transactional registry changes, safety backups, idempotent repair, and conflict-aware uninstall.
- One offline installer with pinned dependencies, corresponding source, notices, and SHA-256 manifests.

It is intentionally narrower than NVIDIA Broadcast: this project handles microphone noise suppression and speaker-echo cancellation only. It does not provide camera effects, room reverberation removal, speaker processing, or a tuning interface.

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
    -> Echo canceller mode effect (Windows 11, see below; Windows supplies the speaker audio)
    -> Equalizer APO capture effect
       -> 48 kHz: RNNoise VST2
       -> 16 kHz: WebRTC APM VST2
       -> every other rate: unchanged pass-through
    -> application
```

Equalizer APO 1.4.2 hosts the unmodified `rnnoise_mono.dll` from Noise Suppression for Voice 1.10 and the project-built `webrtc_apm_16k.dll`. The generated configuration keeps the mutually exclusive rate branches inside `stage == "capture"`; speakers and playback streams are never processed.

Existing vendor audio effects are retained as Equalizer APO child effects rather than discarded. Endpoint attachment follows Equalizer APO's legacy, modern, and combined-device selection rules.

## Filtered microphones and raw capture

Windows lets an application ask for a microphone's *raw* stream, which skips every system audio effect—noise suppression and echo cancellation included. Chromium-based applications (Chrome, Edge, Teams, Discord, and Electron apps such as WD Studio) ask for it whenever the microphone advertises raw support, so before this change their calls bypassed the stack entirely.

On every microphone it filters (one with noise suppression at 16 or 48 kHz), the manager therefore:

- appends ` FILTERED` to the microphone's name, for example `Microphone FILTERED (AT2040USB)`, so every application's device list shows which microphones are filtered. A microphone added later keeps its normal name until it is actually filtered (at the next boot, repair, or install);
- stops the microphone advertising raw capture, so those applications open it normally and receive the filtered signal.

Windows rebuilds the raw-capture setting from the driver whenever a microphone is plugged in and at every boot. The SYSTEM task `Keep microphones filtered` runs a small watcher that notices the rebuild and disables raw capture again within a fraction of a second; it changes nothing else. Both values are journaled like every other change: uninstall restores them, and a name you change yourself is left alone and no longer managed.

Applications that need the unprocessed microphone (some recording or dictation tools) receive the filtered signal on these microphones too.

## Echo cancellation (Windows 11)

When you use speakers instead of headphones, the microphone also hears whatever the speakers play: call participants, videos, game audio. On Windows 11 the stack removes that echo before noise suppression runs.

Windows 11 lets a capture effect that identifies itself as an echo canceller receive a copy of the audio being sent to the active output device. The project's echo canceller (`ncs_aec_apo.dll`) is such an effect. It runs WebRTC's AEC3 algorithm on each 10 ms of microphone audio, using that copy as the reference, and hands the result to Equalizer APO's noise suppression. In testing with real recordings from a TV speaker and three different microphones, it removed 17–40 dB of echo, with a median of about 31 dB.

The manager attaches it automatically to a microphone only when all of these hold:

- Windows 11 (build 22000 or later). Windows 10 has no supported way to give a capture effect the speaker audio, so Windows 10 keeps noise suppression only.
- The microphone uses modern driver effects and its mode-effect (MFX) slot is unused. A driver effect already in that slot—often the vendor's own echo canceller—is never displaced; that microphone is reported and skipped.
- The microphone is not a headset or handset microphone, and not a combined Bluetooth hands-free endpoint. Headsets play through headphones, so there is no acoustic echo to remove.

This includes microphones where Equalizer APO was already set up before the stack was installed (for example with Equalizer APO's own configurator), as long as Equalizer APO sits only in the stream-effect slot, so noise suppression still runs after echo cancellation. On those microphones the manager records and changes only the echo canceller's two registry values; uninstalling removes them and leaves Equalizer APO as it was.

`NoiseCancellingStack.Manager.exe status` shows `AEC` next to each microphone that has echo cancellation attached.

What to expect:

- **Latency:** about 9 ms at 48 kHz (8 ms at 16 kHz), reported to Windows so applications can keep audio and video in sync. The latency stays constant even if processing stops after an error.
- **Tone:** WebRTC applies an ~80 Hz high-pass filter, and on 48 kHz streams it attenuates content above about 19 kHz. Neither affects speech intelligibility, but the 48 kHz path is no longer full-bandwidth while echo cancellation is attached.
- **Scope:** it removes the speakers' sound. It does not remove room reverberation of your own voice, and it cannot remove sound from devices Windows does not play (a separate radio, another computer).
- **Other applications:** Teams, Zoom, Discord, and browsers keep their own echo cancellation; the stack does not ask them to turn it off. The two normally coexist, but if speech is clipped while both people talk, disable one of them.
- **Fail-open:** an unsupported format, processing mode, or any processing error leaves the microphone working with its normal audio.

To turn echo cancellation off without uninstalling, run the following in an elevated terminal, then restart Windows Audio or reboot. Audio then passes through the echo canceller unchanged and without added latency:

```text
reg add HKLM\SOFTWARE\NoiseCancellingStack /v DisableEchoCancellation /t REG_DWORD /d 1 /f
```

Delete that value (or set it to `0`) to turn it back on.

## Tray icon

A microphone icon in the notification area gives each user three controls:

- **Filter microphones**: turns noise suppression and echo cancellation on or off for every microphone. Off means stock Windows behaviour: audio passes through unprocessed, the ` FILTERED` names come off, and raw capture is advertised again. Windows Audio restarts once (a short dropout) so the echo canceller picks up the change. The choice applies to the whole PC and survives reboots, with or without the icon running.
- **Start with Windows**: starts the icon when you sign in (on by default).
- **Close**: removes the icon. Filtering keeps its current state; reopen the icon from the Start menu (**Noise Cancelling Stack tray icon**).

The icon is green while filtering is on, grey while it is off, amber while a change is being applied, and red if the stack is not installed. It runs unelevated: it only writes a single on/off value that setup lets standard users change, and the SYSTEM watcher applies it.

## Is it always running?

There is no persistent filtering application. Windows loads the configured APO and the matching processor when an attached microphone's shared-mode capture graph becomes active, then unloads it when no longer needed. The tray icon and the watcher are small helpers (about 2 MB and 18 MB of private memory) that use no CPU while idle. Measured CPU, memory, power, and latency, with a comparison against NVIDIA Broadcast's published figures, are in [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

A hidden SYSTEM task runs an idempotent repair scan one minute after startup, including on battery. It uses `StartWhenAvailable`, permits only one instance, and has a bounded execution time. The scan repairs the managed configuration, discovers newly registered microphones, and restarts Windows Audio only when an attachment or configuration actually changed.

This makes the stack ready when Windows or an application switches microphones. To inspect the current state at any time, use **Microphone status** from the Start menu or run:

```text
NoiseCancellingStack.Manager.exe status
```

Each registered endpoint is reported as `RNNOISE`, `WEBRTC`, or `PASSTHROUGH`, together with its Equalizer APO attachment state and, on Windows 11, whether echo cancellation (`AEC`) is attached.

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

1. Download the complete v1.2.0 release set from [GitHub Releases](https://github.com/JBWortley/jbw-NoiseCancellingStack/releases), keeping the installer and checksum manifest together.
2. Verify `NoiseCancellingStack-Setup-1.2.0.exe` against `SHA256SUMS.txt` obtained through a trusted channel.
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

Exit code `0` means success, `2` means a safe partial result or preserved conflict, and `1` means failure. Repair validates all three native payload hashes and the WebRTC and echo canceller self-tests before changing configuration or endpoints. Re-running the offline installer restores missing or damaged release payloads; a subsequent repair safely converges on the intended state.

## Safety and rollback

Before every mutating operation, the manager exports the capture-endpoint and Equalizer APO registry areas and copies the active `config.txt`. State, logs, and backups are stored under:

```text
C:\ProgramData\NoiseCancellingStack
```

That directory permits writes only by SYSTEM and Administrators. State is recorded before the first endpoint mutation. During uninstall, a registry value is restored only if it still matches either the project-installed value or its recorded original value. Later changes made by drivers or other audio tools are preserved and reported as conflicts.

The existing Equalizer APO configuration is retained. This project owns only a marked include block and `NoiseCancellingStack.txt`. Equalizer APO itself is deliberately left installed during uninstall because another application may depend on it.

The design invariants are documented in [docs/SAFETY.md](docs/SAFETY.md), and the adversarial review record is in [docs/REVIEW.md](docs/REVIEW.md).

## Privacy

Audio remains inside the local Windows audio pipeline. This software does not save, transmit, or inspect recordings outside the in-process RNNoise, WebRTC, and echo canceller callbacks. On Windows 11, Windows hands the echo canceller a copy of the audio being played to your speakers; it is used in memory as the echo reference inside the Windows audio engine process and is never stored or sent anywhere. It has no listener, web server, analytics client, account, or update checker.

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

The complete v1.2.0 release set is:

```text
NoiseCancellingStack-Setup-1.2.0.exe
NoiseCancellingStack-source-1.2.0.zip
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
- The project-authored echo canceller APO is MIT-licensed; it statically links WebRTC Audio Processing and Abseil under their own notices.
- WebRTC Audio Processing and Abseil retain their BSD-3-Clause, patent-grant, and Apache-2.0 notices.
- Inno Setup is a separately licensed build tool whose generated setup engine is present in the installer.

See [LICENSING.md](LICENSING.md) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) before redistributing a compiled installer.

## Contributing and security

Contributions are welcome under the project's MIT license. Start with [CONTRIBUTING.md](CONTRIBUTING.md). Report security-sensitive issues privately as described in [SECURITY.md](SECURITY.md), particularly anything involving elevated registry writes, scheduled tasks, installer trust, or rollback behavior.

## Project status

Version 1.2.0 adds Windows 11 acoustic echo cancellation in front of the existing noise suppression. Version 1.1.0 added the vendor-agnostic 16 kHz WebRTC fallback while preserving the 48 kHz RNNoise path, fail-open behavior, and startup repair. Automated tests use synthetic audio only; no microphone or playback device is accessed during the release build.
