// SPDX-License-Identifier: MIT
using Microsoft.Win32;

namespace NoiseCancellingStack;

public enum PropertyAction
{
    /// <summary>The live value is already what it should be.</summary>
    None,
    /// <summary>Write the installed value (first time, or Windows reset it to the original).</summary>
    Apply,
    /// <summary>Write the original value back and stop tracking it.</summary>
    Restore,
    /// <summary>Stop tracking without writing: the value is original already, or someone else owns it now.</summary>
    Release
}

/// <summary>
/// Pure rules for the two endpoint Properties values a filtered microphone carries: the FILTERED
/// name, so every application's device list shows which microphones are filtered, and disabled raw
/// capture, so applications that request raw mode (Chromium-based ones always do when it is
/// advertised) still receive the filtered signal.
/// </summary>
public static class FilteredEndpointPolicy
{
    // The driver-derived value is a serialized VT_BOOL: type (4 bytes), 1 (4 bytes), value (2 bytes), padding.
    private const int RawBlobLength = 12;
    private const byte VtBool = 11;
    private const int RawValueOffset = 8;

    /// <summary>A microphone is labelled and kept off raw capture only while noise suppression applies to it.</summary>
    public static bool IsFiltered(uint sampleRate) =>
        AudioRoutePlanner.Select(sampleRate) != CaptureProcessor.PassThrough;

    public static string Label(string description) =>
        description.EndsWith(ProductConstants.FilteredLabelSuffix, StringComparison.Ordinal)
            ? description
            : description + ProductConstants.FilteredLabelSuffix;

    public static string Unlabel(string description) =>
        description.EndsWith(ProductConstants.FilteredLabelSuffix, StringComparison.Ordinal)
            ? description[..^ProductConstants.FilteredLabelSuffix.Length]
            : description;

    /// <summary>
    /// For a value not yet tracked, the original and installed pair to journal, or false when this
    /// value should be left alone (an unexpected layout, raw capture not advertised, or no name).
    /// </summary>
    public static bool TryPlan(
        string name,
        RegistryValueSnapshot current,
        out RegistryValueSnapshot original,
        out RegistryValueSnapshot installed)
    {
        original = installed = RegistryValueSnapshot.Missing();
        if (string.Equals(name, ProductConstants.ConnectionName, StringComparison.OrdinalIgnoreCase))
        {
            if (!current.Exists || current.Kind != RegistryValueKind.String || string.IsNullOrWhiteSpace(current.StringValue))
            {
                return false;
            }
            // A name that already carries the label (an earlier record lost, or a manual test) is
            // journaled with its unlabelled form as the original, so uninstall removes the label.
            var plain = Unlabel(current.StringValue);
            if (string.IsNullOrWhiteSpace(plain))
            {
                return false;
            }
            original = RegistryValueSnapshot.FromValue(RegistryValueKind.String, plain);
            installed = RegistryValueSnapshot.FromValue(RegistryValueKind.String, Label(plain));
            return true;
        }

        if (string.Equals(name, ProductConstants.RawProcessingSupported, StringComparison.OrdinalIgnoreCase))
        {
            if (!IsRawBlob(current, out var bytes) || (bytes[RawValueOffset] == 0 && bytes[RawValueOffset + 1] == 0))
            {
                return false;
            }
            original = current;
            installed = RegistryValueSnapshot.FromValue(RegistryValueKind.Binary, DisabledRawBlob(bytes));
            return true;
        }

        return false;
    }

    /// <summary>What to do with a tracked value given whether the microphone is filtered now.</summary>
    public static PropertyAction Reconcile(
        bool filtered,
        RegistryValueSnapshot original,
        RegistryValueSnapshot installed,
        RegistryValueSnapshot current)
    {
        if (current.EquivalentTo(installed))
        {
            return filtered ? PropertyAction.None : PropertyAction.Restore;
        }
        if (current.EquivalentTo(original))
        {
            // Windows rebuilt the endpoint (raw capture), or the label was removed and the
            // microphone is filtered again: re-apply. Otherwise it is already original.
            return filtered ? PropertyAction.Apply : PropertyAction.Release;
        }
        // Someone else changed it (the user renamed the microphone, a driver update): theirs now.
        return PropertyAction.Release;
    }

    /// <summary>
    /// For the tray switch: the value to write so a tracked value is ours (on) or Windows' own (off),
    /// or null to leave it, including whenever someone else has changed it.
    /// </summary>
    public static RegistryValueSnapshot? ToggleTarget(
        bool enabled,
        RegistryValueSnapshot original,
        RegistryValueSnapshot installed,
        RegistryValueSnapshot current)
    {
        if (enabled)
        {
            return current.EquivalentTo(original) && !current.EquivalentTo(installed) ? installed : null;
        }
        return current.EquivalentTo(installed) && !current.EquivalentTo(original) ? original : null;
    }

    /// <summary>
    /// The background watcher's only job: endpoints whose tracked raw-capture value Windows has
    /// reset to the driver's original since it was disabled. Pending records are skipped.
    /// </summary>
    public static IReadOnlyList<EndpointState> RawCaptureToReapply(
        InstallState state,
        Func<EndpointState, RegistryValueSnapshot?> readCurrentRaw)
    {
        var result = new List<EndpointState>();
        foreach (var endpoint in state.Endpoints)
        {
            if (endpoint.Pending
                || !endpoint.OriginalProperties.TryGetValue(ProductConstants.RawProcessingSupported, out var original)
                || !endpoint.InstalledProperties.TryGetValue(ProductConstants.RawProcessingSupported, out var installed))
            {
                continue;
            }
            var current = readCurrentRaw(endpoint);
            if (current is not null && current.EquivalentTo(original) && !current.EquivalentTo(installed))
            {
                result.Add(endpoint);
            }
        }
        return result;
    }

    private static bool IsRawBlob(RegistryValueSnapshot value, out byte[] bytes)
    {
        bytes = [];
        if (!value.Exists || value.Kind != RegistryValueKind.Binary || value.BinaryBase64Value is null)
        {
            return false;
        }
        bytes = Convert.FromBase64String(value.BinaryBase64Value);
        return bytes.Length == RawBlobLength && bytes[0] == VtBool && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0;
    }

    private static byte[] DisabledRawBlob(byte[] enabled)
    {
        var disabled = (byte[])enabled.Clone();
        disabled[RawValueOffset] = 0;
        disabled[RawValueOffset + 1] = 0;
        return disabled;
    }
}
