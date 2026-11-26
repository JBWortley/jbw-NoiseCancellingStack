// SPDX-License-Identifier: MIT
using Microsoft.Win32;
using Xunit;

namespace NoiseCancellingStack.Tests;

public sealed class FilteredEndpointPolicyTests
{
    // RawProcessingSupported exactly as the AT2040USB, C920, and onboard microphone report it.
    private static readonly byte[] RawEnabled = [11, 0, 0, 0, 1, 0, 0, 0, 255, 255, 0, 0];
    private static readonly byte[] RawDisabled = [11, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0];

    private static RegistryValueSnapshot Binary(byte[] value) => RegistryValueSnapshot.FromValue(RegistryValueKind.Binary, value);
    private static RegistryValueSnapshot Text(string value) => RegistryValueSnapshot.FromValue(RegistryValueKind.String, value);

    [Theory]
    [InlineData(16_000u, true)]
    [InlineData(48_000u, true)]
    [InlineData(44_100u, false)]
    [InlineData(0u, false)]
    public void IsFiltered_OnlyWhereNoiseSuppressionApplies(uint rate, bool expected)
    {
        Assert.Equal(expected, FilteredEndpointPolicy.IsFiltered(rate));
    }

    [Fact]
    public void TryPlan_LabelsTheName()
    {
        Assert.True(FilteredEndpointPolicy.TryPlan(ProductConstants.ConnectionName, Text("Microphone"), out var original, out var installed));
        Assert.Equal("Microphone", original.StringValue);
        Assert.Equal("Microphone FILTERED", installed.StringValue);
    }

    [Fact]
    public void TryPlan_JournalsAnAlreadyLabelledNameWithItsPlainOriginal()
    {
        Assert.True(FilteredEndpointPolicy.TryPlan(ProductConstants.ConnectionName, Text("Microphone FILTERED"), out var original, out var installed));
        Assert.Equal("Microphone", original.StringValue);
        Assert.Equal("Microphone FILTERED", installed.StringValue);
    }

    [Fact]
    public void TryPlan_DisablesAdvertisedRawCaptureOnly()
    {
        Assert.True(FilteredEndpointPolicy.TryPlan(ProductConstants.RawProcessingSupported, Binary(RawEnabled), out var original, out var installed));
        Assert.True(original.EquivalentTo(Binary(RawEnabled)));
        Assert.True(installed.EquivalentTo(Binary(RawDisabled)));

        // Not advertised, absent, or an unknown layout: nothing to own.
        Assert.False(FilteredEndpointPolicy.TryPlan(ProductConstants.RawProcessingSupported, Binary(RawDisabled), out _, out _));
        Assert.False(FilteredEndpointPolicy.TryPlan(ProductConstants.RawProcessingSupported, RegistryValueSnapshot.Missing(), out _, out _));
        Assert.False(FilteredEndpointPolicy.TryPlan(ProductConstants.RawProcessingSupported, Binary([11, 0, 0, 0, 255, 255]), out _, out _));
        Assert.False(FilteredEndpointPolicy.TryPlan(ProductConstants.RawProcessingSupported, Text("true"), out _, out _));
    }

    [Theory]
    [InlineData("installed", true, PropertyAction.None)]
    [InlineData("installed", false, PropertyAction.Restore)]
    [InlineData("original", true, PropertyAction.Apply)]     // Windows rebuilt the endpoint
    [InlineData("original", false, PropertyAction.Release)]
    [InlineData("other", true, PropertyAction.Release)]      // the user renamed it: theirs now
    [InlineData("other", false, PropertyAction.Release)]
    public void Reconcile_NeverFightsAnotherOwner(string live, bool filtered, PropertyAction expected)
    {
        var current = live switch
        {
            "installed" => Text("Microphone FILTERED"),
            "original" => Text("Microphone"),
            _ => Text("Desk mic")
        };
        Assert.Equal(expected, FilteredEndpointPolicy.Reconcile(filtered, Text("Microphone"), Text("Microphone FILTERED"), current));
    }

    [Fact]
    public void ToggleTarget_SwapsOursAndWindowsValuesButNeverAnotherOwners()
    {
        var original = Text("Microphone");
        var installed = Text("Microphone FILTERED");
        var foreign = Text("Desk mic");

        Assert.True(FilteredEndpointPolicy.ToggleTarget(false, original, installed, installed)!.EquivalentTo(original));
        Assert.True(FilteredEndpointPolicy.ToggleTarget(true, original, installed, original)!.EquivalentTo(installed));
        Assert.Null(FilteredEndpointPolicy.ToggleTarget(false, original, installed, original));   // already off
        Assert.Null(FilteredEndpointPolicy.ToggleTarget(true, original, installed, installed));   // already on
        Assert.Null(FilteredEndpointPolicy.ToggleTarget(false, original, installed, foreign));
        Assert.Null(FilteredEndpointPolicy.ToggleTarget(true, original, installed, foreign));
    }

    [Fact]
    public void UserControlSecurity_LetsUsersOnlyReadAndSetValues()
    {
        var security = FilteringControl.BuildUserControlSecurity();
        Assert.True(security.AreAccessRulesProtected);   // nothing inherited from HKLM\SOFTWARE
        var users = security.GetAccessRules(true, false, typeof(System.Security.Principal.SecurityIdentifier))
            .Cast<System.Security.AccessControl.RegistryAccessRule>()
            .Single(rule => rule.IdentityReference.Equals(
                new System.Security.Principal.SecurityIdentifier(System.Security.Principal.WellKnownSidType.BuiltinUsersSid, null)));
        var allowed = System.Security.AccessControl.RegistryRights.QueryValues | System.Security.AccessControl.RegistryRights.SetValue
            | System.Security.AccessControl.RegistryRights.Notify | System.Security.AccessControl.RegistryRights.ReadPermissions;
        Assert.Equal(allowed, users.RegistryRights);
        Assert.Equal(System.Security.AccessControl.InheritanceFlags.None, users.InheritanceFlags);
    }

    [Fact]
    public void RawCaptureToReapply_SelectsOnlyEndpointsWindowsResetToTheDriverValue()
    {
        var reset = Tracked("{11111111-1111-1111-1111-111111111111}");
        var stillDisabled = Tracked("{22222222-2222-2222-2222-222222222222}");
        var userChanged = Tracked("{33333333-3333-3333-3333-333333333333}");
        var pending = Tracked("{44444444-4444-4444-4444-444444444444}");
        pending.Pending = true;
        var untracked = new EndpointState { EndpointId = "{55555555-5555-5555-5555-555555555555}", Pending = false };
        var state = new InstallState { Endpoints = [reset, stillDisabled, userChanged, pending, untracked] };

        var live = new Dictionary<string, RegistryValueSnapshot?>
        {
            [reset.EndpointId] = Binary(RawEnabled),
            [stillDisabled.EndpointId] = Binary(RawDisabled),
            [userChanged.EndpointId] = Binary([11, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]),
            [pending.EndpointId] = Binary(RawEnabled),
            [untracked.EndpointId] = Binary(RawEnabled)
        };

        var result = FilteredEndpointPolicy.RawCaptureToReapply(state, endpoint => live[endpoint.EndpointId]);

        Assert.Equal([reset.EndpointId], result.Select(item => item.EndpointId));
    }

    private static EndpointState Tracked(string id) => new()
    {
        EndpointId = id,
        Pending = false,
        OriginalProperties = new(StringComparer.OrdinalIgnoreCase) { [ProductConstants.RawProcessingSupported] = Binary(RawEnabled) },
        InstalledProperties = new(StringComparer.OrdinalIgnoreCase) { [ProductConstants.RawProcessingSupported] = Binary(RawDisabled) }
    };
}
