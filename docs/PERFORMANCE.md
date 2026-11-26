# Performance

Measured 2026-11-26 on an installed Noise Cancelling Stack 1.2.0, plus a comparison with what NVIDIA publishes for NVIDIA Broadcast.

## Summary

| | Noise Cancelling Stack 1.2.0 (measured) | NVIDIA Broadcast (published or reported) |
| --- | --- | --- |
| Hardware needed | Any x64 CPU; no GPU | NVIDIA RTX 2060 / Quadro RTX 3000 or higher, 8 GB RAM [1] |
| Runs on the test laptop (AMD Radeon 890M) | Yes | No: no RTX GPU |
| CPU, all three microphones filtered | 28% average, 48% peak of one hardware thread (1.2% / 2.0% of the 24-thread CPU) | Not published. RTX Voice was reported at about 2–3% CPU on a desktop in 2020 [4] |
| GPU | None | "Performance impact … within 5% and 10% on a GeForce RTX 5090" [1]; 0.37 ms of GPU time per 10 ms audio frame for the SDK denoiser on an RTX laptop GPU [3] |
| Memory | +6 MB in the audio engine, plus 18 MB watcher and 2 MB tray (private) | 215 MiB of GPU memory at p95 for the SDK denoiser [3]; NVIDIA cut Broadcast's VRAM use by over 40% in version 1.3 [2] |
| Extra power, three microphones | Too small to measure: under 0.4 W | Not published; estimated 4–11 W per microphone on a laptop (see [Performance per watt](#performance-per-watt-estimate)) |
| Added microphone latency | 8–9 ms for echo cancellation, plus 10 ms for the 16 kHz noise suppressor | Not published |
| Network | None, ever | Required during installation, to download AI models [1] |

The two products do different things:
- **Noise Cancelling Stack** filters the real microphones for every application. It adds echo cancellation (removing what the speakers play) and noise suppression.
- **NVIDIA Broadcast** provides a virtual microphone that applications must select. It runs larger neural models on the GPU, and it offers features this stack does not: camera effects, and "Room Echo Removal", which removes room reverberation rather than speaker echo [1].

This document compares resource cost only, not sound quality.

## Test system

| | |
| --- | --- |
| CPU | AMD Ryzen AI 9 HX 370 (12 cores / 24 threads), integrated Radeon 890M |
| RAM | 48 GB |
| OS | Windows 11 Pro 24H2, build 26100 |
| Microphones | AT2040USB (USB, 48 kHz mono), onboard array (48 kHz stereo), Logitech C920 (16 kHz stereo) |
| Filters | Each microphone has echo cancellation (WebRTC AEC3). Noise suppression is RNNoise at 48 kHz and WebRTC at 16 kHz |
| Background load | The user's normal desktop applications, not controlled; this is why power is compared in alternating cycles |

"% of one thread" is Windows' per-process `% Processor Time`, where 100% means one hardware thread fully busy.

## Audio processing

The filters run inside Windows' audio engine (`audiodg.exe`), and only while an application has a filtered microphone open. All three microphones were opened at once, with nothing played and nothing recorded, and sampled every second for 30 s.

| Audio engine | Baseline (one microphone open) | All three microphones open |
| --- | --- | --- |
| CPU, average | 9.4% of one thread | 27.9% of one thread |
| CPU, peak | 20.0% | **47.8%** (2.0% of the whole CPU) |
| Private memory | 12.0 MB | 18.3 MB |
| Threads | 9 | 21 |
| Audio dropouts | — | 0 |

A second run that started from an idle audio engine (1.0% CPU) rose to 32.8% with all three microphones open. Together the two runs put one filtered microphone at about **6–11% of one thread**, covering echo cancellation, noise suppression and Windows' own capture work. The WebRTC echo canceller alone takes 0.09–0.15 ms of CPU per 10 ms of audio, about 1–1.5% of one thread.

## Power

The laptop exposes Windows' Energy Meter counters, which report APU (whole-chip) and CPU-rail power. Three cycles alternated 28 s with the microphones closed and 28 s with all three open:

| Cycle | APU, microphones closed | APU, microphones open | Difference | Audio engine CPU (closed → open) |
| --- | --- | --- | --- | --- |
| 1 | 20.23 W | 20.62 W | +0.40 W | 0.9% → 31.8% |
| 2 | 20.75 W | 21.03 W | +0.28 W | 1.2% → 34.6% |
| 3 | 21.08 W | 20.60 W | −0.47 W | 1.0% → 32.0% |

The mean difference is **+0.07 W**, with a standard deviation of 0.47 W. The CPU rail moved by +0.10 W. Background activity changes the chip's power by about ±0.5 W, so filtering three microphones costs less than that noise: **at most about 0.4 W for all three**, roughly 0.15 W per microphone. That agrees with the CPU time: about a third of one thread at low clocks.

## Background helpers

| Component | When it runs | CPU | Memory |
| --- | --- | --- | --- |
| Watcher (SYSTEM task; keeps raw capture off and applies the tray switch) | Always, from boot | 0% while idle; it wakes for milliseconds when Windows changes a microphone setting, and checks once a minute | 17.7 MB private, 35.8 MB peak working set, 5–8 threads, about 270 handles |
| Tray icon | While you are signed in | 0.9 s of CPU in 18 minutes (0.08% of one thread); it reads two registry values every 1.5 s | 2.2 MB private, 17 MB working set, 1 thread |
| Startup repair | Once, a minute after boot | 0.6–5.2 s per run; native self-tests take 37 ms and 16 ms | — |

## Disk, network and latency

- **Disk:**
  - Installed size: 27.7 MB. Installer: 24.9 MB.
  - Logs: about 2 KB per operation.
  - Safety backups: 0.25 MB at each boot, install and repair. These are never deleted automatically, which comes to about 90 MB a year with a daily boot.
- **Network:** none. The installer is fully offline.
- **Latency:**
  - Echo cancellation adds a fixed 8.0 ms at 16 kHz, 8.1 ms at 32 kHz and 9.0 ms at 48 kHz. It reports this to Windows, so audio and video stay in sync.
  - The 16 kHz WebRTC noise suppressor adds a fixed 10 ms.
  - RNNoise works in 10 ms frames; its plug-in latency was not measured separately.

## Echo removal

These are live measurements on the installed product: speech and music played on a TV speaker, with the processed microphone recorded.

| Measure (C920) | Result |
| --- | --- |
| Echo matching the speaker signal (linear echo) | 25.5 dB less |
| Everything the microphone picked up, full band / speech band | 18 dB / 12 dB less |
| Your voice while the TV was playing (double talk) | Within 0.6 dB of a microphone without echo cancellation; it was not cut or ducked |

On 20 s recordings from three microphones, the same echo canceller removed 17–40 dB of echo offline (median 31 dB).

## Performance per watt (estimate)

There is no NVIDIA GPU in the test laptop, so this part is an estimate, not a measurement.

Noise Cancelling Stack, measured: three microphones cost at most about 0.4 W, or **about 7 or more microphones filtered per watt**.

NVIDIA Broadcast on a laptop, estimated from these assumptions:

1. **Compute.** NVIDIA's SDK benchmark gives the denoiser 0.374 ms of GPU time per 10 ms frame on an RTX 3500 Ada laptop GPU [3]. That GPU is busy about 3.7% of the time. Assuming 30–60 W while it is busy, that averages roughly **1–2 W**.
2. **Keeping the discrete GPU awake.** A laptop normally powers its discrete GPU down when it is idle. An effect that needs the GPU every 10 ms keeps it in an active power state. We assume **3–8 W** for that. This is the largest and least certain term.
3. **The application's own CPU work** (virtual microphone and audio routing): assumed **0.2–0.5 W**. RTX Voice was reported at about 2–3% CPU [4].

That totals roughly **4–11 W per microphone**, or about **0.1–0.25 microphones per watt**.

| | Microphones per watt | Compared with Noise Cancelling Stack |
| --- | --- | --- |
| Noise Cancelling Stack (measured upper bound) | ≥ 7 | — |
| NVIDIA Broadcast, discrete GPU otherwise asleep (estimate) | 0.1–0.25 | about 30–70× less |
| NVIDIA Broadcast, GPU already awake, for example while gaming (estimate: compute plus app, 1–3 W) | 0.3–1 | about 7–20× less, and NVIDIA reports a 5–10% performance cost [1] |

Treat this as order-of-magnitude only. Measuring both products on the same RTX laptop with the same microphone would settle it. NVIDIA's larger models may also handle heavy noise better, so the comparison says nothing about quality.

## Reproduce

```powershell
cd spikes\aec-reference
.\build.ps1                                   # builds the probe; no system changes
.\out\ncs-aec-probe.exe list                  # capture endpoint GUIDs
.\perf\measure-load.ps1 -EndpointId '{guid-1}','{guid-2}','{guid-3}' -Cycles 3
```

The script alternates idle and open periods, and reports the audio engine's CPU, memory and threads, plus APU and CPU power where Windows provides Energy Meter counters. It plays nothing and records nothing. The echo figures come from `ncs-aec-probe.exe run … --play-wav … --wav … --loopback-wav …`, analysed with `eval/live_echo.py`.

## Sources

1. NVIDIA, *NVIDIA Broadcast App FAQ*: system requirements, internet during installation, and "we've measured the performance impact of the effects to be within 5% and 10% on a GeForce RTX 5090". https://www.nvidia.com/en-us/geforce/broadcasting/broadcast-app/faq/
2. NVIDIA, *NVIDIA Broadcast App 1.3 update* (September 2021): "reduces VRAM usage by over 40%". https://www.nvidia.com/en-gb/geforce/news/sept-2021-nvidia-broadcast-update/
3. NVIDIA, *Maxine Audio Effects SDK 3.0.0, Windows performance and deployment guide*: Denoiser v2 on an RTX 3500 Ada Generation Laptop GPU at 16 kHz, with `NvAFX_Run` p50 0.374 ms and p95 0.410 ms per 10 ms frame, and p95 GPU memory 215 MiB. That benchmark processes files at about 27× real time, so its utilisation figures (p50/p95 GPU 11%/68%) are not real-time load. https://docs.nvidia.com/maxine/afx/3.0.0/WindowsAFXSDK/ReleaseReadiness.html
4. PC Gamer, *How to enable RTX Voice on any Nvidia graphics card* (2020, RTX Voice, the predecessor of Broadcast's noise removal): reported about 2–3% CPU and 1.1–1.5% GPU on a desktop. https://www.pcgamer.com/nvidia-rtx-voice-performance/
