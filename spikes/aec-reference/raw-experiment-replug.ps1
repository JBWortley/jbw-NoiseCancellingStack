# SPDX-License-Identifier: MIT
#
# THROWAWAY EXPERIMENT - NOT PRODUCT CODE. Run elevated.
# Prepares the replug test: removes the FxProperties override from raw-experiment-fx.ps1 (it had no
# effect) and sets RawProcessingSupported=false in Properties again, WITHOUT restarting any audio
# service, to learn whether applications see the change immediately and whether a replug undoes it.
# raw-experiment-undo.ps1 restores the original value and name afterwards.
#Requires -RunAsAdministrator
param([Parameter(Mandatory)][string]$EndpointId)
$ErrorActionPreference = 'Stop'

$rawName = '{8943b373-388c-4395-b557-bc6dbaffafdb},2'
$root = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\$EndpointId"
if (-not (Test-Path -LiteralPath (Join-Path $env:ProgramData 'NoiseCancellingStack\backups\raw-experiment.json'))) {
    throw 'raw-experiment.ps1 has not been applied, so there is no record of the original value.'
}
$rights = [System.Security.AccessControl.RegistryRights]'QueryValues, SetValue'
$check = [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree

$fx = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$root\FxProperties", $check, $rights)
try { if ($fx.GetValueNames() -contains $rawName) { $fx.DeleteValue($rawName); 'Removed the FxProperties override.' } }
finally { $fx.Close() }

$properties = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$root\Properties", $check, $rights)
try {
    $value = [byte[]]$properties.GetValue($rawName)
    if ($null -eq $value -or $value.Length -ne 12 -or $value[0] -ne 11) { throw 'Unexpected property layout; nothing changed.' }
    $value[8] = 0; $value[9] = 0
    $properties.SetValue($rawName, $value, [Microsoft.Win32.RegistryValueKind]::Binary)
    'Raw mode is no longer advertised (no service restarted).'
}
finally { $properties.Close() }
