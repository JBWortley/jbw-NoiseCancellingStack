# Public release checklist

This file records the deliberate steps for making the repository public. It is not an instruction to publish automatically.

## Suggested GitHub metadata

- Description: `Offline system-wide RNNoise microphone suppression for Windows—an open alternative when NVIDIA Broadcast is unavailable.`
- Website: leave blank unless a project site is created.
- Topics: `windows`, `audio`, `microphone`, `noise-suppression`, `rnnoise`, `equalizer-apo`, `offline`, `nvidia-broadcast-alternative`

## Before changing visibility

- [ ] Review the entire README as a first-time user.
- [ ] Confirm the owner name and copyright line in `LICENSE`.
- [ ] Confirm MIT is the intended license for all project-authored contributions.
- [ ] Review `LICENSING.md` and `THIRD-PARTY-NOTICES.md`; obtain legal advice if the distribution context requires it.
- [ ] Search every tracked revision for credentials, recordings, registry exports, endpoint GUIDs, machine names, and signing keys.
- [ ] Confirm `local-migration`, `vendor`, `artifacts`, and `dist` remain ignored.
- [ ] Run the test suite and a complete release build from a clean checkout.
- [ ] Run canary installs on clean Windows 10 and Windows 11 systems, including the Equalizer-APO-absent path.
- [ ] Decide whether to obtain an Authenticode certificate; if unsigned, retain the conspicuous SmartScreen warning.
- [ ] Confirm GitHub private vulnerability reporting is enabled.
- [ ] Review issue templates, contribution guidance, and the security policy.

## Proposed first public release

- [ ] Merge the reviewed `public-release-prep` branch locally.
- [ ] Push the reviewed commit only after explicit approval.
- [ ] Change repository visibility only after explicit approval.
- [ ] Build and publish v1.0.1 with the installer, project source, both upstream source archives, and `SHA256SUMS.txt`.
- [ ] Verify GitHub's uploaded asset digests against the local manifest.
- [ ] Confirm CI passes on the public commit and tag.
- [ ] Test the public download instructions while signed out of GitHub.

Do not rewrite or replace the existing v1.0.0 assets: they were released with their original source and license files. Publish the clarified licensing and documentation as v1.0.1.
