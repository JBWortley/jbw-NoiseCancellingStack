// SPDX-License-Identifier: MIT
using Xunit;

namespace NoiseCancellingStack.Tests;

public sealed class ConfigManagerTests
{
    private const string RnNoisePlugin = @"C:\Program Files\NoiseCancellingStack\plugins\rnnoise_mono.dll";
    private const string WebRtcPlugin = @"C:\Program Files\NoiseCancellingStack\plugins\webrtc_apm_16k.dll";

    [Fact]
    public void BuildManagedConfig_RoutesCaptureRatesToExclusiveProcessors()
    {
        var config = ConfigManager.BuildManagedConfig(RnNoisePlugin, WebRtcPlugin)
            .Replace("\r\n", "\n", StringComparison.Ordinal);

        const string expectedRouting = """
            If: stage == "capture"
                If: sampleRate == 48000
                    Channel: all
                    VSTPlugin: Library "C:\Program Files\NoiseCancellingStack\plugins\rnnoise_mono.dll"
                EndIf:
                If: sampleRate == 16000
                    Channel: all
                    VSTPlugin: Library "C:\Program Files\NoiseCancellingStack\plugins\webrtc_apm_16k.dll"
                EndIf:
            EndIf:
            """;

        Assert.Contains(expectedRouting.Replace("\r\n", "\n", StringComparison.Ordinal), config, StringComparison.Ordinal);
        Assert.Equal(1, Count(config, RnNoisePlugin));
        Assert.Equal(1, Count(config, WebRtcPlugin));
        Assert.DoesNotContain("sampleRate == 8000", config, StringComparison.Ordinal);
        Assert.DoesNotContain("sampleRate == 44100", config, StringComparison.Ordinal);
        Assert.DoesNotContain("post-mix", config, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void BuildManagedConfig_RejectsInjectionInEitherPath()
    {
        Assert.Throws<ArgumentException>(() => ConfigManager.BuildManagedConfig("rnnoise.dll\r\nPreamp: 20 dB", WebRtcPlugin));
        Assert.Throws<ArgumentException>(() => ConfigManager.BuildManagedConfig(RnNoisePlugin, "webrtc.dll\nInclude: hostile.txt"));
    }

    [Fact]
    public void StripManagedBlock_PreservesUserConfiguration()
    {
        var input = string.Join("\n",
        [
            "Preamp: -3 dB",
            string.Empty,
            ConfigManager.BeginMarker,
            $"Include: {ConfigManager.ManagedFileName}",
            ConfigManager.EndMarker,
            string.Empty,
            "Filter: ON HP Fc 80 Hz",
            string.Empty
        ]);

        var result = ConfigManager.StripManagedBlock(input).Replace("\r\n", "\n", StringComparison.Ordinal);

        Assert.Equal("Preamp: -3 dB\n\nFilter: ON HP Fc 80 Hz\n", result);
    }

    [Fact]
    public void StripManagedBlock_IsIdempotent()
    {
        const string input = "Preamp: -3 dB\n";
        Assert.Equal(ConfigManager.StripManagedBlock(input), ConfigManager.StripManagedBlock(ConfigManager.StripManagedBlock(input)));
    }

    [Fact]
    public void IsKnownLegacyOnlyConfig_RecognizesMigratedPrototype()
    {
        const string legacy = """
            # Real-time noise suppression for every capture endpoint hosting Equalizer APO.
            # The stage check prevents RNNoise from ever affecting playback devices.
            Device: all
            If: stage == "capture"
            If: sampleRate == 48000
            Channel: all
            VSTPlugin: Library C:\audio\win-rnnoise\vst\rnnoise_mono.dll
            EndIf:
            EndIf:
            """;

        Assert.True(ConfigManager.IsKnownLegacyOnlyConfig(legacy));
    }

    [Fact]
    public void IsManagedInstallation_RequiresExactDualEngineFiles()
    {
        var directory = Path.Combine(Path.GetTempPath(), $"NoiseCancellingStack-tests-{Guid.NewGuid():N}");
        Directory.CreateDirectory(directory);
        try
        {
            File.WriteAllText(
                Path.Combine(directory, "config.txt"),
                $"{ConfigManager.BeginMarker}\nInclude: {ConfigManager.ManagedFileName}\n{ConfigManager.EndMarker}\n");
            File.WriteAllText(
                Path.Combine(directory, ConfigManager.ManagedFileName),
                ConfigManager.BuildManagedConfig(RnNoisePlugin, WebRtcPlugin));

            Assert.True(ConfigManager.IsManagedInstallation(directory, RnNoisePlugin, WebRtcPlugin));

            File.AppendAllText(Path.Combine(directory, ConfigManager.ManagedFileName), "# changed");
            Assert.False(ConfigManager.IsManagedInstallation(directory, RnNoisePlugin, WebRtcPlugin));
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public void IsTrustedManagedInstallation_AcceptsExactVersion101Config()
    {
        var directory = CreateManagedDirectory(BuildVersion101Config(RnNoisePlugin));
        try
        {
            Assert.True(ConfigManager.IsTrustedManagedInstallation(directory, RnNoisePlugin, WebRtcPlugin));
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    [Theory]
    [InlineData("comment")]
    [InlineData("rate")]
    [InlineData("path")]
    public void IsTrustedManagedInstallation_RejectsLegacyNearMatches(string mutation)
    {
        var legacy = BuildVersion101Config(RnNoisePlugin);
        legacy = mutation switch
        {
            "comment" => legacy.Replace("# RNNoise v1.10", "# RNNoise release 1.10", StringComparison.Ordinal),
            "rate" => legacy.Replace("sampleRate == 48000", "sampleRate == 16000", StringComparison.Ordinal),
            "path" => legacy.Replace("rnnoise_mono.dll", "other.dll", StringComparison.Ordinal),
            _ => throw new ArgumentOutOfRangeException(nameof(mutation))
        };
        var directory = CreateManagedDirectory(legacy);
        try
        {
            Assert.False(ConfigManager.IsTrustedManagedInstallation(directory, RnNoisePlugin, WebRtcPlugin));
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public void Install_IsIdempotentForDualEngineConfig()
    {
        var directory = Path.Combine(Path.GetTempPath(), $"NoiseCancellingStack-tests-{Guid.NewGuid():N}");
        var backupDirectory = Path.Combine(directory, "backups");
        Directory.CreateDirectory(directory);
        try
        {
            var logger = new TestLogger();

            Assert.True(ConfigManager.Install(directory, RnNoisePlugin, WebRtcPlugin, backupDirectory, logger));
            Assert.False(ConfigManager.Install(directory, RnNoisePlugin, WebRtcPlugin, backupDirectory, logger));
            Assert.True(ConfigManager.IsManagedInstallation(directory, RnNoisePlugin, WebRtcPlugin));
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    private static string BuildVersion101Config(string pluginPath)
    {
        var encodedPath = pluginPath.Contains(' ', StringComparison.Ordinal) ? $"\"{pluginPath}\"" : pluginPath;
        return string.Join(Environment.NewLine,
        [
            "# Managed by Noise Cancelling Stack. Changes may be replaced during repair.",
            "# RNNoise v1.10 requires exactly 48000 Hz; unsupported streams pass through unchanged.",
            "Device: all",
            "If: stage == \"capture\"",
            "    If: sampleRate == 48000",
            "        Channel: all",
            $"        VSTPlugin: Library {encodedPath}",
            "    EndIf:",
            "EndIf:",
            string.Empty
        ]);
    }

    private static string CreateManagedDirectory(string managedConfig)
    {
        var directory = Path.Combine(Path.GetTempPath(), $"NoiseCancellingStack-tests-{Guid.NewGuid():N}");
        Directory.CreateDirectory(directory);
        File.WriteAllText(
            Path.Combine(directory, "config.txt"),
            $"{ConfigManager.BeginMarker}\nInclude: {ConfigManager.ManagedFileName}\n{ConfigManager.EndMarker}\n");
        File.WriteAllText(Path.Combine(directory, ConfigManager.ManagedFileName), managedConfig);
        return directory;
    }

    private static int Count(string input, string value) =>
        input.Split(value, StringSplitOptions.None).Length - 1;

    private sealed class TestLogger : IOperationLogger
    {
        public void Write(string message)
        {
        }
    }
}
