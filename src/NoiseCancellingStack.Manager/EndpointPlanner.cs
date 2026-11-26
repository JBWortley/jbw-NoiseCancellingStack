// SPDX-License-Identifier: MIT
namespace NoiseCancellingStack;

/// <summary>
/// Reproduces Equalizer APO's endpoint-mode choice and selects the driver effect that must be
/// retained as its pre-mix child. Keeping this logic pure makes registry policy unit-testable.
/// </summary>
public static class EndpointPlanner
{
    public static EndpointInstallMode SelectInstallMode(bool hasLegacySlots, bool hasModernSlots, bool isCombinedDevice)
    {
        if (hasLegacySlots && !hasModernSlots)
        {
            return EndpointInstallMode.LfxGfx;
        }

        return isCombinedDevice ? EndpointInstallMode.SfxMfx : EndpointInstallMode.SfxEfx;
    }

    public static string SelectPreMixChild(
        EndpointInstallMode mode,
        IReadOnlyDictionary<string, RegistryValueSnapshot> originalValues)
    {
        var primary = mode switch
        {
            EndpointInstallMode.LfxGfx => ProductConstants.Lfx,
            EndpointInstallMode.SfxMfx or EndpointInstallMode.SfxEfx => ProductConstants.Sfx,
            _ => throw new ArgumentOutOfRangeException(nameof(mode))
        };

        var secondary = mode switch
        {
            EndpointInstallMode.LfxGfx => ProductConstants.Gfx,
            EndpointInstallMode.SfxMfx => ProductConstants.Mfx,
            EndpointInstallMode.SfxEfx => ProductConstants.Efx,
            _ => throw new ArgumentOutOfRangeException(nameof(mode))
        };

        var candidate = GetString(originalValues, primary);
        if (!originalValues[primary].Exists && !originalValues[secondary].Exists)
        {
            candidate = GetString(originalValues, mode == EndpointInstallMode.LfxGfx ? ProductConstants.Sfx : ProductConstants.Lfx);
        }

        return candidate;
    }

    // PKEY_AudioEndpoint_FormFactor values for microphones that sit next to the listener's ear.
    private const int HeadsetFormFactor = 5;
    private const int HandsetFormFactor = 6;

    /// <summary>
    /// Decides whether the echo canceller may occupy this capture endpoint's mode-effect (MFX)
    /// slot. It never displaces a driver effect: the slot, its mode list, and the composite MFX
    /// list must all be unused. Headset microphones are skipped because their render side is
    /// headphones, so there is no acoustic echo to remove.
    /// </summary>
    public static EchoCancellerDecision EvaluateEchoCanceller(
        EndpointInstallMode mode,
        RegistryValueSnapshot modeEffect,
        RegistryValueSnapshot modeEffectModes,
        bool hasCompositeModeEffects,
        int? formFactor,
        EchoCancellerContext context)
    {
        if (context.WindowsBuild < ProductConstants.EchoCancellerMinimumWindowsBuild)
        {
            return EchoCancellerDecision.Skip("Windows 11 is required for echo cancellation");
        }
        if (!context.Registered)
        {
            return EchoCancellerDecision.Skip("the echo canceller is not registered");
        }
        if (mode != EndpointInstallMode.SfxEfx)
        {
            return EchoCancellerDecision.Skip(mode == EndpointInstallMode.LfxGfx
                ? "legacy driver effects have no mode-effect slot"
                : "combined hands-free endpoints are excluded");
        }
        if (formFactor is HeadsetFormFactor or HandsetFormFactor)
        {
            return EchoCancellerDecision.Skip("headset microphones have no acoustic echo path");
        }
        var slotUsed = modeEffect.Exists
            && !(modeEffect.Kind is Microsoft.Win32.RegistryValueKind.String or Microsoft.Win32.RegistryValueKind.ExpandString
                && string.IsNullOrEmpty(modeEffect.StringValue));
        if (slotUsed || modeEffectModes.Exists || hasCompositeModeEffects)
        {
            return EchoCancellerDecision.Skip("the driver already uses the mode-effect slot");
        }
        return EchoCancellerDecision.Attach;
    }

    /// <summary>
    /// Decides whether the echo canceller may join a microphone whose Equalizer APO attachment
    /// predates this product. Capture effects run EFX → MFX → SFX, so the echo canceller sees the
    /// microphone before noise suppression only when Equalizer APO occupies the stream-effect slot
    /// and no other slot; the usual mode-effect rules then apply unchanged.
    /// </summary>
    public static EchoCancellerDecision EvaluateEchoCancellerBesideExistingEqualizerApo(
        IReadOnlyDictionary<string, RegistryValueSnapshot> slots,
        bool isCombinedDevice,
        bool hasCompositeModeEffects,
        int? formFactor,
        EchoCancellerContext context)
    {
        var mode = isCombinedDevice ? EndpointInstallMode.SfxMfx : EndpointInstallMode.SfxEfx;
        var decision = EvaluateEchoCanceller(
            mode, slots[ProductConstants.Mfx], slots[ProductConstants.MfxModes], hasCompositeModeEffects, formFactor, context);
        if (!decision.ShouldAttach)
        {
            return decision;
        }
        if (slots[ProductConstants.Lfx].Exists || slots[ProductConstants.Gfx].Exists)
        {
            return EchoCancellerDecision.Skip("legacy driver effects have no mode-effect slot");
        }
        if (!IsEqualizerApo(slots[ProductConstants.Sfx], ProductConstants.EqualizerApoPreMixClsid))
        {
            return EchoCancellerDecision.Skip("Equalizer APO is not in the stream-effect slot");
        }
        if (IsEqualizerApo(slots[ProductConstants.Efx], ProductConstants.EqualizerApoPreMixClsid)
            || IsEqualizerApo(slots[ProductConstants.Efx], ProductConstants.EqualizerApoPostMixClsid))
        {
            return EchoCancellerDecision.Skip("Equalizer APO also runs as an endpoint effect, ahead of the echo canceller");
        }
        return EchoCancellerDecision.Attach;
    }

    private static bool IsEqualizerApo(RegistryValueSnapshot value, string clsid) =>
        value.Exists
        && value.Kind is Microsoft.Win32.RegistryValueKind.String or Microsoft.Win32.RegistryValueKind.ExpandString
        && string.Equals(value.StringValue, clsid, StringComparison.OrdinalIgnoreCase);

    private static string GetString(IReadOnlyDictionary<string, RegistryValueSnapshot> values, string name)
    {
        var value = values[name];
        return value.Exists && value.Kind is Microsoft.Win32.RegistryValueKind.String or Microsoft.Win32.RegistryValueKind.ExpandString
            ? value.StringValue ?? string.Empty
            : string.Empty;
    }
}
