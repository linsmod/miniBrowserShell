# tools/navguard-check.ps1 - the nav guard must not crash the engine.
#
# Regression for the mb132 0x5e666f AV: a page hangs past the nav guard (4 s), the
# guard destroys its control, and the request then fails in the engine. Before
# the fix the completion was dispatched into the freed webview and the whole
# browser died when the server closed the socket (6 s here).
#
# Everything is headless: a hidden shell window (AGB_SHELL_HIDDEN), a local
# accept-but-never-answer server, no desktop interaction.
#
#   pwsh -File tools/navguard-check.ps1
param(
    [int]$Port = 18091,
    [int]$CloseAfterMs = 6000,
    [int]$RunSeconds = 12
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build-msvc\Release\minishell.exe'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }

$work = Join-Path $env:TEMP 'mb-navguard-check'
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null

$server = Start-Process -FilePath pwsh `
    -ArgumentList @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'hangserver.ps1'),
                    '-Port', "$Port", '-CloseAfterMs', "$CloseAfterMs") `
    -PassThru -WindowStyle Hidden

Start-Sleep -Milliseconds 800

$env:AGB_SHELL_HIDDEN = '1'
$shell = Start-Process -FilePath $exe `
    -ArgumentList @('--userdata', (Join-Path $work 'profile'),
                    "http://127.0.0.1:$Port/hang") `
    -WorkingDirectory $work -PassThru -WindowStyle Hidden

try {
    $shell.WaitForExit($RunSeconds * 1000) | Out-Null

    if ($shell.HasExited) {
        "FAIL: the shell exited during the hang (exit $($shell.ExitCode))"
        if (Test-Path (Join-Path $work 'agb_crash.txt')) {
            Get-Content (Join-Path $work 'agb_crash.txt') | Select-Object -First 20
        }
        exit 1
    }

    "shell survived ${RunSeconds}s (nav guard rebuilt a view and the request failed)"
    "PASS: destroying a control with a load in flight no longer crashes the engine"
    exit 0
} finally {
    if (-not $shell.HasExited) { Stop-Process -Id $shell.Id -Force -ErrorAction SilentlyContinue }
    Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    Remove-Item Env:AGB_SHELL_HIDDEN -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
