<#
Makes the real-life set's recordings and photos from media.json (written by make.py):
  - each recording spoken by Windows' voices (David and Zira: two people in a call), 16 kHz mono WAV
  - each photo of paper drawn on a desk: a receipt or a letter, a little turned, with some noise

  powershell -ExecutionPolicy Bypass -File tests/agent-bench/reallife/make-media.ps1
#>
param([string]$Set = "") # another set's folder (with its media.json); this one by default
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Speech
Add-Type -AssemblyName System.Drawing
$here = if ($Set) { $Set } else { $PSScriptRoot }
$media = Get-Content (Join-Path $here "media.json") -Raw -Encoding UTF8 | ConvertFrom-Json
$data = Join-Path $here "data"

$voices = @{ David = "Microsoft David Desktop"; Zira = "Microsoft Zira Desktop" }
foreach ($rec in $media.speech) {
    $out = Join-Path $data $rec.file
    New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
    $synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
    $format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
    $synth.SetOutputToWaveFile($out, $format)
    $synth.Rate = 0
    foreach ($line in $rec.lines) {
        $synth.SelectVoice($voices[$line[0]])
        $p = New-Object System.Speech.Synthesis.PromptBuilder
        $p.AppendText($line[1])
        $p.AppendBreak([TimeSpan]::FromMilliseconds(600))
        $synth.Speak($p)
    }
    $synth.SetOutputToNull()
    $synth.Dispose()
    "spoke $($rec.file)"
}

$rng = New-Object System.Random 20261123
foreach ($pic in $media.pictures) {
    $out = Join-Path $data $pic.file
    New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
    $receipt = $pic.kind -eq "receipt"
    $font = if ($receipt) { New-Object System.Drawing.Font("Consolas", 19) } else { New-Object System.Drawing.Font("Arial", 20) }
    $paperW = if ($receipt) { 560 } else { 900 }
    $lineH = if ($receipt) { 30 } else { 38 }
    $paperH = 80 + $lineH * $pic.lines.Count
    $W = $paperW + 260; $H = $paperH + 220
    $bmp = New-Object System.Drawing.Bitmap($W, $H)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = "AntiAlias"; $g.TextRenderingHint = "AntiAliasGridFit"
    $g.Clear([System.Drawing.Color]::FromArgb(118, 96, 78)) # a wooden desk
    $g.TranslateTransform($W / 2, $H / 2)
    $g.RotateTransform($(if ($receipt) { -2.5 } else { 1.5 }))
    $g.TranslateTransform(-$paperW / 2, -$paperH / 2)
    $g.FillRectangle((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(20, 0, 0, 0))), 8, 10, $paperW, $paperH)
    $g.FillRectangle((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(247, 244, 236))), 0, 0, $paperW, $paperH)
    $ink = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(40, 38, 44))
    $y = 40
    foreach ($line in $pic.lines) {
        if ($receipt) {
            $g.DrawString($line, $font, $ink, 30, $y)
        } else {
            $bold = $line -cmatch '^[A-Z][A-Z \-]+$'
            $f = if ($bold) { New-Object System.Drawing.Font("Arial", 22, [System.Drawing.FontStyle]::Bold) } else { $font }
            $g.DrawString($line, $f, $ink, 50, $y)
        }
        $y += $lineH
    }
    $g.ResetTransform()
    # A phone camera's grain.
    for ($i = 0; $i -lt ($W * $H / 30); $i++) {
        $x = $rng.Next($W); $yy = $rng.Next($H)
        $c = $bmp.GetPixel($x, $yy); $d = $rng.Next(-18, 19)
        $bmp.SetPixel($x, $yy, [System.Drawing.Color]::FromArgb([Math]::Max(0, [Math]::Min(255, $c.R + $d)), [Math]::Max(0, [Math]::Min(255, $c.G + $d)), [Math]::Max(0, [Math]::Min(255, $c.B + $d))))
    }
    $g.Dispose()
    $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    "drew $($pic.file)"
}
