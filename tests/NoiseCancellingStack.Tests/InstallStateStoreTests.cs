// SPDX-License-Identifier: MIT
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
    public void Save_StampsCurrentProductVersionWhenUpgradingOldJournal()
    {
        WithStore((store, _) =>
        {
            var state = new InstallState { ProductVersion = "1.0.0" };

            store.Save(state);

            Assert.Equal("1.2.0", store.Load().ProductVersion);
        });
    }

    [Fact]
    public void SaveAndLoad_AcceptsEchoCancellerModeListAsOriginalAndInstalledPair()
    {
        WithStore((store, _) =>
        {
            var state = new InstallState();
            var endpoint = ValidEndpoint("{33333333-3333-3333-3333-333333333333}");
            endpoint.OriginalFx[ProductConstants.MfxModes] = RegistryValueSnapshot.Missing();
            endpoint.InstalledFx[ProductConstants.MfxModes] = RegistryValueSnapshot.FromValue(
                Microsoft.Win32.RegistryValueKind.MultiString,
                new[] { ProductConstants.DefaultProcessingMode });
            state.Endpoints.Add(endpoint);

            store.Save(state);

            var loaded = store.Load().Endpoints.Single();
            Assert.True(loaded.InstalledFx.ContainsKey(ProductConstants.MfxModes));
            Assert.True(loaded.OriginalFx.ContainsKey(ProductConstants.MfxModes));
        });
    }

    [Fact]
    public void Load_AcceptsPreEchoCancellerJournalShape()
    {
        WithStore((store, _) =>
        {
            // 1.0 and 1.1 journals track exactly the required names and nothing optional.
            var state = new InstallState();
            state.Endpoints.Add(ValidEndpoint("{44444444-4444-4444-4444-444444444444}"));
            store.Save(state);

            var loaded = store.Load().Endpoints.Single();
            Assert.False(loaded.InstalledFx.ContainsKey(ProductConstants.MfxModes));
        });
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void Save_RejectsOptionalValueTrackedOnOnlyOneSide(bool onlyOriginal)
    {
        WithStore((store, _) =>
        {
            var state = new InstallState();
            var endpoint = ValidEndpoint("{55555555-5555-5555-5555-555555555555}");
            (onlyOriginal ? endpoint.OriginalFx : endpoint.InstalledFx)[ProductConstants.MfxModes] = RegistryValueSnapshot.Missing();
            state.Endpoints.Add(endpoint);

            Assert.Throws<InvalidDataException>(() => store.Save(state));
        });
    }

    [Fact]
    public void SaveAndLoad_AcceptsEchoCancellerOnlyRecord()
    {
        WithStore((store, _) =>
        {
            var state = new InstallState();
            state.Endpoints.Add(EchoCancellerOnlyEndpoint("{66666666-6666-6666-6666-666666666666}"));

            store.Save(state);

            var loaded = store.Load().Endpoints.Single();
            Assert.True(loaded.EchoCancellerOnly);
            Assert.Equal(2, loaded.InstalledFx.Count);
            Assert.Empty(loaded.InstalledChild);
        });
    }

    [Fact]
    public void Load_ReadsPreEchoCancellerOnlyJournalsAsFullRecords()
    {
        WithStore((store, path) =>
        {
            var state = new InstallState();
            state.Endpoints.Add(ValidEndpoint("{77777777-7777-7777-7777-777777777777}"));
            store.Save(state);
            // A 1.0/1.1 journal has no EchoCancellerOnly property at all.
            File.WriteAllText(path, File.ReadAllText(path).Replace("\"EchoCancellerOnly\": false,", string.Empty));
            Assert.DoesNotContain("EchoCancellerOnly", File.ReadAllText(path));

            Assert.False(store.Load().Endpoints.Single().EchoCancellerOnly);
        });
    }

    [Fact]
    public void Save_RejectsEchoCancellerOnlyRecordThatOwnsEqualizerApoValues()
    {
        WithStore((store, _) =>
        {
            // Such a record must never be able to restore Equalizer APO's own slot or child record.
            var withSfx = EchoCancellerOnlyEndpoint("{88888888-8888-8888-8888-888888888888}");
            withSfx.OriginalFx[ProductConstants.Sfx] = RegistryValueSnapshot.Missing();
            withSfx.InstalledFx[ProductConstants.Sfx] = RegistryValueSnapshot.Missing();
            Assert.Throws<InvalidDataException>(() => store.Save(new InstallState { Endpoints = [withSfx] }));

            var withChild = EchoCancellerOnlyEndpoint("{99999999-9999-9999-9999-999999999999}");
            withChild.OriginalChild = Missing(ProductConstants.ManagedChildNames);
            withChild.InstalledChild = Missing(ProductConstants.ManagedChildNames);
            Assert.Throws<InvalidDataException>(() => store.Save(new InstallState { Endpoints = [withChild] }));

            var newKey = EchoCancellerOnlyEndpoint("{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}");
            newKey.FxKeyExisted = false;
            Assert.Throws<InvalidDataException>(() => store.Save(new InstallState { Endpoints = [newKey] }));
        });
    }

    [Fact]
    public void SaveAndLoad_AcceptsFilteredNameAndRawCaptureProperties()
    {
        WithStore((store, _) =>
        {
            foreach (var endpoint in new[]
            {
                ValidEndpoint("{BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB}"),
                EchoCancellerOnlyEndpoint("{CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC}")
            })
            {
                endpoint.OriginalProperties[ProductConstants.ConnectionName] = Text("Microphone");
                endpoint.InstalledProperties[ProductConstants.ConnectionName] = Text("Microphone FILTERED");
                endpoint.OriginalProperties[ProductConstants.RawProcessingSupported] = RegistryValueSnapshot.Missing();
                endpoint.InstalledProperties[ProductConstants.RawProcessingSupported] = RegistryValueSnapshot.Missing();
                store.Save(new InstallState { Endpoints = [endpoint] });
                Assert.Equal(2, store.Load().Endpoints.Single().InstalledProperties.Count);
            }
        });
    }

    [Fact]
    public void Save_RejectsUnpairedOrForeignProperties()
    {
        WithStore((store, _) =>
        {
            var unpaired = ValidEndpoint("{DDDDDDDD-DDDD-DDDD-DDDD-DDDDDDDDDDDD}");
            unpaired.InstalledProperties[ProductConstants.ConnectionName] = Text("Microphone FILTERED");
            Assert.Throws<InvalidDataException>(() => store.Save(new InstallState { Endpoints = [unpaired] }));

            // Only the name and raw-capture values may ever be restored into Properties.
            var foreign = ValidEndpoint("{EEEEEEEE-EEEE-EEEE-EEEE-EEEEEEEEEEEE}");
            foreign.OriginalProperties[ProductConstants.FormFactor] = RegistryValueSnapshot.Missing();
            foreign.InstalledProperties[ProductConstants.FormFactor] = RegistryValueSnapshot.Missing();
            Assert.Throws<InvalidDataException>(() => store.Save(new InstallState { Endpoints = [foreign] }));
        });
    }

    [Fact]
    public void Load_ReadsJournalsWithoutPropertiesAsOwningNone()
    {
        WithStore((store, path) =>
        {
            store.Save(new InstallState { Endpoints = [ValidEndpoint("{FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF}")] });
            var json = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(path))!;
            var endpoint = json["Endpoints"]![0]!.AsObject();
            endpoint.Remove("OriginalProperties");
            endpoint.Remove("InstalledProperties");
            File.WriteAllText(path, json.ToJsonString());

            var loaded = store.Load().Endpoints.Single();
            Assert.Empty(loaded.OriginalProperties);
            Assert.Empty(loaded.InstalledProperties);
        });
    }

    private static RegistryValueSnapshot Text(string value) =>
        RegistryValueSnapshot.FromValue(Microsoft.Win32.RegistryValueKind.String, value);

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

    private static EndpointState EchoCancellerOnlyEndpoint(string id) => new()
    {
        EndpointId = id,
        Mode = EndpointInstallMode.SfxEfx,
        EchoCancellerOnly = true,
        FxKeyExisted = true,
        OriginalFx = Missing(ProductConstants.EchoCancellerOnlyFxNames),
        InstalledFx = new(StringComparer.OrdinalIgnoreCase)
        {
            [ProductConstants.Mfx] = RegistryValueSnapshot.FromValue(
                Microsoft.Win32.RegistryValueKind.String, ProductConstants.EchoCancellerClsid),
            [ProductConstants.MfxModes] = RegistryValueSnapshot.FromValue(
                Microsoft.Win32.RegistryValueKind.MultiString, new[] { ProductConstants.DefaultProcessingMode })
        }
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
