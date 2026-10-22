# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [switch]$CoreOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$cache = Join-Path $root 'vendor\cache'
$toolsDirectory = Join-Path $root 'artifacts\native-tools'
$buildDirectory = Join-Path $root 'artifacts\native-core'
$sourceDirectory = Join-Path $root 'native\webrtc-apm-vst2'

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

function Reset-NativeBuildDirectory {
    param([Parameter(Mandatory)][string]$Path)

    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $rootPrefix = [IO.Path]::GetFullPath($root).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals((Split-Path -Leaf $fullPath), 'native-core', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to reset unexpected native build path: $fullPath"
    }

    if (Test-Path -LiteralPath $fullPath) {
        Remove-Item -LiteralPath $fullPath -Recurse -Force
    }
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
Reset-NativeBuildDirectory -Path $buildDirectory

& $python -m mesonbuild.mesonmain setup $buildDirectory $sourceDirectory --backend=ninja --buildtype=release -Db_vscrt=mt
if ($LASTEXITCODE -ne 0) { throw 'Native Meson setup failed.' }
& $python -m mesonbuild.mesonmain compile -C $buildDirectory
if ($LASTEXITCODE -ne 0) { throw 'Native compilation failed.' }
& $python -m mesonbuild.mesonmain test -C $buildDirectory --print-errorlogs
if ($LASTEXITCODE -ne 0) { throw 'Native tests failed.' }

if (-not $CoreOnly) {
    Write-Output 'Core-only native build is currently the complete target.'
}
