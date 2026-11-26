// SPDX-License-Identifier: MIT
using System.Xml.Linq;
using Xunit;

namespace NoiseCancellingStack.Tests;

public sealed class ReleaseContractTests
{
    private static readonly string Root = FindRepositoryRoot();

    [Fact]
    public void Version120_IsConsistentAcrossReleaseInputs()
    {
        Assert.Equal("1.2.0", ProductConstants.ProductVersion);

        var project = XDocument.Load(Path("src", "NoiseCancellingStack.Manager", "NoiseCancellingStack.Manager.csproj"));
        Assert.Equal("1.2.0", project.Descendants("Version").Single().Value);
        Assert.Equal("1.2.0.0", project.Descendants("AssemblyVersion").Single().Value);
        Assert.Equal("1.2.0.0", project.Descendants("FileVersion").Single().Value);

        var manifest = XDocument.Load(Path("src", "NoiseCancellingStack.Manager", "app.manifest"));
        XNamespace assemblyNamespace = "urn:schemas-microsoft-com:asm.v1";
        Assert.Equal(
            "1.2.0.0",
            manifest.Descendants(assemblyNamespace + "assemblyIdentity").Single().Attribute("version")?.Value);

        Assert.Contains("#define AppVersion \"1.2.0\"", Read("installer", "NoiseCancellingStack.iss"), StringComparison.Ordinal);
        Assert.Contains("$appVersion = '1.2.0'", Read("scripts", "build.ps1"), StringComparison.Ordinal);
    }

    [Fact]
    public void Installer_PackagesBothProcessorsAndNativeNotices()
    {
        var installer = Read("installer", "NoiseCancellingStack.iss");

        Assert.Contains("artifacts\\payload\\rnnoise_mono.dll", installer, StringComparison.Ordinal);
        Assert.Contains("artifacts\\payload\\webrtc_apm_16k.dll", installer, StringComparison.Ordinal);
        Assert.Contains("WebRTC-LICENSE.txt", installer, StringComparison.Ordinal);
        Assert.Contains("WebRTC-PATENTS.txt", installer, StringComparison.Ordinal);
        Assert.Contains("FST-LICENSE.txt", installer, StringComparison.Ordinal);
        Assert.Contains("WebRTC-Ooura-LICENSE.txt", installer, StringComparison.Ordinal);
        Assert.Contains("WebRTC-FFT-LICENSE.txt", installer, StringComparison.Ordinal);
        Assert.Contains("WebRTC-PFFFT-LICENSE.txt", installer, StringComparison.Ordinal);
        Assert.Contains("WebRTC-RNNoise-COPYING.txt", installer, StringComparison.Ordinal);

        var notices = Read("THIRD-PARTY-NOTICES.md");
        Assert.Contains("Ooura FFT", notices, StringComparison.Ordinal);
        Assert.Contains("WebRTC FFT", notices, StringComparison.Ordinal);
        Assert.Contains("PFFFT", notices, StringComparison.Ordinal);
        Assert.Contains("WebRTC RNNoise", notices, StringComparison.Ordinal);
    }

    [Fact]
    public void Build_CompilesNativePayloadBeforeManagerAndEmbedsItsHash()
    {
        var build = Read("scripts", "build.ps1");
        var nativeBuild = build.IndexOf("build-native.ps1", StringComparison.Ordinal);
        var publish = build.IndexOf("dotnet publish", StringComparison.Ordinal);

        Assert.True(nativeBuild >= 0, "Release build must invoke build-native.ps1.");
        Assert.True(publish > nativeBuild, "Native payload must be built before manager publish.");
        Assert.Contains("WebRtcApmPluginSha256", build, StringComparison.Ordinal);
        Assert.Contains(
            "webrtc-audio-processing-3a6b666a906da6cb3c6f9d474fab21c2ba739f22.zip",
            build,
            StringComparison.Ordinal);
        Assert.Contains("Copy-Item", build, StringComparison.Ordinal);
    }

    [Fact]
    public void Installer_MovesLoadedPayloadsAsideInsteadOfFailingOnFilesInUse()
    {
        // audiodg keeps payload DLLs mapped while microphones are open; an upgrade must not need it closed.
        var installer = Read("installer", "NoiseCancellingStack.iss");
        foreach (var payload in new[] { @"{app}\apo\ncs_aec_apo.dll", @"{app}\plugins\rnnoise_mono.dll", @"{app}\plugins\webrtc_apm_16k.dll" })
        {
            Assert.Contains($"MovePayloadAside(ExpandConstant('{payload}'))", installer, StringComparison.Ordinal);
        }
        var install = installer.IndexOf("if CurStep = ssInstall then", StringComparison.Ordinal);
        var postInstall = installer.IndexOf("if CurStep = ssPostInstall then", StringComparison.Ordinal);
        Assert.True(install >= 0 && install < postInstall, "Payloads must be moved aside before files are copied.");
        Assert.Contains("procedure DeinitializeSetup", installer, StringComparison.Ordinal);
        Assert.Contains(@"Type: files; Name: ""{app}\apo\*.replaced-*""", installer, StringComparison.Ordinal);
        Assert.Contains(@"Type: files; Name: ""{app}\plugins\*.replaced-*""", installer, StringComparison.Ordinal);
    }

    [Fact]
    public void Installer_RegistersEchoCancellerApoAsComServer()
    {
        var installer = Read("installer", "NoiseCancellingStack.iss");
        var line = installer.Split('\n').Single(item =>
            item.StartsWith("Source:", StringComparison.Ordinal) && item.Contains("ncs_aec_apo.dll", StringComparison.Ordinal));
        Assert.Contains("artifacts\\payload\\ncs_aec_apo.dll", line, StringComparison.Ordinal);
        Assert.Contains("{app}\\apo", line, StringComparison.Ordinal);
        Assert.Contains("regserver", line, StringComparison.Ordinal);
        // The manager must look for the APO exactly where setup installs and registers it.
        Assert.EndsWith(
            System.IO.Path.Combine("apo", "ncs_aec_apo.dll"),
            ProductConstants.EchoCancellerPath,
            StringComparison.Ordinal);
    }

    [Fact]
    public void Build_PinsRenderPatchAndEmbedsEchoCancellerHash()
    {
        var build = Read("scripts", "build.ps1");
        Assert.Contains("EchoCancellerApoSha256", build, StringComparison.Ordinal);
        Assert.True(
            build.IndexOf("-p:EchoCancellerApoSha256", StringComparison.Ordinal)
                > build.IndexOf("build-native.ps1", StringComparison.Ordinal),
            "The echo canceller hash must come from the native build.");

        var nativeBuild = Read("scripts", "build-native.ps1");
        Assert.Contains("webrtc-single-threaded-render.patch", nativeBuild, StringComparison.Ordinal);
        Assert.Contains("FCA79C76243FCE3B80D56012570BC229EED87DFB423AD37E482D3A7DDAE390BA", nativeBuild, StringComparison.Ordinal);
        Assert.Contains("ncs_aec_apo.dll", nativeBuild, StringComparison.Ordinal);
        Assert.Contains("aec-apo-smoke", nativeBuild, StringComparison.Ordinal);
    }

    [Fact]
    public void Documentation_DisclosesEchoCancellationScopeAndLimits()
    {
        var readme = Read("README.md");
        foreach (var expected in new[] { "echo cancellation", "Windows 11", "DisableEchoCancellation", "headset", "19 kHz" })
        {
            Assert.Contains(expected, readme, StringComparison.OrdinalIgnoreCase);
        }
        var changelog = Read("CHANGELOG.md");
        Assert.Contains("## 1.2.0", changelog, StringComparison.Ordinal);
        Assert.Contains("9 ms", changelog, StringComparison.Ordinal);
        Assert.Contains("MFX", Read("docs", "SAFETY.md"), StringComparison.Ordinal);
    }

    [Fact]
    public void UserDocumentation_ExplainsExclusiveRoutingAndPassThrough()
    {
        foreach (var relativePath in new[] { "README.md", "docs/ARCHITECTURE.md", "docs/SAFETY.md" })
        {
            var document = File.ReadAllText(System.IO.Path.Combine(Root, relativePath.Replace('/', System.IO.Path.DirectorySeparatorChar)));
            Assert.Contains("16 kHz", document, StringComparison.OrdinalIgnoreCase);
            Assert.Contains("48 kHz", document, StringComparison.OrdinalIgnoreCase);
            Assert.Contains("pass-through", document, StringComparison.OrdinalIgnoreCase);
        }
    }

    [Fact]
    public void ReleaseNotes_DiscloseLatencyAndDoubleSuppressionRisk()
    {
        var changelog = Read("CHANGELOG.md");
        Assert.Contains("10 ms", changelog, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("double suppression", changelog, StringComparison.OrdinalIgnoreCase);
    }

    private static string Read(params string[] parts) => File.ReadAllText(Path(parts));

    private static string Path(params string[] parts) =>
        parts.Aggregate(Root, System.IO.Path.Combine);

    private static string FindRepositoryRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null && !File.Exists(System.IO.Path.Combine(directory.FullName, "NoiseCancellingStack.slnx")))
        {
            directory = directory.Parent;
        }
        return directory?.FullName
            ?? throw new DirectoryNotFoundException("Could not locate the repository root.");
    }
}
