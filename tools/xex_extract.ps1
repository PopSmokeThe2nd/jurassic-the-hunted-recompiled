<#
.SYNOPSIS
  Decrypt and decompress a retail XEX2 (basic compression) into its raw in-memory PE image.
.DESCRIPTION
  Used for reverse engineering (disassembly with powerpc-none-elf-objdump, string search).
  The output is the image as loaded at its base address (0x82000000 for default.xex).
.EXAMPLE
  powershell -File tools\xex_extract.ps1 game\default.xex build\default.bin
#>
param(
  [Parameter(Mandatory)][string]$XexPath,
  [Parameter(Mandatory)][string]$OutPath
)
$ErrorActionPreference = 'Stop'

function BE32([byte[]]$b, [int]$o) { ([uint32]$b[$o] -shl 24) -bor ([uint32]$b[$o+1] -shl 16) -bor ([uint32]$b[$o+2] -shl 8) -bor [uint32]$b[$o+3] }
function BE16([byte[]]$b, [int]$o) { ([uint16]$b[$o] -shl 8) -bor [uint16]$b[$o+1] }

$d = [System.IO.File]::ReadAllBytes((Resolve-Path $XexPath))
if ([System.Text.Encoding]::ASCII.GetString($d, 0, 4) -ne 'XEX2') { throw "Not an XEX2 file" }
$peOff  = BE32 $d 8
$secOff = BE32 $d 0x10
$nOpt   = BE32 $d 0x14
$imageSize = BE32 $d ($secOff + 4)
$loadAddr  = BE32 $d ($secOff + 0x110)

$ffi = $null
for ($i = 0; $i -lt $nOpt; $i++) {
  $key = BE32 $d (0x18 + $i * 8)
  if ($key -eq 0x000003FF) { $ffi = BE32 $d (0x18 + $i * 8 + 4) }
}
if ($null -eq $ffi) { throw "FILE_FORMAT_INFO header missing" }
$ffiSize = BE32 $d $ffi
$enc  = BE16 $d ($ffi + 4)
$comp = BE16 $d ($ffi + 6)
if ($comp -ne 1) { throw "Only basic compression (1) is supported; got $comp" }

$payload = New-Object byte[] ($d.Length - $peOff)
[Array]::Copy($d, $peOff, $payload, 0, $payload.Length)

if ($enc -eq 1) {
  # Public retail XEX2 key (also used by Xenia / XexTool); the per-title session key is wrapped with it.
  $retailKey = [byte[]](0x20,0xB1,0x85,0xA5,0x9D,0x28,0xFD,0xC3,0x40,0x58,0x3F,0xBB,0x08,0x96,0xBF,0x91)
  $fileKey = New-Object byte[] 16
  [Array]::Copy($d, $secOff + 0x150, $fileKey, 0, 16)

  $aes = [System.Security.Cryptography.Aes]::Create()
  $aes.Padding = 'None'
  $aes.Mode = 'ECB'; $aes.Key = $retailKey
  $sessionKey = $aes.CreateDecryptor().TransformFinalBlock($fileKey, 0, 16)

  $aes.Mode = 'CBC'; $aes.Key = $sessionKey; $aes.IV = New-Object byte[] 16
  $len = $payload.Length - ($payload.Length % 16)
  $dec = $aes.CreateDecryptor()
  $plain = New-Object byte[] $payload.Length
  [void]$dec.TransformBlock($payload, 0, $len, $plain, 0)
  $payload = $plain
}

$out = New-Object byte[] $imageSize
$src = 0; $dst = 0
$nBlocks = ($ffiSize - 8) / 8
for ($b = 0; $b -lt $nBlocks; $b++) {
  $dataSize = BE32 $d ($ffi + 8 + $b * 8)
  $zeroSize = BE32 $d ($ffi + 12 + $b * 8)
  [Array]::Copy($payload, $src, $out, $dst, $dataSize)
  $src += $dataSize
  $dst += $dataSize + $zeroSize
}

if ($out[0] -ne 0x4D -or $out[1] -ne 0x5A) { throw "Decrypted image has no MZ header (wrong key?)" }
$dir = Split-Path -Parent $OutPath
if ($dir) { New-Item -ItemType Directory -Force $dir | Out-Null }
[System.IO.File]::WriteAllBytes((Join-Path (Resolve-Path ($(if ($dir) { $dir } else { '.' }))) (Split-Path -Leaf $OutPath)), $out)
"image: {0} bytes at 0x{1:X8} ({2} blocks, encryption={3})" -f $imageSize, $loadAddr, $nBlocks, $enc
