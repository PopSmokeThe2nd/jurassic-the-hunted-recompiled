<#
.SYNOPSIS
  Launch the game with scripted input, wait for the title screen, run a command script, and watch
  for crashes. Leaves the game running for tools\drive.ps1 unless it crashed.
.EXAMPLE
  powershell -File tools\test_session.ps1 -Commands "tap start; wait 3000; tap a" -Watch 60
#>
param(
  [string]$Commands = "",
  [int]$Watch = 30,
  [string]$Shot = "",
  [string]$Preset = "win-amd64-relwithdebinfo",
  # Extra game arguments as one space-separated string (arrays don't survive powershell -File).
  [string]$ExtraArgs = "",
  # Run the game from this folder instead of out\build\<Preset> (e.g. "dist\Jurassic The Hunted").
  [string]$BinDir = "",
  # Saves/profile folder for the test run. Defaults to out\test_profile so tests (which often start
  # a New Game) never touch the real saves in %LOCALAPPDATA%; pass -UserData "" to use those.
  [string]$UserData = (Join-Path (Split-Path -Parent $PSScriptRoot) "out\test_profile")
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = if ($BinDir) { (Resolve-Path $BinDir).Path } else { Join-Path $root "out\build\$Preset" }
$cmdFile = Join-Path $root "out\test_input.txt"

Get-Process -Name jurassichunted -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
Set-Content -Path $cmdFile -Value "# test session $(Get-Date -Format s)" -Encoding ascii
# Never delete existing logs (they may be a player's); just find the one this run creates.
$started = Get-Date

$gameArgs = @('--log_level', 'debug', '--test_input_file', "`"$cmdFile`"")
if ($UserData) {
  New-Item -ItemType Directory -Force $UserData | Out-Null
  $gameArgs += "--user_data_root=`"$UserData`""
}
$gameArgs += @($ExtraArgs -split '\s+' | Where-Object { $_ })
$p = Start-Process -FilePath (Join-Path $bin "jurassichunted.exe") -ArgumentList $gameArgs -WorkingDirectory $bin -PassThru

# The title screen is up once the logo movies stop (last XMPSetPlaybackController(..., 0)).
$log = $null
for ($i = 0; $i -lt 90; $i++) {
  Start-Sleep -Seconds 1
  if ($p.HasExited) { break }
  $log = Get-ChildItem (Join-Path $bin "logs\*.log") -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $started } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if ($log -and @(Select-String -Path $log.FullName -Pattern 'XMPSetPlaybackController\(00000000, 00000000\)').Count -ge 2) { break }
}
if (-not $p.HasExited) {
  Start-Sleep -Seconds 3
  "title reached after ~$($i + 3)s"
  if ($Commands) {
    $lines = $Commands -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ }
    Add-Content -Path $cmdFile -Value $lines -Encoding ascii
  }
  try { $p | Wait-Process -Timeout $Watch -ErrorAction Stop } catch {}
}

$log = Get-ChildItem (Join-Path $bin "logs\*.log") -ErrorAction SilentlyContinue |
  Where-Object { $_.LastWriteTime -ge $started } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($p.HasExited) {
  "GAME EXITED: 0x{0:X8}" -f $p.ExitCode
  if ($log) { Get-Content $log.FullName | Select-String '\[(critical|error)\]' | Select-Object -Last 5 | ForEach-Object { $_.Line } }
} else {
  "still running (pid $($p.Id)); CPU {0:N0}s, {1:N0} MB" -f $p.TotalProcessorTime.TotalSeconds, ($p.WorkingSet64 / 1MB)
  if ($Shot) { & (Join-Path $PSScriptRoot "capture_window.ps1") -ProcessId $p.Id -OutPng $Shot | Out-Null; "shot: $Shot" }
}
