// SPDX-License-Identifier: MIT
using Microsoft.Win32;

namespace NoiseCancellingStack;

public sealed class InstallerEngine
{
    // This class is the transaction coordinator. File/config work happens before endpoint work,
    // while EndpointRegistry owns the finer-grained write-ahead journal for each registry change.
    private readonly InstallStateStore stateStore;
    private readonly OperationLogger logger;

    public InstallerEngine(InstallStateStore stateStore, OperationLogger logger)
    {
        this.stateStore = stateStore;
        this.logger = logger;
    }

    public OperationReport InstallOrRepair(bool createMaintenanceTask, bool forceAudioRestart)
    {
        ValidatePlatformAndEqualizerApo();
        new PayloadValidator(new NativeWebRtcPluginProbe()).Validate(
            ProductConstants.MonoPluginPath,
            ProductConstants.RnNoisePluginSha256,
            ProductConstants.WebRtcPluginPath,
            GeneratedPayloadHashes.WebRtcApmPluginSha256);
        var state = stateStore.Load();
        var configDirectory = GetEqualizerApoConfigDirectory();
        // A nonempty journal can drive elevated registry restoration. Only accept one when the
        // protected Equalizer APO config proves that a prior managed installation created it.
        if (state.Endpoints.Count > 0
            && !ConfigManager.IsTrustedManagedInstallation(
                configDirectory,
                ProductConstants.MonoPluginPath,
                ProductConstants.WebRtcPluginPath))
        {
            throw new InvalidDataException(
                "A nonempty state journal exists without its trusted managed Equalizer APO configuration. " +
                "Refusing to apply pre-staged registry data; consult the recovery backups.");
        }
        SafetyBackup.Create("install-repair", configDirectory, logger);

        var report = new OperationReport
        {
            ConfigChanged = ConfigManager.Install(
                configDirectory,
                ProductConstants.MonoPluginPath,
                ProductConstants.WebRtcPluginPath,
                ProductConstants.BackupDirectory,
                logger)
        };
        state.ConfigInstalled = true;
        state.EqualizerApoConfigPath = configDirectory;
        stateStore.Save(state);

        var endpointReport = new EndpointRegistry(stateStore, logger).AttachAll(state);
        Merge(report, endpointReport);
        stateStore.Save(state);

        if (createMaintenanceTask)
        {
            try
            {
                MaintenanceTask.Create(Environment.ProcessPath ?? throw new InvalidOperationException("Process path is unavailable."), logger);
            }
            catch (Exception ex) when (ex is InvalidOperationException or IOException)
            {
                report.Skipped++;
                report.Messages.Add($"Could not register maintenance task: {ex.Message}");
                logger.Write(report.Messages[^1]);
            }
        }

        if (ShouldRestartAudio(forceAudioRestart, report))
        {
            TryRestartAudio(report);
        }

        return report;
    }

    public OperationReport Uninstall()
    {
        var state = stateStore.Load();
        var configDirectory = TryGetEqualizerApoConfigDirectory();
        if (configDirectory is not null
            && state.EqualizerApoConfigPath is not null
            && !string.Equals(
                Path.GetFullPath(configDirectory).TrimEnd(Path.DirectorySeparatorChar),
                Path.GetFullPath(state.EqualizerApoConfigPath).TrimEnd(Path.DirectorySeparatorChar),
                StringComparison.OrdinalIgnoreCase))
        {
            logger.Write("Ignored a stale configuration path in the state file; the current Equalizer APO registry path is authoritative.");
        }
        SafetyBackup.Create("uninstall", configDirectory, logger);

        var report = new EndpointRegistry(stateStore, logger).DetachOwned(state);
        // Keep the executable, config, and task available when rollback is incomplete. Removing
        // them would strand the recovery journal and make the safe next action harder.
        if (report.HasWarnings)
        {
            logger.Write(
                "Rollback was incomplete, so the managed configuration and maintenance task were retained for recovery.");
            if (report.Restored > 0)
            {
                TryRestartAudio(report);
            }
            return report;
        }

        if (!string.IsNullOrWhiteSpace(configDirectory))
        {
            report.ConfigChanged = ConfigManager.Uninstall(
                configDirectory,
                ProductConstants.MonoPluginPath,
                ProductConstants.WebRtcPluginPath,
                logger);
        }
        state.ConfigInstalled = false;
        stateStore.Save(state);

        try
        {
            MaintenanceTask.Delete(logger);
        }
        catch (Exception ex) when (ex is InvalidOperationException or IOException)
        {
            report.Skipped++;
            report.Messages.Add($"Could not delete maintenance task: {ex.Message}");
        }

        if (report.ConfigChanged || report.Restored > 0)
        {
            TryRestartAudio(report);
        }

        return report;
    }

    public IReadOnlyList<EndpointSummary> Status() => new EndpointRegistry(stateStore, logger).List();

    internal static bool ShouldRestartAudio(bool forceAudioRestart, OperationReport report) =>
        forceAudioRestart || report.ConfigChanged || report.Attached > 0;

    private void ValidatePlatformAndEqualizerApo()
    {
        if (!OperatingSystem.IsWindowsVersionAtLeast(10) || !Environment.Is64BitOperatingSystem || !Environment.Is64BitProcess)
        {
            throw new PlatformNotSupportedException("Windows 10/11 x64 is required.");
        }

        var installedVersion = GetInstalledEqualizerApoVersion();
        if (!string.Equals(installedVersion, ProductConstants.EqualizerApoVersion, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                $"Equalizer APO {ProductConstants.EqualizerApoVersion} is required; found {installedVersion ?? "none"}.");
        }

    }

    private static string? GetInstalledEqualizerApoVersion()
    {
        using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, ProductConstants.RegistryView);
        using var uninstall = hklm.OpenSubKey(
            @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\EqualizerAPO",
            writable: false);
        return uninstall?.GetValue("DisplayVersion") as string;
    }

    private static string GetEqualizerApoConfigDirectory() =>
        TryGetEqualizerApoConfigDirectory()
        ?? throw new InvalidOperationException("Equalizer APO ConfigPath is missing from the registry.");

    private static string? TryGetEqualizerApoConfigDirectory()
    {
        using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, ProductConstants.RegistryView);
        using var product = hklm.OpenSubKey(ProductConstants.EqualizerApoRoot, writable: false);
        return product?.GetValue("ConfigPath") as string;
    }

    private void TryRestartAudio(OperationReport report)
    {
        try
        {
            AudioService.Restart(logger);
        }
        catch (Exception ex) when (ex is InvalidOperationException
            or System.ServiceProcess.TimeoutException
            or System.ComponentModel.Win32Exception)
        {
            report.Skipped++;
            report.Messages.Add($"Windows Audio could not be restarted; reboot to apply changes: {ex.Message}");
            logger.Write(report.Messages[^1]);
        }
    }

    private static void Merge(OperationReport target, OperationReport source)
    {
        target.Attached += source.Attached;
        target.AlreadyAttached += source.AlreadyAttached;
        target.Restored += source.Restored;
        target.Skipped += source.Skipped;
        target.Conflicts += source.Conflicts;
        target.Messages.AddRange(source.Messages);
    }
}
