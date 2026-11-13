# tools/newtab-open-check.ps1 - opening a new tab must not crash the shell.
#
# Regression for the mb132+0x2508e3 crash: createTabIn() activates the tab
# before the first navigation, and the new tab page's theme push used to call
# mbRunJs on a view with no document; the queued script then ran on a frame the
# engine had replaced. The handoff --new-tab path opens the same page as Ctrl+T.
#
# Everything is headless: AGB_SHELL_HIDDEN, no window is shown.
#
#   pwsh -File tools\newtab-open-check.ps1
param(
    [int]$Timeout = 20000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build-msvc\Release\minishell.exe'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }

$work = Join-Path $env:TEMP 'mb-newtab-open-check'
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null

$page = Join-Path $work 'page.html'
'<!DOCTYPE html><html><body style="margin:0">start</body></html>' |
    Set-Content -Encoding UTF8 $page

$env:AGB_SHELL_HIDDEN = '1'
$profileArg = Join-Path $work 'profile'

$shell = Start-Process -FilePath $exe `
    -ArgumentList @('--userdata', $profileArg, ('file:///' + ($page -replace '\\', '/'))) `
    -WorkingDirectory $work -PassThru -WindowStyle Hidden

try {
    Start-Sleep -Milliseconds 2500

    $client = Start-Process -FilePath $exe `
        -ArgumentList @('--userdata', $profileArg, '--new-tab') `
        -WorkingDirectory $work -PassThru -WindowStyle Hidden -Wait

    Start-Sleep -Seconds 2

    $crash = Join-Path $work 'agb_crash.txt'
    if ($shell.HasExited -or (Test-Path $crash)) {
        "FAIL: the shell died opening a new tab (exit $($shell.ExitCode))"
        if (Test-Path $crash) { Get-Content $crash | Select-Object -First 20 }
        exit 1
    }

    'PASS: opening minishell://newtab left the shell alive'
    exit 0
} finally {
    if (-not $shell.HasExited) { Stop-Process -Id $shell.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item Env:AGB_SHELL_HIDDEN -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
