# Safety invariants

- Never touch render/playback endpoints.
- Never load any processor outside the capture stage. The echo canceller receives the speaker reference only from Windows' AEC framework; it never opens render endpoints or loopback streams itself.
- Route exactly 48 kHz exclusively to RNNoise and exactly 16 kHz exclusively to WebRTC APM.
- Keep every unsupported sample rate on unchanged pass-through.
- Keep the WebRTC processor's fixed 10 ms delay and permanent-instance delayed-dry failure path.
- Keep the echo canceller's fixed AEC3 pipeline delay (reported through GetLatency) and its delayed-dry failure path; an unsupported format or mode passes audio through unchanged.
- Never take OS locks, wait, or allocate in either real-time callback: the loopback callback only writes a lock-free ring, and only the capture callback calls WebRTC (enforced by the pinned single-thread capture and render patches and by tests).
- Attach the echo canceller only on Windows 11, only after its COM and APO registration is verified at the installed path, and only to an unused MFX slot (no MFX value, MFX mode list, or composite MFX). Never displace a driver mode effect, and skip headset, handset, and combined hands-free endpoints. If repair finds the registration gone, it detaches the echo canceller (journaled) rather than leave an MFX slot naming a class Windows cannot create.
- On a microphone where Equalizer APO was attached by an earlier installation, add the echo canceller only when Equalizer APO occupies the SFX slot and no other slot, and journal and restore only the MFX value and MFX mode list. Never modify that endpoint's Equalizer APO values or child record.
- In an endpoint's Properties key, change only its device description (to append ` FILTERED`) and RawProcessingSupported (to disable raw capture), only while noise suppression applies to it, and only after journaling both values. Restore each only while it still holds our value; a value changed by the user or a driver is released, never overwritten.
- The background watcher only re-disables raw capture that the journal records as disabled and that Windows has reset to the driver's original value. It writes nothing else, never takes the operation lock, and is stopped before uninstall restores anything.
- The only machine state a standard user can change is `HKLM\SOFTWARE\NoiseCancellingStack\UserControl\FilteringEnabled`, through a key whose explicit, non-inherited ACL grants Users query, set-value, and notify rights only. The watcher reads it as a DWORD (anything but 0 means on), and acting on it only switches between the two managed include variants, the echo canceller's pass-through, and the journaled name and raw-capture values. It never touches a hand-edited include file or a value someone else changed, and it spaces audio restarts at least 3 s apart.
- The tray icon never runs elevated and never changes filtering itself.
- Register the echo canceller for the default processing mode only, so no new processing mode becomes available beside Equalizer APO's default-mode stream effect.
- Journal the MFX value and MFX mode list before writing them; restore both conflict-aware during uninstall.
- Never mutate configuration or endpoint state before all three payload hashes and both native synthetic self-tests (WebRTC APM and echo canceller) pass.
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

Application-specific processing is outside this stack's control. Enabling suppression in both this system-wide stack and an application can cause double suppression; users should disable one layer if speech quality degrades. Shared-mode applications that disable system effects, WASAPI exclusive mode, and ASIO bypass the APO path. Raw-mode capture is closed on filtered microphones by no longer advertising it; between Windows rebuilding a microphone (replug, boot) and the watcher reacting, an application that opens it can still obtain a raw stream.
