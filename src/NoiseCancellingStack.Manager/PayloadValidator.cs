// SPDX-License-Identifier: MIT
using System.Security.Cryptography;

namespace NoiseCancellingStack;

public interface INativePluginProbe
{
    void Verify(string pluginPath);
}

public sealed class PayloadValidator
{
    private readonly INativePluginProbe nativePluginProbe;
    private readonly INativePluginProbe? echoCancellerProbe;

    public PayloadValidator(INativePluginProbe nativePluginProbe, INativePluginProbe? echoCancellerProbe = null)
    {
        this.nativePluginProbe = nativePluginProbe ?? throw new ArgumentNullException(nameof(nativePluginProbe));
        this.echoCancellerProbe = echoCancellerProbe;
    }

    // The echo canceller is validated on every Windows version, even where it is never attached:
    // a corrupt or substituted payload is a release-integrity failure, not a platform limitation.
    public void ValidateEchoCanceller(string echoCancellerPath, string expectedSha256)
    {
        if (echoCancellerProbe is null)
        {
            throw new InvalidOperationException("No echo canceller self-test probe was configured.");
        }
        var fullPath = ValidateFile("echo cancellation APO", echoCancellerPath, expectedSha256);
        echoCancellerProbe.Verify(fullPath);
    }

    public void Validate(
        string rnNoisePath,
        string expectedRnNoiseSha256,
        string webRtcPath,
        string expectedWebRtcSha256)
    {
        ValidateFile("RNNoise", rnNoisePath, expectedRnNoiseSha256);
        var fullWebRtcPath = ValidateFile("WebRTC APM", webRtcPath, expectedWebRtcSha256);
        nativePluginProbe.Verify(fullWebRtcPath);
    }

    private static string ValidateFile(string payloadName, string path, string expectedSha256)
    {
        var fullPath = Path.GetFullPath(path);
        if (!File.Exists(fullPath))
        {
            throw new FileNotFoundException($"The {payloadName} payload is missing.", fullPath);
        }

        using var stream = File.OpenRead(fullPath);
        var actualSha256 = Convert.ToHexString(SHA256.HashData(stream));
        if (!string.Equals(actualSha256, expectedSha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                $"{payloadName} payload checksum mismatch: expected {expectedSha256}, got {actualSha256}.");
        }
        return fullPath;
    }
}
