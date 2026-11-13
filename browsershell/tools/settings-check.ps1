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

/* On startup: the default is the New Tab page, with the page editor hidden. */
var sp = document.querySelector('input[name=startup]:checked');
var startupBefore = sp ? sp.value : 'none';
var row = document.getElementById('startupPagesRow');
var rowHiddenBefore = row.style.display === 'none';
document.querySelector('input[name=startup][value="2"]').click();
var rowShown = row.style.display !== 'none';
var ta = document.getElementById('startupPages');
ta.value = 'baidu.com\nhttps://example.com/path';
ta.dispatchEvent(new Event('change'));

/* The custom dropdown must open on the first click and pick on one click
 * (the native <select> popup needed two and landed in the wrong place). */
var btn = s.querySelector('.dd-btn');
btn.click();
var ddOpen = s.classList.contains('open');
s.querySelector('.dd-item[data-value=bing]').click();
var ddPick = s.getAttribute('data-value');
var ddClosed = !s.classList.contains('open');
probeExit('theme=' + theme + '|search=' + search + '|zoom=' + zoom +
          '|startupBefore=' + startupBefore +
          '|rowHiddenBefore=' + rowHiddenBefore +
          '|rowShown=' + rowShown +
          '|pagesText=' + ta.value.split('\n').join(',') +
          '|version=' + document.getElementById('version').textContent +
          '|versionBuilt=' + document.getElementById('versionBuilt').textContent +
          '|versionHref=' + document.getElementById('versionLink').href +
          '|versionTag=' + document.getElementById('versionLink').tagName +
          /* The link and the build time must stay on one line: the About cell
           * is right-aligned and 460px wide. Compare the text boxes (the <a>
           * inline box is taller than the <code> it wraps). */
          '|versionLine=' + (function(){var a=document.getElementById('version')
               .getBoundingClientRect(),
               b=document.getElementById('versionBuilt').getBoundingClientRect();
               return (a.width>0&&Math.abs(a.top-b.top)<=2)?1:0;})() +
          '|versionColor=' + getComputedStyle(document.getElementById('version')).color +
          '|downloads=' + document.getElementById('downloads').textContent +
          '|scale=' + document.getElementById('scale').textContent +
          '|dpi=' + document.getElementById('dpi').textContent +
          '|profile=' + document.getElementById('profile').textContent +
          '|profileSection=' + (document.getElementById('profile').closest('section')
                                .querySelector('h2').textContent) +
          '|project=' + document.getElementById('project').textContent +
          '|engine=' + document.getElementById('engine').textContent +
          '|enginehash=' + document.getElementById('enginehash').textContent +
          '|launch=' + document.getElementById('launch').textContent +
          '|env=' + document.getElementById('env').textContent +
          '|ddOpen=' + ddOpen + '|ddPick=' + ddPick + '|ddClosed=' + ddClosed);
'@

$evalFile = Join-Path $env:TEMP 'mb-settings-check.js'
Set-Content -Encoding UTF8 $evalFile $eval

# Fresh profile: the page writes settings through the host now, and a stale
# probe profile would carry them into the next run.
$work = Join-Path $env:TEMP ('mb-settings-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null

$log = Join-Path $root 'build-msvc\Release\shell.log'
$before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }

$out = & (Join-Path $PSScriptRoot 'probe.ps1') -Url 'minishell://settings' `
    -EvalFile $evalFile -Wait 500 -Settle 900 -Timeout $Timeout -PassThru `
    -UserData (Join-Path $work 'profile')
Remove-Item $evalFile -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue

$newLog = @()
if (Test-Path $log) {
    $newLog = @(Get-Content $log | Select-Object -Skip $before)
}

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
# On startup (Chrome's model): default is the New Tab page, editor hidden;
# choosing the specific-pages mode reveals the URL editor.
if ($fields['startupBefore'] -ne '0') { $problems += "startup default='$($fields['startupBefore'])' instead of 0 (new tab page)" }
if ($fields['rowHiddenBefore'] -ne 'True') { $problems += "pages editor visible before choosing the mode ('$($fields['rowHiddenBefore'])')" }
if ($fields['rowShown'] -ne 'True') { $problems += 'pages editor did not appear for the specific-pages mode' }
if ($fields['pagesText'] -ne 'baidu.com,https://example.com/path') { $problems += "pages editor text='$($fields['pagesText'])'" }

# The edit above went through the host: the log is the proof it was applied
# (and normalized to URLs). The probe answers sets with the shell's own code.
$startupLog = @($newLog | Where-Object { $_ -match 'settings: startup -> 2' })
$pageLog = @($newLog | Where-Object { $_ -match 'settings: startup page -> https://(baidu\.com|example\.com/path)' })
if ($startupLog.Count -eq 0) { $problems += 'the host never applied startup=2' }
if ($pageLog.Count -lt 2) { $problems += 'the host did not store both startup pages' }
if (-not $fields['version']) { $problems += 'version is empty (host never answered)' }
# Shell and engine are separate products: the shell row must not carry the
# engine's name, and the profile folder belongs to Data, not About.
if ($fields['version'] -match 'miniblink') { $problems += "shell version mentions the engine: '$($fields['version'])'" }
if ($fields['versionBuilt'] -notmatch '^, built ') { $problems += "shell build time missing: '$($fields['versionBuilt'])'" }
# The version half links to GitHub: a short hash goes to its commit page, a tag
# (or "unknown") only to the project, and the build time stays plain text.
if ($fields['versionTag'] -ne 'A') { $problems += "version is not a link (tag='$($fields['versionTag'])')" }
$projectUrl = 'https://github.com/linsmod/miniBrowserShell'
if ($fields['versionHref'] -notlike "$projectUrl*") { $problems += "version link='$($fields['versionHref'])'" }
$local = $fields['version'] -replace '-dirty$', ''
if ($local -match '^[0-9a-f]{6,}$') {
    if ($fields['versionHref'] -ne "$projectUrl/commit/$local") {
        $problems += "version link='$($fields['versionHref'])' instead of the commit page"
    }
} elseif ($fields['versionHref'] -ne $projectUrl) {
    $problems += "non-hash version '$local' links to '$($fields['versionHref'])' instead of the project"
}
if ($fields['versionLine'] -ne '1') { $problems += 'the version link and its build time are not on one line' }
# A clickable value must look clickable: the version text uses the accent
# colour, not the body text colour (checked as a blue-ish rgb in both themes).
if ($fields['versionColor'] -notmatch '^rgba?\((\d+), (\d+), (\d+)') {
    $problems += "version colour='$($fields['versionColor'])'"
} elseif ([int]$Matches[3] - [int]$Matches[1] -lt 40) {
    $problems += "version colour is not the link accent: '$($fields['versionColor'])'"
}
if (-not $fields['profile']) { $problems += 'profile is empty' }
if ($fields['profileSection'] -ne 'Data') { $problems += "profile lives in '$($fields['profileSection'])' instead of Data" }
if (-not $fields['downloads']) { $problems += 'downloads is empty (host never answered)' }
if ($fields['ddOpen'] -ne 'true') { $problems += "dropdown did not open on the first click (ddOpen='$($fields['ddOpen'])')" }
if ($fields['ddPick'] -ne 'bing') { $problems += "dropdown pick did not stick (ddPick='$($fields['ddPick'])')" }
if ($fields['ddClosed'] -ne 'true') { $problems += 'dropdown stayed open after picking' }
if ($fields['scale'] -notmatch '^[0-9]+%$') { $problems += "scale='$($fields['scale'])'" }
if ($fields['dpi'] -notmatch '^[0-9]+ dpi$') { $problems += "dpi='$($fields['dpi'])'" }

# About: the identification a bug report needs. The launch/env summary is
# sanitized, so it must never echo a path or URL.
if ($fields['project'] -ne 'https://github.com/linsmod/miniBrowserShell') { $problems += "project='$($fields['project'])'" }
if ($fields['engine'] -notmatch '^mb132_x64\.dll ') { $problems += "engine='$($fields['engine'])'" }
if ($fields['enginehash'] -notmatch '^[0-9a-f]{64}$') { $problems += "enginehash='$($fields['enginehash'])'" }
if (-not $fields['launch']) { $problems += 'launch is empty' }
if (-not $fields['env']) { $problems += 'env is empty' }
if ($fields['launch'] -match 'C:\\|://') { $problems += "launch leaks a path/url: '$($fields['launch'])'" }
if ($fields['env'] -match 'C:\\|://') { $problems += "env leaks a path/url: '$($fields['env'])'" }

"settings page: $payload"
"exit=$($out.exit) wall=$($out.seconds)s"

if ($problems.Count -gt 0) {
    "FAIL: " + ($problems -join '; ')
    exit 1
}

"PASS: the page received every value from the host"
exit 0
