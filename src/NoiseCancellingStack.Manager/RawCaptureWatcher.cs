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
    private static readonly TimeSpan RestartSpacing = TimeSpan.FromSeconds(3);

    public static int Run(InstallStateStore stateStore, OperationLogger logger, CancellationToken cancellation)
    {
        using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, ProductConstants.RegistryView);
        using var capture = hklm.OpenSubKey(
            ProductConstants.CaptureRoot,
            RegistryKeyPermissionCheck.ReadSubTree,
            RegistryRights.ReadKey)
            ?? throw new InvalidOperationException("Windows capture endpoint registry is missing.");
        using var captureChanged = new AutoResetEvent(false);
        using var controlChanged = new AutoResetEvent(false);
        // Kept open for the watcher's lifetime: closing a watched key signals its event.
        RegistryKey? control = null;
        var lastRestart = DateTime.MinValue;
        logger.Write("Watching filtered microphones and the tray icon's filtering switch.");

        // A registration stays pending until it fires, so each key is re-armed only after its event
        // was consumed; re-arming on every timeout would accumulate registrations in the kernel.
        var captureArmed = false;
        var controlArmed = false;
        try
        {
            while (!cancellation.IsCancellationRequested)
            {
                // Arm before acting so a change made meanwhile is never missed. The watcher's own
                // writes signal it too; the next pass then finds nothing to do.
                if (!captureArmed)
                {
                    Arm(capture, watchSubtree: true, captureChanged);
                    captureArmed = true;
                }
                control ??= hklm.OpenSubKey(ProductConstants.UserControlKey, RegistryKeyPermissionCheck.ReadSubTree, RegistryRights.ReadKey);
                if (control is not null && !controlArmed)
                {
                    Arm(control, watchSubtree: false, controlChanged);
                    controlArmed = true;
                }

                if (ApplyFilteringChoice(hklm, stateStore, logger, ref lastRestart, cancellation))
                {
                    continue; // re-read at once in case the user toggled again meanwhile
                }
                Scan(hklm, stateStore, logger);
                switch (WaitHandle.WaitAny([captureChanged, controlChanged, cancellation.WaitHandle], SafetyScanInterval))
                {
                    case 0: captureArmed = false; break;
                    case 1: controlArmed = false; break;
                }
                if (cancellation.WaitHandle.WaitOne(SettleDelay))
                {
                    break;
                }
            }
        }
        finally
        {
            control?.Dispose();
        }

        return 0;
    }

    private static void Arm(RegistryKey key, bool watchSubtree, AutoResetEvent changed)
    {
        var status = RegNotifyChangeKeyValue(
            key.Handle,
            watchSubtree,
            RegNotifyChangeName | RegNotifyChangeLastSet | RegNotifyThreadAgnostic,
            changed.SafeWaitHandle,
            asynchronous: true);
        if (status != 0)
        {
            throw new Win32Exception(status, $"Could not watch {key.Name}.");
        }
    }

    // Applies a new tray choice, then restarts Windows Audio so new echo-canceller instances read it.
    // Restarts are spaced at least RestartSpacing apart so rapid toggling cannot thrash the service.
    private static bool ApplyFilteringChoice(
        RegistryKey hklm,
        InstallStateStore stateStore,
        OperationLogger logger,
        ref DateTime lastRestart,
        CancellationToken cancellation)
    {
        if (FilteringControl.ReadDesired(hklm) == FilteringControl.ReadApplied(hklm))
        {
            return false;
        }
        var wait = lastRestart + RestartSpacing - DateTime.UtcNow;
        if (wait > TimeSpan.Zero && cancellation.WaitHandle.WaitOne(wait))
        {
            return false;
        }

        try
        {
            var state = stateStore.Load();
            if (!FilteringControl.ApplyIfRequested(hklm, state, InstallerEngine.TryGetEqualizerApoConfigDirectory(), logger))
            {
                return false;
            }
            lastRestart = DateTime.UtcNow;
            AudioService.Restart(logger);
            return true;
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException
            or SecurityException or System.Text.Json.JsonException or InvalidOperationException
            or System.ServiceProcess.TimeoutException or Win32Exception)
        {
            logger.Write($"Could not apply the tray icon's filtering choice; will retry: {ex.Message}");
            return false;
        }
    }

    internal static int Scan(RegistryKey hklm, InstallStateStore stateStore, OperationLogger logger)
    {
        if (!FilteringControl.ReadApplied(hklm))
        {
            return 0; // filtering is off: Windows' own raw-capture value is the right one
        }
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
