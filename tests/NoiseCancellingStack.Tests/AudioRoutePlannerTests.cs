// SPDX-License-Identifier: MIT
using Xunit;

namespace NoiseCancellingStack.Tests;

public sealed class AudioRoutePlannerTests
{
    [Theory]
    [InlineData(16_000u, CaptureProcessor.WebRtcApm)]
    [InlineData(48_000u, CaptureProcessor.RnNoise)]
    [InlineData(0u, CaptureProcessor.PassThrough)]
    [InlineData(8_000u, CaptureProcessor.PassThrough)]
    [InlineData(44_100u, CaptureProcessor.PassThrough)]
    [InlineData(96_000u, CaptureProcessor.PassThrough)]
    public void Select_MapsOnlySupportedRates(uint sampleRate, CaptureProcessor expected)
    {
        Assert.Equal(expected, AudioRoutePlanner.Select(sampleRate));
    }

    [Theory]
    [InlineData(CaptureProcessor.WebRtcApm, "WEBRTC")]
    [InlineData(CaptureProcessor.RnNoise, "RNNOISE")]
    [InlineData(CaptureProcessor.PassThrough, "PASSTHROUGH")]
    public void Label_ReturnsStableStatusText(CaptureProcessor processor, string expected)
    {
        Assert.Equal(expected, AudioRoutePlanner.Label(processor));
    }
}
