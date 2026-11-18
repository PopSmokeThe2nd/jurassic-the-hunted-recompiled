<#
.SYNOPSIS
  Build (Release) and assemble a self-contained, double-clickable copy of the game in
  dist\Jurassic The Hunted\ : launcher, game, runtime DLLs and the Visual C++ runtime.
.DESCRIPTION
  No game data is included; the launcher's GAME ISO tab installs it from the player's own disc
  image into a "game" folder next to the executables. Re-running this script updates the program
  files in place and keeps an existing game\ folder, settings (jurassichunted.toml) and logs.

  -Release builds a package to share (e.g. on GitHub Releases): built with JTH_NO_GAME_ART=ON,
  so the game's launcher art and the controller button images are left out even if they're on
  this machine, assembled in a fresh out\release\JurassicTheHunted-v<Version> folder (dist\ is
  not touched) and zipped, with the zip's SHA-256 printed for the release notes. Every package
  gets THIRD_PARTY_NOTICES.txt, built from the license files of the libraries in the binaries.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\package.ps1
  powershell -ExecutionPolicy Bypass -File tools\package.ps1 -SkipBuild
  powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Release -Version 1.0.0
#>
param(
  [string]$Preset = "win-amd64-release",
  [string]$OutDir = "",
  [switch]$SkipBuild,
  [switch]$Release,
  [string]$Version = "1.0.0",
  # ReXGlue SDK source tree: its install is the CMake prefix, and its license files go into the
  # notices.
  [string]$SdkRoot = "C:/dev/rexglue-sdk"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$releaseName = "JurassicTheHunted-v$Version"
if (-not $OutDir) {
  $OutDir = if ($Release) { Join-Path $root "out\release\$releaseName" } else { Join-Path $root "dist\Jurassic The Hunted" }
}
$bin = Join-Path $root "out\build\$Preset"
if ($Release) {
  # A fresh folder: only what's built here goes in (never game data, settings or logs).
  if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
}

# Visual Studio developer environment (compiler, CMake, vswhere).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -property installationPath
if (-not $SkipBuild) {
  Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
  Enter-VsDevShell -VsInstallPath $vs -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
  Push-Location $root
  try {
    # Configure every time so JTH_NO_GAME_ART matches this run (only the resources rebuild when it
    # changes).
    $noArt = if ($Release) { "ON" } else { "OFF" }
    cmake --preset $Preset "-DCMAKE_PREFIX_PATH=$SdkRoot/out/install/win-amd64" "-DJTH_NO_GAME_ART=$noArt" | Out-Null
    if ($LASTEXITCODE) { throw "configure failed" }
    cmake --build --preset $Preset
    if ($LASTEXITCODE) { throw "build failed" }
  } finally { Pop-Location }
}

# Program files from the build. (Release builds use the unsuffixed SDK DLLs.)
$files = @("JurassicTheHunted.exe", "jurassichunted.exe", "rexruntime.dll", "rexgpu-xenos.dll",
           "amd_fidelityfx_dx12.dll")
foreach ($f in $files) { if (-not (Test-Path (Join-Path $bin $f))) { throw "missing $f in $bin" } }

# Visual C++ runtime, deployed next to the executables (app-local), so players don't need the
# VC++ redistributable installed.
$redist = Get-ChildItem "$vs\VC\Redist\MSVC" -Directory | Where-Object { $_.Name -match '^\d' } |
  Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$crt = Get-ChildItem "$($redist.FullName)\x64" -Directory -Filter "Microsoft.VC*.CRT" | Select-Object -First 1
$crtFiles = "msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll"

New-Item -ItemType Directory -Force $OutDir | Out-Null
foreach ($f in $files) { Copy-Item (Join-Path $bin $f) $OutDir -Force }
foreach ($f in $crtFiles) { Copy-Item (Join-Path $crt.FullName $f) $OutDir -Force }

@"
JURASSIC: THE HUNTED - RECOMPILED
=================================

A native Windows version of the Xbox 360 game, recompiled from the original code.
It includes no game data: you need your own Xbox 360 disc image (USA release).

FIRST TIME
  1. Double-click JurassicTheHunted.exe.
  2. In the GAME ISO tab, click "Select ISO...", choose your disc image and click Install
     (about 3 GB is copied into the "game" folder here; takes under a minute on an SSD).
  3. Click PLAY.

AFTER THAT
  Just double-click JurassicTheHunted.exe and click PLAY.

SETTINGS (in the launcher)
  GRAPHICS  internal resolution (up to 4K), anti-aliasing (FXAA), AMD FSR upscaling,
            frame rate (30-240 fps), vsync, field of view
  CONTROLS  every action with its PC key, Xbox button and PlayStation button,
            "In-game button icons": Xbox, PlayStation or keyboard keys in the game's prompts,
            and the weapon wheel on/off
  CHEATS    god mode, infinite ammo, infinite adrenaline, infinite grenades, one hit kill
  In game, press F2 for field of view, mouse sensitivity, the weapon wheel, AMD FSR
  upscaling, anti-aliasing, the auto-exposure limit and cheats. Changes there apply
  instantly and are saved right away.
  (The game has no adjustable MSAA; a higher internal resolution also smooths edges.)

CONTROLS
  Move WASD   Look mouse   Fire left mouse   Aim right mouse   Jump Space
  Crouch C    Reload R     Next weapon: tap Q / mouse wheel   Grenade G   Adrenaline F
  Sprint Shift   Melee V   Interact E   Objectives Tab   Pause Esc
  Weapon wheel: hold Q, point with the mouse, release (right click cancels).
  Number keys 1-9 pick a weapon directly.
  Your own weapon pictures: put PNGs in a "weapon_images" folder here, named
  ass_bar, ass_m249, ass_scar, pis_colt1911, pis_glock18c, pis_smith, plasmagun,
  rif_coltrifle, rpg7, sho_future, sho_mossberg, sil_crossbow or sni_whitworth
  (e.g. weapon_images\sho_mossberg.png).
  Menus: point and click with the mouse (right click = back, wheel = up/down).
  Skip videos and cutscenes with Space, R or a click (A, X or Start on a controller).
  Balancing on fallen trees: A / D, the left / right arrows or the mouse (tap to correct).
  An Xbox or PlayStation controller works too. Keys can be changed in the [bind]
  section of jurassichunted.toml.

IF THE GAME CRASHES
  The newest log in the "logs" folder ends with a CRASH line, and a crash_*.dmp
  file is saved next to it. Both help track the problem down.

FILES
  jurassichunted.toml   settings (created when you change something)
  game\                 game data installed from your disc image
  logs\                 game logs (newest file is the latest run)
  Saves and shader cache: %LOCALAPPDATA%\JurassicTheHuntedRecomp

REQUIREMENTS
  Windows 10 or 11 (64-bit), a DirectX 12 graphics card.

LICENSES
  This program's own code is MIT licensed. THIRD_PARTY_NOTICES.txt lists the
  libraries it includes and their licenses. Jurassic: The Hunted and its art belong
  to their respective owners and are not included; this project isn't affiliated
  with or endorsed by them.
"@ | Set-Content -Path (Join-Path $OutDir "README.txt") -Encoding ascii
if ($Release) {
  Add-Content -Path (Join-Path $OutDir "README.txt") -Encoding ascii -Value @"

NOTE
  This package contains none of the game's art: the launcher shows a plain title.
  "In-game button icons: PlayStation" needs controller images that aren't included
  (see the project's README); without them the game keeps the Xbox icons.
"@
}

# Third-party notices: the license text of every library built into the shipped binaries
# (rexruntime.dll / rexgpu-xenos.dll statically link most of the ReXGlue SDK's dependencies).
$tp = Join-Path $SdkRoot "thirdparty"
$licenses = @(
  @("Jurassic: The Hunted Recompiled (this program)", "MIT", (Join-Path $root "LICENSE")),
  @("ReXGlue SDK (rexruntime.dll, rexgpu-xenos.dll), including code derived from Xenia", "BSD 3-Clause", (Join-Path $SdkRoot "LICENSE")),
  @("AMD FidelityFX SDK (amd_fidelityfx_dx12.dll)", "MIT", (Join-Path $SdkRoot "out\build\win-amd64\_deps\fidelityfx-src\LICENSE.txt")),
  @("SDL 3", "zlib", (Join-Path $tp "sdl3\LICENSE.txt")),
  @("FFmpeg (XMA audio decoding)", "LGPL 2.1", (Join-Path $tp "FFmpeg\COPYING.LGPLv2.1")),
  @("libmspack (LZX decompression)", "LGPL 2.1", (Join-Path $tp "libmspack\libmspack\COPYING.LIB")),
  @("spdlog", "MIT", (Join-Path $tp "spdlog\LICENSE")),
  @("{fmt}", "MIT", (Join-Path $tp "fmt\LICENSE")),
  @("xxHash", "BSD 2-Clause", (Join-Path $tp "xxHash\LICENSE")),
  @("toml++", "MIT", (Join-Path $tp "tomlplusplus\LICENSE")),
  @("o1heap", "MIT", (Join-Path $tp "o1heap\LICENSE")),
  @("CLI11", "BSD 3-Clause", (Join-Path $tp "cli11\LICENSE")),
  @("UTF8-CPP", "Boost Software License 1.0", (Join-Path $tp "utfcpp\LICENSE")),
  @("SIMDe", "MIT", (Join-Path $tp "simde\COPYING")),
  @("AES-128 implementation", "see below", (Join-Path $tp "aes_128\LICENSE")),
  @("DES implementation", "see below", (Join-Path $tp "crypto\des\LICENSE")),
  @("Dear ImGui", "MIT", (Join-Path $root "third_party\imgui\LICENSE.txt"))
)
$notices = New-Object System.Text.StringBuilder
[void]$notices.AppendLine("THIRD-PARTY NOTICES")
[void]$notices.AppendLine("===================")
[void]$notices.AppendLine("")
[void]$notices.AppendLine("This package includes the software below. Each part keeps its own license.")
[void]$notices.AppendLine("")
[void]$notices.AppendLine("FFmpeg and libmspack are used under the GNU LGPL 2.1 and are built into")
[void]$notices.AppendLine("rexruntime.dll. Their source code: https://ffmpeg.org and")
[void]$notices.AppendLine("https://www.cabextract.org.uk/libmspack/. rexruntime.dll is built from the ReXGlue")
[void]$notices.AppendLine("SDK (https://github.com/rexglue/rexglue-sdk, with this project's patch in")
[void]$notices.AppendLine("patches/), so it can be rebuilt with modified versions of these libraries and")
[void]$notices.AppendLine("replaced in this folder.")
[void]$notices.AppendLine("")
[void]$notices.AppendLine("The Microsoft Visual C++ runtime files (msvcp140*.dll, vcruntime140*.dll) are")
[void]$notices.AppendLine("redistributed under the Microsoft Visual Studio license terms for distributable")
[void]$notices.AppendLine("code.")
foreach ($l in $licenses) {
  if (-not (Test-Path $l[2])) { throw "license file not found: $($l[2])" }
  [void]$notices.AppendLine("")
  [void]$notices.AppendLine(("-" * 78))
  [void]$notices.AppendLine("$($l[0])  ($($l[1]))")
  [void]$notices.AppendLine(("-" * 78))
  [void]$notices.AppendLine((Get-Content -Raw $l[2]).TrimEnd())
}
# stb (stb_image in the game, stb_truetype via Dear ImGui): its license is the end of the header.
$stb = Get-Content -Raw (Join-Path $root "third_party\stb\stb_image.h")
$i = $stb.LastIndexOf("This software is available under 2 licenses")
if ($i -lt 0) { throw "stb license text not found" }
[void]$notices.AppendLine("")
[void]$notices.AppendLine(("-" * 78))
[void]$notices.AppendLine("stb (stb_image, stb_truetype)  (MIT or public domain)")
[void]$notices.AppendLine(("-" * 78))
[void]$notices.AppendLine(($stb.Substring($i) -replace '\*/\s*$', '').TrimEnd())
[System.IO.File]::WriteAllText((Join-Path $OutDir "THIRD_PARTY_NOTICES.txt"), $notices.ToString().Replace("`r`n", "`n").Replace("`n", "`r`n"))

$size = (Get-ChildItem $OutDir -File | Measure-Object Length -Sum).Sum
"Packaged to $OutDir"
Get-ChildItem $OutDir -File | Select-Object Name, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } } | Format-Table -AutoSize | Out-String
"Program files total: {0:N1} MB" -f ($size / 1MB)
if (Test-Path (Join-Path $OutDir "game\default.xex")) { "Game data already installed in this folder." }

if ($Release) {
  $zip = Join-Path (Split-Path -Parent $OutDir) "$releaseName-win64.zip"
  if (Test-Path $zip) { Remove-Item -Force $zip }
  Compress-Archive -Path $OutDir -DestinationPath $zip -CompressionLevel Optimal
  $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
  "Release zip: $zip ({0:N1} MB)" -f ((Get-Item $zip).Length / 1MB)
  "SHA-256: $hash"
}
