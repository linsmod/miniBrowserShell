# tools/window-frame-check.ps1 - the page must not cover the window's resize frame.
#
# The shell window is a DWM-extended-frame window: its client area covers the
# whole window, so the resize border is an area inside the client that the
# shell's WM_NCHITTEST turns into HTLEFT/HTBOTTOM/... If the page control
# reached the client edges it would own those pixels and dragging the edges
# over the content would do nothing. The chrome reserves the frame around the
# page; this checks the geometry and the hit test.
#
# The window is started with AGB_SHELL_HIDDEN, so nothing appears on screen.
#
#   pwsh -File tools\window-frame-check.ps1
param(
    [int]$Timeout = 10000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build-msvc\Release\minishell.exe'
if (-not (Test-Path $exe)) { throw "minishell.exe not found: $exe" }

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class Win {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr param);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr param);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hwnd, StringBuilder name, int count);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    public static IntPtr FindShell(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate (IntPtr hwnd, IntPtr param) {
            uint winPid;
            GetWindowThreadProcessId(hwnd, out winPid);
            if (winPid != pid) return true;
            StringBuilder name = new StringBuilder(64);
            GetClassNameW(hwnd, name, name.Capacity);
            if (name.ToString() == "MiniShellWindow") { found = hwnd; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static IntPtr LargestChild(IntPtr parent) {
        IntPtr best = IntPtr.Zero;
        long bestArea = 0;
        EnumChildWindows(parent, delegate (IntPtr hwnd, IntPtr param) {
            RECT r;
            if (!GetWindowRect(hwnd, out r)) return true;
            long area = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (area > bestArea) { bestArea = area; best = hwnd; }
            return true;
        }, IntPtr.Zero);
        return best;
    }
}
'@

$work = Join-Path $env:TEMP ('mb-frame-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
$page = Join-Path $work 'page.html'
'<!DOCTYPE html><html><body style="margin:0">frame check</body></html>' | Set-Content -Encoding UTF8 $page

$env:AGB_SHELL_HIDDEN = '1'
$shell = Start-Process -FilePath $exe `
    -ArgumentList @('--userdata', (Join-Path $work 'profile'),
                    ('file:///' + ($page -replace '\\', '/'))) `
    -WorkingDirectory $work -PassThru -WindowStyle Hidden
$childId = $shell.Id
$problems = @()

try {
    # Wait for the window (created before the message loop, so this is quick).
    $deadline = (Get-Date).AddMilliseconds($Timeout)
    $hwnd = [IntPtr]::Zero
    while ((Get-Date) -lt $deadline) {
        $hwnd = [Win]::FindShell([uint32]$childId)
        if ($hwnd -ne [IntPtr]::Zero) { break }
        if ($shell.HasExited) { break }
        Start-Sleep -Milliseconds 100
    }

    if ($hwnd -eq [IntPtr]::Zero) {
        "FAIL: no MiniShellWindow for pid $childId"
        exit 1
    }

    # Give the first tab a moment to create its view and take a layout.
    Start-Sleep -Milliseconds 1200

    $wr = New-Object Win+RECT
    $null = [Win]::GetWindowRect($hwnd, [ref]$wr)
    $width = $wr.Right - $wr.Left
    $height = $wr.Bottom - $wr.Top

    $view = [Win]::LargestChild($hwnd)
    $vr = New-Object Win+RECT
    if ($view -eq [IntPtr]::Zero -or -not [Win]::GetWindowRect($view, [ref]$vr)) {
        $problems += 'no page child window'
    } else {
        # The page must be inside the frame: left/right/bottom gaps, and a top
        # gap for the tab strip + toolbar.
        if ($vr.Left -le $wr.Left) { $problems += "page starts at the window's left edge ($($vr.Left))" }
        if ($vr.Right -ge $wr.Right) { $problems += "page reaches the window's right edge ($($vr.Right))" }
        if ($vr.Bottom -ge $wr.Bottom) { $problems += "page reaches the window's bottom edge ($($vr.Bottom))" }
        if ($vr.Top -le $wr.Top) { $problems += "page starts at the window's top edge ($($vr.Top))" }
        "page inset: left=$($vr.Left - $wr.Left) right=$($wr.Right - $vr.Right) bottom=$($wr.Bottom - $vr.Bottom) top=$($vr.Top - $wr.Top)"
    }

    # WM_NCHITTEST = 0x0084, lParam = screen x/y. The constants are the values
    # DefWindowProc expects: HTLEFT=10, HTBOTTOM=15, HTCLIENT=1.
    function HitTest([int]$x, [int]$y) {
        $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
        return [int][Win]::SendMessage($hwnd, 0x0084, [IntPtr]::Zero, $lp)
    }

    $leftHit = HitTest ($wr.Left + 2) ($wr.Top + [int]($height / 2))
    $bottomHit = HitTest ($wr.Left + [int]($width / 2)) ($wr.Bottom - 2)
    $clientHit = HitTest ($wr.Left + [int]($width / 2)) ($wr.Top + [int]($height / 2))

    if ($leftHit -ne 10) { $problems += "left edge hit test returned $leftHit instead of HTLEFT" }
    if ($bottomHit -ne 15) { $problems += "bottom edge hit test returned $bottomHit instead of HTBOTTOM" }
    if ($clientHit -ne 1) { $problems += "content hit test returned $clientHit instead of HTCLIENT" }

    "hit test: left=$leftHit bottom=$bottomHit content=$clientHit"
} finally {
    if (-not $shell.HasExited) { Stop-Process -Id $shell.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item Env:AGB_SHELL_HIDDEN -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

if ($problems.Count -gt 0) {
    'FAIL: ' + ($problems -join '; ')
    exit 1
}

'PASS: the page stays inside the resize frame and the window edges hit-test as resize borders'
exit 0
