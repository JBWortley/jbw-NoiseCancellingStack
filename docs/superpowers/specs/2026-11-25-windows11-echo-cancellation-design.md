# Windows 11 Echo Cancellation Design

**Date:** 2026-11-25

**Status:** Implemented on branch `spike/aec-reference`; live end-to-end validation pending

**Scope:** Windows 11 x64 shared-mode capture, in front of the existing Equalizer APO noise suppression

## Intent

Remove acoustic echo (speaker output picked up by the microphone) system-wide, without a virtual microphone, background process, or vendor runtime, while keeping every existing safety, rollback, and fail-open guarantee.

## The deciding constraint

Echo cancellation needs a reference copy of what the speakers play. Equalizer APO's VST host gives plug-ins only the capture channels, so the existing plug-in route cannot see the reference. Windows 11 supplies a loopback reference to a capture **mode effect (MFX)** that implements `IApoAcousticEchoCancellation` plus the auxiliary-input interfaces; the engine selects the render endpoint, follows default-device changes, converts formats, and delivers the reference on a real-time callback. Alternatives were rejected: a helper process feeding shared memory (persistent process, cross-session IPC, injection surface, privacy), opening a loopback stream inside audiodg (documented deadlock risk), and forking Equalizer APO (violates the unmodified-Equalizer-APO invariant).

## Feasibility evidence (spike, `spikes/aec-reference`)

A pass-through MFX was registered on three microphones (AT2040USB 48 kHz mono, C920 16 kHz stereo, onboard 48 kHz stereo) on Windows 11 24H2 (build 26100):

- The unsigned third-party MFX loaded (as an aggregated COM object) and Windows attached a reference in **both default and Communications modes** on all three: 48 kHz stereo float, pre-volume, about 100 buffers per second, continuous.
- The microphone format was never changed, so Equalizer APO's 16/48 kHz routing is unaffected.
- `AcceptInput` arrives on a separate engine thread and was observed overlapping `APOProcess`, so a lock-free ring is required.
- Offline AEC3 on 20 s mic/reference recordings removed 16.6–40.0 dB of echo (median ≈ 31 dB); echo was 59–86 % of microphone energy. CPU cost was 0.09–0.15 ms per 10 ms frame.

## Design

- **Placement:** `ncs_aec_apo.dll`, a project-authored MIT APO in the capture MFX slot. Capture effects run EFX → MFX → SFX, so echo is removed before Equalizer APO's SFX noise suppression.
- **Mode:** registered for the default processing mode only, matching Equalizer APO's SFX mode list; registering Communications would create a mode in which the noise-suppression SFX might not run.
- **Processing:** WebRTC AEC3 only (no noise suppression or gain control), internal rate up to 48 kHz, multichannel capture and render. The reference is requested as 32-bit float at a native rate with at most two channels, post-volume where available.
- **Threading:** `AcceptInput` only writes a single-producer/single-consumer ring; `APOProcess` drains complete 10 ms blocks (bounded to 250 ms of backlog) and is the only WebRTC caller. A second pinned patch removes APM's render mutex (the capture mutex was already removed); tests confirm no OS lock, wait, or heap call in warm processing.
- **Latency:** AEC3 adds a fixed 128/260/430-frame delay at 16/32/48 kHz (measured; guarded by a test). The APO reports it through `GetLatency`, keeps a matching dry delay line in step, and switches to delayed-dry audio on failure or for a non-10 ms block, so the latency never changes mid-stream. Unsupported formats and modes pass through undelayed with zero reported latency.
- **Tone:** AEC3 always applies its ~80 Hz high-pass filter and, at 48 kHz, attenuates content above about 19 kHz (with shallow dips near the band-split edges). A gate that bypasses AEC when nothing plays was rejected: crossfading between the phase-shifted processed signal and dry audio would add its own artifacts.
- **Applications:** the effect is not advertised through `GetEffectsList`, so applications keep their own echo cancellation.
- **Kill switch:** `HKLM\SOFTWARE\NoiseCancellingStack\DisableEchoCancellation` (DWORD ≠ 0) makes every new instance pass audio through.

## Manager policy

- Attach only on Windows 11 (build ≥ 22000), only after the COM class and APO registration are verified at `{app}\apo\ncs_aec_apo.dll`, only on SFX/EFX endpoints, never on headset/handset form factors, and only when the MFX value, MFX mode list, and composite MFX list are all unused.
- New endpoints receive the MFX CLSID and a default-mode MFX list inside the existing write-ahead endpoint transaction. Already-managed endpoints receive them as a separate journaled sub-transaction with revert-or-recover semantics.
- Endpoints where Equalizer APO was attached before this product (found live: the AT2040USB and onboard microphone had been set up with Equalizer APO's configurator, so 1.2.0's first build skipped them) receive an echo-canceller-only journal record owning just the MFX value and MFX mode list, when Equalizer APO occupies the SFX slot alone. Uninstall restores only those two values.
- The MFX mode list is an optional journal value present as an original/installed pair; journals from 1.0 and 1.1 stay valid without a schema change.
- Repair detaches the echo canceller (journaled) if its registration is gone.
- The payload hash and `NcsAecApoSelfTest` (class factory and interfaces, plus ≥ 20 dB on a synthetic echo) are validated before any mutation on every Windows version.

## Live validation (2026-11-26, installed 1.2.0, Windows 11 build 26100)

Synthetic meeting speech at −24 dBFS plus the user's music played on a TV speaker; the processed microphone (after both effects) and the render loopback were recorded by `spikes/aec-reference` probe and analysed by `eval/live_echo.py`.

- C920 (16 kHz stereo, echo canceller attached): linear echo path gain fell from −11.1 dB to −36.6 dB (25.5 dB); total microphone level with the user silent fell 18 dB (12 dB in the 300–3400 Hz band).
- AT2040USB control (no echo canceller in that run): unchanged (−0.1 dB), confirming identical playback conditions.
- Double talk: the user's voice on the C920 kept its level relative to the control microphone within 0.6 dB, and ended about 10 dB above the residual echo (about −3 dB without the echo canceller).

- Odoo calls in WD Studio (Electron 39) still carried the user's music: Chromium requests raw capture whenever the endpoint advertises it, bypassing every effect. With `RawProcessingSupported` set to false on the AT2040USB (immediate, no restart), the far end no longer heard the music. The Audio Endpoint Builder restores the driver's value on replug and boot, and an FxProperties override is ignored, which is why the product adds a SYSTEM watcher; the ` FILTERED` name persists across both.

## Open items before release

- Live tests still outstanding: post-volume loopback behaviour, default-output switching, and Communications-category applications (which now run in default mode with both effects).
- Upgrade over a running install: setup now renames each loaded payload aside before copying (a mapped DLL can be renamed but not overwritten) and restores it if setup stops early; confirm on a live upgrade while a microphone is open.
- Listening tests for double talk and the 48 kHz tonal change; consider exposing a per-endpoint opt-out.
- Windows 10 remains noise-suppression only.
