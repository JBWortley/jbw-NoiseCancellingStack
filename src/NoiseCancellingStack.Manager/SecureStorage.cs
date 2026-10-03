// SPDX-License-Identifier: MIT
namespace NoiseCancellingStack;

/// <summary>
/// Creates the product-data root with a narrow ACL and refuses reparse points before elevated code
/// writes logs, backups, or state. Existing descendants are reset to inherit the protected ACL.
/// </summary>
public static class SecureStorage
{
    public static void Ensure()
    {
        var root = Path.GetFullPath(ProductConstants.ProgramDataDirectory);
        var programData = Path.GetFullPath(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData))
            .TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (!root.StartsWith(programData, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException("The product data directory resolved outside ProgramData.");
        }

        if (Directory.Exists(root) && IsReparsePoint(root))
        {
            throw new IOException($"Refusing to use a reparse point as product data: {root}");
        }

        Directory.CreateDirectory(root);
        var exitCode = ProcessRunner.Run(
            "icacls.exe",
            [
                root,
                "/inheritance:r",
                "/grant:r",
                "*S-1-5-18:(OI)(CI)F",
                "*S-1-5-32-544:(OI)(CI)F",
                "*S-1-5-32-545:(OI)(CI)RX"
            ],
            hidden: true);
        if (exitCode != 0)
        {
            throw new UnauthorizedAccessException($"Could not secure product data directory; icacls exited {exitCode}.");
        }

        var entries = Directory.EnumerateFileSystemEntries(root, "*", SearchOption.AllDirectories).ToArray();
        foreach (var entry in entries)
        {
            if (IsReparsePoint(entry))
            {
                throw new IOException($"Refusing to follow a reparse point in product data: {entry}");
            }
        }

        if (entries.Length > 0)
        {
            exitCode = ProcessRunner.Run(
                "icacls.exe",
                [Path.Combine(root, "*"), "/reset", "/T", "/C", "/Q", "/L"],
                hidden: true);
            if (exitCode != 0)
            {
                throw new UnauthorizedAccessException(
                    $"Could not secure existing product data contents; icacls exited {exitCode}.");
            }
        }
    }

    private static bool IsReparsePoint(string path) =>
        (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0;
}
