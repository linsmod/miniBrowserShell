# tools/favicon-check.ps1 - real favicons in the tab strip.
#
# Two halves, both headless:
#   1. --ui-shot --ui-favicon <file>: the offline chrome render decodes a real
#      PNG and a real ICO, puts the icon on the first tab and asserts the strip
#      actually painted its pixels (not the letter chip).
#   2. A local HTTP page with <link rel="icon">: after the load the shell asks
#      the page for its icon URLs and downloads them through the control's own
#      network stack (agave_webview_fetch -> mbNetStartUrlRequest; the engine's
#      mbOnNetGetFavicon is a stub in this build and never fires). A hidden
#      shell means nothing is shown.
#
#   pwsh -File tools\favicon-check.ps1
param(
    [int]$Timeout = 15000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build-msvc\Release\minishell.exe'
$log = Join-Path $root 'build-msvc\Release\shell.log'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }

$work = Join-Path $env:TEMP ('mb-favicon-check-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null

# --- fixtures: a 32x32 solid magenta icon (a colour the chrome never uses) ---
Add-Type -AssemblyName System.Drawing

$bitmap = New-Object System.Drawing.Bitmap 32, 32
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.Clear([System.Drawing.Color]::FromArgb(255, 255, 0, 255))
$graphics.Dispose()

$pngPath = Join-Path $work 'icon.png'
$icoPath = Join-Path $work 'icon.ico'
$bitmap.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png)
$icon = [System.Drawing.Icon]::FromHandle($bitmap.GetHicon())
$stream = [System.IO.File]::Create($icoPath)
$icon.Save($stream)
$stream.Close()
$bitmap.Dispose()

$problems = @()

# --- 1. decode + paint, offline ---------------------------------------------
foreach ($case in @(@('png', $pngPath), @('ico', $icoPath))) {
    $kind = $case[0]
    $path = $case[1]
    $shot = Join-Path $work "shot-$kind.bmp"

    $out = & $exe --ui-shot $shot --ui-favicon $path 2>&1 | Out-String

    if ($out -notmatch 'ui-favicon decoded 32x32') {
        $problems += "$kind`: the icon was not decoded"
    }
    if ($out -notmatch 'ui-check tab-favicon-painted\s+ok=1') {
        $problems += "$kind`: the tab strip did not paint the decoded icon"
    }
    if ($out -notmatch 'UI_SHOT PASS') {
        $problems += "$kind`: --ui-shot failed (see its output)"
    }
    $pixels = if ($out -match 'pixels=(\d+)') { [int]$Matches[1] } else { 0 }
    "ui favicon $kind`: painted pixels=$pixels"
}

# --- 2. engine -> shell, over HTTP ------------------------------------------
$port = Get-Random -Minimum 24000 -Maximum 40000
$pngBytes = [System.IO.File]::ReadAllBytes($pngPath)

$server = Start-Job -ScriptBlock {
    param($port, $pngBytes)

    $listener = [System.Net.HttpListener]::new()
    $listener.Prefixes.Add("http://127.0.0.1:$port/")
    $listener.Start()
    $deadline = (Get-Date).AddSeconds(20)

    while ((Get-Date) -lt $deadline) {
        $task = $listener.GetContextAsync()
        while (-not $task.AsyncWaitHandle.WaitOne(250)) {
            if ((Get-Date) -ge $deadline) { break }
        }
        if (-not $task.IsCompleted) { continue }

        $ctx = $task.Result
        $path = $ctx.Request.Url.AbsolutePath

        try {
            if ($path -eq '/icon.png') {
                $ctx.Response.ContentType = 'image/png'
                $ctx.Response.ContentLength64 = $pngBytes.Length
                $ctx.Response.OutputStream.Write($pngBytes, 0, $pngBytes.Length)
            } else {
                $html = '<!DOCTYPE html><html><head>' +
                        '<link rel="icon" type="image/png" href="/icon.png">' +
                        '</head><body>favicon check</body></html>'
                $bytes = [Text.Encoding]::UTF8.GetBytes($html)
                $ctx.Response.ContentType = 'text/html'
                $ctx.Response.ContentLength64 = $bytes.Length
                $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
            }
        } catch {
        }

        $ctx.Response.Close()
    }

    $listener.Stop()
} -ArgumentList $port, $pngBytes

$before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }
$env:AGB_SHELL_HIDDEN = '1'
$shell = $null

try {
    Start-Sleep -Milliseconds 400
    $shell = Start-Process -FilePath $exe `
        -ArgumentList @('--userdata', (Join-Path $work 'profile'),
                        "http://127.0.0.1:$port/page.html") `
        -WorkingDirectory $work -PassThru -WindowStyle Hidden
    $childId = $shell.Id

    $fetched = $false
    $deadline = (Get-Date).AddMilliseconds($Timeout)
    while ((Get-Date) -lt $deadline) {
        $lines = @()
        if (Test-Path $log) {
            $lines = @(Get-Content $log | Select-Object -Skip $before |
                Where-Object { $_ -match "pid=$childId" })
        }
        if (@($lines | Where-Object { $_ -match 'favicon: \d+x\d+ from' }).Count -gt 0) {
            $fetched = $true
            break
        }
        if ($shell.HasExited) { break }
        Start-Sleep -Milliseconds 200
    }

    if ($shell.HasExited) {
        $problems += "the shell exited while loading the page (exit $($shell.ExitCode))"
    } elseif (-not $fetched) {
        $problems += 'the engine never delivered a favicon for http://127.0.0.1/page.html'
    } else {
        'the shell fetched and decoded a 32x32 favicon through the engine'
    }
} finally {
    if ($shell -and -not $shell.HasExited) {
        Stop-Process -Id $shell.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item Env:AGB_SHELL_HIDDEN -ErrorAction SilentlyContinue
    Stop-Job $server -ErrorAction SilentlyContinue
    Remove-Job $server -Force -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

if ($problems.Count -gt 0) {
    'FAIL: ' + ($problems -join '; ')
    exit 1
}

'PASS: favicons are decoded and painted (PNG + ICO), and the shell downloads them through the engine'
exit 0
