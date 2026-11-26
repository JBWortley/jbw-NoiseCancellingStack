// SPDX-License-Identifier: MIT
using Microsoft.Win32;

namespace NoiseCancellingStack;

public static class ProductConstants
{
    public const string ProductName = "Noise Cancelling Stack";
    public const string ProductVersion = "1.2.0";
    public const string StateSchemaVersion = "1";
    public const string EqualizerApoVersion = "1.4.2";
    public const string RnNoiseVersion = "1.10";
    public const string RnNoisePluginSha256 = "7DEBDFC4843461C47B23AC7A736B4663FE581643563AE764CDE395E006004080";

    public const string EqualizerApoPreMixClsid = "{EACD2258-FCAC-4FF4-B36D-419E924A6D79}";
    public const string EqualizerApoPostMixClsid = "{EC1CC9CE-FAED-4822-828A-82A81A6F018F}";
    public const string DefaultProcessingMode = "{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";

    // Project-built acoustic echo cancellation APO (capture mode effect). Windows supplies its
    // loopback reference only on Windows 11 (build 22000) and later.
    public const string EchoCancellerClsid = "{6F0DE50F-5FE3-47EA-BA01-D9AAB0788EF7}";
    public const int EchoCancellerMinimumWindowsBuild = 22000;
    public const string SettingsRoot = @"SOFTWARE\NoiseCancellingStack";
    public const string DisableEchoCancellationValue = "DisableEchoCancellation";

    public const string CaptureRoot = @"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture";
    public const string EqualizerApoRoot = @"SOFTWARE\EqualizerAPO";
    public const string ChildApoRoot = @"SOFTWARE\EqualizerAPO\Child APOs";

    // Windows property keys and APO-slot property names are represented by their canonical
    // fmtid/pid strings in the endpoint registry. Keep these centralized and case-stable.
    public const string ConnectionName = "{a45c254e-df1c-4efd-8020-67d146a850e0},2";
    public const string DeviceName = "{b3f8fa53-0004-438e-9003-51a46e139bfc},6";
    public const string CombinedDevice = "{b3f8fa53-0004-438e-9003-51a46e139bfc},41";
    public const string Format = "{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0";
    public const string FormFactor = "{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},0";

    public const string Lfx = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1";
    public const string Gfx = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2";
    public const string Sfx = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";
    public const string Mfx = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6";
    public const string Efx = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7";
    public const string MultiSfx = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13";
    public const string MultiMfx = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14";
    public const string MultiEfx = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15";
    public const string FxTitle = "{b725f130-47ef-101a-a5f1-02608c9eebac},10";
    public const string SfxModes = "{d3993a3f-99c2-4402-b5ec-a92a0367664b},5";
    public const string MfxModes = "{d3993a3f-99c2-4402-b5ec-a92a0367664b},6";
    public const string DisableEnhancements = "{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";

    public const string PreMixChild = "PreMixChild";
    public const string PostMixChild = "PostMixChild";
    public const string AllowSilentBufferModification = "AllowSilentBufferModification";
    public const string DisableAutomaticAdjustment = "DisableAutomaticAdjustment";
    public const string ChildVersion = "Version";

    public static readonly string[] ApoSlotNames = [Lfx, Gfx, Sfx, Mfx, Efx];
    public static readonly string[] ModernApoSlotNames = [Sfx, Mfx, Efx, MultiSfx, MultiMfx, MultiEfx];
    public static readonly string[] ManagedFxNames = [Lfx, Gfx, Sfx, Mfx, Efx, SfxModes, DisableEnhancements, FxTitle];
    // Journaled only for endpoints that received the echo canceller (1.2.0+). Older journals omit
    // it, so state validation accepts it as an optional pair present in both original and installed.
    public static readonly string[] OptionalManagedFxNames = [MfxModes];
    // The complete journal of an echo-canceller-only endpoint (Equalizer APO attached elsewhere).
    public static readonly string[] EchoCancellerOnlyFxNames = [Mfx, MfxModes];

    // PKEY_Devices_AudioDevice_RawProcessingSupported. Chromium-based applications request raw
    // capture, which bypasses every capture effect, whenever this is true. The Audio Endpoint
    // Builder rewrites it from the driver on every endpoint rebuild (boot, replug).
    public const string RawProcessingSupported = "{8943b373-388c-4395-b557-bc6dbaffafdb},2";
    // Endpoint Properties values the manager may own on a filtered microphone: its name (the
    // renameable device description, ConnectionName) and RawProcessingSupported.
    public static readonly string[] ManagedPropertyNames = [ConnectionName, RawProcessingSupported];
    public const string FilteredLabelSuffix = " FILTERED";
    public static readonly string[] ManagedChildNames =
    [
        Lfx, Gfx, Sfx, Mfx, Efx, PreMixChild, PostMixChild,
        AllowSilentBufferModification, DisableAutomaticAdjustment, ChildVersion
    ];

    public static string ProgramDataDirectory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "NoiseCancellingStack");

    public static string StatePath => Path.Combine(ProgramDataDirectory, "state.json");
    public static string LogDirectory => Path.Combine(ProgramDataDirectory, "logs");
    public static string BackupDirectory => Path.Combine(ProgramDataDirectory, "backups");
    public static string InstallDirectory => AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
    public static string MonoPluginPath => Path.Combine(InstallDirectory, "plugins", "rnnoise_mono.dll");
    public static string WebRtcPluginPath => Path.Combine(InstallDirectory, "plugins", "webrtc_apm_16k.dll");
    public static string EchoCancellerPath => Path.Combine(InstallDirectory, "apo", "ncs_aec_apo.dll");

    public static RegistryView RegistryView => RegistryView.Registry64;
}
