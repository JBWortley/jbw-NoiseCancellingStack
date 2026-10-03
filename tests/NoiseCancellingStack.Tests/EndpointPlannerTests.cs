// SPDX-License-Identifier: MIT
using Microsoft.Win32;
using Xunit;

namespace NoiseCancellingStack.Tests;

public sealed class EndpointPlannerTests
{
    [Theory]
    [InlineData(true, false, false, EndpointInstallMode.LfxGfx)]
    [InlineData(true, true, false, EndpointInstallMode.SfxEfx)]
    [InlineData(false, false, false, EndpointInstallMode.SfxEfx)]
    [InlineData(false, false, true, EndpointInstallMode.SfxMfx)]
    public void SelectInstallMode_MatchesEqualizerApoRules(
        bool legacy,
        bool modern,
        bool combined,
        EndpointInstallMode expected)
    {
        Assert.Equal(expected, EndpointPlanner.SelectInstallMode(legacy, modern, combined));
    }

    [Fact]
    public void SelectPreMixChild_PreservesExistingSfx()
    {
        var values = EmptySlots();
        values[ProductConstants.Sfx] = RegistryValueSnapshot.FromValue(
            RegistryValueKind.String,
            "{11111111-1111-1111-1111-111111111111}");

        Assert.Equal(
            "{11111111-1111-1111-1111-111111111111}",
            EndpointPlanner.SelectPreMixChild(EndpointInstallMode.SfxEfx, values));
    }

    [Fact]
    public void SelectPreMixChild_FallsBackToLegacyLfx()
    {
        var values = EmptySlots();
        values[ProductConstants.Lfx] = RegistryValueSnapshot.FromValue(
            RegistryValueKind.String,
            "{22222222-2222-2222-2222-222222222222}");

        Assert.Equal(
            "{22222222-2222-2222-2222-222222222222}",
            EndpointPlanner.SelectPreMixChild(EndpointInstallMode.SfxEfx, values));
    }

    private static Dictionary<string, RegistryValueSnapshot> EmptySlots() =>
        ProductConstants.ApoSlotNames.ToDictionary(name => name, _ => RegistryValueSnapshot.Missing());
}
