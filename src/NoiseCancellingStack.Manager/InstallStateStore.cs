using System.Text.Json;

namespace NoiseCancellingStack;

public sealed class InstallStateStore
{
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
            RequireExactKeys(endpoint.OriginalFx, ProductConstants.ManagedFxNames, "OriginalFx");
            RequireExactKeys(endpoint.InstalledFx, ProductConstants.ManagedFxNames, "InstalledFx");
            RequireExactKeys(endpoint.OriginalChild, ProductConstants.ManagedChildNames, "OriginalChild");
            RequireExactKeys(endpoint.InstalledChild, ProductConstants.ManagedChildNames, "InstalledChild");
        }
    }

    private static void RequireExactKeys(
        IReadOnlyDictionary<string, RegistryValueSnapshot>? values,
        IEnumerable<string> expected,
        string field)
    {
        if (values is null
            || values.Values.Any(value => value is null)
            || !values.Keys.ToHashSet(StringComparer.OrdinalIgnoreCase).SetEquals(expected))
        {
            throw new InvalidDataException($"Unexpected registry value names in state field {field}.");
        }
    }
}
