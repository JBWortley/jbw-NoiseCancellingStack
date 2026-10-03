[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^\{[0-9a-fA-F-]{36}\}$')]
    [string]$EndpointId,

    [Parameter(Mandatory)]
    [string]$BackupDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$resultLog = Join-Path $PSScriptRoot 'prepare-detached-endpoint-result.txt'
trap {
    ($_ | Format-List * -Force | Out-String) | Set-Content -LiteralPath $resultLog -Encoding UTF8
    exit 1
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Administrator rights are required.'
}

$captureNative = 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture'
$apoNative = 'HKLM\SOFTWARE\EqualizerAPO'
$endpointPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\$EndpointId"
$fxPath = Join-Path $endpointPath 'FxProperties'
$childPath = "HKLM:\SOFTWARE\EqualizerAPO\Child APOs\$EndpointId"
$slots = @(
    '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1',
    '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2',
    '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5',
    '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6',
    '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7'
)
$preMix = '{EACD2258-FCAC-4FF4-B36D-419E924A6D79}'

if (-not (Test-Path -LiteralPath $endpointPath)) { throw "Endpoint not found: $EndpointId" }
if (-not (Test-Path -LiteralPath $childPath)) { throw "Equalizer APO child backup not found: $EndpointId" }
$deviceState = (Get-ItemProperty -LiteralPath $endpointPath).DeviceState
if ($deviceState -eq 1) { throw 'Refusing to detach an active endpoint during integration testing.' }

$fx = Get-Item -LiteralPath $fxPath
$attached = @($slots | Where-Object { [string]$fx.GetValue($_) -ieq $preMix })
if ($attached.Count -eq 0) { throw 'Equalizer APO is not attached to this endpoint.' }

New-Item -ItemType Directory -Path $BackupDirectory -Force | Out-Null
& reg.exe export $captureNative (Join-Path $BackupDirectory 'capture-before-detach.reg') /y | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Capture registry export failed.' }
& reg.exe export $apoNative (Join-Path $BackupDirectory 'equalizer-apo-before-detach.reg') /y | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Equalizer APO registry export failed.' }

$child = Get-Item -LiteralPath $childPath
$hklm = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::LocalMachine,
    [Microsoft.Win32.RegistryView]::Registry64)
$relativeFxPath = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\$EndpointId\FxProperties"
$fxWrite = $hklm.OpenSubKey(
    $relativeFxPath,
    [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
    [System.Security.AccessControl.RegistryRights]::SetValue)
if ($null -eq $fxWrite) { throw 'Could not open FxProperties with SetValue access.' }
foreach ($slot in $slots) {
    $original = [string]$child.GetValue($slot)
    if ($original -eq '!KEY') {
        throw 'The endpoint originally had no FxProperties key; this integration helper deliberately refuses that broad case.'
    }
    if ($original -eq '!VALUE') {
        $fxWrite.DeleteValue($slot, $false)
    }
    else {
        $fxWrite.SetValue($slot, $original, [Microsoft.Win32.RegistryValueKind]::String)
    }
}
$fxWrite.Dispose()
$hklm.Dispose()

Remove-Item -LiteralPath $childPath -Recurse -Force
if (@($slots | Where-Object { [string]$fx.GetValue($_) -ieq $preMix }).Count -gt 0) {
    throw 'Equalizer APO remained attached after the detach operation.'
}
"Detached inactive endpoint for integration test: $EndpointId (DeviceState=$deviceState)" |
    Set-Content -LiteralPath $resultLog -Encoding UTF8
