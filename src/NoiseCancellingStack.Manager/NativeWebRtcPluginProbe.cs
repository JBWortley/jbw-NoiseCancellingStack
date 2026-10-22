// SPDX-License-Identifier: MIT
using System.Runtime.InteropServices;

namespace NoiseCancellingStack;

public sealed class NativeWebRtcPluginProbe : INativePluginProbe
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int SelfTest();

    public void Verify(string pluginPath)
    {
        var fullPath = Path.GetFullPath(pluginPath);
        nint module = 0;
        try
        {
            try
            {
                module = NativeLibrary.Load(fullPath);
            }
            catch (Exception ex) when (ex is DllNotFoundException or BadImageFormatException)
            {
                throw new InvalidDataException(
                    $"The WebRTC APM plug-in could not be loaded for its self-test: {fullPath}",
                    ex);
            }

            if (!NativeLibrary.TryGetExport(module, "NcsWebRtcApmSelfTest", out var export))
            {
                throw new InvalidDataException(
                    $"The WebRTC APM plug-in does not export NcsWebRtcApmSelfTest: {fullPath}");
            }

            var selfTest = Marshal.GetDelegateForFunctionPointer<SelfTest>(export);
            var result = selfTest();
            if (result != 0)
            {
                throw new InvalidDataException(
                    $"The WebRTC APM plug-in self-test returned {result}: {fullPath}");
            }
        }
        finally
        {
            if (module != 0)
            {
                NativeLibrary.Free(module);
            }
        }
    }
}
