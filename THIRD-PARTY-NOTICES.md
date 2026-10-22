# Third-party notices

The root [MIT License](LICENSE) applies only to code and documentation authored for Noise Cancelling Stack. It does not relicense the components listed here. Noise Cancelling Stack is an independent installer and is not affiliated with or endorsed by any upstream project.

## Equalizer APO 1.4.2

- Copyright: Jonas Thedering and Equalizer APO contributors
- Project: <https://sourceforge.net/projects/equalizerapo/>
- License: GNU General Public License version 2.0 (`GPL-2.0-only`)
- Bundled binary: unmodified `EqualizerAPO-x64-1.4.2.exe`
- Binary SHA-256: `7403be7427bbe1936a40dded082829b6e217fc4f5990fee5cba501f0ae055afa`
- Corresponding source distributed alongside releases: `EqualizerAPO-src-1.4.2.zip`

The unmodified upstream license is extracted from the corresponding-source archive at build time and installed as `licenses\EqualizerAPO-LICENSE.txt`.

## Noise Suppression for Voice 1.10

- Copyright: werman and contributors
- Project: <https://github.com/werman/noise-suppression-for-voice>
- License: GNU General Public License version 3.0 (`GPL-3.0-only`)
- Bundled binary source: unmodified `win-rnnoise.zip` release asset
- Release archive SHA-256: `e4dcc186102850f8dcebeb766ad1bf34c24f6280bd614ef84b72dae4783fc517`
- Bundled `rnnoise_mono.dll` SHA-256: `7debdfc4843461c47b23ac7a736b4663fe581643563ae764cde395e006004080`
- Corresponding source distributed alongside releases: `noise-suppression-for-voice-v1.10.zip`

The unmodified upstream license is extracted from the corresponding-source archive at build time and installed as `licenses\RNNoisePlugin-LICENSE.txt`. That source archive includes the project's vendored dependencies and their notices.

## WebRTC Audio Processing m155.8059.4.0

- Copyright: WebRTC project authors and contributors
- Standalone project: <https://github.com/Sqhh99/webrtc-audio-processing>
- Pinned commit: `3a6b666a906da6cb3c6f9d474fab21c2ba739f22`
- License: BSD-3-Clause and compatible bundled third-party notices; an additional patent grant is included
- Source archive SHA-256: `028df34d5ed4a1015b7349d2a099efb27c17a68acefb5c1d448f833207f5db9a`
- Corresponding source distributed alongside releases: `webrtc-audio-processing-3a6b666a906da6cb3c6f9d474fab21c2ba739f22.zip`

The build statically links WebRTC APM into `webrtc_apm_16k.dll`. The installer includes the standalone project's `COPYING`, WebRTC's `LICENSE`, WebRTC's `PATENTS`, and the notices for the linked components below:

- Ooura FFT: `licenses\WebRTC-Ooura-LICENSE.txt`
- WebRTC FFT: `licenses\WebRTC-FFT-LICENSE.txt`
- PFFFT: `licenses\WebRTC-PFFFT-LICENSE.txt`
- WebRTC RNNoise: `licenses\WebRTC-RNNoise-COPYING.txt`

The corresponding source archive retains the original source and notices for these components.

## Abseil 20250814.1

- Copyright: The Abseil Authors
- Project: <https://github.com/abseil/abseil-cpp>
- License: Apache License 2.0
- Source archive SHA-256: `1692f77d1739bacf3f94337188b78583cf09bab7e420d2dc6c5605a4f86785a1`
- Meson wrap patch SHA-256: `f44ad4d72ff2919a6a48cf0887f69aef34c338bfdd8931153596e3766d75f654`
- Corresponding source distributed alongside releases: `abseil-cpp-20250814.1.tar.gz` and `abseil-cpp_20250814.1-1_patch.zip`

Abseil is built from the checksum-pinned source selected by WebRTC's Meson wrap and linked statically. Its Apache-2.0 license is installed as `licenses\Abseil-LICENSE.txt`.

## FST 0.120.0

- Copyright: IOhannes m zmölnig, IEM, and contributors
- Project: <https://git.iem.at/zmoelnig/FST>
- License: GNU General Public License version 3.0 (`GPL-3.0-only`)
- Source used: `external/FST/fst` in `noise-suppression-for-voice-v1.10.zip`

The project-authored WebRTC VST2 sources carry `SPDX-License-Identifier: GPL-3.0-only` because they use the FST VST2 ABI headers. The FST license is installed as `licenses\FST-LICENSE.txt`.

## Inno Setup 6

Inno Setup is the separately licensed build tool used to compile the installer, and its generated setup engine is part of the resulting executable. The compiler is not vendored in this repository. Builders must comply with the current Inno Setup license and should review its commercial-use terms when applicable. See <https://jrsoftware.org/isinfo.php> and the `LICENSE.TXT` shipped with the selected compiler version.

## No implied endorsement

Equalizer APO, Noise Suppression for Voice, RNNoise, WebRTC, Abseil, FST, Inno Setup, NVIDIA, and NVIDIA Broadcast remain the property of their respective owners. Their names identify interoperability or upstream dependencies only.
