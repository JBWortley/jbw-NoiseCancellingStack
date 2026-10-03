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

    private static string GetString(IReadOnlyDictionary<string, RegistryValueSnapshot> values, string name)
    {
        var value = values[name];
        return value.Exists && value.Kind is Microsoft.Win32.RegistryValueKind.String or Microsoft.Win32.RegistryValueKind.ExpandString
            ? value.StringValue ?? string.Empty
            : string.Empty;
    }
}
