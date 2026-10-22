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
$rnNoisePath = 'C:\Program Files\NoiseCancellingStack\plugins\rnnoise_mono.dll'
$webRtcPath = 'C:\Program Files\NoiseCancellingStack\plugins\webrtc_apm_16k.dll'
foreach ($path in $configPath,$managedPath,$rnNoisePath,$webRtcPath) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Expected configuration is missing: $path" }
}

$managed = Get-Content -LiteralPath $managedPath -Raw
if ($managed -notmatch 'stage == "capture"' -or
    $managed -notmatch 'sampleRate == 48000' -or
    $managed -notmatch 'rnnoise_mono\.dll' -or
    $managed -notmatch 'sampleRate == 16000' -or
    $managed -notmatch 'webrtc_apm_16k\.dll') {
    throw 'The managed configuration is missing one or more capture/sample-rate safety guards.'
}

$taskXml = & schtasks.exe /Query /TN 'Noise Cancelling Stack\Register new microphones' /XML
if ($LASTEXITCODE -ne 0 -or $taskXml -notmatch '<BootTrigger>' -or $taskXml -notmatch '<Delay>PT1M</Delay>') {
    throw 'The startup repair task is missing or does not have its delayed boot trigger.'
}

Get-Service -Name Audiosrv | Select-Object Name,Status,StartType
