# tools/settings-check.ps1 - headless regression for minishell://settings.
#
# The page asks the host for its values over agavePostMessage; the probe answers
# with the same settingsSendAll() the shell uses, so the round trip can be
# checked without a GUI. Safe: the probe runs on its own throw-away profile.
#
#   pwsh -File tools\settings-check.ps1
param(
    [int]$Timeout = 30000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$eval = @'
var t = document.querySelector('input[name=theme]:checked');
probeExit('theme=' + (t ? t.value : 'none') +
          '|search=' + document.getElementById('search').value +
          '|zoom=' + document.getElementById('zoom').value +
          '|restore=' + document.getElementById('restore').checked +
          '|version=' + document.getElementById('version').textContent +
          '|downloads=' + document.getElementById('downloads').textContent);
'@

$evalFile = Join-Path $env:TEMP 'mb-settings-check.js'
Set-Content -Encoding UTF8 $evalFile $eval

$out = & (Join-Path $PSScriptRoot 'probe.ps1') -Url 'minishell://settings' `
    -EvalFile $evalFile -Wait 500 -Settle 900 -Timeout $Timeout -PassThru
Remove-Item $evalFile -ErrorAction SilentlyContinue

$line = ($out.output -split "`r?`n" | Where-Object { $_ -match '\[exit\]' } | Select-Object -First 1)
if (-not $line) {
    "FAIL: the page never reported; exit=$($out.exit)"
    $out.output | Select-Object -Last 10 | ForEach-Object { $_ }
    exit 1
}

$payload = ($line -replace '.*\[exit\]\s*', '').Trim()
$fields = @{}
foreach ($part in $payload -split '\|') {
    $eq = $part.IndexOf('=')
    if ($eq -gt 0) { $fields[$part.Substring(0, $eq)] = $part.Substring($eq + 1) }
}

$problems = @()
if ($fields['theme'] -notin @('system', 'light', 'dark')) { $problems += "theme='$($fields['theme'])'" }
if ($fields['search'] -notin @('baidu', 'bing', 'google', 'duckduckgo')) { $problems += "search='$($fields['search'])'" }
if ($fields['zoom'] -notmatch '^(75|90|100|110|125|150)$') { $problems += "zoom='$($fields['zoom'])'" }
if ($fields['restore'] -notin @('true', 'false')) { $problems += "restore='$($fields['restore'])'" }
if (-not $fields['version']) { $problems += 'version is empty (host never answered)' }
if (-not $fields['downloads']) { $problems += 'downloads is empty (host never answered)' }

"settings page: $payload"
"exit=$($out.exit) wall=$($out.seconds)s"

if ($problems.Count -gt 0) {
    "FAIL: " + ($problems -join '; ')
    exit 1
}

"PASS: the page received every value from the host"
exit 0
