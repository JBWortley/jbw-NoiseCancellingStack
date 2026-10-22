# Safety invariants

- Never touch render/playback endpoints.
- Never load either processor outside the capture stage.
- Route exactly 48 kHz exclusively to RNNoise and exactly 16 kHz exclusively to WebRTC APM.
- Keep every unsupported sample rate on unchanged pass-through.
- Keep the WebRTC processor's fixed 10 ms delay and permanent-instance delayed-dry failure path.
- Never mutate configuration or endpoint state before both payload hashes and the WebRTC synthetic self-test pass.
- Never replace an existing Equalizer APO version with a different version.
- Never overwrite the user's Equalizer APO configuration; use a marked include block.
- Never mutate endpoint registry values before a write-ahead state record and full registry export exist.
- Never discard existing driver APO GUIDs; preserve them as Equalizer APO child effects.
- Never restore over a value changed after installation; report a conflict instead.
- Never delete local backups automatically.
- Never uninstall Equalizer APO automatically.
- Never download code on a coworker's machine; the release installer is offline.
- Never trust a nonempty pre-existing state journal unless the protected Equalizer APO configuration proves it belongs to an earlier installation.
- Never remove the manager during an incomplete or conflicted rollback.

Known boundary: an endpoint that denies the documented registry write is skipped and reported. The manager does not seize ownership or broaden its ACL. This is safer than forcing installation at the cost of not supporting every vendor driver.

Application-specific processing is outside this stack's control. Enabling suppression in both this system-wide stack and an application can cause double suppression; users should disable one layer if speech quality degrades. Shared-mode applications that disable system effects, WASAPI exclusive mode, and ASIO bypass the APO path.
