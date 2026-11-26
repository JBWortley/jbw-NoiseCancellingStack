// SPDX-License-Identifier: MIT
using Microsoft.Win32;

namespace NoiseCancellingStack;

/// <summary>
/// Confirms that the installer registered the echo canceller COM class and APO at the expected,
/// administrator-owned path. An endpoint must never point its MFX slot at a CLSID that cannot be
/// created, because the audio engine would then fail to build that microphone's capture graph.
/// </summary>
public static class EchoCancellerRegistration
{
    public static EchoCancellerContext DetectContext() =>
        new(Environment.OSVersion.Version.Build, IsRegistered(ProductConstants.EchoCancellerPath));

    public static bool IsRegistered(string expectedPath)
    {
        var expected = Path.GetFullPath(expectedPath);
        if (!File.Exists(expected))
        {
            return false;
        }

        using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, ProductConstants.RegistryView);
        using var server = hklm.OpenSubKey(
            $@"SOFTWARE\Classes\CLSID\{ProductConstants.EchoCancellerClsid}\InprocServer32",
            writable: false);
        using var apo = hklm.OpenSubKey(
            $@"SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\{ProductConstants.EchoCancellerClsid}",
            writable: false);
        return server?.GetValue(null) is string registered
            && apo is not null
            && string.Equals(Path.GetFullPath(registered), expected, StringComparison.OrdinalIgnoreCase);
    }
}
