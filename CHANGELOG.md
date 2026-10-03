# Changelog

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
