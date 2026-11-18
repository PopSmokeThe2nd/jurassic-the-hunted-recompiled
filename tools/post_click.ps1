<#
.SYNOPSIS
  Post a left click at client coordinates (x, y) to a process's main window, without moving the
  real mouse. For testing the launcher UI.
.EXAMPLE
  powershell -File tools\post_click.ps1 -ProcessName JurassicTheHunted -X 215 -Y 423
#>
param(
  [Parameter(Mandatory)][string]$ProcessName,
  [Parameter(Mandatory)][int]$X,
  [Parameter(Mandatory)][int]$Y
)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class PC {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
"@
$p = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$h = $p.MainWindowHandle
$lp = [IntPtr](($Y -shl 16) -bor ($X -band 0xFFFF))
[void][PC]::PostMessage($h, 0x0200, [IntPtr]0, $lp)   # WM_MOUSEMOVE
Start-Sleep -Milliseconds 80
[void][PC]::PostMessage($h, 0x0201, [IntPtr]1, $lp)   # WM_LBUTTONDOWN
Start-Sleep -Milliseconds 80
[void][PC]::PostMessage($h, 0x0200, [IntPtr]1, $lp)   # WM_MOUSEMOVE (still over the target)
[void][PC]::PostMessage($h, 0x0202, [IntPtr]0, $lp)   # WM_LBUTTONUP
"clicked ($X, $Y)"
