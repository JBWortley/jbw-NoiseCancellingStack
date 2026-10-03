// SPDX-License-Identifier: MIT
using System.Text;
using System.Text.RegularExpressions;

namespace NoiseCancellingStack;

public static partial class ConfigManager
{
    // The main config receives only a marked include. The separate managed file lets uninstall
    // remove this project without rewriting unrelated Equalizer APO filters.
    public const string BeginMarker = "# BEGIN NoiseCancellingStack managed block";
    public const string EndMarker = "# END NoiseCancellingStack managed block";
    public const string ManagedFileName = "NoiseCancellingStack.txt";

    private const string LegacyComment = "# Real-time noise suppression for every capture endpoint hosting Equalizer APO.";

    public static bool Install(string configDirectory, string pluginPath, string backupDirectory, OperationLogger logger)
    {
        ValidatePath(pluginPath);
        Directory.CreateDirectory(configDirectory);
        Directory.CreateDirectory(backupDirectory);

        var mainPath = Path.Combine(configDirectory, "config.txt");
        var managedPath = Path.Combine(configDirectory, ManagedFileName);
        var original = File.Exists(mainPath) ? File.ReadAllText(mainPath) : string.Empty;
        var updatedBase = IsKnownLegacyOnlyConfig(original) ? string.Empty : StripManagedBlock(original);
        var includeBlock = $"{BeginMarker}{Environment.NewLine}Include: {ManagedFileName}{Environment.NewLine}{EndMarker}";
        var updated = AppendBlock(updatedBase, includeBlock);
        var managed = BuildManagedConfig(pluginPath);

        if (File.Exists(mainPath))
        {
            var backup = Path.Combine(backupDirectory, $"config-{DateTime.Now:yyyyMMdd-HHmmss-fff}.txt");
            File.Copy(mainPath, backup, overwrite: false);
            logger.Write($"Backed up Equalizer APO config to {backup}");
        }

        var changed = !string.Equals(original, updated, StringComparison.Ordinal)
            || !File.Exists(managedPath)
            || !string.Equals(File.ReadAllText(managedPath), managed, StringComparison.Ordinal);

        AtomicWrite(managedPath, managed);
        AtomicWrite(mainPath, updated);
        logger.Write("Installed the capture-only Equalizer APO configuration block.");
        return changed;
    }

    public static bool IsManagedInstallation(string configDirectory, string pluginPath)
    {
        ValidatePath(pluginPath);
        var mainPath = Path.Combine(configDirectory, "config.txt");
        var managedPath = Path.Combine(configDirectory, ManagedFileName);
        if (!File.Exists(mainPath) || !File.Exists(managedPath))
        {
            return false;
        }

        var main = File.ReadAllText(mainPath).Replace("\r\n", "\n", StringComparison.Ordinal);
        var expectedBlock = $"{BeginMarker}\nInclude: {ManagedFileName}\n{EndMarker}";
        return main.Contains(expectedBlock, StringComparison.Ordinal)
            && string.Equals(File.ReadAllText(managedPath), BuildManagedConfig(pluginPath), StringComparison.Ordinal);
    }

    public static bool Uninstall(string configDirectory, string pluginPath, OperationLogger logger)
    {
        var mainPath = Path.Combine(configDirectory, "config.txt");
        var managedPath = Path.Combine(configDirectory, ManagedFileName);
        var changed = false;

        if (File.Exists(mainPath))
        {
            var original = File.ReadAllText(mainPath);
            var updated = StripManagedBlock(original);
            if (!string.Equals(original, updated, StringComparison.Ordinal))
            {
                AtomicWrite(mainPath, updated);
                changed = true;
                logger.Write("Removed the managed include from Equalizer APO config.txt.");
            }
        }

        if (File.Exists(managedPath))
        {
            var expected = BuildManagedConfig(pluginPath);
            var actual = File.ReadAllText(managedPath);
            if (string.Equals(actual, expected, StringComparison.Ordinal))
            {
                File.Delete(managedPath);
                changed = true;
                logger.Write("Removed the unmodified managed RNNoise configuration file.");
            }
            else
            {
                logger.Write($"Kept user-modified file during uninstall: {managedPath}");
            }
        }

        return changed;
    }

    public static string BuildManagedConfig(string pluginPath)
    {
        ValidatePath(pluginPath);
        var encodedPath = pluginPath.Contains(' ', StringComparison.Ordinal) ? $"\"{pluginPath}\"" : pluginPath;
        // Both guards are safety boundaries: capture prevents speaker processing, and 48 kHz is
        // the only input rate supported by this pinned RNNoise plugin release.
        return string.Join(Environment.NewLine,
        [
            "# Managed by Noise Cancelling Stack. Changes may be replaced during repair.",
            "# RNNoise v1.10 requires exactly 48000 Hz; unsupported streams pass through unchanged.",
            "Device: all",
            "If: stage == \"capture\"",
            "    If: sampleRate == 48000",
            "        Channel: all",
            $"        VSTPlugin: Library {encodedPath}",
            "    EndIf:",
            "EndIf:",
            string.Empty
        ]);
    }

    public static string StripManagedBlock(string input)
    {
        var normalized = input.Replace("\r\n", "\n", StringComparison.Ordinal);
        var stripped = ManagedBlockRegex().Replace(normalized, string.Empty);
        stripped = Regex.Replace(stripped, "\\n{3,}", "\n\n", RegexOptions.CultureInvariant);
        return stripped.TrimEnd('\n', '\r', ' ', '\t') is { Length: > 0 } value
            ? value.Replace("\n", Environment.NewLine, StringComparison.Ordinal) + Environment.NewLine
            : string.Empty;
    }

    public static bool IsKnownLegacyOnlyConfig(string input)
    {
        var normalized = input.Replace("\r\n", "\n", StringComparison.Ordinal).Trim();
        return normalized.StartsWith(LegacyComment, StringComparison.Ordinal)
            && normalized.Contains("VSTPlugin: Library C:\\audio\\win-rnnoise\\vst\\rnnoise_mono.dll", StringComparison.Ordinal)
            && normalized.EndsWith("EndIf:\nEndIf:", StringComparison.Ordinal);
    }

    private static string AppendBlock(string input, string block)
    {
        var prefix = input.TrimEnd('\r', '\n', ' ', '\t');
        return prefix.Length == 0
            ? block + Environment.NewLine
            : prefix + Environment.NewLine + Environment.NewLine + block + Environment.NewLine;
    }

    private static void AtomicWrite(string path, string content)
    {
        // Same-directory replacement avoids a partially written live config and stays on the same
        // volume, which gives File.Move its strongest replacement semantics on Windows.
        var temporary = path + $".{Guid.NewGuid():N}.new";
        try
        {
            File.WriteAllText(temporary, content, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            File.Move(temporary, path, true);
        }
        finally
        {
            File.Delete(temporary);
        }
    }

    private static void ValidatePath(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || path.IndexOfAny(['\r', '\n']) >= 0)
        {
            throw new ArgumentException("The plugin path is invalid.", nameof(path));
        }
    }

    [GeneratedRegex(
        "(?ms)^# BEGIN NoiseCancellingStack managed block\\s*\\n.*?^# END NoiseCancellingStack managed block\\s*(?:\\n|$)",
        RegexOptions.CultureInvariant)]
    private static partial Regex ManagedBlockRegex();
}
