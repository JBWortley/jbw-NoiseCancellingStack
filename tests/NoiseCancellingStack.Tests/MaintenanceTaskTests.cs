// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Xml.Linq;
using Xunit;

namespace NoiseCancellingStack.Tests;

public sealed class MaintenanceTaskTests
{
    private static readonly XNamespace TaskNamespace =
        "http://schemas.microsoft.com/windows/2004/02/mit/task";

    [Fact]
    public void BuildTaskXml_PreservesReliableHiddenBootRepairContract()
    {
        var document = XDocument.Parse(BuildTaskXml(@"C:\Program Files\NoiseCancellingStack\manager.exe"));

        Assert.Equal("S-1-5-18", Value(document, "UserId"));
        Assert.Equal("HighestAvailable", Value(document, "RunLevel"));
        Assert.Equal("PT1M", Value(document, "Delay"));
        Assert.Equal("true", Value(document, "StartWhenAvailable"));
        Assert.Equal("false", Value(document, "DisallowStartIfOnBatteries"));
        Assert.Equal("false", Value(document, "StopIfGoingOnBatteries"));
        Assert.Equal("IgnoreNew", Value(document, "MultipleInstancesPolicy"));
        Assert.Equal("PT15M", Value(document, "ExecutionTimeLimit"));
        Assert.Equal("true", Value(document, "Hidden"));
        Assert.Equal("repair --quiet --scheduled", Value(document, "Arguments"));
    }

    [Fact]
    public void BuildTaskXml_UsesProcessorNeutralDescription()
    {
        var document = XDocument.Parse(BuildTaskXml(@"C:\Program Files\NoiseCancellingStack\manager.exe"));
        var description = Value(document, "Description");

        Assert.Contains("noise suppression", description, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("RNNoise", description, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("WebRTC", description, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void BuildTaskXml_EscapesManagerPath()
    {
        const string managerPath = @"C:\Program Files\A&B\<manager>.exe";

        var document = XDocument.Parse(BuildTaskXml(managerPath));

        Assert.Equal(managerPath, Value(document, "Command"));
    }

    private static string BuildTaskXml(string managerPath)
    {
        var method = typeof(MaintenanceTask).GetMethod(
            "BuildTaskXml",
            BindingFlags.NonPublic | BindingFlags.Static)
            ?? throw new InvalidOperationException("MaintenanceTask.BuildTaskXml was not found.");
        return (string)(method.Invoke(null, [managerPath])
            ?? throw new InvalidOperationException("MaintenanceTask.BuildTaskXml returned null."));
    }

    private static string Value(XDocument document, string localName) =>
        document.Descendants(TaskNamespace + localName).Single().Value;
}
