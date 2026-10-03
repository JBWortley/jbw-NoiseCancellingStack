# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [string]$ManagerPath = 'C:\Program Files\NoiseCancellingStack\NoiseCancellingStack.Manager.exe'
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $ManagerPath)) {
    throw "Manager not found: $ManagerPath"
}

$status = Start-Process `
    -FilePath $ManagerPath `
    -Verb RunAs `
    -WindowStyle Hidden `
    -ArgumentList 'status','--quiet' `
    -Wait `
    -PassThru
if ($status.ExitCode -ne 0) { throw "Status check failed with exit code $($status.ExitCode)." }

$configPath = 'C:\Program Files\EqualizerAPO\config\config.txt'
$managedPath = 'C:\Program Files\EqualizerAPO\config\NoiseCancellingStack.txt'
foreach ($path in $configPath,$managedPath) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Expected configuration is missing: $path" }
}

$managed = Get-Content -LiteralPath $managedPath -Raw
if ($managed -notmatch 'stage == "capture"' -or $managed -notmatch 'sampleRate == 48000') {
    throw 'The managed configuration is missing its capture/sample-rate safety guards.'
}

Get-Service -Name Audiosrv | Select-Object Name,Status,StartType
