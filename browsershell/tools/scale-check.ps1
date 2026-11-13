# tools/scale-check.ps1 - page scale and the right/bottom edge stay intact.
#
# Two regressions live here, both measured headlessly (no window, no desktop):
#
#  1. devicePixelRatio follows the display scale, not the page zoom. The probe
#     applies --zoom, but the adapter only records it: this miniblink build paints
#     a page-zoom factor twice (layout uses f, the surface uses f*f), which drew
#     the page larger than the window and cut off its right edge.
#  2. a marker pinned to the page's right edge must be visible in a --shot. When
#     the surface gets larger than the control, that marker disappears.
#
#   pwsh -File tools\scale-check.ps1
param(
    [int]$Timeout = 30000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build-msvc\Release\minishell.exe'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }
Add-Type -AssemblyName System.Drawing

$evalPage = Join-Path $env:TEMP 'mb-scale-page.html'
@'
<!DOCTYPE html><html><head><meta charset="utf-8"><title>scale</title></head>
<body style="margin:0;background:#fff">
<div style="height:40px;background:#0088ff"></div>
<div style="position:absolute;right:0;top:60px;width:40px;height:40px;background:#000"></div>
</body></html>
'@ | Set-Content -Encoding UTF8 $evalPage

$evalJs = Join-Path $env:TEMP 'mb-scale-eval.js'
"probeExit('iw=' + innerWidth + ' dpr=' + window.devicePixelRatio)" | Set-Content -Encoding UTF8 $evalJs
$url = 'file:///' + ($evalPage -replace '\\', '/')

function Run-Probe([string[]]$extra, [string]$out)
{
    $args = @('--probe', $url, '--userdata', (Join-Path $env:TEMP 'mb-scale-profile'),
              '--wait', '500', '--settle', '700', '--timeout', "$Timeout") + $extra
    $proc = Start-Process -FilePath $exe -ArgumentList $args -RedirectStandardOutput $out `
        -RedirectStandardError ($out + '.err') -PassThru -WindowStyle Hidden
    $null = $proc.WaitForExit($Timeout + 15000)
    if (-not $proc.HasExited) { $proc.Kill() }
    return $proc.ExitCode
}

$failed = 0

# --- devicePixelRatio follows the display scale -------------------------------
$cases = @(
    @{ name = 'display 125'; dpi = 125; zoom = 100; iw = 880;  dpr = 1.25 },
    @{ name = 'display 125, zoom key'; dpi = 125; zoom = 125; iw = 880; dpr = 1.25 }
)

foreach ($case in $cases) {
    $out = Join-Path $env:TEMP 'mb-scale.out'
    $extra = @('--zoom', "$($case.zoom)", '--eval-file', $evalJs)
    if ($case.dpi -gt 100) { $extra += @('--device-scale', "$($case.dpi)") }

    $null = Run-Probe $extra $out
    $line = (Get-Content $out -Raw) -split "`r?`n" | Where-Object { $_ -match '\[exit\]' } | Select-Object -First 1
    $text = if ($line) { ($line -replace '.*\[exit\]\s*', '').Trim() } else { '' }

    $gotIw = 0; $gotDpr = 0.0
    if ($text -match 'iw=(-?\d+)') { $gotIw = [int]$Matches[1] }
    if ($text -match 'dpr=([0-9.]+)') { $gotDpr = [double]$Matches[1] }

    $ok = ($gotIw -eq $case.iw) -and ([Math]::Abs($gotDpr - $case.dpr) -lt 0.02)
    "$($case.name): iw=$gotIw dpr=$gotDpr (want $($case.iw)/$($case.dpr)) $(if ($ok) { 'ok' } else { 'FAIL' })"
    if (-not $ok) { $failed++ }
    Remove-Item $out, ($out + '.err') -ErrorAction SilentlyContinue
}

# --- the right edge is not clipped -------------------------------------------
$png = Join-Path $env:TEMP 'mb-scale-edge.png'
$null = Run-Probe @('--device-scale', '125', '--shot', $png) (Join-Path $env:TEMP 'mb-scale.out')
if (-not (Test-Path $png)) {
    'FAIL: right-edge capture missing'
    $failed++
} else {
    $b = [System.Drawing.Bitmap]::FromFile($png)
    try {
        $y = [int]($b.Height * 0.11)   # inside the top:60px marker
        $max = -1
        for ($x = 0; $x -lt $b.Width; $x++) {
            $c = $b.GetPixel($x, $y)
            if ($c.R -lt 40 -and $c.G -lt 40 -and $c.B -lt 40) { $max = $x }
        }
        $ok = ($max -ge $b.Width - 3)
        "right edge: marker ends at x=$max of $($b.Width) $(if ($ok) { 'ok' } else { 'FAIL' })"
        if (-not $ok) { $failed++ }
    } finally { $b.Dispose() }
    Remove-Item $png -ErrorAction SilentlyContinue
}

if ($failed -gt 0) {
    "FAIL: $failed check(s)"
    exit 1
}

"PASS: the display scale is applied once and the right edge is intact"
exit 0
