// SPDX-License-Identifier: MIT
using Xunit;

namespace NoiseCancellingStack.Tests;

public sealed class InstallerEngineOrderingTests
{
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
