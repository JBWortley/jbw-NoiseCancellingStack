# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [switch]$CoreOnly,
    [ValidateSet('frame-adapter', 'webrtc-processor', 'vst-smoke')]
    [string]$TestFilter
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$cache = Join-Path $root 'vendor\cache'
$sourceCache = Join-Path $root 'vendor\source'
$toolsDirectory = Join-Path $root 'artifacts\native-tools'
$repositorySourceDirectory = Join-Path $root 'native\webrtc-apm-vst2'
$needsWebRtc = -not $CoreOnly
$buildLeaf = if ($needsWebRtc) { 'native-build' } else { 'native-core' }
$buildDirectory = Join-Path $root "artifacts\$buildLeaf"
$sourceDirectory = $repositorySourceDirectory

$mesonWheel = @{
    Path = Join-Path $cache 'meson-1.12.1-py3-none-any.whl'
    Sha256 = '930BC7542CBD9F57009E182FD014EBA48CF1A0180A7B9006D2D32D8F168D8B02'
    Url = 'https://files.pythonhosted.org/packages/57/99/36cd25f9598eef70db97577440a70a360af505f6f01c2338a18d1556079c/meson-1.12.1-py3-none-any.whl'
}
$ninjaArchive = @{
    Path = Join-Path $cache 'ninja-win-1.13.2.zip'
    Sha256 = '07FC8261B42B20E71D1720B39068C2E14FFCEE6396B76FB7A795FB460B78DC65'
    Url = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip'
}
$webRtcArchive = @{
    Path = Join-Path $sourceCache 'webrtc-audio-processing-3a6b666a906da6cb3c6f9d474fab21c2ba739f22.zip'
    Sha256 = '028DF34D5ED4A1015B7349D2A099EFB27C17A68ACEFB5C1D448F833207F5DB9A'
    Url = 'https://codeload.github.com/Sqhh99/webrtc-audio-processing/zip/3a6b666a906da6cb3c6f9d474fab21c2ba739f22'
}
$fstSourceArchive = @{
    Path = Join-Path $sourceCache 'noise-suppression-for-voice-v1.10.zip'
    Sha256 = '04288A2873E7DC5027E57F24F740C3FA6B624D1B7E51C4F444E0BDB7FFB4232C'
    Url = 'https://github.com/werman/noise-suppression-for-voice/archive/refs/tags/v1.10.zip'
}

function Get-VerifiedFile {
    param([Parameter(Mandatory)][hashtable]$Descriptor)

    New-Item -ItemType Directory -Path (Split-Path -Parent $Descriptor.Path) -Force | Out-Null
    if (Test-Path -LiteralPath $Descriptor.Path) {
        $actual = (Get-FileHash -LiteralPath $Descriptor.Path -Algorithm SHA256).Hash
        if ($actual -eq $Descriptor.Sha256) { return }
    }

    Invoke-WebRequest -Uri $Descriptor.Url -OutFile $Descriptor.Path -UseBasicParsing
    $actual = (Get-FileHash -LiteralPath $Descriptor.Path -Algorithm SHA256).Hash
    if ($actual -ne $Descriptor.Sha256) {
        throw "Checksum mismatch for $($Descriptor.Path): expected $($Descriptor.Sha256), got $actual"
    }
}

function Reset-GeneratedDirectory {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][ValidateSet('native-core', 'native-build', 'native-source')][string]$ExpectedLeaf
    )

    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $rootPrefix = [IO.Path]::GetFullPath($root).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals((Split-Path -Leaf $fullPath), $ExpectedLeaf, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to reset unexpected native build path: $fullPath"
    }

    if (Test-Path -LiteralPath $fullPath) {
        Remove-Item -LiteralPath $fullPath -Recurse -Force
    }
}

function Expand-ZipEntry {
    param(
        [Parameter(Mandatory)][string]$ZipPath,
        [Parameter(Mandatory)][string]$EntryName,
        [Parameter(Mandatory)][string]$Destination
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $entry = $archive.Entries | Where-Object FullName -eq $EntryName | Select-Object -First 1
        if ($null -eq $entry) { throw "Archive entry not found: $EntryName" }
        New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $Destination, $true)
    }
    finally {
        $archive.Dispose()
    }
}

function New-NativeSourceTree {
    $nativeSourceRoot = Join-Path $root 'artifacts\native-source'
    Reset-GeneratedDirectory -Path $nativeSourceRoot -ExpectedLeaf 'native-source'
    $stagingProject = Join-Path $nativeSourceRoot 'project'
    New-Item -ItemType Directory -Path $stagingProject -Force | Out-Null
    Get-ChildItem -LiteralPath $repositorySourceDirectory -Force |
        Copy-Item -Destination $stagingProject -Recurse -Force

    $webRtcExtract = Join-Path $nativeSourceRoot 'webrtc-archive'
    Expand-Archive -LiteralPath $webRtcArchive.Path -DestinationPath $webRtcExtract -Force
    $webRtcRoots = @(Get-ChildItem -LiteralPath $webRtcExtract -Directory)
    if ($webRtcRoots.Count -ne 1) { throw 'WebRTC source archive did not contain exactly one root directory.' }
    $webRtcRoot = $webRtcRoots[0]
    $subprojects = Join-Path $stagingProject 'subprojects'
    New-Item -ItemType Directory -Path $subprojects -Force | Out-Null
    Copy-Item -LiteralPath $webRtcRoot.FullName `
        -Destination (Join-Path $subprojects 'webrtc-audio-processing') -Recurse -Force

    $fstDirectory = Join-Path $stagingProject 'fst'
    foreach ($header in @('aeffect.h', 'aeffectx.h', 'fst.h')) {
        Expand-ZipEntry `
            -ZipPath $fstSourceArchive.Path `
            -EntryName "noise-suppression-for-voice-1.10/external/FST/fst/$header" `
            -Destination (Join-Path $fstDirectory $header)
    }
    return $stagingProject
}

function Import-VisualStudioEnvironment {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio locator was not found.' }
    $installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installation)) {
        throw 'Visual Studio C++ Build Tools were not found.'
    }

    $developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
    $lines = & $env:ComSpec /d /s /c "`"$developerCommand`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the Visual Studio x64 build environment.' }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
}

Get-VerifiedFile $mesonWheel
Get-VerifiedFile $ninjaArchive
if ($needsWebRtc) {
    Get-VerifiedFile $webRtcArchive
    Get-VerifiedFile $fstSourceArchive
    $sourceDirectory = New-NativeSourceTree
}

$pythonLauncher = (Get-Command py.exe -ErrorAction Stop).Source
$python = (& $pythonLauncher -3 -c 'import sys; print(sys.executable)').Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($python)) {
    throw 'Python 3 was not found.'
}
$mesonDirectory = Join-Path $toolsDirectory 'meson-1.12.1'
$mesonModule = Join-Path $mesonDirectory 'mesonbuild\mesonmain.py'
if (-not (Test-Path -LiteralPath $mesonModule)) {
    New-Item -ItemType Directory -Path $mesonDirectory -Force | Out-Null
    & $python -m pip install --disable-pip-version-check --no-deps --target $mesonDirectory $mesonWheel.Path
    if ($LASTEXITCODE -ne 0) { throw 'Could not bootstrap pinned Meson.' }
}

$ninjaDirectory = Join-Path $toolsDirectory 'ninja-1.13.2'
$ninja = Join-Path $ninjaDirectory 'ninja.exe'
if (-not (Test-Path -LiteralPath $ninja)) {
    New-Item -ItemType Directory -Path $ninjaDirectory -Force | Out-Null
    Expand-Archive -LiteralPath $ninjaArchive.Path -DestinationPath $ninjaDirectory -Force
}

Import-VisualStudioEnvironment
$env:Path = "$ninjaDirectory;$($env:Path)"
$env:PYTHONPATH = $mesonDirectory
Reset-GeneratedDirectory -Path $buildDirectory -ExpectedLeaf $buildLeaf

$withWebRtc = if ($needsWebRtc) { 'true' } else { 'false' }
& $python -m mesonbuild.mesonmain setup $buildDirectory $sourceDirectory `
    --backend=ninja --buildtype=release -Db_vscrt=mt -Ddefault_library=static "-Dwith_webrtc=$withWebRtc"
if ($LASTEXITCODE -ne 0) { throw 'Native Meson setup failed.' }
& $python -m mesonbuild.mesonmain compile -C $buildDirectory
if ($LASTEXITCODE -ne 0) { throw 'Native compilation failed.' }
$testArguments = @('-m', 'mesonbuild.mesonmain', 'test', '-C', $buildDirectory, '--print-errorlogs')
if (-not [string]::IsNullOrWhiteSpace($TestFilter)) { $testArguments += $TestFilter }
& $python @testArguments
if ($LASTEXITCODE -ne 0) { throw 'Native tests failed.' }

if ($needsWebRtc) {
    $plugin = Join-Path $buildDirectory 'webrtc_apm_16k.dll'
    if (-not (Test-Path -LiteralPath $plugin)) { throw 'WebRTC VST2 payload was not produced.' }
    $payloadDirectory = Join-Path $root 'artifacts\payload'
    $nativeManifestDirectory = Join-Path $root 'artifacts\native'
    New-Item -ItemType Directory -Path $payloadDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $nativeManifestDirectory -Force | Out-Null
    Copy-Item -LiteralPath $plugin -Destination (Join-Path $payloadDirectory 'webrtc_apm_16k.dll') -Force

    $pluginHash = (Get-FileHash -LiteralPath $plugin -Algorithm SHA256).Hash
    $props = @"
<Project>
  <PropertyGroup>
    <WebRtcApmPluginSha256>$pluginHash</WebRtcApmPluginSha256>
  </PropertyGroup>
</Project>
"@
    $props | Set-Content -LiteralPath (Join-Path $nativeManifestDirectory 'webrtc-apm.props') -Encoding UTF8

    if ([string]::IsNullOrWhiteSpace($TestFilter)) {
        $dependencies = & dumpbin.exe /dependents $plugin
        if ($LASTEXITCODE -ne 0) { throw 'Could not inspect WebRTC VST2 dependencies.' }
        $dependencies | Write-Output
        if ($dependencies -match '(?im)^\s*(MSVCP\d+|VCRUNTIME\d*|webrtc[^\s]*)\.dll\s*$') {
            throw 'WebRTC VST2 has a forbidden dynamic runtime dependency.'
        }
    }

    Write-Output "WebRTC APM payload: $plugin"
    Write-Output "WebRTC APM SHA-256: $pluginHash"
}
