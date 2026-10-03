# Security policy

## Trust model

The installer requires administrator rights because Windows capture endpoint effects live under HKLM and Windows Audio must be restarted. It performs no network access at install time and installs no listening service.

The build process pins and verifies SHA-256 hashes for every bundled upstream artifact. Release artifacts are unsigned until a project-owned Authenticode certificate is configured, so recipients must verify `SHA256SUMS.txt` through a trusted channel.

## Sensitive data

The software does not record or transmit audio. Local logs contain endpoint display names, sample rates, endpoint GUIDs, and operation results. Registry exports can reveal installed audio hardware and stay under the administrator-protected `C:\ProgramData\NoiseCancellingStack\backups` directory.

Voice samples, local registry exports, and machine-specific diagnostic logs under `local-migration` are intentionally excluded from Git.

## Reporting

Report vulnerabilities privately to the repository owner. Include the product version, Windows version, the operation performed, and the relevant local log. Do not attach voice recordings unless they are specifically necessary and safe to share.

