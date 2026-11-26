// SPDX-License-Identifier: MIT
using System.Security;
using System.Security.AccessControl;
using Microsoft.Win32;

namespace NoiseCancellingStack;

public sealed class EndpointRegistry
{
    // Only HKLM capture endpoints are enumerated. Render/playback endpoints never share this path.
    private readonly InstallStateStore stateStore;
    private readonly OperationLogger logger;
    private readonly Lazy<EchoCancellerContext> echoContext;

    public EndpointRegistry(InstallStateStore stateStore, OperationLogger logger, EchoCancellerContext? echoContext = null)
    {
        this.stateStore = stateStore;
        this.logger = logger;
        this.echoContext = new Lazy<EchoCancellerContext>(() => echoContext ?? EchoCancellerRegistration.DetectContext());
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
                        report.Messages.Add(existingState.EchoCancellerOnly
                            ? $"Equalizer APO was already attached: {displayName} (echo cancellation managed)"
                            : $"Already managed: {displayName}");
                        TryAddEchoCanceller(hklm, state, existingState, properties, fxRead, report);
                    }
                    else if (existingState.EchoCancellerOnly && NoInstalledValueRemains(hklm, existingState))
                    {
                        // Another tool (typically the Equalizer APO configurator) rewrote the slot and
                        // nothing of ours is left to restore. Forget the record; the endpoint is then
                        // evaluated afresh on the next run like any other microphone.
                        RestoreProperties(hklm, existingState, report);
                        state.Endpoints.Remove(existingState);
                        stateStore.Save(state);
                        report.Messages.Add($"Echo cancellation was removed from {displayName} by another tool; it is no longer tracked there.");
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
                    TryAttachEchoCancellerOnly(hklm, state, endpointId, displayName, sampleRate, properties, fxRead!, report);
                    continue;
                }

                var prepared = PrepareEndpoint(
                    endpointId, displayName, sampleRate, properties, fxRead, hklm, echoContext.Value, out var echo);
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
                    if (echo.ShouldAttach)
                    {
                        report.EchoCancellersAttached++;
                        report.Messages.Add($"Echo cancellation attached: {displayName}");
                    }
                    else
                    {
                        report.Messages.Add($"Echo cancellation not attached to {displayName}: {echo.Reason}");
                    }
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

        ReconcileFilteredProperties(hklm, state, report);
        return report;
    }

    // Every microphone this product filters carries the FILTERED name and does not advertise raw
    // capture (which would let applications bypass the filters). Both are journaled Properties
    // values; neither needs an audio restart, and neither ever displaces a value someone else set.
    private void ReconcileFilteredProperties(RegistryKey hklm, InstallState state, OperationReport report)
    {
        foreach (var endpoint in state.Endpoints.ToArray())
        {
            if (endpoint.Pending || !StateStillInstalled(hklm, endpoint))
            {
                continue;
            }

            try
            {
                using var properties = hklm.OpenSubKey(
                    $@"{ProductConstants.CaptureRoot}\{endpoint.EndpointId}\Properties",
                    RegistryKeyPermissionCheck.ReadWriteSubTree,
                    RegistryRights.QueryValues | RegistryRights.SetValue);
                if (properties is null)
                {
                    continue;
                }
                var filtered = FilteredEndpointPolicy.IsFiltered(ReadSampleRate(properties));
                foreach (var name in ProductConstants.ManagedPropertyNames)
                {
                    var current = RegistryValueSnapshot.Capture(properties, name);
                    if (endpoint.OriginalProperties.TryGetValue(name, out var original)
                        && endpoint.InstalledProperties.TryGetValue(name, out var installed))
                    {
                        switch (FilteredEndpointPolicy.Reconcile(filtered, original, installed, current))
                        {
                            case PropertyAction.Apply:
                                installed.Apply(properties, name);
                                break;
                            case PropertyAction.Restore:
                                original.Apply(properties, name);
                                UntrackProperty(state, endpoint, name);
                                report.Messages.Add($"{PropertyLabel(name)} removed from {endpoint.DisplayName}: noise suppression does not apply at its sample rate.");
                                break;
                            case PropertyAction.Release:
                                UntrackProperty(state, endpoint, name);
                                if (!current.EquivalentTo(original))
                                {
                                    report.Messages.Add($"{PropertyLabel(name)} on {endpoint.DisplayName} was changed elsewhere and is no longer managed.");
                                }
                                break;
                        }
                    }
                    else if (filtered && FilteredEndpointPolicy.TryPlan(name, current, out var plannedOriginal, out var plannedInstalled))
                    {
                        endpoint.OriginalProperties[name] = plannedOriginal;
                        endpoint.InstalledProperties[name] = plannedInstalled;
                        stateStore.Save(state); // Write-ahead record before the registry write.
                        if (!current.EquivalentTo(plannedInstalled))
                        {
                            plannedInstalled.Apply(properties, name);
                        }
                        report.Messages.Add($"{PropertyLabel(name)} applied: {endpoint.DisplayName}");
                    }
                }
            }
            catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or SecurityException)
            {
                report.Skipped++;
                report.Messages.Add($"Could not label {endpoint.DisplayName} as filtered or disable its raw capture: {ex.Message}");
                logger.Write(report.Messages[^1]);
            }
        }
    }

    private void UntrackProperty(InstallState state, EndpointState endpoint, string name)
    {
        endpoint.OriginalProperties.Remove(name);
        endpoint.InstalledProperties.Remove(name);
        stateStore.Save(state);
    }

    private static string PropertyLabel(string name) =>
        string.Equals(name, ProductConstants.ConnectionName, StringComparison.OrdinalIgnoreCase)
            ? "FILTERED name"
            : "Raw-capture block";

    // Puts back each owned Properties value that still holds our value. One that Windows or the
    // user has replaced since is theirs and stays; these never count as rollback conflicts.
    private static void RestoreProperties(RegistryKey hklm, EndpointState endpoint, OperationReport report)
    {
        if (endpoint.OriginalProperties.Count == 0)
        {
            return;
        }
        try
        {
            using var properties = hklm.OpenSubKey(
                $@"{ProductConstants.CaptureRoot}\{endpoint.EndpointId}\Properties",
                RegistryKeyPermissionCheck.ReadWriteSubTree,
                RegistryRights.QueryValues | RegistryRights.SetValue);
            if (properties is null)
            {
                return;
            }
            foreach (var (name, original) in endpoint.OriginalProperties)
            {
                if (RegistryValueSnapshot.Capture(properties, name).EquivalentTo(endpoint.InstalledProperties[name]))
                {
                    original.Apply(properties, name);
                }
            }
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or SecurityException)
        {
            report.Messages.Add($"Could not restore the name or raw-capture setting of {endpoint.DisplayName}: {ex.Message}");
        }
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
        RegistryKey hklm,
        EchoCancellerContext echoContext,
        out EchoCancellerDecision echo)
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

        // The echo canceller joins the same write-ahead transaction when the endpoint qualifies.
        echo = EvaluateEchoCanceller(mode, fxRead, properties, echoContext);
        if (echo.ShouldAttach)
        {
            AddEchoCancellerValues(originalFx, installedFx, fxRead);
        }

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

    // Upgrade path for an endpoint this product already manages: add the echo canceller as its own
    // write-ahead sub-transaction. If the process stops after the journal is saved, the Pending flag
    // makes the next run restore the whole endpoint conflict-aware and then re-attach it cleanly.
    private void TryAddEchoCanceller(
        RegistryKey hklm,
        InstallState state,
        EndpointState endpoint,
        RegistryKey properties,
        RegistryKey? fxRead,
        OperationReport report)
    {
        if (endpoint.InstalledFx.ContainsKey(ProductConstants.MfxModes))
        {
            // Journaled by 1.2.0 or later. If the APO registration has since disappeared, the MFX
            // slot would name a class the audio engine cannot create; take it back out.
            if (!echoContext.Value.Registered)
            {
                RemoveEchoCanceller(hklm, state, endpoint, report);
            }
            return;
        }

        var decision = EvaluateEchoCanceller(endpoint.Mode, fxRead, properties, echoContext.Value);
        if (!decision.ShouldAttach)
        {
            report.Messages.Add($"Echo cancellation not attached to {endpoint.DisplayName}: {decision.Reason}");
            return;
        }

        var previousInstalledMfx = endpoint.InstalledFx[ProductConstants.Mfx];
        AddEchoCancellerValues(endpoint.OriginalFx, endpoint.InstalledFx, fxRead);
        endpoint.Pending = true;
        stateStore.Save(state);

        var fxPath = $@"{ProductConstants.CaptureRoot}\{endpoint.EndpointId}\FxProperties";
        try
        {
            using var fx = OpenOrCreateWritableKey(hklm, fxPath);
            endpoint.InstalledFx[ProductConstants.MfxModes].Apply(fx, ProductConstants.MfxModes);
            endpoint.InstalledFx[ProductConstants.Mfx].Apply(fx, ProductConstants.Mfx);
            endpoint.Pending = false;
            stateStore.Save(state);
            report.EchoCancellersAttached++;
            report.Messages.Add($"Echo cancellation attached: {endpoint.DisplayName}");
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or SecurityException)
        {
            report.Skipped++;
            report.Messages.Add($"Could not attach echo cancellation to {endpoint.DisplayName}: {ex.Message}");
            logger.Write(report.Messages[^1]);
            if (TryRevertEchoCanceller(hklm, fxPath, endpoint))
            {
                endpoint.OriginalFx.Remove(ProductConstants.MfxModes);
                endpoint.InstalledFx.Remove(ProductConstants.MfxModes);
                endpoint.InstalledFx[ProductConstants.Mfx] = previousInstalledMfx;
                endpoint.Pending = false;
            }
            // Otherwise the endpoint stays Pending with the full journal for recovery on next run.
            stateStore.Save(state);
        }
    }

    // A microphone whose Equalizer APO attachment predates this product already gets noise
    // suppression through the shared configuration. Add only the echo canceller, journaled as its
    // own record that owns nothing but the MFX value and MFX mode list.
    private void TryAttachEchoCancellerOnly(
        RegistryKey hklm,
        InstallState state,
        string endpointId,
        string displayName,
        uint sampleRate,
        RegistryKey properties,
        RegistryKey fxRead,
        OperationReport report)
    {
        var slots = CaptureValues(fxRead, ProductConstants.ManagedFxNames.Concat(ProductConstants.OptionalManagedFxNames));
        var decision = EndpointPlanner.EvaluateEchoCancellerBesideExistingEqualizerApo(
            slots,
            HasValue(properties, ProductConstants.CombinedDevice),
            HasValue(fxRead, ProductConstants.MultiMfx),
            ReadFormFactor(properties),
            echoContext.Value);
        if (!decision.ShouldAttach)
        {
            report.Messages.Add($"Echo cancellation not attached to {displayName}: {decision.Reason}");
            return;
        }

        var originalFx = new Dictionary<string, RegistryValueSnapshot>(StringComparer.OrdinalIgnoreCase)
        {
            [ProductConstants.Mfx] = slots[ProductConstants.Mfx]
        };
        var installedFx = CloneValues(originalFx);
        AddEchoCancellerValues(originalFx, installedFx, fxRead);
        var endpoint = new EndpointState
        {
            EndpointId = endpointId,
            DisplayName = displayName,
            SampleRate = sampleRate,
            Mode = EndpointInstallMode.SfxEfx,
            Pending = true,
            EchoCancellerOnly = true,
            FxKeyExisted = true,
            ChildKeyExisted = true,
            OriginalFx = originalFx,
            InstalledFx = installedFx
        };
        state.Endpoints.Add(endpoint);
        stateStore.Save(state); // Write-ahead record before the first registry mutation.

        var fxPath = $@"{ProductConstants.CaptureRoot}\{endpointId}\FxProperties";
        try
        {
            using var fx = hklm.OpenSubKey(
                fxPath,
                RegistryKeyPermissionCheck.ReadWriteSubTree,
                RegistryRights.QueryValues | RegistryRights.SetValue)
                ?? throw new IOException("FxProperties disappeared.");
            installedFx[ProductConstants.MfxModes].Apply(fx, ProductConstants.MfxModes);
            installedFx[ProductConstants.Mfx].Apply(fx, ProductConstants.Mfx);
            endpoint.Pending = false;
            stateStore.Save(state);
            report.EchoCancellersAttached++;
            report.Messages.Add($"Echo cancellation attached: {displayName}");
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or SecurityException)
        {
            report.Skipped++;
            report.Messages.Add($"Could not attach echo cancellation to {displayName}: {ex.Message}");
            logger.Write(report.Messages[^1]);
            if (TryRevertEchoCanceller(hklm, fxPath, endpoint))
            {
                state.Endpoints.Remove(endpoint);
            }
            // Otherwise the record stays Pending for conflict-aware recovery on the next run.
            stateStore.Save(state);
        }
    }

    private static bool NoInstalledValueRemains(RegistryKey hklm, EndpointState endpoint)
    {
        using var fx = hklm.OpenSubKey(
            $@"{ProductConstants.CaptureRoot}\{endpoint.EndpointId}\FxProperties",
            writable: false);
        return fx is null || endpoint.InstalledFx.All(item =>
            item.Value.EquivalentTo(endpoint.OriginalFx[item.Key])
            || !RegistryValueSnapshot.Capture(fx, item.Key).EquivalentTo(item.Value));
    }

    // Called only for an endpoint whose live values still equal its journaled installed values.
    // Pending is saved first, so an interruption leads to the normal conflict-aware recovery.
    private void RemoveEchoCanceller(RegistryKey hklm, InstallState state, EndpointState endpoint, OperationReport report)
    {
        endpoint.Pending = true;
        stateStore.Save(state);
        var fxPath = $@"{ProductConstants.CaptureRoot}\{endpoint.EndpointId}\FxProperties";
        if (!TryRevertEchoCanceller(hklm, fxPath, endpoint))
        {
            report.Skipped++;
            report.Messages.Add(
                $"Echo canceller is no longer registered but could not be detached from {endpoint.DisplayName}; it will be recovered on the next repair.");
            logger.Write(report.Messages[^1]);
            return;  // stays Pending for recovery
        }
        if (endpoint.EchoCancellerOnly)
        {
            // Without the echo canceller the record owns nothing else; drop it with its labels.
            RestoreProperties(hklm, endpoint, report);
            state.Endpoints.Remove(endpoint);
            stateStore.Save(state);
            report.EchoCancellersRemoved++;
            report.Messages.Add($"Echo cancellation removed from {endpoint.DisplayName}: the echo canceller is not registered.");
            return;
        }
        endpoint.InstalledFx[ProductConstants.Mfx] = endpoint.OriginalFx[ProductConstants.Mfx];
        endpoint.OriginalFx.Remove(ProductConstants.MfxModes);
        endpoint.InstalledFx.Remove(ProductConstants.MfxModes);
        endpoint.Pending = false;
        stateStore.Save(state);
        report.EchoCancellersRemoved++;
        report.Messages.Add($"Echo cancellation removed from {endpoint.DisplayName}: the echo canceller is not registered.");
    }

    private static bool TryRevertEchoCanceller(RegistryKey hklm, string fxPath, EndpointState endpoint)
    {
        try
        {
            using var fx = hklm.OpenSubKey(
                fxPath,
                RegistryKeyPermissionCheck.ReadWriteSubTree,
                RegistryRights.QueryValues | RegistryRights.SetValue);
            if (fx is null)
            {
                return false;
            }
            endpoint.OriginalFx[ProductConstants.Mfx].Apply(fx, ProductConstants.Mfx);
            endpoint.OriginalFx[ProductConstants.MfxModes].Apply(fx, ProductConstants.MfxModes);
            return true;
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or SecurityException)
        {
            return false;
        }
    }

    private static EchoCancellerDecision EvaluateEchoCanceller(
        EndpointInstallMode mode,
        RegistryKey? fx,
        RegistryKey properties,
        EchoCancellerContext context)
    {
        var modeEffect = fx is null ? RegistryValueSnapshot.Missing() : RegistryValueSnapshot.Capture(fx, ProductConstants.Mfx);
        var modeEffectModes = fx is null ? RegistryValueSnapshot.Missing() : RegistryValueSnapshot.Capture(fx, ProductConstants.MfxModes);
        return EndpointPlanner.EvaluateEchoCanceller(
            mode,
            modeEffect,
            modeEffectModes,
            HasValue(fx, ProductConstants.MultiMfx),
            ReadFormFactor(properties),
            context);
    }

    // The default-mode-only registration matches Equalizer APO's SFX mode list, so registering
    // the echo canceller never makes a new processing mode (such as Communications) available.
    private static void AddEchoCancellerValues(
        Dictionary<string, RegistryValueSnapshot> originalFx,
        Dictionary<string, RegistryValueSnapshot> installedFx,
        RegistryKey? fx)
    {
        originalFx[ProductConstants.MfxModes] = fx is null
            ? RegistryValueSnapshot.Missing()
            : RegistryValueSnapshot.Capture(fx, ProductConstants.MfxModes);
        installedFx[ProductConstants.MfxModes] = MultiStringSnapshot(ProductConstants.DefaultProcessingMode);
        installedFx[ProductConstants.Mfx] = StringSnapshot(ProductConstants.EchoCancellerClsid);
    }

    private static int? ReadFormFactor(RegistryKey properties) =>
        properties.GetValue(ProductConstants.FormFactor) is int value ? value : null;

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
        RestoreProperties(hklm, endpoint, report);
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

        if (endpoint.EchoCancellerOnly)
        {
            // Equalizer APO's own child record belongs to its earlier installation; never touch it.
            fx?.Dispose();
            return;
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
        if (endpoint.EchoCancellerOnly)
        {
            return fx is not null
                && endpoint.InstalledFx.All(item => RegistryValueSnapshot.Capture(fx, item.Key).EquivalentTo(item.Value));
        }
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
        var echoCanceller = string.Equals(
            fx?.GetValue(ProductConstants.Mfx) as string,
            ProductConstants.EchoCancellerClsid,
            StringComparison.OrdinalIgnoreCase);
        return new EndpointSummary(id, ReadDisplayName(properties), ReadSampleRate(properties), HasEqualizerApo(fx), echoCanceller);
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
