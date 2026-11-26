# SPDX-License-Identifier: MIT
#
# THROWAWAY EXPERIMENT - NOT PRODUCT CODE. Run elevated.
# Makes one microphone stop advertising raw capture mode (so Chromium-based apps such as WD Studio
# open it with system effects) and appends FILTERED to its name. Records the original values and a
# registry export first, restarts Windows Audio, and writes what applications now see to
# out\raw-experiment.txt. Undo with raw-experiment-undo.ps1.
#Requires -RunAsAdministrator
param([Parameter(Mandatory)][string]$EndpointId)
$ErrorActionPreference = 'Stop'

$rawName = '{8943b373-388c-4395-b557-bc6dbaffafdb},2'   # RawProcessingSupported (VT_BOOL blob)
$descName = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'  # DeviceDesc, the renameable part of the name
$path = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\$EndpointId\Properties"
$backups = Join-Path $env:ProgramData 'NoiseCancellingStack\backups'
$state = Join-Path $backups 'raw-experiment.json'
$probe = Join-Path $PSScriptRoot 'out\ncs-aec-probe.exe'
$report = Join-Path $PSScriptRoot 'out\raw-experiment.txt'

if (Test-Path -LiteralPath $state) { throw 'The experiment is already applied; run raw-experiment-undo.ps1 first.' }
$key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
    $path,
    [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
    [System.Security.AccessControl.RegistryRights]'QueryValues, SetValue')
if ($null -eq $key) { throw "Capture endpoint not found: $EndpointId" }
try {
    $originalRaw = [byte[]]$key.GetValue($rawName)
    $originalDesc = [string]$key.GetValue($descName)
    if ($null -eq $originalRaw -or $originalRaw.Length -ne 12 -or $originalRaw[0] -ne 11 -or [string]::IsNullOrEmpty($originalDesc)) {
        throw 'Unexpected property layout; nothing was changed.'
    }

    New-Item -ItemType Directory -Force -Path $backups | Out-Null
    & reg.exe export "HKLM\$path" (Join-Path $backups "raw-experiment-$(Get-Date -Format yyyyMMdd-HHmmss).reg") /y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Registry export failed; nothing was changed.' }
    @{ EndpointId = $EndpointId; Raw = [Convert]::ToBase64String($originalRaw); Desc = $originalDesc } |
        ConvertTo-Json | Set-Content -LiteralPath $state -Encoding UTF8

    $newRaw = [byte[]]$originalRaw.Clone()
    $newRaw[8] = 0; $newRaw[9] = 0   # VARIANT_TRUE (0xFFFF) -> VARIANT_FALSE
    $key.SetValue($rawName, $newRaw, [Microsoft.Win32.RegistryValueKind]::Binary)
    $key.SetValue($descName, "$originalDesc FILTERED", [Microsoft.Win32.RegistryValueKind]::String)
}
finally {
    $key.Close()
}

"== changed $EndpointId; restarting Windows Audio only" | Set-Content -LiteralPath $report
Restart-Service Audiosrv -Force
& $probe list 2>&1 | Out-File -LiteralPath $report -Append -Encoding utf8
"== after also restarting the Audio Endpoint Builder" | Out-File -LiteralPath $report -Append -Encoding utf8
Restart-Service AudioEndpointBuilder -Force
Start-Service Audiosrv
& $probe list 2>&1 | Out-File -LiteralPath $report -Append -Encoding utf8
"Done. Results: $report"
