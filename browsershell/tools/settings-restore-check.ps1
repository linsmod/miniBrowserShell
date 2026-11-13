# tools/settings-restore-check.ps1 - a restored minishell://settings tab must
# get its values even when it starts in the background.
#
# Regression: onPageMessage() ignored every message from a tab that was not the
# active one. A restored session loads its tabs one by one and the last tab is
# the active one, so a settings tab restored behind it posted its one
# "__agb_settings_get__" while in the background, the host dropped it, and the
# page stayed blank until a manual reload asked again.
#
# The run is headless: AGB_SHELL_HIDDEN keeps the window off the desktop, and
# the shell only has to answer, so nothing is shown or focused.
#
#   pwsh -File tools\settings-restore-check.ps1
param(
    [int]$Timeout = 20000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build-msvc\Release\minishell.exe'
$log = Join-Path $root 'build-msvc\Release\shell.log'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }

$work = Join-Path $env:TEMP ('mb-settings-restore-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$profile = Join-Path $work 'profile'
New-Item -ItemType Directory -Force $profile | Out-Null

$page = Join-Path $work 'page.html'
'<!DOCTYPE html><html><body style="margin:0">settings restore check</body></html>' |
    Set-Content -Encoding UTF8 $page

# Session restore on, and a session whose first tab is settings. createTab()
# activates every tab it creates, so the file page below ends up active and
# settings loads in the background - exactly the reported situation.
Set-Content -Encoding ASCII (Join-Path $profile 'prefs.txt') "search=baidu`nzoom=100`nrestore=1"
Set-Content -Encoding ASCII (Join-Path $profile 'session.txt') ("minishell://settings`n" +
    ('file:///' + ($page -replace '\\', '/')))

$before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }

$env:AGB_SHELL_HIDDEN = '1'
$shell = Start-Process -FilePath $exe `
    -ArgumentList @('--userdata', $profile) `
    -WorkingDirectory $work -PassThru -WindowStyle Hidden
$childId = $shell.Id

try {
    $lines = @()
    $deadline = (Get-Date).AddMilliseconds($Timeout)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $log) {
            $lines = @(Get-Content $log | Select-Object -Skip $before |
                Where-Object { $_ -match "pid=$childId" })
        }
        if (@($lines | Where-Object { $_ -match 'settings: values sent' }).Count -gt 0) { break }
        if ($shell.HasExited) { break }
        Start-Sleep -Milliseconds 200
    }

    if ($shell.HasExited) {
        "FAIL: the shell exited while restoring the session (exit $($shell.ExitCode))"
        exit 1
    }

    $background = @($lines | Where-Object { $_ -match 'settings: background tab requested values' })
    $sent = @($lines | Where-Object { $_ -match 'settings: values sent delivered=(\d+)' })
    $delivered = 0
    foreach ($line in $sent) {
        if ($line -match 'delivered=(\d+)') { $delivered = [int]$Matches[1] }
    }

    if ($background.Count -eq 0) {
        'FAIL: the settings tab was not restored in the background; the test did not exercise the fix'
        $lines | ForEach-Object { "  $_" }
        exit 1
    }
    if ($sent.Count -eq 0) {
        'FAIL: the restored settings tab never got an answer'
        $lines | ForEach-Object { "  $_" }
        exit 1
    }
    if ($delivered -lt 10) {
        "FAIL: only $delivered values reached the page (expected the full set)"
        exit 1
    }

    "background request answered; values delivered=$delivered"
    'PASS: a restored background settings tab receives its values without a reload'
    exit 0
} finally {
    if (-not $shell.HasExited) { Stop-Process -Id $shell.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item Env:AGB_SHELL_HIDDEN -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
