# Spike: does Windows give a capture effect a speaker reference?

Throwaway feasibility test, not product code. It answers the first go/no-go question for echo
cancellation: will Windows 11 attach a loopback reference stream to a third-party, unsigned
capture **mode effect (MFX)** that declares itself an echo canceller, and in which processing
modes (DEFAULT vs COMMUNICATIONS)?

## What is here

| Path | Purpose |
| --- | --- |
| `apo/ncs_aec_spike_apo.cpp` | Pass-through MFX. Implements `IApoAcousticEchoCancellation2`, `IApoAuxiliaryInputConfiguration` and `IApoAuxiliaryInputRT`; logs everything Windows hands it. Never changes the audio. |
| `probe/ncs_aec_probe.cpp` | Normal-user tool: lists endpoints, or opens a capture stream in default or Communications category while optionally playing noise bursts on the default speakers. |
| `build.ps1` | Builds both into `out/`. No elevation, no system changes. |
| `install.ps1` | **Elevated.** Attaches the spike to one microphone and restarts Windows Audio. |
| `uninstall.ps1` | **Elevated.** Restores the microphone exactly and restarts Windows Audio. |

## Run

```powershell
.\build.ps1
.\out\ncs-aec-probe.exe list

# elevated PowerShell (one or more capture endpoint GUIDs)
.\install.ps1 -EndpointId '{capture-endpoint-guid}','{another-guid}' [-RecordSeconds 20] [-AdvertiseAec] [-PostVolumeLoopback]

# normal PowerShell
.\out\ncs-aec-probe.exe run '{capture-endpoint-guid}' --seconds 12 --category default --play
.\out\ncs-aec-probe.exe run '{capture-endpoint-guid}' --seconds 12 --category communications --play
Get-Content C:\ProgramData\NcsAecSpike\apo.log

# elevated PowerShell
.\uninstall.ps1
```

Spike options live in `HKLM\SOFTWARE\NcsAecSpike` and are read each time Windows builds the
capture graph, so they can be changed between probe runs without restarting Windows Audio.

## Safety

- Touches only the MFX slot of the listed capture endpoints. Validates all of them first and
  refuses if any slot is already in use.
- Writes a journal and a `reg export` backup before the first change; uninstall restores the
  original values only if they still hold the spike's values.
- The spike passes microphone audio through unchanged. Optional recordings are written only when
  `-RecordSeconds` is given, to `C:\ProgramData\NcsAecSpike\recordings`, and are never sent anywhere.
- While installed, Noise Cancelling Stack repair reports that microphone as externally modified
  (its MFX value differs from the recorded state). Uninstall clears that.

## Findings (2026-11-25, Windows 11 24H2 build 26100)

- **Gate 1 — reference delivery: passed.** The unsigned MFX loaded on all three microphones
  (AT2040USB 48 kHz mono, C920 16 kHz stereo, onboard 48 kHz stereo). Windows attached a 48 kHz
  stereo float pre-volume reference in both DEFAULT and COMMUNICATIONS modes, about 100 buffers
  per second, continuously. Microphone formats were never changed.
- **Threading:** `AcceptInput` runs on one engine loopback thread separate from `APOProcess`, and
  the two overlapped 9 times in one 25 s run. A lock-free ring is required.
- **Gate 2 — slot availability:** the MFX slot was free on every microphone on this machine.
- **Gate 3 — echo removal (offline, `eval/`):** AEC3 removed 16.6–40.0 dB of echo (median ≈ 31 dB)
  from 20 s recordings in which echo was 59–86 % of microphone energy, at 0.09–0.15 ms per 10 ms.
  The production configuration reproduces the same numbers.
- Note: the AddAuxiliaryInput payload is 88 bytes, the same size as both
  `AcousticEchoCanceller_Reference_Input` and `APOInitSystemEffects2`; the spike's decoder read it
  as the former, but the contents matched `APOInitSystemEffects2`. Production code does not parse it.

The production design is in `docs/superpowers/specs/2026-11-25-windows11-echo-cancellation-design.md`.
