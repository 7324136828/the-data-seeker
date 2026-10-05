$icoPath = Join-Path $PSScriptRoot "..\cpp\resources\icons\app.ico"
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)

# ICONDIR
$bw.Write([uint16]0) # Reserved
$bw.Write([uint16]1) # Type (1=ICO)
$bw.Write([uint16]1) # Count (1 image)

# ICONDIRENTRY
$width = 32
$height = 32
$bw.Write([byte]$width)
$bw.Write([byte]$height)
$bw.Write([byte]0)   # Color count
$bw.Write([byte]0)   # Reserved
$bw.Write([uint16]1) # Planes
$bw.Write([uint16]32)# BitCount
$imageSize = 40 + ($width * $height * 4) + ($width * $height / 8)
$bw.Write([uint32]$imageSize)
$bw.Write([uint32]22) # Offset

# BITMAPINFOHEADER
$bw.Write([uint32]40) # biSize
$bw.Write([int32]$width) # biWidth
$bw.Write([int32]($height * 2)) # biHeight (64)
$bw.Write([uint16]1) # biPlanes
$bw.Write([uint16]32)# biBitCount
$bw.Write([uint32]0) # biCompression (BI_RGB)
$bw.Write([uint32]($width * $height * 4)) # biSizeImage
$bw.Write([int32]0)  # biXPelsPerMeter
$bw.Write([int32]0)  # biYPelsPerMeter
$bw.Write([uint32]0) # biClrUsed
$bw.Write([uint32]0) # biClrImportant

# Pixel data: bottom-to-top, 32x32, 4 bytes per pixel (B, G, R, A)
for ($y = 0; $y -lt $height; $y++) {
    for ($x = 0; $x -lt $width; $x++) {
        # Border
        if ($x -eq 0 -or $x -eq ($width - 1) -or $y -eq 0 -or $y -eq ($height - 1)) {
            $bw.Write([byte]0x3B); $bw.Write([byte]0x29); $bw.Write([byte]0x1E); $bw.Write([byte]0xFF)
        }
        # Inset shape (DataForge 'D' logo)
        elseif ($x -ge 7 -and $x -le 11 -and $y -ge 7 -and $y -le 24) {
            $bw.Write([byte]0xFF); $bw.Write([byte]0xFF); $bw.Write([byte]0xFF); $bw.Write([byte]0xFF)
        }
        elseif ($x -ge 11 -and $x -le 22 -and ($y -ge 21 -or $y -le 10)) {
            $bw.Write([byte]0xFF); $bw.Write([byte]0xFF); $bw.Write([byte]0xFF); $bw.Write([byte]0xFF)
        }
        elseif ($x -ge 19 -and $x -le 23 -and $y -ge 10 -and $y -le 21) {
            $bw.Write([byte]0xFF); $bw.Write([byte]0xFF); $bw.Write([byte]0xFF); $bw.Write([byte]0xFF)
        }
        else {
            # DataForge blue (#3B82F6 -> B:0xF6, G:0x82, R:0x3B)
            $bw.Write([byte]0xF6); $bw.Write([byte]0x82); $bw.Write([byte]0x3B); $bw.Write([byte]0xFF)
        }
    }
}

# AND mask: 32 * 32 / 8 = 128 bytes of 0 (opaque)
for ($i = 0; $i -lt 128; $i++) {
    $bw.Write([byte]0)
}

[System.IO.File]::WriteAllBytes($icoPath, $ms.ToArray())
$bw.Close()
$ms.Close()
Write-Host "Generated $icoPath with size $([System.IO.FileInfo]::new($icoPath).Length) bytes"
