$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$sourcePath = Join-Path $root 'assets/app-icon.png'
$targetPath = Join-Path $root 'assets/app-icon.ico'
Add-Type -AssemblyName System.Drawing

$source = [System.Drawing.Image]::FromFile($sourcePath)
$frames = [System.Collections.Generic.List[byte[]]]::new()
try {
    foreach ($size in @(16, 24, 32, 48, 64, 128, 256)) {
        $bitmap = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            try {
                $graphics.Clear([System.Drawing.Color]::Transparent)
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $graphics.DrawImage($source, 0, 0, $size, $size)
                $stream = [System.IO.MemoryStream]::new()
                try {
                    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
                    $frames.Add($stream.ToArray())
                } finally { $stream.Dispose() }
            } finally { $graphics.Dispose() }
        } finally { $bitmap.Dispose() }
    }
} finally { $source.Dispose() }

$output = [System.IO.MemoryStream]::new()
$writer = [System.IO.BinaryWriter]::new($output)
try {
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]$frames.Count)
    $dataOffset = 6 + 16 * $frames.Count
    for ($i = 0; $i -lt $frames.Count; $i++) {
        $size = @(16, 24, 32, 48, 64, 128, 256)[$i]
        $dimension = if ($size -eq 256) { [byte]0 } else { [byte]$size }
        $writer.Write($dimension)
        $writer.Write($dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([UInt16]1)
        $writer.Write([UInt16]32)
        $writer.Write([UInt32]$frames[$i].Length)
        $writer.Write([UInt32]$dataOffset)
        $dataOffset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $writer.Write($frame) }
    [System.IO.File]::WriteAllBytes($targetPath, $output.ToArray())
} finally {
    $writer.Dispose()
    $output.Dispose()
}
Write-Host "Icono generado: $targetPath"
