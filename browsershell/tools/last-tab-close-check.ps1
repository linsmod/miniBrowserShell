# tools/last-tab-close-check.ps1 - closing the last tab closes the window.
#
# Chrome behaviour: Ctrl+W / the close chip on the only tab closes the window
# (and, with one window, the process). The shell used to leave an empty window
# behind. AGB_TEST_CLOSE_TAB_MS closes the active tab from the message loop, so
# this is headless: nothing is shown and no input is synthesized.
#
#   pwsh -File tools\last-tab-close-check.ps1
param(
    [int]$Timeout = 15000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build-msvc\Release\minishell.exe'
$log = Join-Path $root 'build-msvc\Release\shell.log'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }

$work = Join-Path $env:TEMP ('mb-lasttab-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null

$page = Join-Path $work 'page.html'
'<!DOCTYPE html><html><body>last tab check</body></html>' | Set-Content -Encoding UTF8 $page

$before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }

$env:AGB_SHELL_HIDDEN = '1'
$env:AGB_TEST_CLOSE_TAB_MS = '800'
$shell = Start-Process -FilePath $exe `
    -ArgumentList @('--userdata', (Join-Path $work 'profile'),
                    ('file:///' + ($page -replace '\\', '/'))) `
    -WorkingDirectory $work -PassThru -WindowStyle Hidden
$childId = $shell.Id

try {
    $null = $shell.WaitForExit($Timeout)

    $lines = @()
    if (Test-Path $log) {
        $lines = @(Get-Content $log | Select-Object -Skip $before |
            Where-Object { $_ -match "pid=$childId" })
    }

    $closed = @($lines | Where-Object { $_ -match 'no tabs left, closing the window' })
    $crash = Get-ChildItem (Join-Path $work 'agb_crash*.txt') -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1

    if (-not $shell.HasExited) {
        "FAIL: the window stayed open after its last tab was closed"
        exit 1
    }
    if ($crash) {
        "FAIL: closing the last tab crashed (exit $($shell.ExitCode))"
        Get-Content $crash.FullName | Select-Object -First 15
        exit 1
    }
    if ($shell.ExitCode -ne 0) {
        "FAIL: the shell exited with $($shell.ExitCode)"
        exit 1
    }
    if ($closed.Count -eq 0) {
        'FAIL: the log does not show the last-tab close path'
        $lines | Select-Object -Last 10 | ForEach-Object { "  $_" }
        exit 1
    }

    "last tab closed the window; process exited (code $($shell.ExitCode))"
    'PASS: closing the last tab closes the window'
    exit 0
} finally {
    if (-not $shell.HasExited) { Stop-Process -Id $shell.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item Env:AGB_SHELL_HIDDEN, Env:AGB_TEST_CLOSE_TAB_MS -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
