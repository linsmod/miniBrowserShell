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
var s = document.getElementById('searchDD');
var z = document.getElementById('zoomDD');
var theme = t ? t.value : 'none';
var search = s ? s.getAttribute('data-value') : '';
var zoom = z ? z.getAttribute('data-value') : '';
/* The custom dropdown must open on the first click and pick on one click
 * (the native <select> popup needed two and landed in the wrong place). */
var btn = s.querySelector('.dd-btn');
btn.click();
var ddOpen = s.classList.contains('open');
s.querySelector('.dd-item[data-value=bing]').click();
var ddPick = s.getAttribute('data-value');
var ddClosed = !s.classList.contains('open');
probeExit('theme=' + theme + '|search=' + search + '|zoom=' + zoom +
          '|restore=' + document.getElementById('restore').checked +
          '|version=' + document.getElementById('version').textContent +
          '|downloads=' + document.getElementById('downloads').textContent +
          '|ddOpen=' + ddOpen + '|ddPick=' + ddPick + '|ddClosed=' + ddClosed);
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
if ($fields['ddOpen'] -ne 'true') { $problems += "dropdown did not open on the first click (ddOpen='$($fields['ddOpen'])')" }
if ($fields['ddPick'] -ne 'bing') { $problems += "dropdown pick did not stick (ddPick='$($fields['ddPick'])')" }
if ($fields['ddClosed'] -ne 'true') { $problems += 'dropdown stayed open after picking' }

"settings page: $payload"
"exit=$($out.exit) wall=$($out.seconds)s"

if ($problems.Count -gt 0) {
    "FAIL: " + ($problems -join '; ')
    exit 1
}

"PASS: the page received every value from the host"
exit 0
