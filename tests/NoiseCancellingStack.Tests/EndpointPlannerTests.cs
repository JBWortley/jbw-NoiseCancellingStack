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

    private static readonly EchoCancellerContext Windows11 = new(26100, Registered: true);

    private static EchoCancellerDecision Evaluate(
        EndpointInstallMode mode = EndpointInstallMode.SfxEfx,
        RegistryValueSnapshot? modeEffect = null,
        RegistryValueSnapshot? modes = null,
        bool composite = false,
        int? formFactor = 4,
        EchoCancellerContext? context = null) =>
        EndpointPlanner.EvaluateEchoCanceller(
            mode,
            modeEffect ?? RegistryValueSnapshot.Missing(),
            modes ?? RegistryValueSnapshot.Missing(),
            composite,
            formFactor,
            context ?? Windows11);

    [Theory]
    [InlineData(4)]     // Microphone
    [InlineData(null)]  // form factor not reported
    public void EvaluateEchoCanceller_AttachesToFreeModernMicrophoneOnWindows11(int? formFactor)
    {
        Assert.True(Evaluate(formFactor: formFactor).ShouldAttach);
    }

    [Fact]
    public void EvaluateEchoCanceller_TreatsEmptyModeEffectStringAsFree()
    {
        Assert.True(Evaluate(modeEffect: RegistryValueSnapshot.FromValue(RegistryValueKind.String, string.Empty)).ShouldAttach);
    }

    [Fact]
    public void EvaluateEchoCanceller_RequiresWindows11()
    {
        var decision = Evaluate(context: new EchoCancellerContext(19045, Registered: true));
        Assert.False(decision.ShouldAttach);
        Assert.Contains("Windows 11", decision.Reason, StringComparison.Ordinal);
        Assert.True(Evaluate(context: new EchoCancellerContext(22000, Registered: true)).ShouldAttach);
    }

    [Fact]
    public void EvaluateEchoCanceller_RequiresVerifiedRegistration()
    {
        var decision = Evaluate(context: new EchoCancellerContext(26100, Registered: false));
        Assert.False(decision.ShouldAttach);
        Assert.Contains("not registered", decision.Reason, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(EndpointInstallMode.LfxGfx, "legacy")]
    [InlineData(EndpointInstallMode.SfxMfx, "combined")]
    public void EvaluateEchoCanceller_SkipsLegacyAndCombinedEndpoints(EndpointInstallMode mode, string reason)
    {
        var decision = Evaluate(mode: mode);
        Assert.False(decision.ShouldAttach);
        Assert.Contains(reason, decision.Reason, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(5)]  // Headset
    [InlineData(6)]  // Handset
    public void EvaluateEchoCanceller_SkipsHeadsetMicrophones(int formFactor)
    {
        var decision = Evaluate(formFactor: formFactor);
        Assert.False(decision.ShouldAttach);
        Assert.Contains("headset", decision.Reason, StringComparison.Ordinal);
    }

    [Fact]
    public void EvaluateEchoCanceller_NeverDisplacesADriverModeEffect()
    {
        var driverMfx = RegistryValueSnapshot.FromValue(RegistryValueKind.String, "{33333333-3333-3333-3333-333333333333}");
        var driverModes = RegistryValueSnapshot.FromValue(RegistryValueKind.MultiString, new[] { ProductConstants.DefaultProcessingMode });

        Assert.False(Evaluate(modeEffect: driverMfx).ShouldAttach);
        Assert.False(Evaluate(modes: driverModes).ShouldAttach);
        Assert.False(Evaluate(composite: true).ShouldAttach);
        Assert.Contains("mode-effect slot", Evaluate(modeEffect: driverMfx).Reason, StringComparison.Ordinal);
    }

    // The layout the Equalizer APO configurator leaves on an ordinary microphone (seen live on an
    // AT2040USB): Equalizer APO's pre-mix APO in SFX for the default mode, nothing else.
    private static Dictionary<string, RegistryValueSnapshot> ExistingEqualizerApoSlots()
    {
        var slots = ProductConstants.ManagedFxNames.Concat(ProductConstants.OptionalManagedFxNames)
            .ToDictionary(name => name, _ => RegistryValueSnapshot.Missing(), StringComparer.OrdinalIgnoreCase);
        slots[ProductConstants.Sfx] = Text(ProductConstants.EqualizerApoPreMixClsid);
        slots[ProductConstants.SfxModes] = RegistryValueSnapshot.FromValue(
            Microsoft.Win32.RegistryValueKind.MultiString, new[] { ProductConstants.DefaultProcessingMode });
        return slots;
    }

    private static RegistryValueSnapshot Text(string value) =>
        RegistryValueSnapshot.FromValue(Microsoft.Win32.RegistryValueKind.String, value);

    private static EchoCancellerDecision EvaluateBesideExisting(
        Dictionary<string, RegistryValueSnapshot> slots,
        bool combined = false,
        int? formFactor = 4,
        EchoCancellerContext? context = null) =>
        EndpointPlanner.EvaluateEchoCancellerBesideExistingEqualizerApo(
            slots, combined, hasCompositeModeEffects: false, formFactor, context ?? Windows11);

    [Fact]
    public void BesideExistingEqualizerApo_AttachesWhenEqualizerApoIsOnlyTheStreamEffect()
    {
        Assert.True(EvaluateBesideExisting(ExistingEqualizerApoSlots()).ShouldAttach);
    }

    [Fact]
    public void BesideExistingEqualizerApo_KeepsTheUsualPlatformAndEndpointRules()
    {
        Assert.Contains("Windows 11", EvaluateBesideExisting(ExistingEqualizerApoSlots(), context: new(19045, true)).Reason);
        Assert.Contains("not registered", EvaluateBesideExisting(ExistingEqualizerApoSlots(), context: new(26100, false)).Reason);
        Assert.Contains("combined", EvaluateBesideExisting(ExistingEqualizerApoSlots(), combined: true).Reason);
        Assert.Contains("headset", EvaluateBesideExisting(ExistingEqualizerApoSlots(), formFactor: 5).Reason);

        var driverMfx = ExistingEqualizerApoSlots();
        driverMfx[ProductConstants.Mfx] = Text("{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}");
        Assert.Contains("mode-effect slot", EvaluateBesideExisting(driverMfx).Reason);
    }

    [Fact]
    public void BesideExistingEqualizerApo_RequiresEqualizerApoAfterTheEchoCanceller()
    {
        // Capture runs EFX -> MFX -> SFX: noise suppression must not run ahead of echo cancellation.
        var notInSfx = ExistingEqualizerApoSlots();
        notInSfx[ProductConstants.Sfx] = Text("{BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB}");
        notInSfx[ProductConstants.Efx] = Text(ProductConstants.EqualizerApoPostMixClsid);
        Assert.Contains("stream-effect slot", EvaluateBesideExisting(notInSfx).Reason);

        var alsoInEfx = ExistingEqualizerApoSlots();
        alsoInEfx[ProductConstants.Efx] = Text(ProductConstants.EqualizerApoPostMixClsid);
        Assert.Contains("endpoint effect", EvaluateBesideExisting(alsoInEfx).Reason);

        var legacy = ExistingEqualizerApoSlots();
        legacy[ProductConstants.Lfx] = Text(ProductConstants.EqualizerApoPreMixClsid);
        Assert.Contains("legacy", EvaluateBesideExisting(legacy).Reason);
    }

    private static Dictionary<string, RegistryValueSnapshot> EmptySlots() =>
        ProductConstants.ApoSlotNames.ToDictionary(name => name, _ => RegistryValueSnapshot.Missing());
}
