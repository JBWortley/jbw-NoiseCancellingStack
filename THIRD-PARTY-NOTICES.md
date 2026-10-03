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

## Inno Setup 6

Inno Setup is the separately licensed build tool used to compile the installer, and its generated setup engine is part of the resulting executable. The compiler is not vendored in this repository. Builders must comply with the current Inno Setup license and should review its commercial-use terms when applicable. See <https://jrsoftware.org/isinfo.php> and the `LICENSE.TXT` shipped with the selected compiler version.

## No implied endorsement

Equalizer APO, Noise Suppression for Voice, RNNoise, Inno Setup, NVIDIA, and NVIDIA Broadcast remain the property of their respective owners. Their names identify interoperability or upstream dependencies only.
