// SPDX-License-Identifier: MIT
namespace NoiseCancellingStack;

public enum CaptureProcessor
{
    PassThrough,
    WebRtcApm,
    RnNoise
}

public static class AudioRoutePlanner
{
    public static CaptureProcessor Select(uint sampleRate) => sampleRate switch
    {
        16_000 => CaptureProcessor.WebRtcApm,
        48_000 => CaptureProcessor.RnNoise,
        _ => CaptureProcessor.PassThrough
    };

    public static string Label(CaptureProcessor processor) => processor switch
    {
        CaptureProcessor.WebRtcApm => "WEBRTC",
        CaptureProcessor.RnNoise => "RNNOISE",
        CaptureProcessor.PassThrough => "PASSTHROUGH",
        _ => throw new ArgumentOutOfRangeException(nameof(processor))
    };
}
