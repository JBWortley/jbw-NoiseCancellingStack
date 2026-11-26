# SPDX-License-Identifier: MIT
#
# Measures what the capture filters cost: alternates idle periods with periods where the given
# microphones are open in the default mode, sampling every second the audio engine's CPU and memory
# and, where Windows exposes energy meters (many recent AMD and Intel laptops), APU and CPU power.
# Nothing is played and nothing is recorded to disk. Runs as a normal user.
#
#   .\measure-load.ps1 -EndpointId '{capture-endpoint-guid}','{another-guid}' [-Cycles 3] [-Seconds 30]
#
# Endpoint GUIDs: ..\out\ncs-aec-probe.exe list (build it with ..\build.ps1 first).
param(
    [Parameter(Mandatory)][string[]]$EndpointId,
    [int]$Cycles = 3,
    [int]$Seconds = 30
)
$ErrorActionPreference = 'Stop'
$probe = Join-Path $PSScriptRoot '..\out\ncs-aec-probe.exe'
if (-not (Test-Path -LiteralPath $probe)) { throw 'Build the probe first: ..\build.ps1' }

$counters = @('\Process(audiodg)\% Processor Time', '\Process(audiodg)\Private Bytes', '\Process(audiodg)\Thread Count')
$energy = @('\Energy Meter(Apu Power)\Power', '\Energy Meter(CPU Power)\Power') |
    Where-Object { try { [void](Get-Counter $_ -MaxSamples 1 -ErrorAction Stop); $true } catch { $false } }
$counters += $energy

function Measure-Period([int]$count) {
    $rows = foreach ($set in (Get-Counter $counters -SampleInterval 1 -MaxSamples $count)) {
        $row = @{}
        foreach ($sample in $set.CounterSamples) {
            $row[$sample.Path.Substring($sample.Path.LastIndexOf([char]92) + 1) + '|' + $sample.InstanceName] = $sample.CookedValue
        }
        [pscustomobject]$row
    }
    $summary = [ordered]@{}
    foreach ($name in $rows[0].PSObject.Properties.Name) {
        $values = $rows.$name
        $summary[$name] = [pscustomobject]@{
            Avg = ($values | Measure-Object -Average).Average
            Max = ($values | Measure-Object -Maximum).Maximum
        }
    }
    $summary
}

function Format-Value([string]$name, $value) {
    if ($name -like '% processor time*') { '{0:N1}% of one thread' -f $value }
    elseif ($name -like 'private bytes*') { '{0:N1} MB' -f ($value / 1MB) }
    elseif ($name -like 'power*') { '{0:N2} W' -f ($value / 1000) }
    else { '{0:N0}' -f $value }
}

$settle = 2
for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
    Start-Sleep -Seconds $settle
    $idle = Measure-Period ($Seconds - $settle)
    $probes = foreach ($id in $EndpointId) {
        $info = New-Object Diagnostics.ProcessStartInfo $probe, "run $id --seconds $($Seconds + 1) --category default"
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true
        [Diagnostics.Process]::Start($info)
    }
    Start-Sleep -Seconds $settle
    $open = Measure-Period ($Seconds - $settle)
    $probes | ForEach-Object { $_.WaitForExit() }

    "== cycle $cycle (idle -> $($EndpointId.Count) microphone(s) open)"
    foreach ($name in $idle.Keys) {
        '  {0,-28} idle avg {1,-20} open avg {2,-20} open max {3}' -f $name,
            (Format-Value $name $idle[$name].Avg), (Format-Value $name $open[$name].Avg), (Format-Value $name $open[$name].Max)
    }
}
if ($energy.Count -eq 0) { 'This PC exposes no Energy Meter counters; power was not measured.' }
