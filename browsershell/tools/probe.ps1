# tools/probe.ps1 - headless probe runner (no window, no focus, no desktop capture).
#
# Adopted from the agave rule set: every test goes through --probe; the process
# is never shown, never activated, and the shell never touches the desktop. The
# exit code is the verdict (0 OK, 1 usage, 2..4 timeout / no exit, 5 load error,
# 6 empty document).
#
#   pwsh -File tools/probe.ps1 -Url https://example.com/ -Eval "probeExit(document.title)"
#   pwsh -File tools/probe.ps1 -Url file:///C:/x.html -Inject inspect.js -EvalFile e.js
#   pwsh -File tools/probe.ps1 -Url ... -Eval "..." -Show     # debug only: show the probe window
#
# -Show sets AGB_PROBE_SHOW=1 for paths that behave differently when visible; it
# still never activates the window, so it cannot steal the user's focus.
#
# The child is started through Start-Process with redirected stdio: a nested
# pwsh cannot reliably run a console child with `&` in every sandbox, and the
# redirect also keeps the probe's stdout out of the caller's console noise.
param(
    [Parameter(Mandatory = $true)][string]$Url,
    [string]$Eval = "",
    [string]$EvalFile = "",
    [string]$Inject = "",
    [int]$Wait = 3000,
    [int]$Settle = 1000,
    [int]$Timeout = 60000,
    [string]$UserData = "",
    [switch]$Show,
    [switch]$PassThru
)

$ErrorActionPreference = 'Stop'
$exe = Join-Path (Split-Path $PSScriptRoot -Parent) 'build-msvc\Release\minishell.exe'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }

$cli = @('--probe', $Url, '--wait', "$Wait", '--settle', "$Settle", '--timeout', "$Timeout")
if ($Eval) { $cli += @('--eval', $Eval) }
if ($EvalFile) { $cli += @('--eval-file', $EvalFile) }
if ($Inject) { $cli += @('--inject', $Inject) }
if ($UserData) { $cli += @('--userdata', $UserData) }

if ($Show) { $env:AGB_PROBE_SHOW = '1' } else { Remove-Item Env:AGB_PROBE_SHOW -ErrorAction SilentlyContinue }

$outFile = Join-Path $env:TEMP ("mbprobe-" + [guid]::NewGuid().ToString("N").Substring(0, 8) + ".out")
$errFile = $outFile + '.err'

# Start-Process joins an array with spaces and does not quote: a script argument
# with a space used to arrive as several argv entries (the probe then treated the
# tail as the URL). Quote every argument that needs it.
$argLine = ($cli | ForEach-Object {
    if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
}) -join ' '

$sw = [Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory (Split-Path $exe) `
    -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru -WindowStyle Hidden
if (-not $proc.WaitForExit([Math]::Max(1, $Timeout / 1000 + 10) * 1000)) {
    $proc.Kill()
    $code = -2
} else {
    $code = $proc.ExitCode
}
$sw.Stop()

$text = ''
if (Test-Path $outFile) { $text += (Get-Content $outFile -Raw) }
if (Test-Path $errFile) {
    $err = Get-Content $errFile -Raw
    if ($err) { $text += $err }
}
Remove-Item $outFile, $errFile -ErrorAction SilentlyContinue

if ($PassThru) {
    return [pscustomobject]@{
        exit    = $code
        seconds = [math]::Round($sw.Elapsed.TotalSeconds, 2)
        output  = $text
    }
}

# Compact by default: only the probe/exit/inspect lines, so a passing run does
# not flood the caller's output.
$text -split "`r?`n" | Where-Object { $_ -match '\[(probe|exit|sanity|inspect|ctx|dump)\]' } | ForEach-Object { $_ }
"exit=$code wall=$([math]::Round($sw.Elapsed.TotalSeconds, 2))s"
exit $code
