// SPDX-License-Identifier: MIT
using Microsoft.Win32;

namespace NoiseCancellingStack;

public static class SafetyBackup
{
    // Coarse full-key exports complement the precise state journal: the journal powers automatic
    // rollback, while these files remain available for manual disaster recovery.
    public static string Create(string operation, string? configDirectory, OperationLogger logger)
    {
        var safeOperation = string.Concat(operation.Where(c => char.IsLetterOrDigit(c) || c == '-'));
        var destination = Path.Combine(
            ProductConstants.BackupDirectory,
            $"{DateTime.Now:yyyyMMdd-HHmmss-fff}-{safeOperation}");
        Directory.CreateDirectory(destination);

        ExportRegistry(
            @"HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture",
            Path.Combine(destination, "capture-endpoints.reg"),
            logger);

        using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, ProductConstants.RegistryView);
        if (hklm.OpenSubKey(ProductConstants.EqualizerApoRoot, writable: false) is not null)
        {
            ExportRegistry(
                @"HKLM\SOFTWARE\EqualizerAPO",
                Path.Combine(destination, "equalizer-apo.reg"),
                logger);
        }

        if (!string.IsNullOrWhiteSpace(configDirectory))
        {
            var configPath = Path.Combine(configDirectory, "config.txt");
            if (File.Exists(configPath))
            {
                File.Copy(configPath, Path.Combine(destination, "config.txt"), overwrite: false);
            }
        }

        logger.Write($"Created safety backup: {destination}");
        return destination;
    }

    private static void ExportRegistry(string key, string output, OperationLogger logger)
    {
        var exitCode = ProcessRunner.Run("reg.exe", ["export", key, output, "/y"], hidden: true, logger);
        if (exitCode != 0 || !File.Exists(output))
        {
            throw new InvalidOperationException($"Could not export registry key {key}.");
        }
    }
}
