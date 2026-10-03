# Safety invariants

- Never touch render/playback endpoints.
- Never load RNNoise outside the capture stage.
- Never load RNNoise at a sample rate other than 48,000 Hz.
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
