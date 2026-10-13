$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$assets = Join-Path $PSScriptRoot 'assets'
New-Item -ItemType Directory -Path $assets -Force | Out-Null

$master = [Drawing.Bitmap]::new((Join-Path $assets 'oled-off-source.png'))

$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$images = @()
try {
    foreach ($size in $sizes) {
        $bitmap = [Drawing.Bitmap]::new($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $canvas = [Drawing.Graphics]::FromImage($bitmap)
        $canvas.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
        $canvas.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $canvas.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $canvas.DrawImage($master, [Drawing.Rectangle]::new(0, 0, $size, $size))
        $stream = [IO.MemoryStream]::new()
        try {
            $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
            if ($size -eq 256) {
                [IO.File]::WriteAllBytes((Join-Path $assets 'oled-off.png'), $stream.ToArray())
                $images += ,$stream.ToArray()
            }
            else {
                # Traditional DIB entries let Shell icon extraction handle
                # small icons; the 256px entry uses PNG compression.
                $dib = [IO.MemoryStream]::new()
                $dibWriter = [IO.BinaryWriter]::new($dib)
                try {
                    $maskStride = [int]([Math]::Ceiling($size / 32.0) * 4)
                    $dibWriter.Write([uint32]40)
                    $dibWriter.Write([int32]$size); $dibWriter.Write([int32]($size * 2))
                    $dibWriter.Write([uint16]1); $dibWriter.Write([uint16]32)
                    $dibWriter.Write([uint32]0); $dibWriter.Write([uint32]($size * $size * 4))
                    foreach ($field in 1..4) { $dibWriter.Write([uint32]0) }
                    for ($row = $size - 1; $row -ge 0; $row--) {
                        for ($column = 0; $column -lt $size; $column++) {
                            $pixel = $bitmap.GetPixel($column, $row)
                            $dibWriter.Write($pixel.B); $dibWriter.Write($pixel.G)
                            $dibWriter.Write($pixel.R); $dibWriter.Write($pixel.A)
                        }
                    }
                    for ($row = $size - 1; $row -ge 0; $row--) {
                        $mask = [byte[]]::new($maskStride)
                        for ($column = 0; $column -lt $size; $column++) {
                            if ($bitmap.GetPixel($column, $row).A -eq 0) {
                                $byteIndex = [int][Math]::Floor($column / 8.0)
                                $mask[$byteIndex] = $mask[$byteIndex] -bor (0x80 -shr ($column % 8))
                            }
                        }
                        $dibWriter.Write($mask)
                    }
                    $dibWriter.Flush()
                    $images += ,$dib.ToArray()
                }
                finally { $dibWriter.Dispose(); $dib.Dispose() }
            }
        }
        finally { $stream.Dispose(); $canvas.Dispose(); $bitmap.Dispose() }
    }
}
finally { $master.Dispose() }

$iconStream = [IO.MemoryStream]::new()
$writer = [IO.BinaryWriter]::new($iconStream)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($index = 0; $index -lt $sizes.Count; $index++) {
        $dimension = $sizes[$index] % 256
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$images[$index].Length); $writer.Write([uint32]$offset)
        $offset += $images[$index].Length
    }
    foreach ($bytes in $images) { $writer.Write([byte[]]$bytes) }
    $writer.Flush()
    [IO.File]::WriteAllBytes((Join-Path $assets 'oled-off.ico'), $iconStream.ToArray())
}
finally { $writer.Dispose(); $iconStream.Dispose() }
Write-Output 'Generated oled-off.ico from selected source: 16, 20, 24, 32, 40, 48, 64, 128, 256 px.'
