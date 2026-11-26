# SPDX-License-Identifier: MIT
#
# THROWAWAY FEASIBILITY SPIKE - NOT PRODUCT CODE.
#
# Measures acoustic echo in a live recording made by
#   ncs-aec-probe run <mic> --play-wav speech.wav --wav <dir>/mic.wav --loopback-wav <dir>/loop.wav
# The processed microphone (after the whole capture chain) is compared with what the default render
# endpoint played. With the user silent, microphone power while the far end talks, minus the noise
# floor while it is quiet, is the echo that reached the application.
#
#   python live_echo.py <run-dir> [--delay-ms N]
#   python live_echo.py --compare <baseline-run-dir> <aec-run-dir>
import argparse
import json
import pathlib
import struct
import sys

import numpy as np

FRAME_S = 0.02


def read_wav(path):
    data = pathlib.Path(path).read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a WAV file")
    offset, fmt, pcm = 12, None, None
    while offset + 8 <= len(data):
        chunk = data[offset:offset + 4]
        size = struct.unpack("<I", data[offset + 4:offset + 8])[0]
        body = data[offset + 8:offset + 8 + size]
        if chunk == b"fmt ":
            tag, channels, rate = struct.unpack("<HHI", body[:8])
            bits = struct.unpack("<H", body[14:16])[0]
            fmt = (tag, channels, rate, bits)
        elif chunk == b"data":
            pcm = body
        offset += 8 + size + (size & 1)
    tag, channels, rate, bits = fmt
    if tag == 3 and bits == 32:
        samples = np.frombuffer(pcm, dtype="<f4").astype(np.float64)
    elif tag == 1 and bits == 16:
        samples = np.frombuffer(pcm, dtype="<i2").astype(np.float64) / 32768.0
    else:
        raise ValueError(f"{path}: unsupported format tag {tag}/{bits} bits")
    samples = samples[: len(samples) // channels * channels].reshape(-1, channels)
    return samples, rate


def to_rate(signal, rate, target):
    if rate == target:
        return signal
    if rate % target == 0:
        factor = rate // target
        taps = np.arange(-64 * factor, 64 * factor + 1)
        cutoff = 0.45 / factor
        kernel = 2 * cutoff * np.sinc(2 * cutoff * taps) * np.hanning(len(taps))
        return np.convolve(signal, kernel / kernel.sum(), mode="same")[::factor]
    times = np.arange(int(len(signal) * target / rate)) * rate / target
    return np.interp(times, np.arange(len(signal)), signal)


def frame_power(signal, rate):
    size = int(rate * FRAME_S)
    count = len(signal) // size
    return (signal[: count * size].reshape(count, size) ** 2).mean(axis=1)


def db(power):
    return 10 * np.log10(np.maximum(power, 1e-20))


def load_run(run_dir):
    run = pathlib.Path(run_dir)
    meta = json.loads((run / "mic.wav.json").read_text())
    mic, mic_rate = read_wav(run / "mic.wav")
    loop, loop_rate = read_wav(run / "loop.wav")
    mic_mono = mic.mean(axis=1)
    ref = to_rate(loop.mean(axis=1), loop_rate, mic_rate)
    # Place the loopback on the microphone's timeline using both streams' QPC start times.
    shift = int(round((meta["loopback_first_qpc_hns"] - meta["mic_first_qpc_hns"]) * mic_rate / 1e7))
    aligned = np.zeros_like(mic_mono)
    source_start, target_start = max(0, -shift), max(0, shift)
    count = max(0, min(len(ref) - source_start, len(aligned) - target_start))
    aligned[target_start:target_start + count] = ref[source_start:source_start + count]
    return meta, mic, mic_rate, mic_mono, aligned


def estimate_delay_frames(mic_power, ref_power, max_frames):
    # Correlate log envelopes; returns (lag, correlation) where lag is the acoustic + device delay.
    a = db(mic_power)
    b = db(ref_power)
    a = a - a.mean()
    b = b - b.mean()
    best, best_corr = 0, -1.0
    for lag in range(0, max_frames):
        x, y = a[lag:], b[: len(b) - lag]
        corr = float(np.dot(x, y) / (np.linalg.norm(x) * np.linalg.norm(y) + 1e-12))
        if corr > best_corr:
            best, best_corr = lag, corr
    return best, best_corr


def coherent_echo(mic_mono, ref, rate, lag_samples):
    # Linear echo estimate that needs no quiet periods (music may play throughout): Welch cross-
    # spectra between the microphone and the delay-compensated reference; per bin, |Sxy|^2/Sxx is
    # the microphone power linearly explained by the reference. The 1/K coherence bias of K
    # segments is subtracted, so the estimate bottoms out near (mic power)/K.
    nfft = 8192 if rate >= 32000 else 4096
    hop = nfft // 2
    shifted = np.concatenate([np.zeros(lag_samples), ref[: len(ref) - lag_samples]])
    settle = int(3.0 * rate)
    x, y = shifted[settle:], mic_mono[settle:]
    window = np.hanning(nfft)
    sxx = syy = 0.0
    sxy = 0.0
    segments = 0
    for start in range(0, len(y) - nfft, hop):
        xf = np.fft.rfft(x[start:start + nfft] * window)
        yf = np.fft.rfft(y[start:start + nfft] * window)
        sxx = sxx + np.abs(xf) ** 2
        syy = syy + np.abs(yf) ** 2
        sxy = sxy + yf * np.conj(xf)
        segments += 1
    explained = np.abs(sxy) ** 2 / np.maximum(sxx, 1e-30)
    explained = np.maximum(explained - syy / segments, 0.0)
    scale = 1.0 / (segments * nfft * (window ** 2).mean() * nfft / 2)
    freqs = np.fft.rfftfreq(nfft, 1 / rate)
    band = (freqs >= 100) & (freqs <= min(8000, rate / 2 * 0.9))
    mic_power = syy[band].sum() * scale
    echo_power = explained[band].sum() * scale
    ref_power = sxx[band].sum() * scale
    floor_power = (syy[band] / segments).sum() * scale
    return mic_power, echo_power, ref_power, floor_power, segments


def analyze(run_dir, delay_ms=None):
    meta, mic, rate, mic_mono, ref = load_run(run_dir)
    mic_power = frame_power(mic_mono, rate)
    ref_power = frame_power(ref, rate)
    count = min(len(mic_power), len(ref_power))
    mic_power, ref_power = mic_power[:count], ref_power[:count]

    lag, corr = estimate_delay_frames(mic_power, ref_power, int(0.6 / FRAME_S))
    if delay_ms is not None:
        lag = int(round(delay_ms / 1000 / FRAME_S))
    delayed = np.concatenate([np.zeros(lag), ref_power[: count - lag]])
    peak = db(np.percentile(delayed, 99))
    active = db(delayed) > peak - 35
    # Quiet: no far-end energy from 700 ms before to 100 ms after, so reverberation has decayed.
    loud = db(delayed) > peak - 60
    window_back, window_ahead = int(0.7 / FRAME_S), int(0.1 / FRAME_S)
    recent = np.array([loud[max(0, k - window_back): k + window_ahead + 1].any() for k in range(count)])
    quiet = ~recent
    settle = int(3.0 / FRAME_S)  # ignore the first 3 s (initial echo canceller convergence)
    steady = np.arange(count) >= settle

    lin_mic, lin_echo, lin_ref, lin_floor, segments = coherent_echo(mic_mono, ref, rate, int(lag * FRAME_S * rate))
    coherent = {
        "band_hz": "100-8000",
        "mic_dbfs": round(float(db(lin_mic)), 1),
        "reference_dbfs": round(float(db(lin_ref)), 1),
        "linear_echo_dbfs": round(float(db(lin_echo)), 1),
        "echo_fraction_of_mic_pct": round(100 * lin_echo / lin_mic, 1),
        "echo_path_gain_db": round(float(db(lin_echo) - db(lin_ref)), 1),
        "measurement_floor_dbfs": round(float(db(lin_floor)), 1),
        "segments": segments,
    }
    if not (quiet & steady).any():
        result = {"run": str(run_dir), "category": meta["category"], "mic_rate": rate,
                  "mic_channels": int(mic.shape[1]), "seconds": round(count * FRAME_S, 1),
                  "delay_ms": lag * FRAME_S * 1000, "delay_envelope_corr": round(corr, 3),
                  "coherent": coherent, "note": "no quiet periods (continuous far-end audio)"}
        (pathlib.Path(run_dir) / "metrics.json").write_text(json.dumps(result, indent=2))
        return result
    floor = mic_power[quiet & steady].mean()
    talking = mic_power[active & steady].mean()
    echo = max(talking - floor, 1e-20)
    first = mic_power[active & ~steady].mean() if (active & ~steady).any() else float("nan")

    # Echo per 5 s of program time, to show convergence and stability.
    blocks = []
    block = int(5.0 / FRAME_S)
    for start in range(0, count, block):
        sel = slice(start, start + block)
        if active[sel].sum() >= 25:
            blocks.append(round(float(db(max(mic_power[sel][active[sel]].mean() - floor, 1e-20))), 1))
        else:
            blocks.append(None)

    result = {
        "run": str(run_dir),
        "category": meta["category"],
        "mic_rate": rate,
        "mic_channels": int(mic.shape[1]),
        "seconds": round(count * FRAME_S, 1),
        "stream_latency_ms": meta["stream_latency_hns"] / 1e4,
        "delay_ms": lag * FRAME_S * 1000,
        "delay_envelope_corr": round(corr, 3),
        "far_end_level_dbfs": round(float(db(ref_power[active].mean() if active.any() else 0)), 1),
        "mic_while_far_end_talks_dbfs": round(float(db(talking)), 1),
        "mic_floor_dbfs": round(float(db(floor)), 1),
        "echo_dbfs": round(float(db(echo)), 1),
        "echo_above_floor_db": round(float(db(talking) - db(floor)), 1),
        "first_3s_mic_while_talking_dbfs": round(float(db(first)), 1) if first == first else None,
        "echo_per_5s_dbfs": blocks,
        "active_frames": int((active & steady).sum()),
        "quiet_frames": int((quiet & steady).sum()),
        "mic_peak_dbfs": round(float(db(np.max(mic ** 2))), 1),
        "coherent": coherent,
    }
    (pathlib.Path(run_dir) / "metrics.json").write_text(json.dumps(result, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("runs", nargs="+")
    parser.add_argument("--delay-ms", type=float)
    parser.add_argument("--compare", action="store_true")
    args = parser.parse_args()
    if args.compare:
        if len(args.runs) != 2:
            sys.exit("--compare needs <baseline-run> <aec-run>")
        base = analyze(args.runs[0], args.delay_ms)
        aec = analyze(args.runs[1], args.delay_ms if args.delay_ms is not None else base["delay_ms"])
        for item in (base, aec):
            print(json.dumps(item, indent=2))
        b, a = base["coherent"], aec["coherent"]
        # Normalizing by reference power cancels differences in what happened to be playing.
        print(f"linear echo path gain: {b['echo_path_gain_db']} dB -> {a['echo_path_gain_db']} dB, "
              f"reduction {b['echo_path_gain_db'] - a['echo_path_gain_db']:.1f} dB "
              f"(echo {b['echo_fraction_of_mic_pct']}% -> {a['echo_fraction_of_mic_pct']}% of mic power; "
              f"AEC-run measurement floor {a['measurement_floor_dbfs']} dBFS vs echo {a['linear_echo_dbfs']} dBFS)")
        if "echo_dbfs" in base and "echo_dbfs" in aec:
            print(f"quiet-gap echo reduction: {base['echo_dbfs'] - aec['echo_dbfs']:.1f} dB")
        return
    for run in args.runs:
        print(json.dumps(analyze(run, args.delay_ms), indent=2))


if __name__ == "__main__":
    main()
