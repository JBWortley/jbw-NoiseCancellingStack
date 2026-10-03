# Security policy

## Trust model

The installer requires administrator rights because Windows capture endpoint effects live under HKLM and Windows Audio must be restarted. It performs no network access at install time and installs no listening service.

The build process pins and verifies SHA-256 hashes for every bundled upstream artifact. Release artifacts are unsigned until a project-owned Authenticode certificate is configured, so recipients must verify `SHA256SUMS.txt` through a trusted channel.

## Sensitive data

The software does not record or transmit audio. Local logs contain endpoint display names, sample rates, endpoint GUIDs, and operation results. Registry exports can reveal installed audio hardware and stay under the administrator-protected `C:\ProgramData\NoiseCancellingStack\backups` directory.

Voice samples, local registry exports, and machine-specific diagnostic logs under `local-migration` are intentionally excluded from Git.

## Reporting

Do not open a public issue for a suspected vulnerability. Use GitHub's **Report a vulnerability** action on the repository Security tab to start a private advisory. If that feature is unavailable, contact the repository owner privately before disclosing details.

Include the product version, Windows version, affected operation, whether Equalizer APO was already installed, and the smallest relevant log excerpt. Remove endpoint GUIDs and hardware names when they are not necessary. Do not attach voice recordings, complete registry exports, credentials, signing material, or other sensitive files.

Particularly sensitive areas include elevated registry writes, write-ahead state validation, ProgramData ACLs, the SYSTEM maintenance task, installer payload verification, config-path handling, and conflict-aware rollback.
