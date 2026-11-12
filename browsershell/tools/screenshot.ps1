# tools/screenshot.ps1 - headless page capture (mirror buffer, no window shown).
#
# The GUI shell is never started and nothing is captured from the desktop:
# --probe --shot grabs the control's own backing store, which works even though
# the window was never shown. Prefer numbers over pixels; use this when pixels
# are the only proof.
#
#   pwsh -File tools/screenshot.ps1 -Url https://www.baidu.com/ -Out baidu.png
#   pwsh -File tools/screenshot.ps1 -Url file:///C:/x.html -Out part.png -Rect 0,0,400,300
param(
    [Parameter(Mandatory = $true)][string]$Url,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Rect = "",
    [int]$Wait = 3000,
    [int]$Settle = 1000,
    [int]$Timeout = 60000,
    [string]$Eval = "",
    [string]$EvalFile = "",
    [string]$Inject = ""
)

$ErrorActionPreference = 'Stop'
$exe = Join-Path (Split-Path $PSScriptRoot -Parent) 'build-msvc\Release\minishell.exe'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }

$cli = @('--probe', $Url, '--shot', $Out,
         '--wait', "$Wait", '--settle', "$Settle", '--timeout', "$Timeout")
if ($Rect) { $cli += @('--shot-rect', $Rect) }
if ($Eval) { $cli += @('--eval', $Eval) }
if ($EvalFile) { $cli += @('--eval-file', $EvalFile) }
if ($Inject) { $cli += @('--inject', $Inject) }

$argLine = ($cli | ForEach-Object {
    if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
}) -join ' '

$outFile = Join-Path $env:TEMP ("mbshot-" + [guid]::NewGuid().ToString("N").Substring(0, 8) + ".out")
$errFile = $outFile + '.err'
Remove-Item Env:AGB_PROBE_SHOW -ErrorAction SilentlyContinue

$proc = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory (Split-Path $exe) `
    -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru -WindowStyle Hidden
$proc.WaitForExit([Math]::Max(1, $Timeout / 1000 + 10) * 1000) | Out-Null
$code = $proc.ExitCode

$text = ''
if (Test-Path $outFile) { $text += (Get-Content $outFile -Raw) }
if (Test-Path $errFile) { $text += (Get-Content $errFile -Raw) }
Remove-Item $outFile, $errFile -ErrorAction SilentlyContinue

$file = Get-Item $Out -ErrorAction SilentlyContinue
if ($file) {
    "shot=$Out $($file.Length) bytes exit=$code"
} else {
    "shot failed: no file written (exit=$code)"
    $text -split "`r?`n" | Select-Object -Last 8 | ForEach-Object { $_ }
}
exit $code
