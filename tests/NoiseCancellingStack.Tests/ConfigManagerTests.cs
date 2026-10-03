using Xunit;

namespace NoiseCancellingStack.Tests;

public sealed class ConfigManagerTests
{
    [Fact]
    public void BuildManagedConfig_IsCaptureOnlyAnd48KhzOnly()
    {
        var config = ConfigManager.BuildManagedConfig(@"C:\Program Files\NoiseCancellingStack\plugins\rnnoise_mono.dll");

        Assert.Contains("stage == \"capture\"", config, StringComparison.Ordinal);
        Assert.Contains("sampleRate == 48000", config, StringComparison.Ordinal);
        Assert.Contains("Channel: all", config, StringComparison.Ordinal);
        Assert.Contains("Library \"C:\\Program Files\\NoiseCancellingStack\\plugins\\rnnoise_mono.dll\"", config, StringComparison.Ordinal);
        Assert.DoesNotContain("post-mix", config, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void BuildManagedConfig_RejectsLineInjection()
    {
        Assert.Throws<ArgumentException>(() => ConfigManager.BuildManagedConfig("plugin.dll\r\nPreamp: 20 dB"));
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
    public void IsManagedInstallation_RequiresBothExactFiles()
    {
        var directory = Path.Combine(Path.GetTempPath(), $"NoiseCancellingStack-tests-{Guid.NewGuid():N}");
        Directory.CreateDirectory(directory);
        try
        {
            const string plugin = @"C:\Program Files\NoiseCancellingStack\plugins\rnnoise_mono.dll";
            File.WriteAllText(
                Path.Combine(directory, "config.txt"),
                $"{ConfigManager.BeginMarker}\nInclude: {ConfigManager.ManagedFileName}\n{ConfigManager.EndMarker}\n");
            File.WriteAllText(
                Path.Combine(directory, ConfigManager.ManagedFileName),
                ConfigManager.BuildManagedConfig(plugin));

            Assert.True(ConfigManager.IsManagedInstallation(directory, plugin));

            File.AppendAllText(Path.Combine(directory, ConfigManager.ManagedFileName), "# changed");
            Assert.False(ConfigManager.IsManagedInstallation(directory, plugin));
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }
}
