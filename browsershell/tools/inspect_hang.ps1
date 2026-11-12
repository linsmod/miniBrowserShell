# Attach cdb to a running minishell, dump all thread stacks + critical
# sections, detach, and symbolize minishell/mb132 frames with the map/export
# tables.  Read-only: the target is resumed when the script ends.
#
#   pwsh -File tools\inspect_hang.ps1                 # auto-pick a stuck window
#   pwsh -File tools\inspect_hang.ps1 -ProcessId 1234
#   pwsh -File tools\inspect_hang.ps1 -Symbols        # also .symfix (slow)
param(
    [int]$ProcessId = 0,
    [switch]$Symbols,
    [int]$TimeoutSec = 90
)

$ErrorActionPreference = 'Stop'

function Find-Cdb {
    $alias = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\cdbX64.exe'
    if (Test-Path $alias) { return $alias }
    $pkg = Get-AppxPackage -Name 'Microsoft.WinDbg' -ErrorAction SilentlyContinue |
           Sort-Object Version -Descending | Select-Object -First 1
    if ($pkg) {
        $real = Join-Path $pkg.InstallLocation 'amd64\cdb.exe'
        if (Test-Path $real) { return $real }
    }
    $cmd = Get-Command cdb.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw 'cdb.exe not found (install WinDbg from the Store)'
}

function Get-MainWindow([int]$targetPid) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public class HangWin {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
}
"@ -ErrorAction SilentlyContinue
    $script:found = [IntPtr]::Zero
    $script:title = ''
    [HangWin]::EnumWindows({
        param($h, $l)
        $wpid = 0
        [HangWin]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
        if ($wpid -eq $targetPid -and [HangWin]::IsWindowVisible($h)) {
            $sb = New-Object System.Text.StringBuilder 256
            [HangWin]::GetWindowText($h, $sb, 256) | Out-Null
            if ($sb.Length -gt 0 -and $script:found -eq [IntPtr]::Zero) {
                $script:found = $h
                $script:title = $sb.ToString()
            }
        }
        return $true
    }, [IntPtr]::Zero) | Out-Null
    return @{ Hwnd = $script:found; Title = $script:title }
}

function Get-UiThread([IntPtr]$hwnd) {
    if ($hwnd -eq [IntPtr]::Zero) { return 0 }
    $tid = 0
    [HangWin]::GetWindowThreadProcessId($hwnd, [ref]$tid) | Out-Null
    return $tid
}

function Resolve-ByRva($table, [UInt64]$rva) {
    if (-not $table -or $table.Count -eq 0) { return $null }
    $lo = 0; $hi = $table.Count - 1; $best = $null
    while ($lo -le $hi) {
        $mid = [int](($lo + $hi) / 2)
        if ($table[$mid].Rva -le $rva) { $best = $table[$mid]; $lo = $mid + 1 }
        else { $hi = $mid - 1 }
    }
    if ($best) { return ('{0}+0x{1:X}' -f $best.Name, ($rva - $best.Rva)) }
    return $null
}

function Load-ExeMap([string]$mapPath) {
    $rows = New-Object System.Collections.ArrayList
    if (-not (Test-Path $mapPath)) { return $rows }
    $base = [UInt64]0x140000000
    foreach ($line in Get-Content $mapPath) {
        $m = [regex]::Match($line, '^\s+0001:([0-9A-Fa-f]+)\s+(\S+)\s+([0-9A-Fa-f]+)')
        if ($m.Success) {
            $abs = [Convert]::ToUInt64($m.Groups[3].Value, 16)
            if ($abs -ge $base) {
                [void]$rows.Add([pscustomobject]@{
                    Rva  = [UInt64]($abs - $base)
                    Name = $m.Groups[2].Value
                })
            }
        }
    }
    $rows.Sort([System.Comparison[object]]{ param($a,$b) $a.Rva.CompareTo($b.Rva) })
    return $rows
}

function Load-DllExports([string]$dllPath) {
    $rows = New-Object System.Collections.ArrayList
    if (-not (Test-Path $dllPath)) { return $rows }
    $objdump = (Get-Command objdump.exe -ErrorAction SilentlyContinue).Source
    if (-not $objdump) { return $rows }
    foreach ($line in & $objdump -p $dllPath) {
        $m = [regex]::Match($line, '^\s+\[\s*\d+\]\s+\+base\[\s*\d+\]\s+([0-9A-Fa-f]+)\s+(\w+)\s*$')
        if ($m.Success) {
            [void]$rows.Add([pscustomobject]@{
                Rva  = [Convert]::ToUInt64($m.Groups[1].Value, 16)
                Name = $m.Groups[2].Value
            })
        }
    }
    $rows.Sort([System.Comparison[object]]{ param($a,$b) $a.Rva.CompareTo($b.Rva) })
    return $rows
}

# ---- pick the process ------------------------------------------------------
if ($ProcessId -eq 0) {
    $procs = @(Get-Process minishell -ErrorAction SilentlyContinue)
    if ($procs.Count -eq 0) { throw 'no minishell.exe process is running' }
    $stuck = $procs | Where-Object { $_.MainWindowTitle -eq 'MiniShell - miniblink' } |
             Select-Object -First 1
    if (-not $stuck) { $stuck = $procs | Select-Object -First 1 }
    $ProcessId = $stuck.Id
}

$proc = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
if (-not $proc) { throw "process $ProcessId is gone" }

$cdb = Find-Cdb
$root = Split-Path $PSScriptRoot -Parent
$work = Join-Path $root 'build-msvc\Release'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$cmdFile = Join-Path $work "hang-$stamp.cdb"
$logFile = Join-Path $work "hang-$stamp.txt"

$win = Get-MainWindow $ProcessId
$uiTid = Get-UiThread $win.Hwnd
$uiSel = if ($uiTid) { '~~[{0}] kb 60' -f $uiTid } else { '.echo (no UI thread found)' }

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('.echo ===== PROCESS =====')
$lines.Add('.echo pid=' + $ProcessId + ' title=' + $win.Title + ' ui_tid=' + $uiTid)
if ($Symbols) {
    $lines.Add('.symfix')
    $lines.Add('.reload')
}
$lines.Add('.echo ===== THREADS =====')
$lines.Add('!threads')
$lines.Add('.echo ===== ALL STACKS =====')
$lines.Add('~* kb 40')
$lines.Add('.echo ===== UI THREAD STACK =====')
$lines.Add($uiSel)
$lines.Add('.echo ===== CRITICAL SECTIONS =====')
$lines.Add('!cs -l')
$lines.Add('.echo ===== DETACH =====')
$lines.Add('.detach')
$lines.Add('q')
Set-Content -Path $cmdFile -Value ($lines -join "`r`n") -Encoding Ascii

Write-Host "attaching cdb to pid $ProcessId ($($win.Title)) ..."
$p = Start-Process -FilePath $cdb -ArgumentList @('-p', "$ProcessId", '-cf', $cmdFile) `
     -RedirectStandardOutput $logFile -RedirectStandardError ($logFile + '.err') -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    Write-Warning "cdb did not exit in $TimeoutSec s; killing it"
    $p.Kill()
}
Write-Host "cdb exit=$($p.ExitCode) log=$logFile"

# ---- symbolize -------------------------------------------------------------
$exeMap = Load-ExeMap (Join-Path $root 'build-msvc\minishell.map')
$dllExp = Load-DllExports (Join-Path $root '..\mb132_x64.dll')
$text = Get-Content $logFile -Raw -ErrorAction SilentlyContinue
if (-not $text) { throw "cdb produced no log: $logFile" }

$text = [regex]::Replace($text, 'minishell\+0x([0-9A-Fa-f]+)', {
    param($m)
    $rva = [Convert]::ToUInt64($m.Groups[1].Value, 16)
    $s = Resolve-ByRva $exeMap $rva
    if ($s) { return $s } else { return $m.Value }
})
$text = [regex]::Replace($text, 'mb132_x64\+0x([0-9A-Fa-f]+)', {
    param($m)
    $rva = [Convert]::ToUInt64($m.Groups[1].Value, 16)
    $s = Resolve-ByRva $dllExp $rva
    if ($s) { return $s } else { return $m.Value }
})

Set-Content -Path $logFile -Value $text -Encoding UTF8

# Print the useful parts: all stacks, then a focused list of waits/our frames.
$inStacks = $false
foreach ($line in ($text -split "`r?`n")) {
    if ($line -match '===== (ALL STACKS|UI THREAD STACK|CRITICAL SECTIONS|THREADS) =====') { $inStacks = $true }
    if ($inStacks) { Write-Host $line }
}

Write-Host ''
Write-Host '===== FOCUSED (waits / our modules) ====='
$text -split "`r?`n" | Select-String -Pattern 'minishell\+|mb132_x64\+|NtWait|WaitForSingleObject|WaitForMultipleObjects|NtAlert|CritSec|Sleep' |
    Select-Object -First 80 | ForEach-Object { $_.Line }
