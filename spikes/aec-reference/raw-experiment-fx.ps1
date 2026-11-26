# SPDX-License-Identifier: MIT
#
# THROWAWAY EXPERIMENT - NOT PRODUCT CODE. Run elevated.
# Tests whether RawProcessingSupported=false written to the endpoint's FxProperties (which the Audio
# Endpoint Builder does not rebuild, unlike Properties) overrides the driver-derived value. Adds only
# that one value, restarts the Audio Endpoint Builder and Windows Audio, and writes what applications
# see to out\raw-experiment-fx.txt.   Undo: raw-experiment-fx.ps1 -Undo
#Requires -RunAsAdministrator
param(
    [Parameter(Mandatory)][string]$EndpointId,
    [switch]$Undo
)
$ErrorActionPreference = 'Stop'

$rawName = '{8943b373-388c-4395-b557-bc6dbaffafdb},2'
$falseBlob = [byte[]](11, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0)   # VT_BOOL, VARIANT_FALSE (same layout as the driver's value)
$path = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\$EndpointId\FxProperties"
$probe = Join-Path $PSScriptRoot 'out\ncs-aec-probe.exe'
$report = Join-Path $PSScriptRoot 'out\raw-experiment-fx.txt'

$key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
    $path,
    [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
    [System.Security.AccessControl.RegistryRights]'QueryValues, SetValue')
if ($null -eq $key) { throw "FxProperties not found for $EndpointId" }
try {
    $present = $key.GetValueNames() -contains $rawName
    if ($Undo) {
        if ($present) { $key.DeleteValue($rawName); 'Removed the FxProperties override.' } else { 'No override present.' }
    } else {
        if ($present) { throw 'An override is already present; run with -Undo first.' }
        $key.SetValue($rawName, $falseBlob, [Microsoft.Win32.RegistryValueKind]::Binary)
        'Added the FxProperties override.'
    }
}
finally {
    $key.Close()
}

Restart-Service AudioEndpointBuilder -Force
Start-Service Audiosrv
Start-Sleep -Seconds 3
"== FxProperties override $(if ($Undo) { 'removed' } else { 'added' }); after restarting the Audio Endpoint Builder" |
    Set-Content -LiteralPath $report
& $probe list 2>&1 | Out-File -LiteralPath $report -Append -Encoding utf8
"Done. Results: $report"
