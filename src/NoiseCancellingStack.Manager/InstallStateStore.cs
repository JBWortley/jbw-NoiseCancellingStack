// SPDX-License-Identifier: MIT
using System.Text.Json;

namespace NoiseCancellingStack;

public sealed class InstallStateStore
{
    // The journal influences elevated registry restoration and is therefore treated as untrusted
    // input even though SecureStorage restricts ordinary users from modifying it after creation.
    private readonly string path;

    public InstallStateStore(string? path = null)
    {
        this.path = path ?? ProductConstants.StatePath;
    }

    public InstallState Load()
    {
        if (!File.Exists(path))
        {
            return new InstallState();
        }

        var json = File.ReadAllText(path);
        var state = JsonSerializer.Deserialize(json, InstallStateJsonContext.Default.InstallState)
            ?? throw new InvalidDataException($"The installation state is empty: {path}");
        if (!string.Equals(state.SchemaVersion, ProductConstants.StateSchemaVersion, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                $"Unsupported state schema {state.SchemaVersion}; expected {ProductConstants.StateSchemaVersion}.");
        }

        Validate(state);
        return state;
    }

    public void Save(InstallState state)
    {
        var directory = Path.GetDirectoryName(path)
            ?? throw new InvalidOperationException("The state path has no parent directory.");
        Directory.CreateDirectory(directory);
        state.ProductVersion = ProductConstants.ProductVersion;
        state.UpdatedUtc = DateTimeOffset.UtcNow;
        Validate(state);
        var temporary = path + $".{Guid.NewGuid():N}.new";
        try
        {
            File.WriteAllText(temporary, JsonSerializer.Serialize(state, InstallStateJsonContext.Default.InstallState));
            File.Move(temporary, path, true);
        }
        finally
        {
            File.Delete(temporary);
        }
    }

    private static void Validate(InstallState state)
    {
        if (state.Endpoints is null)
        {
            throw new InvalidDataException("The state endpoint collection is missing.");
        }
        if (state.Endpoints.Count > 4096)
        {
            throw new InvalidDataException("The state contains an unreasonable number of endpoints.");
        }

        // Braced GUIDs are the only valid child names below the fixed capture registry roots. This
        // blocks path-like identifiers and duplicate records before any mutation is attempted.
        var ids = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var endpoint in state.Endpoints)
        {
            if (endpoint is null)
            {
                throw new InvalidDataException("The state contains a null endpoint.");
            }
            if (!Guid.TryParseExact(endpoint.EndpointId, "B", out _))
            {
                throw new InvalidDataException($"Invalid endpoint identifier in state: {endpoint.EndpointId}");
            }
            if (!ids.Add(endpoint.EndpointId))
            {
                throw new InvalidDataException($"Duplicate endpoint identifier in state: {endpoint.EndpointId}");
            }
            RequireKeys(endpoint.OriginalProperties, [], ProductConstants.ManagedPropertyNames, "OriginalProperties");
            RequireKeys(endpoint.InstalledProperties, [], ProductConstants.ManagedPropertyNames, "InstalledProperties");
            if (!endpoint.OriginalProperties!.Keys.ToHashSet(StringComparer.OrdinalIgnoreCase).SetEquals(endpoint.InstalledProperties!.Keys))
            {
                throw new InvalidDataException("State fields OriginalProperties and InstalledProperties track different values.");
            }
            if (endpoint.EchoCancellerOnly)
            {
                // Owns exactly the MFX value and MFX mode list of a key that already existed, and no
                // Equalizer APO child record, so restoration can never touch Equalizer APO's values.
                RequireKeys(endpoint.OriginalFx, ProductConstants.EchoCancellerOnlyFxNames, [], "OriginalFx");
                RequireKeys(endpoint.InstalledFx, ProductConstants.EchoCancellerOnlyFxNames, [], "InstalledFx");
                RequireKeys(endpoint.OriginalChild, [], [], "OriginalChild");
                RequireKeys(endpoint.InstalledChild, [], [], "InstalledChild");
                if (!endpoint.FxKeyExisted || endpoint.Mode != EndpointInstallMode.SfxEfx)
                {
                    throw new InvalidDataException("An echo-canceller-only endpoint record is inconsistent.");
                }
                continue;
            }
            RequireKeys(endpoint.OriginalFx, ProductConstants.ManagedFxNames, ProductConstants.OptionalManagedFxNames, "OriginalFx");
            RequireKeys(endpoint.InstalledFx, ProductConstants.ManagedFxNames, ProductConstants.OptionalManagedFxNames, "InstalledFx");
            // An optional value is journaled as an original/installed pair or not at all; a half
            // record could not be restored safely.
            if (!endpoint.OriginalFx!.Keys.ToHashSet(StringComparer.OrdinalIgnoreCase).SetEquals(endpoint.InstalledFx!.Keys))
            {
                throw new InvalidDataException("State fields OriginalFx and InstalledFx track different registry values.");
            }
            RequireKeys(endpoint.OriginalChild, ProductConstants.ManagedChildNames, [], "OriginalChild");
            RequireKeys(endpoint.InstalledChild, ProductConstants.ManagedChildNames, [], "InstalledChild");
        }
    }

    // Every required name must be present; optional names may be present; nothing else may be.
    private static void RequireKeys(
        IReadOnlyDictionary<string, RegistryValueSnapshot>? values,
        IEnumerable<string> required,
        IEnumerable<string> optional,
        string field)
    {
        if (values is null || values.Values.Any(value => value is null))
        {
            throw new InvalidDataException($"Unexpected registry value names in state field {field}.");
        }
        var keys = values.Keys.ToHashSet(StringComparer.OrdinalIgnoreCase);
        var allowed = required.Concat(optional).ToHashSet(StringComparer.OrdinalIgnoreCase);
        if (!keys.IsSupersetOf(required) || !keys.IsSubsetOf(allowed))
        {
            throw new InvalidDataException($"Unexpected registry value names in state field {field}.");
        }
    }
}
