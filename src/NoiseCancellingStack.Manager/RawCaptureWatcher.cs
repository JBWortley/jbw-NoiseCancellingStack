// SPDX-License-Identifier: MIT
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security;
using System.Security.AccessControl;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace NoiseCancellingStack;

/// <summary>
/// The background watcher (the SYSTEM task that runs <c>watch</c>). Windows rebuilds an endpoint's
/// Properties from the driver whenever the device is plugged in or the audio services start, which
/// re-advertises raw capture on a filtered microphone and lets applications bypass its filters. The
/// watcher waits for registry changes below the capture endpoints and, where the journal records raw
/// capture as disabled and Windows has put the driver's original value back, disables it again.
/// It writes nothing else. Uninstall stops it before restoring anything.
/// </summary>
public static class RawCaptureWatcher
{
    private const int RegNotifyChangeName = 0x1;
    private const int RegNotifyChangeLastSet = 0x4;
    private const int RegNotifyThreadAgnostic = 0x10000000;
    // Registry notifications are the trigger; the periodic scan only covers a missed notification.
    private static readonly TimeSpan SafetyScanInterval = TimeSpan.FromMinutes(1);
    // Lets the Audio Endpoint Builder finish rebuilding an endpoint before it is inspected.
    private static readonly TimeSpan SettleDelay = TimeSpan.FromMilliseconds(250);

    public static int Run(InstallStateStore stateStore, OperationLogger logger, CancellationToken cancellation)
    {
        using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, ProductConstants.RegistryView);
        using var capture = hklm.OpenSubKey(
            ProductConstants.CaptureRoot,
            RegistryKeyPermissionCheck.ReadSubTree,
            RegistryRights.ReadKey)
            ?? throw new InvalidOperationException("Windows capture endpoint registry is missing.");
        using var changed = new AutoResetEvent(false);
        logger.Write("Watching filtered microphones so raw capture stays disabled.");

        while (!cancellation.IsCancellationRequested)
        {
            // Arm before scanning so a change made during the scan is never missed. The watcher's own
            // writes signal it too; the next scan then finds nothing to do.
            var status = RegNotifyChangeKeyValue(
                capture.Handle,
                watchSubtree: true,
                RegNotifyChangeName | RegNotifyChangeLastSet | RegNotifyThreadAgnostic,
                changed.SafeWaitHandle,
                asynchronous: true);
            if (status != 0)
            {
                throw new Win32Exception(status, "Could not watch the capture endpoint registry.");
            }

            Scan(hklm, stateStore, logger);
            WaitHandle.WaitAny([changed, cancellation.WaitHandle], SafetyScanInterval);
            if (cancellation.WaitHandle.WaitOne(SettleDelay))
            {
                break;
            }
        }

        return 0;
    }

    internal static int Scan(RegistryKey hklm, InstallStateStore stateStore, OperationLogger logger)
    {
        InstallState state;
        try
        {
            state = stateStore.Load();
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException
            or System.Text.Json.JsonException)
        {
            // A journal being replaced or a damaged one: try again on the next change or minute.
            logger.Write($"Could not read the installation state; will retry: {ex.Message}");
            return 0;
        }

        var reapplied = 0;
        foreach (var endpoint in FilteredEndpointPolicy.RawCaptureToReapply(state, item => ReadRaw(hklm, item)))
        {
            try
            {
                using var properties = hklm.OpenSubKey(
                    PropertiesPath(endpoint),
                    RegistryKeyPermissionCheck.ReadWriteSubTree,
                    RegistryRights.QueryValues | RegistryRights.SetValue);
                if (properties is null)
                {
                    continue;
                }
                endpoint.InstalledProperties[ProductConstants.RawProcessingSupported]
                    .Apply(properties, ProductConstants.RawProcessingSupported);
                reapplied++;
                logger.Write($"Raw capture disabled again after Windows rebuilt {endpoint.DisplayName}.");
            }
            catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or SecurityException)
            {
                logger.Write($"Could not disable raw capture on {endpoint.DisplayName}: {ex.Message}");
            }
        }
        return reapplied;
    }

    private static RegistryValueSnapshot? ReadRaw(RegistryKey hklm, EndpointState endpoint)
    {
        using var properties = hklm.OpenSubKey(PropertiesPath(endpoint), writable: false);
        return properties is null ? null : RegistryValueSnapshot.Capture(properties, ProductConstants.RawProcessingSupported);
    }

    private static string PropertiesPath(EndpointState endpoint) =>
        $@"{ProductConstants.CaptureRoot}\{endpoint.EndpointId}\Properties";

    [DllImport("advapi32.dll")]
    private static extern int RegNotifyChangeKeyValue(
        SafeRegistryHandle key,
        [MarshalAs(UnmanagedType.Bool)] bool watchSubtree,
        int notifyFilter,
        SafeWaitHandle eventHandle,
        [MarshalAs(UnmanagedType.Bool)] bool asynchronous);
}
