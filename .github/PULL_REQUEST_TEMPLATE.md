## Summary

Describe the problem and the smallest change that solves it.

## Safety impact

- [ ] Capture endpoints only; no render/playback changes.
- [ ] The exclusive 16 kHz WebRTC, 48 kHz RNNoise, and capture-stage guards remain enforced.
- [ ] Unsupported sample rates remain pass-through.
- [ ] Existing APO values and user configuration remain preserved.
- [ ] Registry mutation still follows durable write-ahead state and backup creation.
- [ ] Uninstall remains conflict-aware and recoverable.
- [ ] No recordings, registry exports, endpoint GUIDs, machine data, binaries, credentials, or signing keys are included.

## Verification

List automated tests and any install, repair, rollback, uninstall, or clean-machine scenarios performed.

## Licensing

- [ ] I am authorized to submit this contribution under the repository's MIT License.
- [ ] Any third-party material is identified with its origin and license.
- [ ] Native dependency, patent, and corresponding-source notices remain complete.
