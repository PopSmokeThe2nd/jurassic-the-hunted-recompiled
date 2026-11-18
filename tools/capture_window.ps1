<#
.SYNOPSIS
  Save a PNG of the game's window (by process id) for quick visual checks while debugging.
#>
param(
  [Parameter(Mandatory)][int]$ProcessId,
  [Parameter(Mandatory)][string]$OutPng
)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[void][Win]::SetProcessDPIAware()
$p = Get-Process -Id $ProcessId -ErrorAction Stop
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { throw "Process has no main window" }
# PrintWindow + PW_RENDERFULLCONTENT copies only this window's own pixels (works for D3D12
# swap chains on Windows 10/11 and ignores any windows covering it).
$r = New-Object Win+RECT
[void][Win]::GetClientRect($h, [ref]$r)
$w = $r.R - $r.L; $hgt = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[void][Win]::PrintWindow($h, $hdc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc)
$bmp.Save($OutPng, [System.Drawing.Imaging.ImageFormat]::Png)
"saved {0}x{1} -> {2} (title: {3})" -f $w, $hgt, $OutPng, $p.MainWindowTitle
