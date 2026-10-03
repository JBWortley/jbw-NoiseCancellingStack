# Noise Cancelling Stack

An offline Windows installer that applies the same RNNoise microphone filtering stack proven on the original development machine:

- Equalizer APO 1.4.2 provides the Windows system-effect host.
- `noise-suppression-for-voice` v1.10 provides the RNNoise VST2 filter.
- Every currently registered capture endpoint is attached using Equalizer APO's own SFX/EFX selection rules.
- The filter is guarded so it runs only at the capture stage and only at 48,000 Hz.

No cloud service, account, telemetry, recording, or AI service is involved at runtime.

## Supported systems

- Windows 10 or Windows 11, x64
- Shared-mode applications that use the Windows system-effects pipeline
- Mono or multichannel capture streams at exactly 48,000 Hz

ASIO, WASAPI exclusive mode, applications that explicitly disable audio enhancements, and hardware/driver paths that bypass Windows system effects cannot be filtered by this stack. RNNoise v1.10 requires 48 kHz; 8/16 kHz Bluetooth hands-free streams and 44.1 kHz streams pass through unchanged rather than being processed unsafely.

## Install

Run `NoiseCancellingStack-Setup-1.0.0.exe` and approve the Windows administrator prompt. Setup is offline and includes pinned copies of its two upstream components.

Installation briefly restarts Windows Audio. End active calls and recordings first. A Start-menu **Repair all microphones** shortcut is installed for adding a newly connected microphone without AI assistance. A SYSTEM task performs the same idempotent rescan one minute after startup and restarts audio only if a new endpoint was attached.

The installer is currently unsigned. Windows SmartScreen may show **Unknown publisher**. Verify the installer against `SHA256SUMS.txt` before running it. A future signed build can be produced by adding an Authenticode certificate to the release process; no certificate is stored in this repository.

## Safety and rollback

Before every install, repair, or uninstall mutation, the manager exports the complete Windows capture-endpoint registry and Equalizer APO product registry and copies `config.txt`. Backups and logs are stored under:

```text
C:\ProgramData\NoiseCancellingStack
```

Endpoint changes use a write-ahead state file. Existing driver APOs are preserved as Equalizer APO child effects. Uninstall restores only values that still match what this product installed; if another tool changed a value afterward, that value is preserved and reported as a conflict.

Existing Equalizer APO `config.txt` content is retained. The installer adds a marked include block and owns only `NoiseCancellingStack.txt`. Uninstall removes that block but deliberately leaves Equalizer APO itself installed because other software may use it.

## Build

Requirements:

- .NET SDK 9.0.304 or a compatible patch selected by `global.json`
- Inno Setup 6
- Windows 10/11 x64

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

The build downloads missing upstream artifacts only from pinned official URLs, rejects checksum mismatches, runs the test suite, publishes the self-contained manager, and produces the installer and corresponding-source archives in `dist`.

## Redistribution and source

When sharing the installer, share these files together:

```text
NoiseCancellingStack-Setup-1.0.0.exe
NoiseCancellingStack-source-1.0.0.zip
EqualizerAPO-src-1.4.2.zip
noise-suppression-for-voice-v1.10.zip
SHA256SUMS.txt
```

The three source archives contain this project's buildable source and the corresponding source for both bundled GPL components. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Maintenance commands

From an elevated terminal in the installation directory:

```text
NoiseCancellingStack.Manager.exe status
NoiseCancellingStack.Manager.exe repair
NoiseCancellingStack.Manager.exe uninstall
```

Exit code `0` means success, `2` means a safe partial result or preserved conflict, and `1` means failure.
