<#
.SYNOPSIS
  Runs Glint headless scripts (see platform/win32/glint_headless_win32.hpp)
  in parallel against an app and summarizes the results.

.DESCRIPTION
  Each script runs in its own process: <Exe> --glint-headless <script>. Output
  (screenshots, report.txt) goes to <Out>\<script name>[_cpu]. With -Compare,
  every screenshot is compared with the one of the same name under
  <Compare>\<script name>[_cpu] (e.g. the -Out of a run of a baseline build):
  a difference of more than 2/255 fails the script, smaller ones are listed.

  Exit code: 0 when every script passed.

.EXAMPLE
  # Baseline from one build, then a new build compared against it:
  .\glint_headless_run.ps1 -Exe base\glint_demo.exe -Scripts tests\headless -Out out\base
  .\glint_headless_run.ps1 -Exe new\glint_demo.exe  -Scripts tests\headless -Out out\new -Compare out\base
#>
param(
  [Parameter(Mandatory)][string]   $Exe,
  [Parameter(Mandatory)][string[]] $Scripts,            # script files and/or folders of *.txt
  [Parameter(Mandatory)][string]   $Out,
  [string]   $Compare = '',
  [ValidateSet('gpu', 'cpu', 'both')][string] $Renderer = 'gpu',
  [int]      $Jobs = [Math]::Max(1, [Environment]::ProcessorCount / 2),
  [int]      $TimeoutSec = 120
)

$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$files = foreach ($s in $Scripts) {
  if (Test-Path -LiteralPath $s -PathType Container) { Get-ChildItem -LiteralPath $s -Filter *.txt | Sort-Object Name }
  else { Get-Item -LiteralPath $s }
}
$renderers = if ($Renderer -eq 'both') { @('gpu', 'cpu') } else { @($Renderer) }
New-Item -ItemType Directory -Force $Out | Out-Null

$queue = [System.Collections.Generic.Queue[object]]::new()
foreach ($f in $files) {
  foreach ($r in $renderers) {
    $name = $f.BaseName + $(if ($r -eq 'cpu') { '_cpu' } else { '' })
    $queue.Enqueue([pscustomobject]@{ Script = $f.FullName; Name = $name; Renderer = $r })
  }
}

$started = [Diagnostics.Stopwatch]::StartNew()
$running = @()
$done = @()
while ($queue.Count -gt 0 -or $running.Count -gt 0) {
  while ($queue.Count -gt 0 -and $running.Count -lt $Jobs) {
    $job = $queue.Dequeue()
    $outDir = Join-Path (Resolve-Path $Out).Path $job.Name
    $argList = @('--glint-headless', "`"$($job.Script)`"", '--glint-headless-out', "`"$outDir`"")
    if ($Compare) { $argList += @('--glint-headless-compare', "`"$(Join-Path (Resolve-Path $Compare).Path $job.Name)`"") }
    $psi = [Diagnostics.ProcessStartInfo]::new($Exe, ($argList -join ' '))
    $psi.WorkingDirectory = Split-Path $Exe
    $psi.UseShellExecute = $false
    $psi.EnvironmentVariables['GLINT_RENDERER'] = $(if ($job.Renderer -eq 'cpu') { 'cpu' } else { '' })
    $job | Add-Member Process ([Diagnostics.Process]::Start($psi))
    $job | Add-Member OutDir $outDir
    $job | Add-Member Clock ([Diagnostics.Stopwatch]::StartNew())
    $running += $job
  }
  Start-Sleep -Milliseconds 50
  foreach ($job in @($running)) {
    if (-not $job.Process.HasExited -and $job.Clock.Elapsed.TotalSeconds -lt $TimeoutSec) { continue }
    if (-not $job.Process.HasExited) { $job.Process.Kill(); $job | Add-Member TimedOut $true }
    $job.Clock.Stop()
    $running = @($running | Where-Object { $_ -ne $job })
    $done += $job
  }
}

$failed = 0
foreach ($job in $done | Sort-Object Name) {
  $report = Join-Path $job.OutDir 'report.txt'
  $lines = if (Test-Path -LiteralPath $report) { Get-Content -LiteralPath $report } else { @() }
  $result = ($lines | Select-String '^result ' | Select-Object -Last 1).Line
  if ($job.PSObject.Properties['TimedOut']) { $result = "result failed (timed out after $TimeoutSec s)" }
  if (-not $result) { $result = 'result failed (no report)' }
  if ($result -ne 'result ok') { $failed++ }
  '{0,-28} {1,5:N1} s  {2}' -f $job.Name, $job.Clock.Elapsed.TotalSeconds, $result
  foreach ($l in $lines) {
    if ($l -match '\[error\]|differs|-> private_mb') { '    ' + ($l -replace '^\d+: ', '') }
  }
}
'{0} scripts in {1:N1} s, {2} failed' -f $done.Count, $started.Elapsed.TotalSeconds, $failed
exit $(if ($failed -eq 0) { 0 } else { 1 })
