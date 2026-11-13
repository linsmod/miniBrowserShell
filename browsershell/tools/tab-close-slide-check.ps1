# tools/tab-close-slide-check.ps1 - clicking a tab's X slides the row, so the
# next tab's X lands under the pointer; the widths re-flow only once the pointer
# leaves the tab strip.
#
# Headless: the real pointer is never touched. AGB_TEST_CLOSE_CHIP puts a
# synthetic cursor on the active tab's close chip before closing it, then
# AGB_TEST_CLOSE_TAB_MS' timer moves that cursor out of the strip and posts a
# mouse move, which is what ends the slide. Extra tabs keep the window alive.
#
#   pwsh -File tools\tab-close-slide-check.ps1
param(
    [int]$Timeout = 15000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build-msvc\Release\minishell.exe'
$log = Join-Path $root 'build-msvc\Release\shell.log'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }

$work = Join-Path $env:TEMP ('mb-slide-check-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null

$page = Join-Path $work 'page.html'
'<!DOCTYPE html><html><body>close slide check</body></html>' | Set-Content -Encoding UTF8 $page

$before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }

$env:AGB_SHELL_HIDDEN = '1'
$env:AGB_TEST_CLOSE_TAB_MS = '900'
$env:AGB_TEST_CLOSE_CHIP = '1'
$env:AGB_TEST_EXTRA_TABS = '2'
$shell = Start-Process -FilePath $exe `
    -ArgumentList @('--userdata', (Join-Path $work 'profile'),
                    ('file:///' + ($page -replace '\\', '/'))) `
    -WorkingDirectory $work -PassThru -WindowStyle Hidden
$childId = $shell.Id

try {
    $deadline = (Get-Date).AddMilliseconds($Timeout)
    $lines = @()
    $all = @()

    while ((Get-Date) -lt $deadline) {
        if (Test-Path $log) {
            $all = @(Get-Content $log | Select-Object -Skip $before)
            $lines = @($all | Where-Object { $_ -match "pid=$childId" })
        }
        if (@($lines | Where-Object { $_ -match 'close slide: pointer left' }).Count -gt 0) { break }
        if ($shell.HasExited) { break }
        Start-Sleep -Milliseconds 150
    }

    $closed = @($all | Where-Object { $_ -match 'tab close index=' })
    $frozen = @($lines | Where-Object { $_ -match 'close slide: widths frozen' })
    $ended = @($lines | Where-Object { $_ -match 'close slide: pointer left' })

    if ($shell.HasExited) {
        "FAIL: the shell died during the close slide (exit $($shell.ExitCode))"
        exit 1
    }
    if ($closed.Count -eq 0) {
        'FAIL: the test did not close a tab'
        exit 1
    }
    if ($frozen.Count -eq 0) {
        'FAIL: closing on the X did not freeze the tab widths'
        $lines | Select-Object -Last 12 | ForEach-Object { "  $_" }
        exit 1
    }
    if ($ended.Count -eq 0) {
        'FAIL: leaving the tab strip did not re-flow the tabs'
        exit 1
    }

    "closed on the X -> widths frozen; cursor left the strip -> relayout"
    'PASS: the close slide works and ends when the pointer leaves the tab strip'
    exit 0
} finally {
    if (-not $shell.HasExited) { Stop-Process -Id $shell.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item Env:AGB_SHELL_HIDDEN, Env:AGB_TEST_CLOSE_TAB_MS, Env:AGB_TEST_CLOSE_CHIP, Env:AGB_TEST_EXTRA_TABS -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
