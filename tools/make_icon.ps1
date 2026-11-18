<#
.SYNOPSIS
  Build a multi-size Windows .ico (PNG-compressed entries) from one square PNG.
.EXAMPLE
  powershell -File tools\make_icon.ps1 -Png title_icon.png -Ico launcher\assets\icon.ico
#>
param(
  [Parameter(Mandatory)][string]$Png,
  [Parameter(Mandatory)][string]$Ico,
  [int[]]$Sizes = @(16, 24, 32, 48, 64, 128, 256)
)
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Png))
$images = foreach ($s in $Sizes) {
  $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.InterpolationMode = 'HighQualityBicubic'
  $g.PixelOffsetMode = 'HighQuality'
  $g.SmoothingMode = 'HighQuality'
  $g.DrawImage($src, 0, 0, $s, $s)
  $ms = New-Object System.IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  , @($s, $ms.ToArray())
}
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$images.Count)   # ICONDIR
$offset = 6 + 16 * $images.Count
foreach ($im in $images) {                                                    # ICONDIRENTRY
  $s = $im[0]; $data = $im[1]
  $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256)); $w.Write([byte]0); $w.Write([byte]0)
  $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$data.Length); $w.Write([uint32]$offset)
  $offset += $data.Length
}
foreach ($im in $images) { $w.Write([byte[]]$im[1]) }
$w.Flush()
[System.IO.File]::WriteAllBytes((Join-Path (Resolve-Path (Split-Path -Parent $Ico)) (Split-Path -Leaf $Ico)), $out.ToArray())
"wrote $Ico ($($images.Count) sizes, $($out.Length) bytes)"
