// SPDX-License-Identifier: MIT
using System.Runtime.InteropServices;

namespace NoiseCancellingStack;

/// <summary>
/// Loads a hash-verified native payload by absolute path and runs its synthetic self-test export,
/// which must return 0. Used before any configuration or endpoint mutation.
/// </summary>
public sealed class NativeSelfTestProbe : INativePluginProbe
{
    public static NativeSelfTestProbe WebRtcPlugin { get; } = new("WebRTC APM plug-in", "NcsWebRtcApmSelfTest");
    public static NativeSelfTestProbe EchoCanceller { get; } = new("echo cancellation APO", "NcsAecApoSelfTest");

    private readonly string payloadName;
    private readonly string exportName;

    public NativeSelfTestProbe(string payloadName, string exportName)
    {
        this.payloadName = payloadName;
        this.exportName = exportName;
    }

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
                throw new InvalidDataException($"The {payloadName} could not be loaded for its self-test: {fullPath}", ex);
            }

            if (!NativeLibrary.TryGetExport(module, exportName, out var export))
            {
                throw new InvalidDataException($"The {payloadName} does not export {exportName}: {fullPath}");
            }

            var result = Marshal.GetDelegateForFunctionPointer<SelfTest>(export)();
            if (result != 0)
            {
                throw new InvalidDataException($"The {payloadName} self-test returned {result}: {fullPath}");
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
