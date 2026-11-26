# SPDX-License-Identifier: MIT
# Builds the throwaway AEC reference spike into .\out. Needs no elevation and changes nothing on
# the system.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = $PSScriptRoot
$out = Join-Path $root 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Import-VisualStudioEnvironment {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installation)) { throw 'Visual Studio C++ tools were not found.' }
    $developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
    $lines = & $env:ComSpec /d /s /c "`"$developerCommand`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the Visual Studio x64 environment.' }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
    }
}

Import-VisualStudioEnvironment

$common = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/O2', '/MT', '/Zi', '/permissive-', '/DUNICODE', '/D_UNICODE')
Push-Location $out
try {
    & cl.exe @common (Join-Path $root 'apo\ncs_aec_spike_apo.cpp') /LD /Fe:ncs_aec_spike_apo.dll /link `
        "/DEF:$(Join-Path $root 'apo\ncs_aec_spike_apo.def')" /DEBUG /OPT:REF `
        audiobaseprocessingobject.lib audiomediatypecrt.lib audioeng.lib legacy_stdio_definitions.lib `
        ole32.lib advapi32.lib
    if ($LASTEXITCODE -ne 0) { throw 'APO build failed.' }

    & cl.exe @common (Join-Path $root 'probe\ncs_aec_probe.cpp') /Fe:ncs-aec-probe.exe /link /DEBUG ole32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Probe build failed.' }
}
finally {
    Pop-Location
}

Get-ChildItem $out -Include *.dll, *.exe -Recurse | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $_.Name
}
