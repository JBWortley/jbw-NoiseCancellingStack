# SPDX-License-Identifier: MIT
# Builds the offline AEC3 evaluator against the WebRTC/Abseil static libraries produced by
# scripts\build-native.ps1 (artifacts\native-build). No elevation, no system changes.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$build = Join-Path $repo 'artifacts\native-build'
$source = Join-Path $repo 'artifacts\native-source\project'
$out = Join-Path $PSScriptRoot '..\out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
if (-not (Test-Path (Join-Path $build 'subprojects\webrtc-audio-processing\webrtc\modules\audio_processing\libwebrtc-audio-processing-3.a'))) {
    throw 'Run scripts\build-native.ps1 first; the WebRTC static library is missing.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$lines = & $env:ComSpec /d /s /c "`"$(Join-Path $installation 'Common7\Tools\VsDevCmd.bat')`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
foreach ($line in $lines) { if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') } }

$abseil = 'subprojects\abseil-cpp-20250814.1'
$includes = @(
    "/I$build", "/I$source", "/I$(Join-Path $build $abseil)", "/I$(Join-Path $source $abseil)",
    "/I$(Join-Path $source 'subprojects\webrtc-audio-processing\webrtc')",
    "/I$(Join-Path $build 'subprojects\webrtc-audio-processing\webrtc')",
    "/I$(Join-Path $source 'aec')"
)
$libraries = @(
    (Join-Path $build 'libncs-echo-processor.a'),
    (Join-Path $build 'subprojects\webrtc-audio-processing\webrtc\modules\audio_processing\libwebrtc-audio-processing-3.a')
) + (@('base', 'flags', 'container', 'debugging', 'strings', 'crc', 'numeric', 'profiling', 'hash', 'synchronization', 'time') |
    ForEach-Object { Join-Path $build "$abseil\libabsl_$_.a" }) + @('winmm.lib', 'ole32.lib', 'advapi32.lib', 'user32.lib')

Push-Location $out
try {
    & cl.exe /nologo /MT /utf-8 /Zc:__cplusplus /W4 /EHsc /std:c++20 /permissive- /O2 /Gw /wd4068 /wd4100 /wd4146 /wd4191 `
        @includes (Join-Path $PSScriptRoot 'aec_eval.cpp') /Fe:aec-eval.exe /link /OPT:REF @libraries
    if ($LASTEXITCODE -ne 0) { throw 'aec-eval build failed.' }
}
finally { Pop-Location }
