# SPDX-License-Identifier: MIT
#
# THROWAWAY EXPERIMENT - NOT PRODUCT CODE. Run elevated.
# Restores the microphone changed by raw-experiment.ps1. A value is restored only while it still
# holds the experiment's value; anything changed since is reported and left alone.
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

$rawName = '{8943b373-388c-4395-b557-bc6dbaffafdb},2'
$descName = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'
$state = Join-Path $env:ProgramData 'NoiseCancellingStack\backups\raw-experiment.json'
if (-not (Test-Path -LiteralPath $state)) { throw 'No experiment is applied.' }
$saved = Get-Content -LiteralPath $state -Raw | ConvertFrom-Json
$originalRaw = [Convert]::FromBase64String($saved.Raw)
$experimentRaw = [byte[]]$originalRaw.Clone()
$experimentRaw[8] = 0; $experimentRaw[9] = 0

$path = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\$($saved.EndpointId)\Properties"
$key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
    $path,
    [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
    [System.Security.AccessControl.RegistryRights]'QueryValues, SetValue')
if ($null -eq $key) { throw "Capture endpoint not found: $($saved.EndpointId)" }
try {
    $currentRaw = [byte[]]$key.GetValue($rawName)
    if ($null -ne $currentRaw -and [Convert]::ToBase64String($currentRaw) -eq [Convert]::ToBase64String($experimentRaw)) {
        $key.SetValue($rawName, $originalRaw, [Microsoft.Win32.RegistryValueKind]::Binary)
        'Restored raw-mode support.'
    } else { 'Raw-mode value changed since the experiment; left as is.' }

    if ([string]$key.GetValue($descName) -eq "$($saved.Desc) FILTERED") {
        $key.SetValue($descName, $saved.Desc, [Microsoft.Win32.RegistryValueKind]::String)
        'Restored the name.'
    } else { 'Name changed since the experiment; left as is.' }
}
finally {
    $key.Close()
}

Remove-Item -LiteralPath $state
# Both values take effect for applications immediately; no audio service restart is needed.
'Done.'
