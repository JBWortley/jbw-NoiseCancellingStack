// SPDX-License-Identifier: MIT
using Xunit;

namespace NoiseCancellingStack.Tests;

public sealed class InstallerEngineOrderingTests
{
    [Theory]
    [InlineData(true, false, 0, true)]
    [InlineData(false, true, 0, true)]
    [InlineData(false, false, 1, true)]
    [InlineData(false, false, 0, false)]
    public void ShouldRestartAudio_CoversPayloadReplacementAndManagedMutations(
        bool forceAudioRestart,
        bool configChanged,
        int attached,
        bool expected)
    {
        var report = new OperationReport
        {
            ConfigChanged = configChanged,
            Attached = attached
        };

        Assert.Equal(expected, InstallerEngine.ShouldRestartAudio(forceAudioRestart, report));
    }

    [Fact]
    public void Program_ForcesRestartOnlyForExplicitInstall()
    {
        var sourcePath = Path.Combine(
            FindRepositoryRoot(),
            "src",
            "NoiseCancellingStack.Manager",
            "Program.cs");
        var source = string.Concat(File.ReadAllText(sourcePath).Where(character => !char.IsWhiteSpace(character)));

        Assert.Contains(
            "\"install\"=>Report(engine.InstallOrRepair(createMaintenanceTask:true,forceAudioRestart:true),logger)",
            source,
            StringComparison.Ordinal);
        Assert.Contains(
            "\"repair\"=>Report(engine.InstallOrRepair(createMaintenanceTask:!args.Contains(\"--scheduled\",StringComparer.OrdinalIgnoreCase),forceAudioRestart:false),logger)",
            source,
            StringComparison.Ordinal);
    }

    [Fact]
    public void InstallOrRepair_ValidatesPayloadsBeforeAnyMutation()
    {
        var sourcePath = Path.Combine(
            FindRepositoryRoot(),
            "src",
            "NoiseCancellingStack.Manager",
            "InstallerEngine.cs");
        var source = File.ReadAllText(sourcePath);
        var methodStart = source.IndexOf(
            "public OperationReport InstallOrRepair",
            StringComparison.Ordinal);
        var methodEnd = source.IndexOf(
            "public OperationReport Uninstall",
            methodStart,
            StringComparison.Ordinal);
        Assert.True(methodStart >= 0 && methodEnd > methodStart);
        var method = source[methodStart..methodEnd];
        var validation = method.IndexOf("PayloadValidator", StringComparison.Ordinal);
        Assert.True(validation >= 0, "InstallOrRepair must invoke PayloadValidator.");

        foreach (var mutation in new[]
        {
            "SafetyBackup.Create",
            "ConfigManager.Install",
            "stateStore.Save",
            "AttachAll",
            "TryRestartAudio"
        })
        {
            var mutationIndex = method.IndexOf(mutation, StringComparison.Ordinal);
            Assert.True(mutationIndex >= 0, $"Expected mutation call was not found: {mutation}");
            Assert.True(validation < mutationIndex, $"Payload validation must precede {mutation}.");
        }
    }

    private static string FindRepositoryRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null && !File.Exists(Path.Combine(directory.FullName, "NoiseCancellingStack.slnx")))
        {
            directory = directory.Parent;
        }
        return directory?.FullName
            ?? throw new DirectoryNotFoundException("Could not locate the repository root.");
    }
}
