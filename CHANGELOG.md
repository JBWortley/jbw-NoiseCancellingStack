# Changelog

## 1.2.0 (unreleased)

- Added Windows 11 acoustic echo cancellation: a project-built capture mode effect (MFX) runs WebRTC AEC3 against the speaker reference that Windows supplies, before Equalizer APO's noise suppression.
- The echo canceller is attached automatically only on Windows 11 (build 22000+), only to modern endpoints whose MFX slot, MFX mode list, and composite MFX list are unused, and never to headset, handset, or combined hands-free microphones. Existing driver effects are never displaced.
- It is registered for the default processing mode only, matching Equalizer APO, so it never makes a new processing mode (such as Communications) available.
- Echo cancellation adds a fixed latency of about 9 ms at 48 kHz (8 ms at 16 kHz), reported to Windows. On failure it switches to delayed dry audio so the latency never changes mid-stream.
- WebRTC's ~80 Hz high-pass filter applies while echo cancellation is attached, and 48 kHz streams are attenuated above about 19 kHz.
- `DisableEchoCancellation` (HKLM\SOFTWARE\NoiseCancellingStack, DWORD 1) turns it into unchanged pass-through without uninstalling.
- Upgrades add echo cancellation to already-managed microphones through a write-ahead journal entry; older journals remain valid.
- Microphones where Equalizer APO was attached before the stack was installed also receive the echo canceller when Equalizer APO occupies only their SFX slot. They are journaled as echo-canceller-only records that own just the MFX value and MFX mode list, so uninstall never touches their Equalizer APO setup.
- Filtered microphones are renamed `… FILTERED` and no longer advertise raw capture, so Chromium-based call apps (Teams, Discord, browsers, Electron apps such as WD Studio), which request raw streams that skip all system effects, receive the filtered signal. A new SYSTEM watcher task keeps raw capture disabled when Windows rebuilds a microphone after a replug or boot. Both values are journaled and restored on uninstall.
- Setup moves loaded payload DLLs aside before copying their replacements, so upgrading while microphones are in use no longer depends on Windows Audio releasing the files; the old copies are deleted once unloaded.
- Repair validates the echo canceller's SHA-256 digest and synthetic self-test before any mutation, on every Windows version.
- Applications keep their own echo cancellation; the stack does not advertise its effect to them. If speech is clipped during double talk, disable one layer.

## 1.1.0 (unreleased)

- Added a hardware-vendor-agnostic WebRTC APM noise-suppression path for exactly 16 kHz capture streams, including Logitech C920 and Bluetooth hands-free use cases.
- Kept exactly 48 kHz streams on RNNoise and all other sample rates on unchanged pass-through.
- Added a fixed 10 ms delay to the 16 kHz processor so arbitrary host block sizes remain ordered and failure can switch to delayed dry audio safely.
- Added pre-mutation checksum and synthetic self-test validation for both native payloads, plus hidden SYSTEM startup repair using the existing one-minute boot trigger.
- Added pinned WebRTC, Abseil, Meson, Ninja, FST, license, patent, and corresponding-source packaging.
- Upgrade repairs now stamp the durable state journal with the installed product version.
- Explicit installer runs now restart Windows Audio even when only plug-in payloads changed; unchanged manual and scheduled repairs remain non-disruptive.
- Applications with their own filtering can cause double suppression; disable one layer if speech sounds metallic, pumps, or loses quiet syllables.

## 1.0.1 (unreleased)

- Prepared the repository for public review with a use-case-focused README and contribution guidance.
- Licensed project-authored code under MIT with an explicit attribution notice.
- Separated the project's MIT license from the bundled GPL-2.0 and GPL-3.0 component obligations.
- Added SPDX identifiers and commentary around elevated, transactional, and rollback-sensitive code.
- Added no functional audio, endpoint-registration, or rollback behavior changes.

## 1.0.0

- First redistributable offline installer.
- Capture-only RNNoise v1.10 filtering at 48 kHz through Equalizer APO 1.4.2.
- Automatic registration of all known capture endpoints with original APO chaining.
- Write-ahead state, full backups, conflict-aware uninstall, and startup repair.
- Pinned upstream hashes and corresponding-source release assets.
