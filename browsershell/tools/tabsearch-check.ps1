# tools/tabsearch-check.ps1 - the tab-search popup must survive page load.
#
# Regression for the popup closing by itself a few seconds after a fresh start:
# the engine sends a synthetic WM_ACTIVATE(WA_INACTIVE) to the shell while the
# shell is still the active window, and the shell used to close the popup on
# every WA_INACTIVE. AGB_TEST_TABSEARCH opens the popup before the message loop;
# AGB_SHELL_HIDDEN keeps it off the desktop.
#
#   pwsh -File tools\tabsearch-check.ps1
param(
    [int]$Timeout = 20000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build-msvc\Release\minishell.exe'
$log = Join-Path $root 'build-msvc\Release\shell.log'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }

$work = Join-Path $env:TEMP 'mb-tabsearch-check'
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null

$page = Join-Path $work 'page.html'
'<!DOCTYPE html><html><body style="margin:0">tab search check</body></html>' |
    Set-Content -Encoding UTF8 $page

$before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }

$env:AGB_SHELL_HIDDEN = '1'
$env:AGB_TEST_TABSEARCH = '1'

$shell = Start-Process -FilePath $exe `
    -ArgumentList @('--userdata', (Join-Path $work 'profile'),
                    ('file:///' + ($page -replace '\\', '/'))) `
    -WorkingDirectory $work -PassThru -WindowStyle Hidden
$childId = $shell.Id

try {
    # The synthetic WA_INACTIVE arrives ~3 s after start; wait past it.
    Start-Sleep -Seconds 5

    $lines = @()
    if (Test-Path $log) {
        $lines = Get-Content $log | Select-Object -Skip $before | Where-Object { $_ -match "pid=$childId" }
    }

    $opened = @($lines | Where-Object { $_ -match 'tab search: open' })
    $closed = @($lines | Where-Object { $_ -match 'tab search: close' })
    $ignored = @($lines | Where-Object { $_ -match 'ignore synthetic WA_INACTIVE' })

    if ($shell.HasExited) {
        "FAIL: the shell exited while the tab search was open (exit $($shell.ExitCode))"
        exit 1
    }
    if ($opened.Count -eq 0) {
        'FAIL: the tab search never opened'
        exit 1
    }
    if ($closed.Count -gt 0) {
        'FAIL: the tab search closed by itself:'
        $closed | ForEach-Object { "  $_" }
        exit 1
    }

    "opened; synthetic deactivation ignored: $($ignored.Count -gt 0)"
    'PASS: the tab search stays open across page load / synthetic WA_INACTIVE'
    exit 0
} finally {
    if (-not $shell.HasExited) { Stop-Process -Id $shell.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item Env:AGB_SHELL_HIDDEN, Env:AGB_TEST_TABSEARCH -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}



