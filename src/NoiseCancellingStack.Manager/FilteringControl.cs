// SPDX-License-Identifier: MIT
using System.Security;
using System.Security.AccessControl;
using System.Security.Principal;
using Microsoft.Win32;

namespace NoiseCancellingStack;

/// <summary>
/// The on/off switch behind the tray icon. The user's choice lives in a value standard users may
/// write (UserControl\FilteringEnabled); only the SYSTEM watcher acts on it, and it records what it
/// applied in AppliedFilteringEnabled, which only administrators can write. Turning filtering off
/// makes noise suppression and echo cancellation pass audio through and returns the microphones'
/// names and raw capture to Windows' own values; turning it on reverses exactly that.
/// </summary>
public static class FilteringControl
{
    public static bool ReadDesired(RegistryKey hklm)
    {
        using var key = hklm.OpenSubKey(ProductConstants.UserControlKey, writable: false);
        // Anything but an explicit DWORD 0 means on, so a damaged value never silently disables filtering.
        return key?.GetValue(ProductConstants.FilteringEnabledValue) is not int value || value != 0;
    }

    public static bool ReadApplied(RegistryKey hklm)
    {
        using var key = hklm.OpenSubKey(ProductConstants.SettingsRoot, writable: false);
        return key?.GetValue(ProductConstants.AppliedFilteringEnabledValue) is not int value || value != 0;
    }

    public static void WriteApplied(RegistryKey hklm, bool enabled)
    {
        using var key = hklm.CreateSubKey(ProductConstants.SettingsRoot, writable: true)
            ?? throw new UnauthorizedAccessException($"Could not open {ProductConstants.SettingsRoot}.");
        key.SetValue(ProductConstants.AppliedFilteringEnabledValue, enabled ? 1 : 0, RegistryValueKind.DWord);
    }

    /// <summary>
    /// Creates the control key with an explicit ACL: SYSTEM and Administrators full control, Users
    /// may read, write values, and be notified, and nothing is inherited. Re-applied on every install
    /// and repair so the ACL cannot drift. A missing choice defaults to on.
    /// </summary>
    public static void EnsureUserControlKey(OperationLogger logger)
    {
        var security = BuildUserControlSecurity();
        using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, ProductConstants.RegistryView);
        using var key = hklm.CreateSubKey(
            ProductConstants.UserControlKey,
            RegistryKeyPermissionCheck.ReadWriteSubTree,
            security)
            ?? throw new UnauthorizedAccessException($"Could not create {ProductConstants.UserControlKey}.");
        key.SetAccessControl(security);
        if (key.GetValue(ProductConstants.FilteringEnabledValue) is not int)
        {
            key.SetValue(ProductConstants.FilteringEnabledValue, 1, RegistryValueKind.DWord);
        }
        logger.Write("Prepared the tray icon's filtering switch.");
    }

    internal static RegistrySecurity BuildUserControlSecurity()
    {
        var security = new RegistrySecurity();
        security.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        foreach (var (sid, rights) in new[]
        {
            (WellKnownSidType.LocalSystemSid, RegistryRights.FullControl),
            (WellKnownSidType.BuiltinAdministratorsSid, RegistryRights.FullControl),
            (WellKnownSidType.BuiltinUsersSid,
                RegistryRights.QueryValues | RegistryRights.SetValue | RegistryRights.Notify | RegistryRights.ReadPermissions)
        })
        {
            security.AddAccessRule(new RegistryAccessRule(
                new SecurityIdentifier(sid, null), rights, InheritanceFlags.None, PropagationFlags.None, AccessControlType.Allow));
        }
        return security;
    }

    /// <summary>Removes the switch and the applied state during a complete uninstall.</summary>
    public static void Remove(OperationLogger logger)
    {
        using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, ProductConstants.RegistryView);
        hklm.DeleteSubKey(ProductConstants.UserControlKey, throwOnMissingSubKey: false);
        using var settings = hklm.OpenSubKey(ProductConstants.SettingsRoot, writable: true);
        settings?.DeleteValue(ProductConstants.AppliedFilteringEnabledValue, throwOnMissingValue: false);
        logger.Write("Removed the tray icon's filtering switch.");
    }

    /// <summary>
    /// Applies the user's choice if it differs from what was applied last. Called by the watcher only.
    /// Returns true when Windows Audio must restart so new echo-canceller instances read the new state.
    /// </summary>
    public static bool ApplyIfRequested(RegistryKey hklm, InstallState state, string? configDirectory, OperationLogger logger)
    {
        var desired = ReadDesired(hklm);
        if (desired == ReadApplied(hklm))
        {
            return false;
        }

        if (configDirectory is not null)
        {
            ConfigManager.SetFiltering(
                configDirectory, ProductConstants.MonoPluginPath, ProductConstants.WebRtcPluginPath, desired, logger);
        }
        ApplyProperties(hklm, state, desired, logger);
        WriteApplied(hklm, desired);
        logger.Write(desired ? "Filtering turned on from the tray icon." : "Filtering turned off from the tray icon.");
        return true;
    }

    // Swaps each journaled name and raw-capture value between ours and Windows' own, never touching
    // one that someone else has changed. The journal keeps tracking both while filtering is off.
    private static void ApplyProperties(RegistryKey hklm, InstallState state, bool enabled, OperationLogger logger)
    {
        foreach (var endpoint in state.Endpoints.Where(item => !item.Pending && item.OriginalProperties.Count > 0))
        {
            try
            {
                using var properties = hklm.OpenSubKey(
                    $@"{ProductConstants.CaptureRoot}\{endpoint.EndpointId}\Properties",
                    RegistryKeyPermissionCheck.ReadWriteSubTree,
                    RegistryRights.QueryValues | RegistryRights.SetValue);
                if (properties is null)
                {
                    continue;
                }
                foreach (var (name, original) in endpoint.OriginalProperties)
                {
                    FilteredEndpointPolicy
                        .ToggleTarget(enabled, original, endpoint.InstalledProperties[name], RegistryValueSnapshot.Capture(properties, name))
                        ?.Apply(properties, name);
                }
            }
            catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or SecurityException)
            {
                logger.Write($"Could not update the name or raw-capture setting of {endpoint.DisplayName}: {ex.Message}");
            }
        }
    }
}
