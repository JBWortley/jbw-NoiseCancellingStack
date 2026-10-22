# Architecture

The distributable has two layers:

1. An Inno Setup bootstrapper installs the self-contained manager, the pinned RNNoise VST2 DLL, and the project-built statically linked WebRTC APM VST2 DLL. If Equalizer APO is absent, it silently runs the bundled, unmodified 1.4.2 x64 installer. A different installed Equalizer APO version causes a fail-closed stop rather than an implicit downgrade.
2. `NoiseCancellingStack.Manager.exe` owns configuration, capture-endpoint registration, repair, status, and rollback.

For each endpoint, the manager selects LFX/GFX, SFX/MFX, or SFX/EFX using the same rules as Equalizer APO 1.4.2: legacy slots are retained only for legacy-only drivers, combined Bluetooth endpoints use SFX/MFX, and other modern endpoints use SFX/EFX. Capture endpoints receive only the pre-mix APO.

The main Equalizer APO config gets a marked include block. Inside `stage == "capture"`, exactly 48 kHz routes exclusively to RNNoise and exactly 16 kHz routes exclusively to WebRTC APM; every unsupported rate is pass-through. Equalizer APO instantiates the mono VST per selected channel, allowing `Channel: all` without relying on a fixed stereo layout.

The WebRTC plug-in adapts arbitrary callback sizes to mono 160-sample frames and advertises a fixed 160-sample (10 ms) delay. WebRTC is configured for high noise suppression only: echo cancellation, both gain controllers, high-pass filtering, and transient suppression remain disabled. Any processing failure permanently switches that instance to correctly delayed dry audio.

Before install or repair mutates configuration or endpoint state, the manager verifies both native SHA-256 digests and invokes the WebRTC DLL's synthetic self-test export. The DLL statically links WebRTC, Abseil, and the Microsoft runtime; its only dynamic dependencies are Windows system libraries. The existing hidden SYSTEM task performs the same fail-closed, idempotent repair one minute after boot.

The endpoint state journal records original and intended-installed values before mutation. Repair is idempotent. Uninstall restores a value only when its current value still equals either the product's installed value or the original value; third-party changes are reported and retained.
