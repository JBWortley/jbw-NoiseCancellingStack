# Licensing and redistribution

This repository separates the permissive license for its original installer code from the licenses of the software it packages.

## Project-authored work

Unless a file says otherwise, source code, build scripts, tests, and documentation authored for Noise Cancelling Stack are licensed under the MIT License in [LICENSE](LICENSE).

MIT permits use, modification, distribution, sublicensing, and commercial use. Its attribution condition is simple: retain the copyright notice and MIT permission notice in copies or substantial portions of the project-authored software.

## Bundled and build-time software

| Component | Relationship | License | Source supplied with a release |
| --- | --- | --- | --- |
| Noise Cancelling Stack manager and installer script | Project-authored | MIT | `NoiseCancellingStack-source-<version>.zip` |
| Equalizer APO 1.4.2 | Unmodified bundled installer and separately running Windows APO host | GPL-2.0-only | `EqualizerAPO-src-1.4.2.zip` |
| Noise Suppression for Voice 1.10 | Unmodified bundled VST2 DLL loaded by Equalizer APO | GPL-3.0-only | `noise-suppression-for-voice-v1.10.zip` |
| Inno Setup 6 | Separately installed build tool; generated setup engine is included in the output executable | Inno Setup's own license | Obtain from the selected Inno Setup distribution |

The MIT license does not override or replace any third-party license. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for upstream links, copyright holders, filenames, and pinned hashes.

## Redistribution checklist

When distributing a compiled Noise Cancelling Stack installer:

1. Keep the installer, `SHA256SUMS.txt`, and all three corresponding-source archives available together.
2. Preserve `LICENSE`, `THIRD-PARTY-NOTICES.md`, and the upstream license files installed by the package.
3. Do not describe the distribution as endorsed by any upstream project or by NVIDIA.
4. If you modify an upstream GPL-covered component, provide the complete corresponding source for the exact modified component and follow that component's license.
5. If you build with Inno Setup, review the license shipped with that compiler version and any current commercial-use terms.
6. If you sign a fork, identify your publisher accurately; do not imply that JBWortley signed or endorsed it.

This file is an engineering-oriented disclosure, not legal advice. Redistributors remain responsible for confirming that their distribution and jurisdiction-specific obligations are satisfied.
