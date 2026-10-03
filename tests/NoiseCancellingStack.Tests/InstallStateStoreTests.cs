using Xunit;

namespace NoiseCancellingStack.Tests;

public sealed class InstallStateStoreTests
{
    [Fact]
    public void SaveAndLoad_RoundTripsValidState()
    {
        WithStore((store, _) =>
        {
            var state = new InstallState();
            state.Endpoints.Add(ValidEndpoint("{11111111-1111-1111-1111-111111111111}"));

            store.Save(state);

            var loaded = store.Load();
            Assert.Single(loaded.Endpoints);
            Assert.Equal(state.Endpoints[0].EndpointId, loaded.Endpoints[0].EndpointId);
        });
    }

    [Fact]
    public void Save_RejectsInvalidEndpointIdentifier()
    {
        WithStore((store, _) =>
        {
            var state = new InstallState();
            state.Endpoints.Add(ValidEndpoint(@"..\outside"));

            Assert.Throws<InvalidDataException>(() => store.Save(state));
        });
    }

    [Fact]
    public void Save_RejectsUnexpectedRegistryNames()
    {
        WithStore((store, _) =>
        {
            var state = new InstallState();
            var endpoint = ValidEndpoint("{22222222-2222-2222-2222-222222222222}");
            endpoint.OriginalFx["unexpected"] = RegistryValueSnapshot.Missing();
            state.Endpoints.Add(endpoint);

            Assert.Throws<InvalidDataException>(() => store.Save(state));
        });
    }

    [Fact]
    public void Load_RejectsNullEndpointCollection()
    {
        WithStore((store, path) =>
        {
            File.WriteAllText(path, "{\"SchemaVersion\":\"1\",\"Endpoints\":null}");
            Assert.Throws<InvalidDataException>(() => store.Load());
        });
    }

    private static EndpointState ValidEndpoint(string id) => new()
    {
        EndpointId = id,
        OriginalFx = Missing(ProductConstants.ManagedFxNames),
        InstalledFx = Missing(ProductConstants.ManagedFxNames),
        OriginalChild = Missing(ProductConstants.ManagedChildNames),
        InstalledChild = Missing(ProductConstants.ManagedChildNames)
    };

    private static Dictionary<string, RegistryValueSnapshot> Missing(IEnumerable<string> names) =>
        names.ToDictionary(name => name, _ => RegistryValueSnapshot.Missing(), StringComparer.OrdinalIgnoreCase);

    private static void WithStore(Action<InstallStateStore, string> test)
    {
        var directory = Path.Combine(Path.GetTempPath(), $"NoiseCancellingStack-tests-{Guid.NewGuid():N}");
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, "state.json");
        try
        {
            test(new InstallStateStore(path), path);
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }
}
