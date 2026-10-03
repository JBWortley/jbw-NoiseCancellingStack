// SPDX-License-Identifier: MIT
using System.Security.AccessControl;
using Microsoft.Win32;

namespace NoiseCancellingStack;

public sealed class EndpointRegistry
{
    // Only HKLM capture endpoints are enumerated. Render/playback endpoints never share this path.
    private readonly InstallStateStore stateStore;
    private readonly OperationLogger logger;

    public EndpointRegistry(InstallStateStore stateStore, OperationLogger logger)
    {
        this.stateStore = stateStore;
        this.logger = logger;
    }

    public IReadOnlyList<EndpointSummary> List()
    {
        using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, ProductConstants.RegistryView);
        using var capture = hklm.OpenSubKey(ProductConstants.CaptureRoot, writable: false)
            ?? throw new InvalidOperationException("Windows capture endpoint registry is missing.");
        return capture.GetSubKeyNames().Select(id => ReadSummary(capture, id)).ToList();
    }

    public OperationReport AttachAll(InstallState state)
    {
        var report = new OperationReport();
        RecoverPending(state, report);

        using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, ProductConstants.RegistryView);
        using var capture = hklm.OpenSubKey(ProductConstants.CaptureRoot, writable: false)
            ?? throw new InvalidOperationException("Windows capture endpoint registry is missing.");

        foreach (var endpointId in capture.GetSubKeyNames())
        {
            var existingState = state.Endpoints.FirstOrDefault(
                item => string.Equals(item.EndpointId, endpointId, StringComparison.OrdinalIgnoreCase));

            try
            {
                using var endpoint = capture.OpenSubKey(endpointId, writable: false)
                    ?? throw new InvalidOperationException("Endpoint disappeared during enumeration.");
                using var properties = endpoint.OpenSubKey("Properties", writable: false)
                    ?? throw new InvalidOperationException("Endpoint Properties key is missing.");
                using var fxRead = endpoint.OpenSubKey("FxProperties", writable: false);
                var displayName = ReadDisplayName(properties);
                var sampleRate = ReadSampleRate(properties);

                if (existingState is not null)
                {
                    if (StateStillInstalled(hklm, existingState))
                    {
                        report.AlreadyAttached++;
                        report.Messages.Add($"Already managed: {displayName}");
                    }
                    else
                    {
                        report.Conflicts++;
                        report.Messages.Add($"Skipped externally modified endpoint: {displayName}");
                    }

                    continue;
                }

                if (HasEqualizerApo(fxRead))
                {
                    report.AlreadyAttached++;
                    report.Messages.Add($"Equalizer APO was already attached: {displayName}");
                    continue;
                }

                var prepared = PrepareEndpoint(endpointId, displayName, sampleRate, properties, fxRead, hklm);
                state.Endpoints.Add(prepared);
                // The original and intended values must reach durable storage before either
                // registry key changes, so an interrupted operation can be reversed on next run.
                stateStore.Save(state); // Write-ahead record before the first registry mutation.

                try
                {
                    ApplyPreparedEndpoint(hklm, prepared);
                    prepared.Pending = false;
                    stateStore.Save(state);
                    report.Attached++;
                    report.Messages.Add($"Attached: {displayName} ({sampleRate} Hz)");
                }
                catch
                {
                    RestoreEndpoint(hklm, prepared, conflictAware: false, report);
                    state.Endpoints.Remove(prepared);
                    stateStore.Save(state);
                    throw;
                }
            }
            catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or InvalidOperationException)
            {
                report.Skipped++;
                var message = $"Skipped endpoint {endpointId}: {ex.Message}";
                report.Messages.Add(message);
                logger.Write(message);
            }
        }

        return report;
    }

    public OperationReport DetachOwned(InstallState state)
    {
        var report = new OperationReport();
        using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, ProductConstants.RegistryView);

        foreach (var endpoint in state.Endpoints.ToArray())
        {
            try
            {
                var conflictsBefore = report.Conflicts;
                RestoreEndpoint(hklm, endpoint, conflictAware: true, report);
                if (report.Conflicts == conflictsBefore)
                {
                    state.Endpoints.Remove(endpoint);
                    report.Restored++;
                    stateStore.Save(state);
                }
            }
            catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or InvalidOperationException)
            {
                report.Skipped++;
                var message = $"Could not restore endpoint {endpoint.DisplayName}: {ex.Message}";
                report.Messages.Add(message);
                logger.Write(message);
            }
        }

        return report;
    }

    private void RecoverPending(InstallState state, OperationReport report)
    {
        var pending = state.Endpoints.Where(item => item.Pending).ToArray();
        if (pending.Length == 0)
        {
            return;
        }

        using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, ProductConstants.RegistryView);
        foreach (var endpoint in pending)
        {
            logger.Write($"Recovering interrupted endpoint transaction: {endpoint.DisplayName}");
            var conflictsBefore = report.Conflicts;
            RestoreEndpoint(hklm, endpoint, conflictAware: true, report);
            if (report.Conflicts == conflictsBefore)
            {
                state.Endpoints.Remove(endpoint);
            }
        }

        stateStore.Save(state);
    }

    private static EndpointState PrepareEndpoint(
        string endpointId,
        string displayName,
        uint sampleRate,
        RegistryKey properties,
        RegistryKey? fxRead,
        RegistryKey hklm)
    {
        var fxKeyExisted = fxRead is not null;
        var originalFx = CaptureValues(fxRead, ProductConstants.ManagedFxNames);
        ValidateApoSlots(originalFx);

        var hasLegacy = originalFx[ProductConstants.Lfx].Exists || originalFx[ProductConstants.Gfx].Exists;
        var hasModern = ProductConstants.ModernApoSlotNames.Any(name => HasValue(fxRead, name));
        var isCombined = HasValue(properties, ProductConstants.CombinedDevice);
        var mode = EndpointPlanner.SelectInstallMode(hasLegacy, hasModern, isCombined);

        // Equalizer APO uses the child record to retain the driver's prior APO chain. Reusing a
        // pre-existing child record without knowing its owner could silently corrupt that chain.
        using var childRead = hklm.OpenSubKey($@"{ProductConstants.ChildApoRoot}\{endpointId}", writable: false);
        var originalChild = CaptureValues(childRead, ProductConstants.ManagedChildNames);
        if (childRead is not null)
        {
            throw new InvalidOperationException("A stale Equalizer APO child record exists; manual review is required.");
        }

        var installedFx = CloneValues(originalFx);
        if (!fxKeyExisted)
        {
            installedFx[ProductConstants.FxTitle] = StringSnapshot("Equalizer APO");
        }

        if (mode == EndpointInstallMode.LfxGfx)
        {
            installedFx[ProductConstants.Lfx] = StringSnapshot(ProductConstants.EqualizerApoPreMixClsid);
            installedFx[ProductConstants.Sfx] = RegistryValueSnapshot.Missing();
            installedFx[ProductConstants.Mfx] = RegistryValueSnapshot.Missing();
            installedFx[ProductConstants.Efx] = RegistryValueSnapshot.Missing();
        }
        else
        {
            installedFx[ProductConstants.Lfx] = RegistryValueSnapshot.Missing();
            installedFx[ProductConstants.Gfx] = RegistryValueSnapshot.Missing();
            installedFx[ProductConstants.Sfx] = StringSnapshot(ProductConstants.EqualizerApoPreMixClsid);
            if (!installedFx[ProductConstants.SfxModes].Exists)
            {
                installedFx[ProductConstants.SfxModes] = MultiStringSnapshot(ProductConstants.DefaultProcessingMode);
            }
        }

        installedFx[ProductConstants.DisableEnhancements] = RegistryValueSnapshot.Missing();

        var installedChild = CloneValues(originalChild);
        foreach (var slot in ProductConstants.ApoSlotNames)
        {
            installedChild[slot] = StringSnapshot(
                !fxKeyExisted ? "!KEY" : originalFx[slot].Exists ? originalFx[slot].StringValue ?? string.Empty : "!VALUE");
        }

        installedChild[ProductConstants.PreMixChild] = StringSnapshot(EndpointPlanner.SelectPreMixChild(mode, originalFx));
        installedChild[ProductConstants.PostMixChild] = StringSnapshot(string.Empty);
        installedChild[ProductConstants.AllowSilentBufferModification] = StringSnapshot("false");
        installedChild[ProductConstants.DisableAutomaticAdjustment] = RegistryValueSnapshot.Missing();
        installedChild[ProductConstants.ChildVersion] = StringSnapshot("2");

        return new EndpointState
        {
            EndpointId = endpointId,
            DisplayName = displayName,
            SampleRate = sampleRate,
            Mode = mode,
            Pending = true,
            FxKeyExisted = fxKeyExisted,
            ChildKeyExisted = false,
            OriginalFx = originalFx,
            InstalledFx = installedFx,
            OriginalChild = originalChild,
            InstalledChild = installedChild
        };
    }

    private static void ApplyPreparedEndpoint(RegistryKey hklm, EndpointState endpoint)
    {
        // Write the recovery/child record first. If the process stops before the live FxProperties
        // write completes, the Pending journal plus this record still contains everything needed.
        var childPath = $@"{ProductConstants.ChildApoRoot}\{endpoint.EndpointId}";
        using var child = hklm.CreateSubKey(childPath, writable: true)
            ?? throw new UnauthorizedAccessException($"Could not create {childPath}.");
        ApplyChangedValues(child, endpoint.OriginalChild, endpoint.InstalledChild);

        var fxPath = $@"{ProductConstants.CaptureRoot}\{endpoint.EndpointId}\FxProperties";
        using var fx = OpenOrCreateWritableKey(hklm, fxPath);
        ApplyChangedValues(fx, endpoint.OriginalFx, endpoint.InstalledFx);
    }

    private static void RestoreEndpoint(
        RegistryKey hklm,
        EndpointState endpoint,
        bool conflictAware,
        OperationReport report)
    {
        var fxPath = $@"{ProductConstants.CaptureRoot}\{endpoint.EndpointId}\FxProperties";
        var deleteEmptyFxKey = false;
        using var fx = hklm.OpenSubKey(
            fxPath,
            RegistryKeyPermissionCheck.ReadWriteSubTree,
            RegistryRights.QueryValues | RegistryRights.SetValue);
        if (fx is null)
        {
            if (endpoint.FxKeyExisted)
            {
                report.Conflicts++;
                report.Messages.Add($"FxProperties disappeared: {endpoint.DisplayName}");
            }
        }
        else
        {
            RestoreValues(fx, endpoint.OriginalFx, endpoint.InstalledFx, conflictAware, endpoint.DisplayName, report);
            deleteEmptyFxKey = !endpoint.FxKeyExisted && IsEmpty(fx);
        }

        var childPath = $@"{ProductConstants.ChildApoRoot}\{endpoint.EndpointId}";
        var deleteEmptyChildKey = false;
        using var child = hklm.OpenSubKey(
            childPath,
            RegistryKeyPermissionCheck.ReadWriteSubTree,
            RegistryRights.QueryValues | RegistryRights.SetValue);
        if (child is not null)
        {
            RestoreValues(child, endpoint.OriginalChild, endpoint.InstalledChild, conflictAware, endpoint.DisplayName, report);
            deleteEmptyChildKey = !endpoint.ChildKeyExisted && IsEmpty(child);
        }
        else if (endpoint.ChildKeyExisted)
        {
            report.Conflicts++;
            report.Messages.Add($"Equalizer APO child backup disappeared: {endpoint.DisplayName}");
        }

        fx?.Dispose();
        child?.Dispose();
        if (deleteEmptyFxKey)
        {
            DeleteSubKeyIfEmpty(hklm, $@"{ProductConstants.CaptureRoot}\{endpoint.EndpointId}", "FxProperties");
        }
        if (deleteEmptyChildKey)
        {
            DeleteSubKeyIfEmpty(hklm, ProductConstants.ChildApoRoot, endpoint.EndpointId);
        }
    }

    private static void RestoreValues(
        RegistryKey key,
        IReadOnlyDictionary<string, RegistryValueSnapshot> originals,
        IReadOnlyDictionary<string, RegistryValueSnapshot> installed,
        bool conflictAware,
        string displayName,
        OperationReport report)
    {
        foreach (var (name, original) in originals)
        {
            var current = RegistryValueSnapshot.Capture(key, name);
            // During uninstall, never overwrite a value changed by a driver or another audio tool
            // after our installation. Original values are accepted to make retries idempotent.
            if (!conflictAware || current.EquivalentTo(installed[name]) || current.EquivalentTo(original))
            {
                original.Apply(key, name);
            }
            else
            {
                report.Conflicts++;
                report.Messages.Add($"Preserved externally changed registry value on {displayName}: {name}");
            }
        }
    }

    private static bool StateStillInstalled(RegistryKey hklm, EndpointState endpoint)
    {
        using var fx = hklm.OpenSubKey(
            $@"{ProductConstants.CaptureRoot}\{endpoint.EndpointId}\FxProperties",
            writable: false);
        using var child = hklm.OpenSubKey($@"{ProductConstants.ChildApoRoot}\{endpoint.EndpointId}", writable: false);
        return fx is not null
            && child is not null
            && endpoint.InstalledFx.All(item => RegistryValueSnapshot.Capture(fx, item.Key).EquivalentTo(item.Value))
            && endpoint.InstalledChild.All(item => RegistryValueSnapshot.Capture(child, item.Key).EquivalentTo(item.Value));
    }

    private static RegistryKey OpenOrCreateWritableKey(RegistryKey hklm, string path)
    {
        var existing = hklm.OpenSubKey(
            path,
            RegistryKeyPermissionCheck.ReadWriteSubTree,
            RegistryRights.QueryValues | RegistryRights.SetValue);
        if (existing is not null)
        {
            return existing;
        }

        return hklm.CreateSubKey(path, writable: true)
            ?? throw new UnauthorizedAccessException($"Could not create {path}.");
    }

    private static bool IsEmpty(RegistryKey key) => key.ValueCount == 0 && key.SubKeyCount == 0;

    private static void DeleteSubKeyIfEmpty(RegistryKey hklm, string parentPath, string childName)
    {
        using var parent = hklm.OpenSubKey(parentPath, writable: true)
            ?? throw new UnauthorizedAccessException($"Could not open {parentPath} to remove an empty key.");
        using var child = parent.OpenSubKey(childName, writable: false);
        if (child is not null && IsEmpty(child))
        {
            child.Dispose();
            parent.DeleteSubKey(childName, throwOnMissingSubKey: false);
        }
    }

    private static void ApplyChangedValues(
        RegistryKey key,
        IReadOnlyDictionary<string, RegistryValueSnapshot> original,
        IReadOnlyDictionary<string, RegistryValueSnapshot> installed)
    {
        foreach (var (name, desired) in installed)
        {
            if (!desired.EquivalentTo(original[name]))
            {
                desired.Apply(key, name);
            }
        }
    }

    private static Dictionary<string, RegistryValueSnapshot> CaptureValues(RegistryKey? key, IEnumerable<string> names)
    {
        return names.ToDictionary(
            name => name,
            name => key is null ? RegistryValueSnapshot.Missing() : RegistryValueSnapshot.Capture(key, name),
            StringComparer.OrdinalIgnoreCase);
    }

    private static Dictionary<string, RegistryValueSnapshot> CloneValues(
        IReadOnlyDictionary<string, RegistryValueSnapshot> values)
    {
        return values.ToDictionary(
            item => item.Key,
            item => new RegistryValueSnapshot
            {
                Exists = item.Value.Exists,
                Kind = item.Value.Kind,
                StringValue = item.Value.StringValue,
                StringArrayValue = item.Value.StringArrayValue?.ToArray(),
                BinaryBase64Value = item.Value.BinaryBase64Value,
                IntegerValue = item.Value.IntegerValue
            },
            StringComparer.OrdinalIgnoreCase);
    }

    private static RegistryValueSnapshot StringSnapshot(string value) =>
        RegistryValueSnapshot.FromValue(RegistryValueKind.String, value);

    private static RegistryValueSnapshot MultiStringSnapshot(params string[] values) =>
        RegistryValueSnapshot.FromValue(RegistryValueKind.MultiString, values);

    private static bool HasEqualizerApo(RegistryKey? fx)
    {
        return fx is not null && ProductConstants.ApoSlotNames.Any(name =>
        {
            var value = fx.GetValue(name) as string;
            return string.Equals(value, ProductConstants.EqualizerApoPreMixClsid, StringComparison.OrdinalIgnoreCase)
                || string.Equals(value, ProductConstants.EqualizerApoPostMixClsid, StringComparison.OrdinalIgnoreCase);
        });
    }

    private static bool HasValue(RegistryKey? key, string name) =>
        key is not null && key.GetValueNames().Contains(name, StringComparer.OrdinalIgnoreCase);

    private static void ValidateApoSlots(IReadOnlyDictionary<string, RegistryValueSnapshot> values)
    {
        foreach (var name in ProductConstants.ApoSlotNames)
        {
            var value = values[name];
            if (value.Exists && value.Kind is not (RegistryValueKind.String or RegistryValueKind.ExpandString))
            {
                throw new InvalidOperationException($"APO slot {name} is not a string value.");
            }
        }
    }

    private static EndpointSummary ReadSummary(RegistryKey capture, string id)
    {
        using var endpoint = capture.OpenSubKey(id, writable: false)
            ?? throw new InvalidOperationException($"Capture endpoint disappeared: {id}");
        using var properties = endpoint.OpenSubKey("Properties", writable: false)
            ?? throw new InvalidOperationException($"Capture endpoint lacks Properties: {id}");
        using var fx = endpoint.OpenSubKey("FxProperties", writable: false);
        return new EndpointSummary(id, ReadDisplayName(properties), ReadSampleRate(properties), HasEqualizerApo(fx));
    }

    private static string ReadDisplayName(RegistryKey properties)
    {
        var connection = properties.GetValue(ProductConstants.ConnectionName) as string;
        var device = properties.GetValue(ProductConstants.DeviceName) as string;
        return string.Join(" — ", new[] { connection, device }.Where(value => !string.IsNullOrWhiteSpace(value)))
            is { Length: > 0 } name ? name : "Unknown microphone";
    }

    private static uint ReadSampleRate(RegistryKey properties)
    {
        var bytes = properties.GetValue(ProductConstants.Format) as byte[];
        return bytes is { Length: >= 16 } ? BitConverter.ToUInt32(bytes, 12) : 0;
    }
}
