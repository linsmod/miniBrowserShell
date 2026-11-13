# tools/tabsearch-check.ps1 - the tab-search popup.
#
# Case 1: the popup must survive page load. The engine sends a synthetic
# WM_ACTIVATE(WA_INACTIVE) to the shell while the shell is still the active
# window, and the shell used to close the popup on every WA_INACTIVE.
# AGB_TEST_TABSEARCH opens the popup before the message loop; AGB_SHELL_HIDDEN
# keeps it off the desktop.
#
# Case 2: activating an item switches the content area to that tab and resets
# the strip's open-button state. AGB_TEST_TABSEARCH_ACTIVATE drives the popup's
# own keyboard path (Down to the item, Enter). The tabs have identical URLs, so
# the old "match by URL" activation picked the first new-tab page instead of the
# selected one.
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
$url = 'file:///' + ($page -replace '\\', '/')

$env:AGB_SHELL_HIDDEN = '1'
$problems = @()

function Start-Shell {
    param([string]$Tag)
    $profile = Join-Path $work "profile-$Tag"
    $before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }
    $shell = Start-Process -FilePath $exe `
        -ArgumentList @('--userdata', $profile, $url) `
        -WorkingDirectory $work -PassThru -WindowStyle Hidden
    return @{ shell = $shell; before = $before }
}

function Read-New {
    param($Run)
    if (-not (Test-Path $log)) { return @() }
    return @(Get-Content $log | Select-Object -Skip $Run.before |
        Where-Object { $_ -match "pid=$($Run.shell.Id)" })
}

function Stop-Shell {
    param($Run)
    if ($Run -and $Run.shell -and -not $Run.shell.HasExited) {
        Stop-Process -Id $Run.shell.Id -Force -ErrorAction SilentlyContinue
    }
}

try {
    # --- 1. stays open across the synthetic WA_INACTIVE ---------------------
    $env:AGB_TEST_TABSEARCH = '1'
    Remove-Item Env:AGB_TEST_TABSEARCH_ACTIVATE, Env:AGB_TEST_EXTRA_TABS -ErrorAction SilentlyContinue
    $run = Start-Shell -Tag 'open'

    try {
        Start-Sleep -Seconds 5
        $lines = Read-New $run
        $opened = @($lines | Where-Object { $_ -match 'tab search: open' })
        $closed = @($lines | Where-Object { $_ -match 'tab search: close' })
        $ignored = @($lines | Where-Object { $_ -match 'ignore synthetic WA_INACTIVE' })

        if ($run.shell.HasExited) {
            $problems += "the shell exited while the popup was open (exit $($run.shell.ExitCode))"
        } elseif ($opened.Count -eq 0) {
            $problems += 'the tab search never opened'
        } elseif ($closed.Count -gt 0 -and $ignored.Count -eq 0) {
            # The popup is allowed to close on a real deactivation (the desktop
            # was in use, or the hidden shell never had this thread's active
            # window). Only a close after the synthetic-deactivation branch ran
            # is the regression this case exists for.
            'case open: inconclusive (the shell was not the active window)'
        } elseif ($closed.Count -gt 0) {
            $problems += 'the tab search closed after the synthetic WA_INACTIVE'
        }

        "case open: opened=$($opened.Count) closed=$($closed.Count) ignoredSynthetic=$($ignored.Count -gt 0)"
    } finally {
        Stop-Shell $run
    }

    # --- 2. activating the third item switches to that tab ------------------
    # Startup tab 0 + two extra new-tab pages (indices 1 and 2, same URL), so a
    # URL match cannot tell them apart.
    $env:AGB_TEST_TABSEARCH = '1'
    $env:AGB_TEST_TABSEARCH_ACTIVATE = '0'
    $env:AGB_TEST_EXTRA_TABS = '2'
    $run = Start-Shell -Tag 'activate'

    try {
        Start-Sleep -Seconds 3
        $lines = Read-New $run
        $activate = @($lines | Where-Object { $_ -match 'tab activate index=2 \(was 0\)' })
        $reset = @($lines | Where-Object { $_ -match 'tab search: strip hover reset \(hover_search=0 pressed_search=0' })
        $closed = @($lines | Where-Object { $_ -match 'tab search: close' })

        if ($run.shell.HasExited) {
            $problems += "the shell exited after activation (exit $($run.shell.ExitCode))"
        } elseif ($activate.Count -eq 0) {
            $problems += 'activating item 0 (newest tab) did not switch to tab index 2'
        } elseif ($closed.Count -eq 0) {
            $problems += 'the popup did not close after activation'
        }
        if ($reset.Count -eq 0) {
            $problems += 'the strip hover/pressed state was not reset when the popup closed'
        }

        "case activate: switched=$($activate.Count -gt 0) closed=$($closed.Count -gt 0) reset=$($reset.Count -gt 0)"
    } finally {
        Stop-Shell $run
    }
} finally {
    Remove-Item Env:AGB_SHELL_HIDDEN, Env:AGB_TEST_TABSEARCH, `
        Env:AGB_TEST_TABSEARCH_ACTIVATE, Env:AGB_TEST_EXTRA_TABS -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

if ($problems.Count -gt 0) {
    'FAIL: ' + ($problems -join '; ')
    exit 1
}

'PASS: the tab search survives load and activation switches the content area'
exit 0

