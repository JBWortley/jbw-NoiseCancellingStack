# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [switch]$SkipTests,
    [switch]$SkipSourceBundle
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$appVersion = '1.1.0'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$vendorCache = Join-Path $root 'vendor\cache'
$vendorSource = Join-Path $root 'vendor\source'
$artifacts = Join-Path $root 'artifacts'
$dist = Join-Path $root 'dist'

$files = @(
    # Binary inputs are accepted only when their exact upstream digest matches this manifest.
    @{
        Path = Join-Path $vendorCache 'EqualizerAPO-x64-1.4.2.exe'
        Sha256 = '7403BE7427BBE1936A40DDED082829B6E217FC4F5990FEE5CBA501F0AE055AFA'
        Urls = @(
            'https://netix.dl.sourceforge.net/project/equalizerapo/1.4.2/EqualizerAPO-x64-1.4.2.exe',
            'https://netcologne.dl.sourceforge.net/project/equalizerapo/1.4.2/EqualizerAPO-x64-1.4.2.exe'
        )
    },
    @{
        Path = Join-Path $vendorCache 'win-rnnoise-v1.10.zip'
        Sha256 = 'E4DCC186102850F8DCEBEB766AD1BF34C24F6280BD614EF84B72DAE4783FC517'
        Urls = @('https://github.com/werman/noise-suppression-for-voice/releases/download/v1.10/win-rnnoise.zip')
    }
)

$sourceFiles = @(
    @{
        Path = Join-Path $vendorSource 'EqualizerAPO-src-1.4.2.zip'
        Sha256 = 'BACD5C78BE71F3AAAB010684AB39F0E5C1BBB45FCD75CBB92C103A145A731DC4'
        Urls = @(
            'https://netix.dl.sourceforge.net/project/equalizerapo/1.4.2/EqualizerAPO-src-1.4.2.zip',
            'https://netcologne.dl.sourceforge.net/project/equalizerapo/1.4.2/EqualizerAPO-src-1.4.2.zip'
        )
    },
    @{
        Path = Join-Path $vendorSource 'noise-suppression-for-voice-v1.10.zip'
        Sha256 = '04288A2873E7DC5027E57F24F740C3FA6B624D1B7E51C4F444E0BDB7FFB4232C'
        Urls = @('https://github.com/werman/noise-suppression-for-voice/archive/refs/tags/v1.10.zip')
    },
    @{
        Path = Join-Path $vendorSource 'webrtc-audio-processing-3a6b666a906da6cb3c6f9d474fab21c2ba739f22.zip'
        Sha256 = '028DF34D5ED4A1015B7349D2A099EFB27C17A68ACEFB5C1D448F833207F5DB9A'
        Urls = @('https://codeload.github.com/Sqhh99/webrtc-audio-processing/zip/3a6b666a906da6cb3c6f9d474fab21c2ba739f22')
    },
    @{
        Path = Join-Path $vendorSource 'abseil-cpp-20250814.1.tar.gz'
        Sha256 = '1692F77D1739BACF3F94337188B78583CF09BAB7E420D2DC6C5605A4F86785A1'
        Urls = @('https://github.com/abseil/abseil-cpp/releases/download/20250814.1/abseil-cpp-20250814.1.tar.gz')
    },
    @{
        Path = Join-Path $vendorSource 'abseil-cpp_20250814.1-1_patch.zip'
        Sha256 = 'F44AD4D72FF2919A6A48CF0887F69AEF34C338BFDD8931153596E3766D75F654'
        Urls = @('https://wrapdb.mesonbuild.com/v2/abseil-cpp_20250814.1-1/get_patch')
    }
)

function Get-VerifiedFile {
    param([Parameter(Mandatory)][hashtable]$Descriptor)

    $parent = Split-Path -Parent $Descriptor.Path
    New-Item -ItemType Directory -Path $parent -Force | Out-Null

    if (Test-Path -LiteralPath $Descriptor.Path) {
        $actual = (Get-FileHash -LiteralPath $Descriptor.Path -Algorithm SHA256).Hash
        if ($actual -eq $Descriptor.Sha256) { return }
    }

    $failures = [System.Collections.Generic.List[string]]::new()
    foreach ($url in $Descriptor.Urls) {
        try {
            Invoke-WebRequest -Uri $url -OutFile $Descriptor.Path -UseBasicParsing
            $actual = (Get-FileHash -LiteralPath $Descriptor.Path -Algorithm SHA256).Hash
            if ($actual -ne $Descriptor.Sha256) {
                throw "checksum mismatch: expected $($Descriptor.Sha256), got $actual"
            }
            return
        }
        catch {
            $failures.Add("$url : $($_.Exception.Message)")
        }
    }

    throw "Could not fetch a verified copy of $($Descriptor.Path):`n$($failures -join "`n")"
}

function Reset-BuildDirectory {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$ExpectedLeaf
    )

    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $rootPrefix = [IO.Path]::GetFullPath($root).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals((Split-Path -Leaf $fullPath), $ExpectedLeaf, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to reset unexpected build path: $fullPath"
    }

    if (Test-Path -LiteralPath $fullPath) {
        Remove-Item -LiteralPath $fullPath -Recurse -Force
    }
    New-Item -ItemType Directory -Path $fullPath -Force | Out-Null
}

function New-ProjectSourceArchive {
    # Build outputs, downloaded binaries, and machine-local diagnostics are selected out rather
    # than copied and deleted afterward, reducing the chance that private data enters a release.
    $stagingParent = Join-Path $artifacts 'source-staging'
    Reset-BuildDirectory -Path $stagingParent -ExpectedLeaf 'source-staging'
    $sourceRoot = Join-Path $stagingParent "NoiseCancellingStack-$appVersion"
    New-Item -ItemType Directory -Path $sourceRoot -Force | Out-Null

    $rootFiles = @(
        '.gitattributes', '.gitignore', 'CHANGELOG.md', 'CONTRIBUTING.md', 'Directory.Build.props', 'global.json',
        'LICENSE', 'LICENSING.md', 'NoiseCancellingStack.slnx', 'README.md', 'SECURITY.md',
        'THIRD-PARTY-NOTICES.md'
    )
    foreach ($name in $rootFiles) {
        Copy-Item -LiteralPath (Join-Path $root $name) -Destination $sourceRoot
    }

    $sourceDirectories = @('.github', 'docs', 'installer', 'native', 'scripts', 'src', 'tests')
    foreach ($directory in $sourceDirectories) {
        $sourceDirectory = Join-Path $root $directory
        if (-not (Test-Path -LiteralPath $sourceDirectory)) { continue }

        Get-ChildItem -LiteralPath $sourceDirectory -Recurse -File |
            Where-Object {
                $_.FullName -notmatch '[\\/](bin|obj|TestResults)[\\/]' -and
                $_.Name -notlike '*-result.txt' -and
                $_.Extension -notin @('.user', '.suo')
            } |
            ForEach-Object {
                $relative = $_.FullName.Substring($root.Length).TrimStart([char[]]'\/')
                $destination = Join-Path $sourceRoot $relative
                New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
                Copy-Item -LiteralPath $_.FullName -Destination $destination
            }
    }

    $archive = Join-Path $dist "NoiseCancellingStack-source-$appVersion.zip"
    Compress-Archive -LiteralPath $sourceRoot -DestinationPath $archive -CompressionLevel Optimal
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

foreach ($descriptor in $files) { Get-VerifiedFile $descriptor }
foreach ($descriptor in $sourceFiles) { Get-VerifiedFile $descriptor }

Reset-BuildDirectory -Path $artifacts -ExpectedLeaf 'artifacts'
Reset-BuildDirectory -Path $dist -ExpectedLeaf 'dist'

& powershell -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\build-native.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }

$nativePropsPath = Join-Path $artifacts 'native\webrtc-apm.props'
if (-not (Test-Path -LiteralPath $nativePropsPath)) { throw 'Native payload manifest is missing.' }
[xml]$nativeProps = Get-Content -LiteralPath $nativePropsPath -Raw
$webRtcPluginHash = [string]$nativeProps.Project.PropertyGroup.WebRtcApmPluginSha256
if ($webRtcPluginHash -notmatch '^[0-9A-F]{64}$') { throw 'Native payload manifest has an invalid SHA-256 digest.' }

if (-not $SkipTests) {
    & dotnet test (Join-Path $root 'NoiseCancellingStack.slnx') --configuration Release --nologo `
        "-p:WebRtcApmPluginSha256=$webRtcPluginHash"
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}

Expand-ZipEntry `
    -ZipPath (Join-Path $vendorCache 'win-rnnoise-v1.10.zip') `
    -EntryName 'win-rnnoise/vst/rnnoise_mono.dll' `
    -Destination (Join-Path $artifacts 'payload\rnnoise_mono.dll')

$pluginHash = (Get-FileHash -LiteralPath (Join-Path $artifacts 'payload\rnnoise_mono.dll') -Algorithm SHA256).Hash
if ($pluginHash -ne '7DEBDFC4843461C47B23AC7A736B4663FE581643563AE764CDE395E006004080') {
    throw "Extracted RNNoise plugin checksum mismatch: $pluginHash"
}

Expand-ZipEntry `
    -ZipPath (Join-Path $vendorSource 'EqualizerAPO-src-1.4.2.zip') `
    -EntryName 'License.txt' `
    -Destination (Join-Path $artifacts 'licenses\EqualizerAPO-LICENSE.txt')
Expand-ZipEntry `
    -ZipPath (Join-Path $vendorSource 'noise-suppression-for-voice-v1.10.zip') `
    -EntryName 'noise-suppression-for-voice-1.10/LICENSE' `
    -Destination (Join-Path $artifacts 'licenses\RNNoisePlugin-LICENSE.txt')
Expand-ZipEntry `
    -ZipPath (Join-Path $vendorSource 'noise-suppression-for-voice-v1.10.zip') `
    -EntryName 'noise-suppression-for-voice-1.10/external/FST/LICENSE' `
    -Destination (Join-Path $artifacts 'licenses\FST-LICENSE.txt')
$webRtcArchiveRoot = 'webrtc-audio-processing-3a6b666a906da6cb3c6f9d474fab21c2ba739f22'
Expand-ZipEntry `
    -ZipPath (Join-Path $vendorSource "$webRtcArchiveRoot.zip") `
    -EntryName "$webRtcArchiveRoot/COPYING" `
    -Destination (Join-Path $artifacts 'licenses\WebRTCAudioProcessing-COPYING.txt')
Expand-ZipEntry `
    -ZipPath (Join-Path $vendorSource "$webRtcArchiveRoot.zip") `
    -EntryName "$webRtcArchiveRoot/webrtc/LICENSE" `
    -Destination (Join-Path $artifacts 'licenses\WebRTC-LICENSE.txt')
Expand-ZipEntry `
    -ZipPath (Join-Path $vendorSource "$webRtcArchiveRoot.zip") `
    -EntryName "$webRtcArchiveRoot/webrtc/PATENTS" `
    -Destination (Join-Path $artifacts 'licenses\WebRTC-PATENTS.txt')
Expand-ZipEntry `
    -ZipPath (Join-Path $vendorSource "$webRtcArchiveRoot.zip") `
    -EntryName "$webRtcArchiveRoot/webrtc/common_audio/third_party/ooura/LICENSE" `
    -Destination (Join-Path $artifacts 'licenses\WebRTC-Ooura-LICENSE.txt')
Expand-ZipEntry `
    -ZipPath (Join-Path $vendorSource "$webRtcArchiveRoot.zip") `
    -EntryName "$webRtcArchiveRoot/webrtc/modules/third_party/fft/LICENSE" `
    -Destination (Join-Path $artifacts 'licenses\WebRTC-FFT-LICENSE.txt')
Expand-ZipEntry `
    -ZipPath (Join-Path $vendorSource "$webRtcArchiveRoot.zip") `
    -EntryName "$webRtcArchiveRoot/webrtc/third_party/pffft/LICENSE" `
    -Destination (Join-Path $artifacts 'licenses\WebRTC-PFFFT-LICENSE.txt')
Expand-ZipEntry `
    -ZipPath (Join-Path $vendorSource "$webRtcArchiveRoot.zip") `
    -EntryName "$webRtcArchiveRoot/webrtc/third_party/rnnoise/COPYING" `
    -Destination (Join-Path $artifacts 'licenses\WebRTC-RNNoise-COPYING.txt')
$abseilLicense = Join-Path $artifacts 'native-source\project\subprojects\abseil-cpp-20250814.1\LICENSE'
if (-not (Test-Path -LiteralPath $abseilLicense)) { throw 'Abseil license was not found in the verified native source tree.' }
Copy-Item -LiteralPath $abseilLicense -Destination (Join-Path $artifacts 'licenses\Abseil-LICENSE.txt') -Force

& dotnet publish (Join-Path $root 'src\NoiseCancellingStack.Manager\NoiseCancellingStack.Manager.csproj') `
    --configuration Release `
    --runtime win-x64 `
    --self-contained true `
    --output (Join-Path $artifacts 'publish') `
    "-p:WebRtcApmPluginSha256=$webRtcPluginHash" `
    --nologo
if ($LASTEXITCODE -ne 0) { throw 'Manager publish failed.' }

$isccCandidates = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
)
$iscc = $isccCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($null -eq $iscc) { throw 'Inno Setup 6 compiler was not found.' }

& $iscc "/DAppVersion=$appVersion" (Join-Path $root 'installer\NoiseCancellingStack.iss')
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }

if (-not $SkipSourceBundle) {
    foreach ($descriptor in $sourceFiles) {
        Copy-Item -LiteralPath $descriptor.Path -Destination $dist -Force
    }
    New-ProjectSourceArchive
}

$checksums = Get-ChildItem -LiteralPath $dist -File |
    Where-Object Name -ne 'SHA256SUMS.txt' |
    Sort-Object Name |
    ForEach-Object {
    "{0}  {1}" -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
}
$checksums | Set-Content -LiteralPath (Join-Path $dist 'SHA256SUMS.txt') -Encoding ascii

Write-Output "Build complete: $dist"
Get-ChildItem -LiteralPath $dist -File | Select-Object Name,Length,LastWriteTime
