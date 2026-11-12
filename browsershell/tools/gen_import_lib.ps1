# Regenerates lib\mb132_x64.def + lib\mb132_x64.lib from the DLL's export table.
# Requires MSYS2's objdump.exe (mingw64\bin) and VS 2022 (lib.exe via vcvars64).
param(
    [string]$Dll = (Join-Path $PSScriptRoot '..\..\mb132_x64.dll')
)

$ErrorActionPreference = 'Stop'

$Dll = (Resolve-Path $Dll).Path
$libDir = Join-Path $PSScriptRoot '..\lib'
New-Item -ItemType Directory -Force $libDir | Out-Null

$objdump = (Get-Command objdump.exe -ErrorAction SilentlyContinue).Source
if (-not $objdump) {
    throw 'objdump.exe not found; add C:\msys64\mingw64\bin to PATH'
}

$exports = & $objdump -p $Dll |
    Where-Object { $_ -match '^\s+\[\s*\d+\].*\s(mb\w+)$' } |
    ForEach-Object { ($_ -replace '^\s+\[\s*\d+\].*\s', '').Trim() } |
    Sort-Object -Unique

if ($exports.Count -eq 0) {
    throw "no mb* exports found in $Dll"
}

$defPath = Join-Path $libDir 'mb132_x64.def'
$libPath = Join-Path $libDir 'mb132_x64.lib'
$def = "LIBRARY mb132_x64.dll`r`nEXPORTS`r`n" + (($exports | ForEach-Object { "  $_" }) -join "`r`n") + "`r`n"
Set-Content -Path $defPath -Value $def -Encoding Ascii

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'vswhere.exe not found; install Visual Studio 2022 Build Tools'
}

$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) {
    throw 'Visual Studio with C++ tools not found'
}

$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found under $vsRoot"
}

cmd /c "call `"$vcvars`" >nul && lib /nologo /def:`"$defPath`" /out:`"$libPath`" /machine:x64"
if ($LASTEXITCODE -ne 0) {
    throw "lib.exe failed with exit code $LASTEXITCODE"
}

Write-Host "import library written: $libPath ($($exports.Count) exports)"
