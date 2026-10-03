using System.Text.Json.Serialization;
using Microsoft.Win32;

namespace NoiseCancellingStack;

public enum EndpointInstallMode
{
    LfxGfx,
    SfxMfx,
    SfxEfx
}

public sealed class InstallState
{
    public string SchemaVersion { get; set; } = ProductConstants.StateSchemaVersion;
    public string ProductVersion { get; set; } = ProductConstants.ProductVersion;
    public DateTimeOffset CreatedUtc { get; set; } = DateTimeOffset.UtcNow;
    public DateTimeOffset UpdatedUtc { get; set; } = DateTimeOffset.UtcNow;
    public bool ConfigInstalled { get; set; }
    public string? EqualizerApoConfigPath { get; set; }
    public List<EndpointState> Endpoints { get; set; } = [];
}

public sealed class EndpointState
{
    public required string EndpointId { get; set; }
    public string DisplayName { get; set; } = "Unknown microphone";
    public uint SampleRate { get; set; }
    public EndpointInstallMode Mode { get; set; }
    public bool Pending { get; set; } = true;
    public bool FxKeyExisted { get; set; }
    public bool ChildKeyExisted { get; set; }
    public Dictionary<string, RegistryValueSnapshot> OriginalFx { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    public Dictionary<string, RegistryValueSnapshot> InstalledFx { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    public Dictionary<string, RegistryValueSnapshot> OriginalChild { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    public Dictionary<string, RegistryValueSnapshot> InstalledChild { get; set; } = new(StringComparer.OrdinalIgnoreCase);
}

public sealed class RegistryValueSnapshot
{
    public bool Exists { get; set; }
    public RegistryValueKind Kind { get; set; } = RegistryValueKind.Unknown;
    public string? StringValue { get; set; }
    public string[]? StringArrayValue { get; set; }
    public string? BinaryBase64Value { get; set; }
    public long? IntegerValue { get; set; }

    public static RegistryValueSnapshot Missing() => new() { Exists = false };

    public static RegistryValueSnapshot Capture(RegistryKey key, string name)
    {
        if (!key.GetValueNames().Contains(name, StringComparer.OrdinalIgnoreCase))
        {
            return Missing();
        }

        var kind = key.GetValueKind(name);
        var value = key.GetValue(name, null, RegistryValueOptions.DoNotExpandEnvironmentNames);
        return FromValue(kind, value);
    }

    public static RegistryValueSnapshot FromValue(RegistryValueKind kind, object? value)
    {
        var snapshot = new RegistryValueSnapshot { Exists = true, Kind = kind };
        switch (kind)
        {
            case RegistryValueKind.String:
            case RegistryValueKind.ExpandString:
                snapshot.StringValue = Convert.ToString(value, System.Globalization.CultureInfo.InvariantCulture) ?? string.Empty;
                break;
            case RegistryValueKind.MultiString:
                snapshot.StringArrayValue = value as string[] ?? [];
                break;
            case RegistryValueKind.Binary:
            case RegistryValueKind.None:
                snapshot.BinaryBase64Value = Convert.ToBase64String(value as byte[] ?? []);
                break;
            case RegistryValueKind.DWord:
                snapshot.IntegerValue = Convert.ToInt64(value, System.Globalization.CultureInfo.InvariantCulture);
                break;
            case RegistryValueKind.QWord:
                snapshot.IntegerValue = Convert.ToInt64(value, System.Globalization.CultureInfo.InvariantCulture);
                break;
            default:
                throw new NotSupportedException($"Unsupported registry value kind: {kind}");
        }

        return snapshot;
    }

    public void Apply(RegistryKey key, string name)
    {
        if (!Exists)
        {
            key.DeleteValue(name, false);
            return;
        }

        object value = Kind switch
        {
            RegistryValueKind.String or RegistryValueKind.ExpandString => StringValue ?? string.Empty,
            RegistryValueKind.MultiString => StringArrayValue ?? [],
            RegistryValueKind.Binary or RegistryValueKind.None => Convert.FromBase64String(BinaryBase64Value ?? string.Empty),
            RegistryValueKind.DWord => checked((int)(IntegerValue ?? 0)),
            RegistryValueKind.QWord => IntegerValue ?? 0,
            _ => throw new NotSupportedException($"Unsupported registry value kind: {Kind}")
        };
        key.SetValue(name, value, Kind);
    }

    public bool EquivalentTo(RegistryValueSnapshot other)
    {
        if (Exists != other.Exists)
        {
            return false;
        }

        if (!Exists)
        {
            return true;
        }

        return Kind == other.Kind
            && string.Equals(StringValue, other.StringValue, StringComparison.Ordinal)
            && (StringArrayValue ?? []).SequenceEqual(other.StringArrayValue ?? [], StringComparer.Ordinal)
            && string.Equals(BinaryBase64Value, other.BinaryBase64Value, StringComparison.Ordinal)
            && IntegerValue == other.IntegerValue;
    }
}

public sealed record EndpointSummary(string EndpointId, string DisplayName, uint SampleRate, bool EqualizerApoAttached);

public sealed class OperationReport
{
    public int Attached { get; set; }
    public int AlreadyAttached { get; set; }
    public int Restored { get; set; }
    public int Skipped { get; set; }
    public int Conflicts { get; set; }
    public bool ConfigChanged { get; set; }
    public List<string> Messages { get; } = [];
    public bool HasWarnings => Skipped > 0 || Conflicts > 0;
}

[JsonSourceGenerationOptions(WriteIndented = true, UseStringEnumConverter = true)]
[JsonSerializable(typeof(InstallState))]
internal sealed partial class InstallStateJsonContext : JsonSerializerContext;

