using Microsoft.Win32;
using Xunit;

namespace NoiseCancellingStack.Tests;

public sealed class RegistryValueSnapshotTests
{
    [Fact]
    public void EquivalentTo_DistinguishesMissingAndEmptyString()
    {
        var missing = RegistryValueSnapshot.Missing();
        var empty = RegistryValueSnapshot.FromValue(RegistryValueKind.String, string.Empty);

        Assert.False(missing.EquivalentTo(empty));
        Assert.True(missing.EquivalentTo(RegistryValueSnapshot.Missing()));
    }

    [Fact]
    public void EquivalentTo_ComparesMultiStringContents()
    {
        var first = RegistryValueSnapshot.FromValue(RegistryValueKind.MultiString, new[] { "a", "b" });
        var same = RegistryValueSnapshot.FromValue(RegistryValueKind.MultiString, new[] { "a", "b" });
        var reordered = RegistryValueSnapshot.FromValue(RegistryValueKind.MultiString, new[] { "b", "a" });

        Assert.True(first.EquivalentTo(same));
        Assert.False(first.EquivalentTo(reordered));
    }

    [Theory]
    [InlineData(RegistryValueKind.DWord, 42)]
    [InlineData(RegistryValueKind.QWord, 42L)]
    public void FromValue_PreservesIntegerKinds(RegistryValueKind kind, object value)
    {
        var snapshot = RegistryValueSnapshot.FromValue(kind, value);

        Assert.True(snapshot.Exists);
        Assert.Equal(kind, snapshot.Kind);
        Assert.Equal(42, snapshot.IntegerValue);
    }
}

