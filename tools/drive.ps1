<#
.SYNOPSIS
  Send scripted-input commands to a running test session and screenshot the result.
.DESCRIPTION
  The game must have been started with --test_input_file pointing at -CommandFile
  (default: out\test_input.txt). Commands use the syntax documented in src\test_input.h,
  separated by ';'.
.EXAMPLE
  powershell -File tools\drive.ps1 -Commands "tap a; wait 500; tap down" -Wait 4 -Shot menu.png
#>
param(
  [string]$Commands = "",
  [double]$Wait = 3,
  [string]$Shot = "",
  [string]$CommandFile = (Join-Path $PSScriptRoot "..\out\test_input.txt")
)
$ErrorActionPreference = 'Stop'
$p = Get-Process -Name jurassichunted -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "GAME NOT RUNNING"; exit 1 }
if ($Commands) {
  $lines = $Commands -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ }
  Add-Content -Path $CommandFile -Value $lines -Encoding ascii
}
Start-Sleep -Milliseconds ([int]($Wait * 1000))
if ($p.HasExited -or -not (Get-Process -Id $p.Id -ErrorAction SilentlyContinue)) { "GAME EXITED"; exit 2 }
if ($Shot) {
  & (Join-Path $PSScriptRoot "capture_window.ps1") -ProcessId $p.Id -OutPng $Shot | Out-Null
  "shot: $Shot"
}
