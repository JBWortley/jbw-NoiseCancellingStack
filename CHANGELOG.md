# Changelog

## 1.1.0 (unreleased)

- Added a hardware-vendor-agnostic WebRTC APM noise-suppression path for exactly 16 kHz capture streams, including Logitech C920 and Bluetooth hands-free use cases.
- Kept exactly 48 kHz streams on RNNoise and all other sample rates on unchanged pass-through.
- Added a fixed 10 ms delay to the 16 kHz processor so arbitrary host block sizes remain ordered and failure can switch to delayed dry audio safely.
- Added pre-mutation checksum and synthetic self-test validation for both native payloads, plus hidden SYSTEM startup repair using the existing one-minute boot trigger.
- Added pinned WebRTC, Abseil, Meson, Ninja, FST, license, patent, and corresponding-source packaging.
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
