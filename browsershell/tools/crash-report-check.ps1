# tools/crash-report-check.ps1 - the crash report identifies the build, and stays
# privacy-free.
#
# Runs a headless AGB_TEST_CRASH crash (AGB_CRASH_POPUP=0, so no page/window),
# then asserts that the report (agb_crash_test_<timestamp>.txt - test runs never
# touch the user's agb_crash_<timestamp>.txt) carries the project URL, the
# mb132_x64.dll SHA-256 and the sanitized launch args/env - while never echoing
# the startup URL, profile path or user name.
#
#   pwsh -File tools\crash-report-check.ps1
param(
    [int]$Timeout = 20000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build-msvc\Release\minishell.exe'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }

$work = Join-Path $env:TEMP 'mb-crash-report-check'
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null
# Reports are named agb_crash[_test]_<timestamp>.txt: pick the one this run wrote.
$reportPattern = Join-Path $work 'agb_crash_test_*.txt'
$legacyPattern = Join-Path $work 'agb_crash*.txt'

$env:AGB_TEST_CRASH = '1'
$env:AGB_CRASH_POPUP = '0'
$portable = @{
    AGB_NAV_GUARD_MS = '5000'
}
foreach ($k in $portable.Keys) { Set-Item -Path "Env:$k" -Value $portable[$k] }

$shell = Start-Process -FilePath $exe `
    -ArgumentList @('--userdata', (Join-Path $work 'profile'),
                    'https://example.com/some/private/page?token=abc') `
    -WorkingDirectory $work -PassThru -WindowStyle Hidden

try {
    $null = $shell.WaitForExit($Timeout)
    if (-not $shell.HasExited) {
        'FAIL: the test crash did not happen'
        exit 1
    }

    $report = Get-ChildItem $reportPattern -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $report) {
        $report = Get-ChildItem $legacyPattern -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime | Select-Object -Last 1
    }

    if (-not $report) {
        "FAIL: no crash report (exit $($shell.ExitCode))"
        exit 1
    }

    $text = Get-Content $report.FullName -Raw
    $problems = @()

    $pdb = Join-Path $root 'build-msvc\Release\minishell.pdb'
    if (-not (Test-Path $pdb)) { $problems += 'minishell.pdb missing (no /DEBUG build)' }
    if ($text -notmatch 'minishell\.exe\+0x[0-9a-f]+  [A-Za-z_][A-Za-z0-9_:]*\+0x[0-9a-f]+ \([a-z_]+\.c:[0-9]+\)') {
        $problems += 'no symbol + source line for the shell frames'
    }
    if ($text -notmatch '\(export\)') { $problems += 'no export fallback marker' }

    if ($text -notmatch 'Shell\s+MiniShell [^\r\n]+, built') { $problems += 'shell version/build missing' }
    if ($text -notmatch 'Engine\s+mb132_x64\.dll ') { $problems += 'engine version missing' }
    if ($text -notmatch 'Engine hash SHA-256 [0-9a-f]{64}') { $problems += 'engine SHA-256 missing' }
    if ($text -notmatch 'Project\s+https://github\.com/linsmod/miniBrowserShell') { $problems += 'project URL missing' }
    if ($text -notmatch 'Launch\r?\n------\r?\nargs\s+') { $problems += 'launch args missing' }
    if ($text -notmatch 'env\s+AGB_') { $problems += 'launch env missing' }
    if ($text -match 'example\.com|token=abc') { $problems += 'the startup URL leaked' }
    if ($text -match 'C:\\|Users\\') { $problems += 'a file path leaked' }

    $hashLine = ($text -split "`r?`n" | Where-Object { $_ -match 'Engine hash SHA-256' } | Select-Object -First 1)
    $hash = if ($hashLine -match '([0-9a-f]{64})') { $Matches[1] } else { '' }
    $dll = Join-Path $root 'build-msvc\Release\mb132_x64.dll'
    if ($hash -and (Test-Path $dll)) {
        $real = (Get-FileHash $dll -Algorithm SHA256).Hash.ToLower()
        if ($hash -ne $real) { $problems += 'hash does not match the DLL on disk' }
    }

    "report: $hashLine"
    ($text -split "`r?`n" | Where-Object { $_ -match '^(Project|args|env|Privacy)\s' }) |
        ForEach-Object { "  $_" }

    if ($problems.Count -gt 0) {
        'FAIL: ' + ($problems -join '; ')
        exit 1
    }

    'PASS: the report identifies the build and leaks no URL/path'
    exit 0
} finally {
    if (-not $shell.HasExited) { Stop-Process -Id $shell.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item Env:AGB_TEST_CRASH, Env:AGB_CRASH_POPUP, Env:AGB_NAV_GUARD_MS -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
