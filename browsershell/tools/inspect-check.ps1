# tools/inspect-check.ps1 - headless regression for the inspect page half.
#
# The host marker itself is a window, but everything that can be wrong about it
# is decided by assets/inspect.js: the rect it reports for an element, and the
# label it builds. This runs the probe (no window, no focus, no desktop capture),
# injects inspect.js, and asks it to report a known element; the probe prints
# [inspect] and the values are asserted here.
#
#   pwsh -File tools\inspect-check.ps1
#   pwsh -File tools\inspect-check.ps1 -Page C:\path\page.html
#
# Expected page: a #target element at 10,20 with size 100x40 (the default writes
# a temp page that has exactly that).
param(
    [string]$Page = "",
    [string]$Selector = "#target",
    [int]$ExpectX = 10,
    [int]$ExpectY = 20,
    [int]$ExpectW = 100,
    [int]$ExpectH = 40
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

if (-not $Page) {
    $Page = Join-Path $env:TEMP 'mb-inspect-check.html'
    @'
<!DOCTYPE html><html><head><meta charset="utf-8"><title>inspect check</title></head>
<body style="margin:0;padding:0">
<div id="target" style="position:absolute;left:10px;top:20px;width:100px;height:40px;background:#eee"></div>
</body></html>
'@ | Set-Content -Encoding UTF8 $Page
}

$url = 'file:///' + ($Page -replace '\\', '/')
$eval = "window.__mbInspect.report(document.querySelector('$Selector')); probeExit('dpr=' + window.devicePixelRatio)"

$out = & (Join-Path $PSScriptRoot 'probe.ps1') -Url $url -Inject inspect.js -Eval $eval -Wait 300 -Settle 200 -PassThru

$line = ($out.output -split "`r?`n" | Where-Object { $_ -match '\[inspect\]' } | Select-Object -First 1)
if (-not $line) {
    "FAIL: no [inspect] line; exit=$($out.exit)"
    $out.output | Select-Object -Last 10 | ForEach-Object { $_ }
    exit 1
}

# Payload: <device x,y,w,h>|<css x,y,w,h>|<dpr>|<label>|<selector>
$payload = ($line -replace '.*\[inspect\]\s*', '').Trim()
$parts = $payload -split '\|'
if ($parts.Count -lt 5) {
    "FAIL: payload is not dev|css|dpr|label|selector: $payload"
    exit 1
}

$dev = $parts[0] -split ','
$css = $parts[1] -split ','

if ($dev.Count -ne 4 -or $css.Count -ne 4) {
    "FAIL: rect is not x,y,w,h: $payload"
    exit 1
}

# The page's own ratio decides the conversion; the host must not apply its own.
$dpr = 1.0
if ($parts[2] -match '^[0-9.]+$') { $dpr = [double]$parts[2] }

$want = @($ExpectX, $ExpectY, $ExpectW, $ExpectH)
$cssOk = ($css[0] -eq "$($want[0])") -and ($css[1] -eq "$($want[1])") -and
         ($css[2] -eq "$($want[2])") -and ($css[3] -eq "$($want[3])")

$devOk = $true
for ($i = 0; $i -lt 4; $i++) {
    $expected = [math]::Round([int]$css[$i] * $dpr)
    if ([math]::Abs([int]$dev[$i] - $expected) -gt 1) { $devOk = $false }
}

"inspect report=$payload dpr=$dpr exit=$($out.exit) wall=$($out.seconds)s"
if ($cssOk -and $devOk) {
    "PASS: css $($css -join ',') (want $($want -join ',')) dev $($dev -join ',') at dpr $dpr"
    exit 0
}

"FAIL: css $($css -join ',') != expected $($want -join ','), dev $($dev -join ',') at dpr $dpr"
exit 1
