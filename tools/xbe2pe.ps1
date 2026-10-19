# Port of black-pc/tools/xbe2pe.py: wrap the XBE's mapped image in a minimal PE32 (one RWX section, image base kept).
param([string]$In, [string]$Out)
$ErrorActionPreference = 'Stop'
$d = [IO.File]::ReadAllBytes($In)
function U32($o) { [BitConverter]::ToUInt32($d, $o) }
$base = U32 0x104; $hdrSize = U32 0x108; $imgSize = U32 0x10C; $timedate = U32 0x114
$nsec = U32 0x11C; $secHdr = (U32 0x120) - $base
$entry = [int64](U32 0x128) -bxor 0xA8FC57ABL            # retail key
if ($entry -lt $base -or $entry -ge $base + $imgSize) { $entry = [int64](U32 0x128) -bxor 0x94859D4BL }  # debug key
$PAGE = 0x1000
$size = ($imgSize + $PAGE - 1) -band (-bnot ($PAGE - 1))
$img = New-Object byte[] $size
[Array]::Copy($d, 0, $img, 0, $hdrSize)
for ($i = 0; $i -lt $nsec; $i++) {
    $h = $secHdr + $i * 0x38
    $va = U32 ($h + 4); $raw = U32 ($h + 0xC); $rawSize = U32 ($h + 0x10)
    [Array]::Copy($d, $raw, $img, $va - $base, $rawSize)
}
$bodyLen = $size - $PAGE
$ms = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter($ms)
$w.Write([byte[]](0x4D, 0x5A)); $w.Write((New-Object byte[] 58)); $w.Write([uint32]0x40)
# PE header + COFF
$w.Write([byte[]](0x50, 0x45, 0, 0)); $w.Write([uint16]0x14C); $w.Write([uint16]1); $w.Write([uint32]$timedate)
$w.Write([uint32]0); $w.Write([uint32]0); $w.Write([uint16]0xE0); $w.Write([uint16]0x0103)
# Optional header (PE32)
$w.Write([uint16]0x10B); $w.Write([byte]14); $w.Write([byte]0)
foreach ($v in @($bodyLen, 0, 0, ($entry - $base), $PAGE, $PAGE, $base, $PAGE, $PAGE)) { $w.Write([uint32]$v) }
foreach ($v in @(4, 0, 0, 0, 4, 0)) { $w.Write([uint16]$v) }
foreach ($v in @(0, $size, $PAGE, 0)) { $w.Write([uint32]$v) }
$w.Write([uint16]3); $w.Write([uint16]0)
foreach ($v in @(0x100000, 0x1000, 0x100000, 0x1000, 0, 16)) { $w.Write([uint32]$v) }
$w.Write((New-Object byte[] (16 * 8)))            # data directories
# Section header
$w.Write([Text.Encoding]::ASCII.GetBytes(".image`0`0"))
foreach ($v in @($bodyLen, $PAGE, $bodyLen, $PAGE, 0, 0)) { $w.Write([uint32]$v) }
$w.Write([uint16]0); $w.Write([uint16]0); $w.Write([uint32]0xE0000020L)
$w.Write((New-Object byte[] ($PAGE - $ms.Length)))
$w.Write($img, $PAGE, $bodyLen); $w.Flush()
[IO.File]::WriteAllBytes($Out, $ms.ToArray())
"wrote {0}: base 0x{1:x}, size 0x{2:x}, entry 0x{3:x}, {4} sections" -f $Out, $base, $size, $entry, $nsec
