# tools/startup-check.ps1 - the "On startup" setting (Chrome's model).
#
# A fresh profile opens the New Tab page. "Continue where you left off" restores
# the session; "Open a specific page or set of pages" opens every configured URL
# as a tab. Each case runs a hidden shell on its own throw-away profile and
# reads the pid-tagged tab-create lines from shell.log. No window, no input.
#
#   pwsh -File tools\startup-check.ps1
param(
    [int]$Timeout = 8000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build-msvc\Release\minishell.exe'
$log = Join-Path $root 'build-msvc\Release\shell.log'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }

$page = '<!DOCTYPE html><html><body>startup check</body></html>'
$env:AGB_SHELL_HIDDEN = '1'
$problems = @()

function Invoke-Case {
    param(
        [string]$Name,
        [string]$Prefs,
        [string]$Session,
        [string[]]$Expect
    )

    $work = Join-Path $env:TEMP ('mb-startup-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $profile = Join-Path $work 'profile'
    New-Item -ItemType Directory -Force $profile | Out-Null

    if ($Prefs) { Set-Content -Encoding ASCII (Join-Path $profile 'prefs.txt') $Prefs }
    if ($Session) { Set-Content -Encoding ASCII (Join-Path $profile 'session.txt') $Session }

    $before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }
    $shell = Start-Process -FilePath $exe `
        -ArgumentList @('--userdata', $profile) `
        -WorkingDirectory $work -PassThru -WindowStyle Hidden
    $childId = $shell.Id

    $lines = @()
    try {
        $deadline = (Get-Date).AddMilliseconds($Timeout)
        while ((Get-Date) -lt $deadline) {
            if (Test-Path $log) {
                $lines = @(Get-Content $log | Select-Object -Skip $before |
                    Where-Object { $_ -match "pid=$childId" })
            }
            $missing = @($Expect | Where-Object {
                $pattern = $_
                @($lines | Where-Object { $_ -match $pattern }).Count -eq 0
            })
            if ($missing.Count -eq 0) { break }
            if ($shell.HasExited) { break }
            Start-Sleep -Milliseconds 150
        }

        if ($shell.HasExited) {
            $script:problems += "$Name`: the shell exited (exit $($shell.ExitCode))"
        } else {
            foreach ($pattern in $Expect) {
                if (@($lines | Where-Object { $_ -match $pattern }).Count -eq 0) {
                    $script:problems += "$Name`: no '$pattern' in the log"
                }
            }
        }

        "case $Name"
        $lines | Where-Object { $_ -match 'startup|tab create' } |
            ForEach-Object { "  $($_ -replace '^\s*\[[^]]*\]\s*', '')" }
    } finally {
        if (-not $shell.HasExited) { Stop-Process -Id $shell.Id -Force -ErrorAction SilentlyContinue }
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    }
}

try {
    Invoke-Case -Name 'new-tab-default' -Prefs "startup=0`n" -Session '' `
        -Expect @('startup: new tab page', 'tab create index=0 .*url=minishell://newtab')

    Invoke-Case -Name 'continue-session' `
        -Prefs "startup=1`n" -Session "https://example.com/one`n" `
        -Expect @('startup: continue where you left off \(tabs=1 first=https://example\.com/one',
                  'tab create index=0 .*url=https://example\.com/one')

    Invoke-Case -Name 'continue-without-session' -Prefs "startup=1`n" -Session '' `
        -Expect @('startup: continue where you left off \(tabs=0 first=minishell://newtab',
                  'tab create index=0 .*url=minishell://newtab')

    Invoke-Case -Name 'specific-pages' `
        -Prefs "startup=2`npage=https://www.baidu.com/`npage=https://example.com/path`n" `
        -Session '' `
        -Expect @('startup: 2 configured page\(s\), first=https://www\.baidu\.com/',
                  'tab create index=0 .*url=https://www\.baidu\.com/',
                  'tab create index=1 .*url=https://example\.com/path')

    # Migration: the previous release's restore flag and single homepage.
    Invoke-Case -Name 'legacy-restore' `
        -Prefs "restore=1`n" -Session "https://example.com/legacy`n" `
        -Expect @('tab create index=0 .*url=https://example\.com/legacy')

    Invoke-Case -Name 'legacy-home' `
        -Prefs "restore=0`nhome=https://example.com/home`n" -Session '' `
        -Expect @('startup: 1 configured page\(s\), first=https://example\.com/home',
                  'tab create index=0 .*url=https://example\.com/home')
} finally {
    Remove-Item Env:AGB_SHELL_HIDDEN -ErrorAction SilentlyContinue
}

if ($problems.Count -gt 0) {
    'FAIL: ' + ($problems -join '; ')
    exit 1
}

'PASS: the On startup setting opens the new tab page, the session, or the configured pages'
exit 0


