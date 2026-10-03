# Adversarial review record

Version 1.0.0 received two focused pre-release review passes on 2026-10-03.

## Round 1: mutation and rollback safety

The review traced interruption, repair, and uninstall behavior across configuration files, the endpoint registry, and the scheduled SYSTEM task. Findings and remediations:

- Quoted the RNNoise VST path so spaces in `Program Files` cannot break loading.
- Reordered endpoint writes so the child-APO recovery record exists before the live effect slot changes.
- Added cleanup for manager-created registry keys during rollback.
- Replaced predictable temporary filenames with randomized same-directory files and cleanup-on-failure.
- Made uninstall retain the manager, managed config, task, and state when rollback is incomplete.
- Forced the install location to the protected fixed Program Files path used by the SYSTEM task.
- Replaced Task Scheduler's battery-hostile defaults so startup repair also runs on laptops using battery power.
- Hardened ProgramData ACLs to SYSTEM/Administrators full control and Users read/execute, and rejected reparse points.
- Rejected malformed, duplicate, path-like, oversized, or structurally unexpected endpoint journals.
- Rejected nonempty pre-staged journals unless an exact protected managed configuration already exists.

## Round 2: distribution and supply-chain safety

The review treated every release input and output as untrusted until verified. Findings and remediations:

- Pinned and checked SHA-256 for both upstream release archives and the extracted RNNoise DLL.
- Added the project's own source archive alongside both upstream corresponding-source archives.
- Made builds clear only validated build-output directories so stale artifacts cannot enter a release manifest.
- Excluded voice recordings, registry exports, endpoint identifiers, generated integration results, vendor binaries, and local logs from source control and source archives.
- Added CI for warning-as-error release tests and retained the unsigned-installer disclosure and checksum verification instructions.

## Verification performed

- Unit tests cover configuration isolation, line-injection rejection, managed-block preservation, endpoint-mode selection, registry snapshot fidelity, state round-tripping, and malformed-state rejection.
- Install and idempotent repair were exercised on the development machine across nine registered capture endpoints.
- A disconnected endpoint was detached, reattached, and restored to its exact original registry state.
- The real uninstaller and subsequent reinstall were exercised.
- A live 48 kHz capture confirmed that Windows Audio loaded both Equalizer APO and the packaged RNNoise DLL.

The installer is intentionally unsigned in this release. Recipients must obtain `SHA256SUMS.txt` through the same trusted channel as the release announcement and verify the installer before elevation.

## Remaining validation boundary

The development machine already had Equalizer APO 1.4.2, so the "Equalizer APO absent" bootstrap branch was not destructively exercised there. Windows Sandbox and other local VM runners were unavailable (the Sandbox feature was disabled). That branch uses the bundled, hash-pinned, unmodified upstream installer with its documented silent switch, but a canary install on one clean Windows 10 and one clean Windows 11 machine is still recommended before organization-wide rollout.
