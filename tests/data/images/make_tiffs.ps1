<#
Makes the TIFF fixtures with Windows' own TIFF encoder (WIC, through WPF), an encoder independent
of the app's decoder:  powershell -ExecutionPolicy Bypass -File tests\data\images\make_tiffs.ps1

  fax-none.tif, fax-g3.tif, fax-g4.tif, fax-rle.tif   300 x 40 black and white, the pattern below
  grey-lzw.tif, grey16.tif, palette-lzw.tif, rgba-zip.tif, rgb-rle.tif   8 x 4, red / blue (grey: light / dark)

The fax pattern (TestMain.cpp computes it the same way): rows 0-3 white; rows 4-6 black from x 10
to 249 (runs longer than 64 need the make-up codes); below, black where
(x + y / 3) % 37 < 5 + y % 11 (shapes that drift from row to row, for the two-dimensional codes).
make_tiff_extras.py writes the tiled, predictor, planar and CMYK ones.
#>
Add-Type -AssemblyName PresentationCore, WindowsBase
$here = $PSScriptRoot

function Save-Tiff($source, [string]$name, $compression) {
    $enc = New-Object System.Windows.Media.Imaging.TiffBitmapEncoder
    $enc.Compression = $compression
    $enc.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($source))
    $fs = [IO.File]::Create((Join-Path $here $name))
    try { $enc.Save($fs) } finally { $fs.Close() }
}

function New-Bitmap([int]$w, [int]$h, $format, [byte[]]$pixels, [int]$stride, $palette = $null) {
    [System.Windows.Media.Imaging.BitmapSource]::Create($w, $h, 96, 96, $format, $palette, $pixels, $stride)
}

$C = [System.Windows.Media.Imaging.TiffCompressOption]
$F = [System.Windows.Media.PixelFormats]

# Black and white, 1 bit a pixel, 1 = white (WPF's BlackWhite).
$W = 300; $H = 40; $stride = [int][math]::Ceiling($W / 8)
$bits = New-Object byte[] ($stride * $H)
for ($y = 0; $y -lt $H; $y++) {
    for ($x = 0; $x -lt $W; $x++) {
        if ($y -lt 4) { $black = $false }
        elseif ($y -lt 7) { $black = $x -ge 10 -and $x -lt 250 }
        else { $black = (($x + [math]::Floor($y / 3)) % 37) -lt (5 + $y % 11) }
        if (-not $black) { $bits[$y * $stride + [math]::Floor($x / 8)] = $bits[$y * $stride + [math]::Floor($x / 8)] -bor (0x80 -shr ($x % 8)) }
    }
}
$fax = New-Bitmap $W $H $F::BlackWhite $bits $stride
Save-Tiff $fax "fax-none.tif" $C::None
Save-Tiff $fax "fax-g3.tif" $C::Ccitt3
Save-Tiff $fax "fax-g4.tif" $C::Ccitt4
Save-Tiff $fax "fax-rle.tif" $C::Rle

# 8 x 4: left half red, right half blue (grey: light, dark).
$w = 8; $h = 4
$bgra = New-Object byte[] ($w * $h * 4); $bgr = New-Object byte[] ($w * $h * 3); $grey = New-Object byte[] ($w * $h)
$grey16 = New-Object byte[] ($w * $h * 2); $index = New-Object byte[] ($w * $h)
for ($y = 0; $y -lt $h; $y++) {
    for ($x = 0; $x -lt $w; $x++) {
        $i = $y * $w + $x; $left = $x -lt $w / 2
        $r = if ($left) { 230 } else { 20 }; $b = if ($left) { 20 } else { 230 }
        $bgra[$i * 4] = $b; $bgra[$i * 4 + 1] = 20; $bgra[$i * 4 + 2] = $r; $bgra[$i * 4 + 3] = 255
        $bgr[$i * 3] = $b; $bgr[$i * 3 + 1] = 20; $bgr[$i * 3 + 2] = $r
        $g = if ($left) { 220 } else { 30 }
        $grey[$i] = $g; $grey16[$i * 2] = 0; $grey16[$i * 2 + 1] = $g
        $index[$i] = if ($left) { 0 } else { 1 }
    }
}
$colours = New-Object 'System.Collections.Generic.List[System.Windows.Media.Color]'
$colours.Add([System.Windows.Media.Color]::FromRgb(230, 20, 20)); $colours.Add([System.Windows.Media.Color]::FromRgb(20, 20, 230))
$palette = [System.Windows.Media.Imaging.BitmapPalette]::new($colours)
Save-Tiff (New-Bitmap $w $h $F::Gray8 $grey $w) "grey-lzw.tif" $C::Lzw
Save-Tiff (New-Bitmap $w $h $F::Gray16 $grey16 ($w * 2)) "grey16.tif" $C::None
Save-Tiff (New-Bitmap $w $h $F::Indexed8 $index $w $palette) "palette-lzw.tif" $C::Lzw
Save-Tiff (New-Bitmap $w $h $F::Bgra32 $bgra ($w * 4)) "rgba-zip.tif" $C::Zip
Save-Tiff (New-Bitmap $w $h $F::Bgr24 $bgr ($w * 3)) "rgb-rle.tif" $C::Rle
"made the TIFF fixtures in $here"
