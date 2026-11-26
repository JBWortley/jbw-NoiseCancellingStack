# SPDX-License-Identifier: MIT
# THROWAWAY FEASIBILITY SPIKE. Run from an elevated PowerShell.
#
# Reverses install.ps1: restores the endpoint's original MFX values exactly (only if they still
# hold the spike's values), unregisters and deletes the DLL, removes the options key, and
# restarts Windows Audio. Logs, backups and recordings in %ProgramData%\NcsAecSpike are kept.
[CmdletBinding()]
param([switch] $NoRestart)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$clsid = '{0B3A9C87-CF54-45CD-9E30-942C93D8EDB6}'
$mfxName = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6'
$mfxModesName = '{d3993a3f-99c2-4402-b5ec-a92a0367664b},6'
$installDir = Join-Path $env:ProgramFiles 'NcsAecSpike'
$dataDir = Join-Path $env:ProgramData 'NcsAecSpike'
$journalPath = Join-Path $dataDir 'install-journal.json'
$dll = Join-Path $installDir 'ncs_aec_spike_apo.dll'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an elevated PowerShell.'
}

$hklm = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
$problems = 0

function Restore-Value([Microsoft.Win32.RegistryKey] $key, [string] $name, $original, $installed) {
    $exists = $key.GetValueNames() -contains $name
    $current = if ($exists) { $key.GetValue($name) } else { $null }
    $isInstalled = $exists -and ((@($current) -join '|') -eq (@($installed) -join '|'))
    $isOriginal = ($exists -eq [bool]$original.Exists) -and (-not $exists -or ((@($current) -join '|') -eq (@($original.Value) -join '|')))
    if ($isOriginal) { return $true }
    if (-not $isInstalled) {
        Write-Warning "$name was changed by something else since install; left as-is."
        return $false
    }
    if ($original.Exists) {
        $value = if ($original.Kind -eq 'MultiString') { [string[]]$original.Value } else { $original.Value }
        $key.SetValue($name, $value, [Microsoft.Win32.RegistryValueKind]$original.Kind)
    } else {
        $key.DeleteValue($name)
    }
    Write-Host "Restored $name"
    return $true
}

if (Test-Path -LiteralPath $journalPath) {
    $journal = Get-Content -LiteralPath $journalPath -Raw | ConvertFrom-Json
    foreach ($endpoint in @($journal.Endpoints)) {
        Write-Host "Endpoint $($endpoint.EndpointId):"
        $fxSubKey = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\$($endpoint.EndpointId)\FxProperties"
        # Administrators hold only ReadKey + SetValue on FxProperties; DeleteValue needs SetValue.
        $fx = $hklm.OpenSubKey($fxSubKey, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
            [System.Security.AccessControl.RegistryRights]'QueryValues, SetValue')
        if ($null -eq $fx) {
            Write-Warning "FxProperties for $($endpoint.EndpointId) is missing; nothing to restore there."
            $problems++
            continue
        }
        if (-not (Restore-Value $fx $mfxName $endpoint.OriginalMfx $journal.Clsid)) { $problems++ }
        if (-not (Restore-Value $fx $mfxModesName $endpoint.OriginalMfxModes $journal.InstalledModes)) { $problems++ }
        $fx.Dispose()
    }
} else {
    Write-Warning "No journal at $journalPath; only removing the DLL registration and options."
}

if (Test-Path -LiteralPath $dll) {
    $unregister = Start-Process -FilePath "$env:windir\System32\regsvr32.exe" -ArgumentList @('/u', '/s', "`"$dll`"") -Wait -PassThru
    if ($unregister.ExitCode -ne 0) { Write-Warning "regsvr32 /u exited with $($unregister.ExitCode)."; $problems++ }
}
$hklm.DeleteSubKeyTree('SOFTWARE\NcsAecSpike', $false)
$hklm.DeleteSubKeyTree("SOFTWARE\Classes\CLSID\$clsid", $false)
$hklm.DeleteSubKeyTree("SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$clsid", $false)

if (-not $NoRestart) {
    Write-Host 'Restarting Windows Audio...'
    Restart-Service -Name Audiosrv -Force
    Write-Host "Windows Audio: $((Get-Service Audiosrv).Status)"
}

if (Test-Path -LiteralPath $installDir) {
    try { Remove-Item -LiteralPath $installDir -Recurse -Force; Write-Host "Removed $installDir" }
    catch { Write-Warning "Could not delete $installDir yet (still loaded?): $($_.Exception.Message)"; $problems++ }
}

if ((Test-Path -LiteralPath $journalPath) -and $problems -eq 0) {
    Move-Item -LiteralPath $journalPath -Destination (Join-Path $dataDir "install-journal.done-$(Get-Date -Format 'yyyyMMdd-HHmmss').json")
}
Write-Host "Kept logs, backups and recordings in $dataDir (delete manually when finished)."
if ($problems -gt 0) { Write-Warning "$problems item(s) need attention; see warnings above."; exit 2 }
Write-Host 'Spike removed.'
