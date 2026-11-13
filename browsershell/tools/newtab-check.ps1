# tools/newtab-check.ps1 - the new tab page, its search box and the engine
# attribution parameters.
#
# Loads minishell://newtab in the headless probe, fills the box and submits it.
# The page posts __agb_newtab_search__ and the probe prints the URL the shell
# would navigate to ([search] ...), so no network is needed.
#
#   pwsh -File tools\newtab-check.ps1
param(
    [int]$Timeout = 30000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build-msvc\Release\minishell.exe'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }

$evalFile = Join-Path $env:TEMP 'mb-newtab-check.js'
@'
(function(){
  var q = document.getElementById('q');
  var f = document.getElementById('f');
  if (!q || !f) { probeExit('missing search box'); return; }
  q.value = QUERY;
  f.dispatchEvent(new Event('submit', { cancelable: true }));
})();
'@ | Set-Content -Encoding UTF8 $evalFile

function Run-Query([string]$query, [string]$profile)
{
    $script = (Get-Content $evalFile -Raw).Replace('QUERY', "'" + $query.Replace("'", "\'") + "'")
    $file = Join-Path $env:TEMP ('mb-newtab-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.js')
    Set-Content -Encoding UTF8 $file $script

    $out = Join-Path $env:TEMP ('mb-newtab-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.out')
    $cli = @('--probe', 'minishell://newtab', '--userdata', $profile, '--eval-file', $file,
             '--wait', '400', '--settle', '700', '--timeout', "$Timeout")
    $proc = Start-Process -FilePath $exe -ArgumentList $cli -RedirectStandardOutput $out `
        -RedirectStandardError ($out + '.err') -PassThru -WindowStyle Hidden
    $null = $proc.WaitForExit($Timeout + 15000)
    if (-not $proc.HasExited) { $proc.Kill() }

    $text = Get-Content $out -Raw
    Remove-Item $file, $out, ($out + '.err') -ErrorAction SilentlyContinue

    $line = ($text -split "`r?`n" | Where-Object { $_ -match '\[search\]' } | Select-Object -First 1)
    if (-not $line) { return $null }
    return ($line -replace '.*\[search\]\s*', '').Trim()
}

$hash = (Get-FileHash (Join-Path $root 'build-msvc\Release\mb132_x64.dll') -Algorithm SHA256).Hash.ToLower()
$hash6 = $hash.Substring(0, 6)

function Run-PageCheck([bool]$dark)
{
    $file = Join-Path $env:TEMP ('mb-newtab-page-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.js')
    @'
    var m = document.querySelector('main').getBoundingClientRect();
    var wm = document.querySelector('.watermark svg path');
    probeExit('bg=' + getComputedStyle(document.body).backgroundColor +
              '|wm=' + (wm ? 1 : 0) +
              '|top=' + Math.round(m.top) + '|ih=' + innerHeight);
'@ | Set-Content -Encoding UTF8 $file

    if ($dark) { $env:AGB_NEWTAB_THEME = 'dark' }
    else { Remove-Item Env:AGB_NEWTAB_THEME -ErrorAction SilentlyContinue }

    $out = Join-Path $env:TEMP ('mb-newtab-page-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.out')
    $proc = Start-Process -FilePath $exe -ArgumentList @(
        '--probe', 'minishell://newtab', '--userdata', (Join-Path $env:TEMP 'mb-newtab-page-profile'),
        '--eval-file', $file, '--wait', '400', '--settle', '800', '--timeout', "$Timeout") `
        -RedirectStandardOutput $out -RedirectStandardError ($out + '.err') -PassThru -WindowStyle Hidden
    $null = $proc.WaitForExit($Timeout + 15000)
    if (-not $proc.HasExited) { $proc.Kill() }

    $text = Get-Content $out -Raw
    Remove-Item $file, $out, ($out + '.err') -ErrorAction SilentlyContinue
    Remove-Item Env:AGB_NEWTAB_THEME -ErrorAction SilentlyContinue

    $line = ($text -split "`r?`n" | Where-Object { $_ -match '\[exit\]' } | Select-Object -First 1)
    if (-not $line) { return $null }
    return ($line -replace '.*\[exit\]\s*', '').Trim()
}

$pageLight = Run-PageCheck $false
$pageDark = Run-PageCheck $true

$profile = Join-Path $env:TEMP 'mb-newtab-profile-baidu'
Remove-Item -Recurse -Force $profile -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $profile | Out-Null
"search=baidu`nzoom=100`nrestore=0`n" | Set-Content -Encoding ASCII (Join-Path $profile 'prefs.txt')

$baidu = Run-Query 'hello world' $profile
$urlQuery = Run-Query 'example.com' $profile

$profileGoogle = Join-Path $env:TEMP 'mb-newtab-profile-google'
Remove-Item -Recurse -Force $profileGoogle -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $profileGoogle | Out-Null
"search=google`nzoom=100`nrestore=0`n" | Set-Content -Encoding ASCII (Join-Path $profileGoogle 'prefs.txt')

$google = Run-Query 'hello world' $profileGoogle

$problems = @()

$baiduMatch = if ($baidu) { [regex]::Match($baidu, '^https://www\.baidu\.com/s\?wd=hello%20world&tn=minibrowsershell-([0-9a-f]{6})-([^&]+)$') } else { $null }
if (-not $baiduMatch -or -not $baiduMatch.Success) {
    $problems += "baidu URL='$baidu'"
} elseif ($baiduMatch.Groups[1].Value -ne $hash6) {
    $problems += "baidu hash6=$($baiduMatch.Groups[1].Value) want $hash6"
}

$googleMatch = if ($google) { [regex]::Match($google, '^https://www\.google\.com/search\?q=hello%20world&client=minibrowsershell-([0-9a-f]{6})-([^&]+)$') } else { $null }
if (-not $googleMatch -or -not $googleMatch.Success) {
    $problems += "google URL='$google'"
} elseif ($google -match '[?&]tn=') {
    $problems += 'google used tn= instead of client='
} elseif ($googleMatch.Groups[1].Value -ne $hash6) {
    $problems += "google hash6=$($googleMatch.Groups[1].Value) want $hash6"
}

if ($urlQuery -ne 'http://example.com') {
    $problems += "address-like query should navigate, got '$urlQuery'"
}

function Fields([string]$text)
{
    $map = @{}
    foreach ($part in ($text -split '\|')) {
        $eq = $part.IndexOf('=')
        if ($eq -gt 0) { $map[$part.Substring(0, $eq)] = $part.Substring($eq + 1) }
    }
    return $map
}

$light = Fields $pageLight
$dark = Fields $pageDark

if ($light['bg'] -ne 'rgb(255, 255, 255)') { $problems += "light background='$($light['bg'])'" }
if ($light['wm'] -ne '1') { $problems += 'logo watermark missing' }
$top = [int]$light['top']
$ih = [int]$light['ih']
if ($top -lt 40 -or ($ih -gt 0 -and $top -ge [int]($ih / 2))) {
    $problems += "content is not top-aligned: top=$top innerHeight=$ih"
}

if ($dark['bg'] -ne 'rgb(32, 33, 36)') { $problems += "dark background='$($dark['bg'])'" }
if ($dark['wm'] -ne '1') { $problems += 'logo watermark missing in dark mode' }

"baidu:  $baidu"
"google: $google"
"address: $urlQuery"
"engine hash6: $hash6"
"light page: $pageLight"
"dark page:  $pageDark"

if ($problems.Count -gt 0) {
    Remove-Item -Recurse -Force $profile, $profileGoogle -ErrorAction SilentlyContinue
    'FAIL: ' + ($problems -join '; ')
    exit 1
}

Remove-Item -Recurse -Force $profile, $profileGoogle -ErrorAction SilentlyContinue
'PASS: new tab search uses the default engine and the build attribution'
exit 0
