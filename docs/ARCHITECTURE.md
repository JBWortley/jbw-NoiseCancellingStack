# Architecture

The distributable has two layers:

1. An Inno Setup bootstrapper installs the self-contained manager and the pinned RNNoise VST2 DLL. If Equalizer APO is absent, it silently runs the bundled, unmodified 1.4.2 x64 installer. A different installed Equalizer APO version causes a fail-closed stop rather than an implicit downgrade.
2. `NoiseCancellingStack.Manager.exe` owns configuration, capture-endpoint registration, repair, status, and rollback.

For each endpoint, the manager selects LFX/GFX, SFX/MFX, or SFX/EFX using the same rules as Equalizer APO 1.4.2: legacy slots are retained only for legacy-only drivers, combined Bluetooth endpoints use SFX/MFX, and other modern endpoints use SFX/EFX. Capture endpoints receive only the pre-mix APO.

The main Equalizer APO config gets a marked include block. The included configuration uses `stage == "capture"` and `sampleRate == 48000`, so speakers and unsupported sample rates are pass-through. Equalizer APO instantiates the mono VST per selected channel, allowing `Channel: all` without relying on a fixed stereo layout.

The endpoint state journal records original and intended-installed values before mutation. Repair is idempotent. Uninstall restores a value only when its current value still equals either the product's installed value or the original value; third-party changes are reported and retained.

