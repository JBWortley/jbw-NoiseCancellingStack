// SPDX-License-Identifier: MIT
using System.Security.Cryptography;
using Xunit;

namespace NoiseCancellingStack.Tests;

public sealed class PayloadValidatorTests
{
    [Theory]
    [InlineData(true, false, "RNNoise")]
    [InlineData(false, true, "WebRTC")]
    public void Validate_RejectsMissingPayloadBeforeProbe(bool omitRnNoise, bool omitWebRtc, string expectedName)
    {
        using var fixture = new PayloadFixture();
        if (omitRnNoise) { File.Delete(fixture.RnNoisePath); }
        if (omitWebRtc) { File.Delete(fixture.WebRtcPath); }
        var probe = new FakeProbe();

        var error = Assert.Throws<FileNotFoundException>(() =>
            new PayloadValidator(probe).Validate(
                fixture.RnNoisePath,
                fixture.RnNoiseHash,
                fixture.WebRtcPath,
                fixture.WebRtcHash));

        Assert.Contains(expectedName, error.Message, StringComparison.Ordinal);
        Assert.Equal(0, probe.CallCount);
    }

    [Theory]
    [InlineData(true, false, "RNNoise")]
    [InlineData(false, true, "WebRTC")]
    public void Validate_RejectsWrongHashBeforeProbe(bool corruptRnNoise, bool corruptWebRtc, string expectedName)
    {
        using var fixture = new PayloadFixture();
        if (corruptRnNoise) { File.AppendAllText(fixture.RnNoisePath, "corrupt"); }
        if (corruptWebRtc) { File.AppendAllText(fixture.WebRtcPath, "corrupt"); }
        var probe = new FakeProbe();

        var error = Assert.Throws<InvalidDataException>(() =>
            new PayloadValidator(probe).Validate(
                fixture.RnNoisePath,
                fixture.RnNoiseHash,
                fixture.WebRtcPath,
                fixture.WebRtcHash));

        Assert.Contains(expectedName, error.Message, StringComparison.Ordinal);
        Assert.Equal(0, probe.CallCount);
    }

    [Fact]
    public void Validate_AcceptsBothHashesAndCallsProbeExactlyOnce()
    {
        using var fixture = new PayloadFixture();
        var probe = new FakeProbe();

        new PayloadValidator(probe).Validate(
            fixture.RnNoisePath,
            fixture.RnNoiseHash.ToLowerInvariant(),
            fixture.WebRtcPath,
            fixture.WebRtcHash.ToLowerInvariant());

        Assert.Equal(1, probe.CallCount);
        Assert.Equal(Path.GetFullPath(fixture.WebRtcPath), probe.LastPath);
    }

    [Fact]
    public void Validate_PropagatesProbeFailure()
    {
        using var fixture = new PayloadFixture();
        var expected = new InvalidDataException("self-test returned 7");
        var probe = new FakeProbe { Failure = expected };

        var actual = Assert.Throws<InvalidDataException>(() =>
            new PayloadValidator(probe).Validate(
                fixture.RnNoisePath,
                fixture.RnNoiseHash,
                fixture.WebRtcPath,
                fixture.WebRtcHash));

        Assert.Same(expected, actual);
        Assert.Equal(1, probe.CallCount);
    }

    [Theory]
    [InlineData("export NcsWebRtcApmSelfTest is absent")]
    [InlineData("self-test returned nonzero")]
    public void Validate_RejectsAbsentExportOrNonzeroSelfTest(string failure)
    {
        using var fixture = new PayloadFixture();
        var probe = new FakeProbe { Failure = new InvalidDataException(failure) };

        var error = Assert.Throws<InvalidDataException>(() =>
            new PayloadValidator(probe).Validate(
                fixture.RnNoisePath,
                fixture.RnNoiseHash,
                fixture.WebRtcPath,
                fixture.WebRtcHash));

        Assert.Contains(failure, error.Message, StringComparison.Ordinal);
        Assert.Equal(1, probe.CallCount);
    }

    private sealed class FakeProbe : INativePluginProbe
    {
        public int CallCount { get; private set; }
        public string? LastPath { get; private set; }
        public Exception? Failure { get; init; }

        public void Verify(string pluginPath)
        {
            CallCount++;
            LastPath = pluginPath;
            if (Failure is not null)
            {
                throw Failure;
            }
        }
    }

    private sealed class PayloadFixture : IDisposable
    {
        private readonly string directory = Path.Combine(
            Path.GetTempPath(),
            $"NoiseCancellingStack-payload-tests-{Guid.NewGuid():N}");

        public PayloadFixture()
        {
            Directory.CreateDirectory(directory);
            RnNoisePath = Path.Combine(directory, "rnnoise_mono.dll");
            WebRtcPath = Path.Combine(directory, "webrtc_apm_16k.dll");
            File.WriteAllBytes(RnNoisePath, [1, 2, 3, 4]);
            File.WriteAllBytes(WebRtcPath, [5, 6, 7, 8]);
            RnNoiseHash = Hash(RnNoisePath);
            WebRtcHash = Hash(WebRtcPath);
        }

        public string RnNoisePath { get; }
        public string WebRtcPath { get; }
        public string RnNoiseHash { get; }
        public string WebRtcHash { get; }

        public void Dispose() => Directory.Delete(directory, recursive: true);

        private static string Hash(string path)
        {
            using var stream = File.OpenRead(path);
            return Convert.ToHexString(SHA256.HashData(stream));
        }
    }
}
