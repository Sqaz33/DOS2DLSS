[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(5,60)][int]$Seconds = 12,
    [ValidateRange(1,15)][int]$WarmupSeconds = 3,
    [ValidateRange(0,60)][int]$InitialFocusWaitSeconds = 15,
    [ValidateRange(1,4)][int]$Rounds = 2,
    [int[]]$Modes = @(0,1,2,3,4)
)
$ErrorActionPreference = 'Stop'
$statusTool = Join-Path $PSScriptRoot '..\build\dos2dlss_status.exe'
if (-not (Test-Path -LiteralPath $statusTool)) { throw 'Build the mod/status tool first.' }
if ($Modes | Where-Object { $_ -lt 0 -or $_ -gt 4 }) { throw 'Modes must be 0 through 4.' }
$game = @(Get-Process EoCApp -ErrorAction Stop)
if ($game.Count -ne 1) { throw 'Exactly one game process is required.' }
$gameProcessId = $game[0].Id
if (-not ('DOS2BenchmarkWindow' -as [type])) {
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class DOS2BenchmarkWindow {
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
    public static uint ForegroundProcess() { uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
}
'@
}
function Read-Snapshot {
    $before = [Diagnostics.Stopwatch]::GetTimestamp()
    $lines = & $statusTool
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read game frame counter.' }
    $after = [Diagnostics.Stopwatch]::GetTimestamp()
    $state = @{}
    foreach ($line in $lines) {
        $pair = $line.Split([char[]]'=',2)
        if ($pair.Length -eq 2) { $state[$pair[0]] = $pair[1] }
    }
    [pscustomobject]@{ State=$state; Tick=($before/2.0+$after/2.0); ReadMs=1000.0*($after-$before)/[Diagnostics.Stopwatch]::Frequency }
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
if (Test-Path -LiteralPath (Join-Path $OutputDirectory 'samples.csv')) {
    throw 'Choose a new output directory; an existing samples.csv will not be overwritten.'
}
$rows = @()
$names = @('Off','DLAA','Quality','Balanced','Performance')
Write-Output 'Activate the loaded game scene and keep the camera unchanged during measurement.'
for ($wait=0; [DOS2BenchmarkWindow]::ForegroundProcess() -ne $gameProcessId -and
             $wait -lt $InitialFocusWaitSeconds; ++$wait) {
    Start-Sleep -Seconds 1
}
$initialMode = [int](Read-Snapshot).State.quality_mode
try {
for ($round=1; $round -le $Rounds; ++$round) {
    $order = @($Modes)
    if ($round % 2 -eq 0) { [array]::Reverse($order) }
    foreach ($mode in $order) {
        if ([DOS2BenchmarkWindow]::ForegroundProcess() -ne $gameProcessId) {
            throw 'Game lost foreground focus; stopped instead of measuring background FPS.'
        }
        & $statusTool --mode $mode | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Mode switch failed.' }
        Start-Sleep -Seconds $WarmupSeconds
        $first = Read-Snapshot
        # Check focus throughout the sample. No screenshots/recording in this interval.
        for ($second=0; $second -lt $Seconds; ++$second) {
            Start-Sleep -Seconds 1
            if ([DOS2BenchmarkWindow]::ForegroundProcess() -ne $gameProcessId) {
                throw 'Game lost foreground focus; sample discarded.'
            }
        }
        $last = Read-Snapshot
        $elapsed = ($last.Tick-$first.Tick)/[Diagnostics.Stopwatch]::Frequency
        $frames = [long]$last.State.frame - [long]$first.State.frame
        $evaluations = [long]$last.State.ngx_evaluated_frames - [long]$first.State.ngx_evaluated_frames
        if ($frames -le 0 -or [int]$last.State.quality_mode -ne $mode) { throw 'Invalid game sample.' }
        $row = [pscustomobject]@{
            Time=(Get-Date -Format o); ProcessId=$gameProcessId; Round=$round; Mode=$names[$mode]
            RenderSize=$last.State.dlss_render_size; OutputSize=$last.State.backbuffer
            Seconds=[math]::Round($elapsed,4); Frames=$frames
            AverageFPS=[math]::Round($frames/$elapsed,2)
            AverageFrameMs=[math]::Round(1000.0*$elapsed/$frames,3)
            NGXFrames=$evaluations; NGXResult=$last.State.ngx_evaluate_result
            ReadUncertaintyMs=[math]::Round(($first.ReadMs+$last.ReadMs)/2.0,3)
        }
        $rows += $row
        $rows | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'samples.csv') -NoTypeInformation -Encoding UTF8
        $last.State | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory ('status-'+$round+'-'+$mode+'.json')) -Encoding UTF8
        Write-Output ('Round {0} {1}: {2} FPS, {3} ms, {4}/{5} NGX frames' -f $round,$names[$mode],$row.AverageFPS,$row.AverageFrameMs,$evaluations,$frames)
    }
}
} finally {
    if (Get-Process -Id $gameProcessId -ErrorAction SilentlyContinue) {
        & $statusTool --mode $initialMode | Out-Null
    }
}
# Average throughput only. This polling method does not measure per-frame percentiles.
