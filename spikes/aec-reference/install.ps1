# SPDX-License-Identifier: MIT
# THROWAWAY FEASIBILITY SPIKE. Run from an elevated PowerShell.
#
# Attaches the pass-through spike MFX to one or more capture endpoints, then restarts Windows
# Audio. Changes made (all reversed by uninstall.ps1):
#   - copies out\ncs_aec_spike_apo.dll to %ProgramFiles%\NcsAecSpike and COM/APO-registers it
#   - writes HKLM\SOFTWARE\NcsAecSpike (spike options)
#   - sets each endpoint's MFX CLSID and MFX processing modes (DEFAULT + COMMUNICATIONS)
#   - creates %ProgramData%\NcsAecSpike for the log, journal, backups and optional recordings
# Every endpoint is validated before any is changed. Refuses if any endpoint already has an
# MFX, so no driver effect is ever displaced.
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string[]] $EndpointId,
    [ValidateRange(0, 120)] [int] $RecordSeconds = 0,
    [switch] $AdvertiseAec,
    [switch] $PostVolumeLoopback,
    [switch] $NoRestart
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$clsid = '{0B3A9C87-CF54-45CD-9E30-942C93D8EDB6}'
$mfxName = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6'
$mfxModesName = '{d3993a3f-99c2-4402-b5ec-a92a0367664b},6'
$modes = [string[]]@('{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}', '{98951333-B9CD-48B1-A0A3-FF40682D73F7}')
$captureRoot = 'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture'
$installDir = Join-Path $env:ProgramFiles 'NcsAecSpike'
$dataDir = Join-Path $env:ProgramData 'NcsAecSpike'
$journalPath = Join-Path $dataDir 'install-journal.json'
$configPath = 'SOFTWARE\NcsAecSpike'
$source = Join-Path $PSScriptRoot 'out\ncs_aec_spike_apo.dll'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an elevated PowerShell.'
}
if (-not (Test-Path -LiteralPath $source)) { throw "Build first: $source is missing." }
if (Test-Path -LiteralPath $journalPath) { throw "A spike journal already exists ($journalPath). Run uninstall.ps1 first." }

function Read-Snapshot([Microsoft.Win32.RegistryKey] $key, [string] $name) {
    if ($key.GetValueNames() -contains $name) {
        return [ordered]@{ Exists = $true; Kind = $key.GetValueKind($name).ToString(); Value = $key.GetValue($name) }
    }
    return [ordered]@{ Exists = $false; Kind = $null; Value = $null }
}

$hklm = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')

# Administrators hold only ReadKey + SetValue on FxProperties (Audiosrv/AudioEndpointBuilder own
# the rest), so request exactly those rights, as the product manager does.
function Open-FxForWrite([string] $subKey) {
    return $hklm.OpenSubKey($subKey, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
        [System.Security.AccessControl.RegistryRights]'QueryValues, SetValue')
}

# Phase 1: validate every endpoint, including write access; change nothing.
$targets = @(foreach ($raw in $EndpointId) {
    $id = '{' + $raw.Trim().Trim('{}').ToLowerInvariant() + '}'
    if ($id -notmatch '^\{[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\}$') { throw "Not an endpoint GUID: $raw" }
    $fxSubKey = "$captureRoot\$id\FxProperties"
    $fx = Open-FxForWrite $fxSubKey
    if ($null -eq $fx) { throw "Capture endpoint $id has no FxProperties key; nothing was changed." }
    $originalMfx = Read-Snapshot $fx $mfxName
    $originalModes = Read-Snapshot $fx $mfxModesName
    $fx.Dispose()
    if ($originalMfx.Exists -and -not [string]::IsNullOrEmpty([string]$originalMfx.Value)) {
        throw "Endpoint $id already has an MFX ($($originalMfx.Value)); nothing was changed."
    }
    [ordered]@{ EndpointId = $id; OriginalMfx = $originalMfx; OriginalMfxModes = $originalModes; Backup = $null }
})

# Phase 2: data directory, backups and write-ahead journal.
# Data directory ACL: SYSTEM/Administrators full, LOCAL SERVICE (audiodg) modify, Users read.
New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
& icacls.exe $dataDir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-19:(OI)(CI)M' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not set the data directory ACL.' }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
foreach ($target in $targets) {
    $target.Backup = Join-Path $dataDir "FxProperties-$($target.EndpointId.Trim('{}'))-$stamp.reg"
    & reg.exe export "HKLM\$captureRoot\$($target.EndpointId)\FxProperties" $target.Backup /y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Registry backup failed for $($target.EndpointId); nothing was changed." }
}

[ordered]@{
    Clsid = $clsid
    InstalledModes = $modes
    InstallDir = $installDir
    Endpoints = $targets
    CreatedAt = (Get-Date).ToString('o')
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $journalPath -Encoding UTF8
Write-Host "Journal: $journalPath"

# Phase 3: register the DLL, write options, attach.
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
Copy-Item -LiteralPath $source -Destination $installDir -Force
$dll = Join-Path $installDir 'ncs_aec_spike_apo.dll'
$register = Start-Process -FilePath "$env:windir\System32\regsvr32.exe" -ArgumentList @('/s', "`"$dll`"") -Wait -PassThru
if ($register.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($register.ExitCode). Run uninstall.ps1 to clean up." }

$config = $hklm.CreateSubKey($configPath, $true)
$config.SetValue('RecordSeconds', [int]$RecordSeconds, 'DWord')
$config.SetValue('AdvertiseAec', [int][bool]$AdvertiseAec, 'DWord')
$config.SetValue('PostVolumeLoopback', [int][bool]$PostVolumeLoopback, 'DWord')
$config.Dispose()

foreach ($target in $targets) {
    $fx = Open-FxForWrite "$captureRoot\$($target.EndpointId)\FxProperties"
    if ($null -eq $fx) { throw "FxProperties for $($target.EndpointId) is not writable. Run uninstall.ps1 to clean up." }
    $fx.SetValue($mfxName, $clsid, 'String')
    $fx.SetValue($mfxModesName, $modes, 'MultiString')
    $fx.Dispose()
    Write-Host "Attached spike MFX to $($target.EndpointId) (backup $($target.Backup))"
}
Write-Host "Options: RecordSeconds=$RecordSeconds AdvertiseAec=$([bool]$AdvertiseAec) PostVolumeLoopback=$([bool]$PostVolumeLoopback)"

if (-not $NoRestart) {
    Write-Host 'Restarting Windows Audio...'
    Restart-Service -Name Audiosrv -Force
    Write-Host "Windows Audio: $((Get-Service Audiosrv).Status)"
}
Write-Host "Log: $(Join-Path $dataDir 'apo.log')"
