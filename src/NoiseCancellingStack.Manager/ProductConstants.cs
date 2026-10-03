using Microsoft.Win32;

namespace NoiseCancellingStack;

public static class ProductConstants
{
    public const string ProductName = "Noise Cancelling Stack";
    public const string ProductVersion = "1.0.0";
    public const string StateSchemaVersion = "1";
    public const string EqualizerApoVersion = "1.4.2";
    public const string RnNoiseVersion = "1.10";

    public const string EqualizerApoPreMixClsid = "{EACD2258-FCAC-4FF4-B36D-419E924A6D79}";
    public const string EqualizerApoPostMixClsid = "{EC1CC9CE-FAED-4822-828A-82A81A6F018F}";
    public const string DefaultProcessingMode = "{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";

    public const string CaptureRoot = @"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture";
    public const string EqualizerApoRoot = @"SOFTWARE\EqualizerAPO";
    public const string ChildApoRoot = @"SOFTWARE\EqualizerAPO\Child APOs";

    public const string ConnectionName = "{a45c254e-df1c-4efd-8020-67d146a850e0},2";
    public const string DeviceName = "{b3f8fa53-0004-438e-9003-51a46e139bfc},6";
    public const string CombinedDevice = "{b3f8fa53-0004-438e-9003-51a46e139bfc},41";
    public const string Format = "{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0";

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
    public const string DisableEnhancements = "{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";

    public const string PreMixChild = "PreMixChild";
    public const string PostMixChild = "PostMixChild";
    public const string AllowSilentBufferModification = "AllowSilentBufferModification";
    public const string DisableAutomaticAdjustment = "DisableAutomaticAdjustment";
    public const string ChildVersion = "Version";

    public static readonly string[] ApoSlotNames = [Lfx, Gfx, Sfx, Mfx, Efx];
    public static readonly string[] ModernApoSlotNames = [Sfx, Mfx, Efx, MultiSfx, MultiMfx, MultiEfx];
    public static readonly string[] ManagedFxNames = [Lfx, Gfx, Sfx, Mfx, Efx, SfxModes, DisableEnhancements, FxTitle];
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

    public static RegistryView RegistryView => RegistryView.Registry64;
}

