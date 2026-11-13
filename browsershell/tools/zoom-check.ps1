# tools/zoom-check.ps1 - page zoom really scales the page, and nothing is clipped.
#
# Zoom is a CSS zoom on the document's root (see mb_view.c): the engine's
# mbSetZoomFactor paints its factor twice in this build and cuts the page off.
# So what has to hold here is:
#   - the painted page grows/shrinks with the requested zoom (per display pixel),
#   - window.devicePixelRatio and innerWidth keep describing the display, not the
#     zoom (unlike an engine-level page zoom),
#   - the right edge of the page stays inside the window at every zoom.
#
#   pwsh -File tools\zoom-check.ps1
param(
    [int]$Timeout = 20000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build-msvc\Release\minishell.exe'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }
Add-Type -AssemblyName System.Drawing

$page = Join-Path $env:TEMP 'mb-zoom-check.html'
@'
<!DOCTYPE html><html><head><meta charset="utf-8"><title>zoom</title></head>
<body style="margin:0;background:#fff">
<div style="height:40px;background:#0088ff"></div>
<div style="position:absolute;right:0;top:60px;width:40px;height:40px;background:#000"></div>
</body></html>
'@ | Set-Content -Encoding UTF8 $page

$evalJs = Join-Path $env:TEMP 'mb-zoom-check.js'
"probeExit('iw=' + innerWidth + ' dpr=' + window.devicePixelRatio)" |
    Set-Content -Encoding UTF8 $evalJs

$url = 'file:///' + ($page -replace '\\', '/')
$profile = Join-Path $env:TEMP 'mb-zoom-check-profile'
$failed = 0

function Run-Probe([string[]]$extra, [string]$out)
{
    $cli = @('--probe', $url, '--userdata', $profile,
             '--wait', '400', '--settle', '600', '--timeout', "$Timeout") + $extra
    Remove-Item Env:AGB_PROBE_SHOW -ErrorAction SilentlyContinue
    $proc = Start-Process -FilePath $exe -ArgumentList $cli -RedirectStandardOutput $out `
        -RedirectStandardError ($out + '.err') -PassThru -WindowStyle Hidden
    $null = $proc.WaitForExit($Timeout + 15000)
    if (-not $proc.HasExited) { $proc.Kill() }
    return $proc.ExitCode
}

# Baseline: the display scale the engine paints at (1.25 on a 125% display).
$out = Join-Path $env:TEMP 'mb-zoom-check.out'
$null = Run-Probe @('--zoom', '100', '--eval-file', $evalJs) $out
$line = (Get-Content $out -Raw) -split "`r?`n" | Where-Object { $_ -match '\[exit\]' } | Select-Object -First 1
$text = if ($line) { ($line -replace '.*\[exit\]\s*', '').Trim() } else { '' }
$baseIw = 0; $dpr = 0.0
if ($text -match 'iw=(-?\d+)') { $baseIw = [int]$Matches[1] }
if ($text -match 'dpr=([0-9.]+)') { $dpr = [double]$Matches[1] }
if ($dpr -le 0) { throw "probe reported no devicePixelRatio: '$text'" }

foreach ($zoom in 80, 100, 125, 200) {
    $png = Join-Path $env:TEMP "mb-zoom-$zoom.png"
    $null = Run-Probe @('--zoom', "$zoom", '--shot', $png) ($out + '.shot')

    $bar = -1; $edge = -1; $w = 0
    if (Test-Path $png) {
        $b = [System.Drawing.Bitmap]::FromFile($png)
        try {
            $w = $b.Width
            for ($y = 0; $y -lt [Math]::Min(600, $b.Height); $y++) {
                $c = $b.GetPixel(5, $y)
                if (-not ($c.B -gt 150 -and $c.R -lt 80)) { break }
                $bar = $y + 1
            }
            for ($x = 0; $x -lt $b.Width; $x++) {
                for ($y = 0; $y -lt $b.Height; $y++) {
                    $c = $b.GetPixel($x, $y)
                    if ($c.R -lt 40 -and $c.G -lt 40 -and $c.B -lt 40) { $edge = $x; break }
                }
            }
        } finally { $b.Dispose() }
    }

    $wantBar = [int][Math]::Round(40 * $zoom / 100 * $dpr)
    $okBar = [Math]::Abs($bar - $wantBar) -le 2
    $okEdge = $w -gt 0 -and $edge -ge ($w - 3)
    "zoom $zoom%: painted bar=$bar (want ~$wantBar at dpr $dpr), right edge x=$edge/$w $(if ($okBar -and $okEdge) { 'ok' } else { 'FAIL' })"
    if (-not ($okBar -and $okEdge)) { $failed++ }
    Remove-Item $png -ErrorAction SilentlyContinue
}

Remove-Item $out, ($out + '.err'), ($out + '.shot'), ($out + '.shot.err') -ErrorAction SilentlyContinue

if ($failed -gt 0) {
    "FAIL: $failed zoom case(s)"
    exit 1
}

"PASS: zoom scales the page once and keeps the right edge (innerWidth $baseIw, dpr $dpr)"
exit 0
