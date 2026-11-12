# tools/zoom-check.ps1 - page zoom and the display scale stay independent.
#
# The shell's chrome scales with the Windows DPI (uiThemeScale), the page does
# not: page zoom is the only thing that changes window.devicePixelRatio, so
# 100% means 100% whatever the display is set to. This pins that policy down:
#
#   zoom 100, display 100/125  ->  dpr 1.0,  innerWidth 1100
#   zoom 125, display 100/125  ->  dpr 1.25, innerWidth 880
#
#   pwsh -File tools\zoom-check.ps1
#   pwsh -File tools\zoom-check.ps1 -Exe build-check\Release\minishell.exe
param(
    [string]$Exe = "",
    [int]$Timeout = 30000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Exe) { $Exe = Join-Path $root 'build-msvc\Release\minishell.exe' }
if (-not (Test-Path $Exe)) { throw "minishell.exe not found: $Exe" }

$page = Join-Path $env:TEMP 'mb-zoom-check.html'
@'
<!DOCTYPE html><html><head><meta charset="utf-8"><title>zoom</title></head>
<body style="margin:0"><p>zoom check</p></body></html>
'@ | Set-Content -Encoding UTF8 $page

$eval = Join-Path $env:TEMP 'mb-zoom-check.js'
"probeExit('iw=' + innerWidth + ' dpr=' + window.devicePixelRatio)" | Set-Content -Encoding UTF8 $eval

$url = 'file:///' + ($page -replace '\\', '/')

$cases = @(
    @{ name = 'zoom 100, display 100'; zoom = 100; dpi = 100; iw = 1100; dpr = '1' },
    @{ name = 'zoom 100, display 125'; zoom = 100; dpi = 125; iw = 1100; dpr = '1' },
    @{ name = 'zoom 125, display 125'; zoom = 125; dpi = 125; iw = 880; dpr = '1.25' }
)

$failed = 0
foreach ($case in $cases) {
    $out = Join-Path $env:TEMP 'mb-zoom.out'
    $userData = Join-Path $env:TEMP ("mb-zoom-profile-" + $case.zoom + "-" + $case.dpi)
    $args = @('--probe', $url, '--zoom', "$($case.zoom)", '--userdata', $userData,
              '--eval-file', $eval, '--wait', '400', '--settle', '600', '--timeout', "$Timeout")
    if ($case.dpi -gt 100) { $args += @('--device-scale', "$($case.dpi)") }

    $proc = Start-Process -FilePath $Exe -ArgumentList $args -RedirectStandardOutput $out `
        -RedirectStandardError ($out + '.err') -PassThru -WindowStyle Hidden
    $null = $proc.WaitForExit($Timeout + 10000)
    if (-not $proc.HasExited) { $proc.Kill() }

    $line = (Get-Content $out -Raw) -split "`r?`n" | Where-Object { $_ -match '\[exit\]' } | Select-Object -First 1
    if (-not $line) {
        "FAIL: $($case.name): no result (exit $($proc.ExitCode))"
        $failed++
        continue
    }

    $text = ($line -replace '.*\[exit\]\s*', '').Trim()
    $gotIw = 0
    $gotDpr = ''
    if ($text -match 'iw=(-?\d+)') { $gotIw = [int]$Matches[1] }
    if ($text -match 'dpr=([0-9.]+)') { $gotDpr = [double]$Matches[1] }

    $ok = ($gotIw -eq $case.iw) -and ([Math]::Abs($gotDpr - [double]$case.dpr) -lt 0.01)
    "$($case.name): iw=$gotIw dpr=$gotDpr (want iw=$($case.iw) dpr=$($case.dpr)) $(if ($ok) { 'ok' } else { 'FAIL' })"
    if (-not $ok) { $failed++ }

    Remove-Item $out, ($out + '.err') -ErrorAction SilentlyContinue
}

if ($failed -gt 0) {
    "FAIL: page zoom leaked into the display scale (or the reverse)"
    exit 1
}

"PASS: page zoom is the only thing that changes devicePixelRatio"
exit 0
