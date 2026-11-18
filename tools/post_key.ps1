<#
.SYNOPSIS
  Post a key press (down + up) to the game window's message queue, for testing hotkeys
  (e.g. F2) without focusing the window or touching the real keyboard.
.EXAMPLE
  powershell -File tools\post_key.ps1 -VirtualKey 0x71   # F2
#>
param(
  [Parameter(Mandatory)][int]$VirtualKey,
  [string]$ProcessName = "jurassichunted",
  [int]$Repeat = 1
)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class PK {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
}
"@
$p = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$h = $p.MainWindowHandle
$scan = [PK]::MapVirtualKey([uint32]$VirtualKey, 0)
$down = [IntPtr](1 -bor ($scan -shl 16))
$up = [IntPtr]([int64](1 -bor ($scan -shl 16) -bor (1 -shl 30)) -bor ([int64]1 -shl 31))
for ($i = 0; $i -lt $Repeat; $i++) {
  [void][PK]::PostMessage($h, 0x100, [IntPtr]$VirtualKey, $down)
  Start-Sleep -Milliseconds 80
  [void][PK]::PostMessage($h, 0x101, [IntPtr]$VirtualKey, $up)
  Start-Sleep -Milliseconds 120
}
"posted vk=0x{0:X2} x{1} to window {2}" -f $VirtualKey, $Repeat, $h
