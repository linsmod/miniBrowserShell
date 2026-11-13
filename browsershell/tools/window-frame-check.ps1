# tools/window-frame-check.ps1 - the page is full-bleed, and the window edges
# still resize.
#
# The shell window is a DWM-extended-frame window: its client area covers the
# whole window, so the resize border is an area inside the client that the
# shell's WM_NCHITTEST turns into HTLEFT/HTBOTTOM/... The page is a child
# control and covers that strip too; the control reports it as HTTRANSPARENT so
# the hit test falls through to the shell, which starts the resize. Rendering
# gives up no pixels.
#
# The window starts hidden (AGB_SHELL_HIDDEN), so nothing appears on screen.
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

    /* WM_NCHITTEST = 0x0084, lParam = screen x/y. */
    public static int HitTest(IntPtr hwnd, int x, int y) {
        IntPtr lp = (IntPtr)((y << 16) | (x & 0xFFFF));
        return (int)SendMessage(hwnd, 0x0084, IntPtr.Zero, lp);
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
        # Full-bleed: the page reaches the window's left/right/bottom (allow the
        # 1-2 px DWM frame), and starts below the tab strip + toolbar.
        if ($vr.Left -gt $wr.Left + 2) { $problems += "page does not reach the left edge (inset $($vr.Left - $wr.Left))" }
        if ($vr.Right -lt $wr.Right - 2) { $problems += "page does not reach the right edge (inset $($wr.Right - $vr.Right))" }
        if ($vr.Bottom -lt $wr.Bottom - 2) { $problems += "page does not reach the bottom edge (inset $($wr.Bottom - $vr.Bottom))" }
        if ($vr.Top -le $wr.Top) { $problems += "page starts at the window's top edge ($($vr.Top))" }
        "page rect: left=$($vr.Left) top=$($vr.Top) right=$($vr.Right) bottom=$($vr.Bottom) window=$($wr.Left),$($wr.Top),$($wr.Right),$($wr.Bottom)"
    }

    $midX = $wr.Left + [int]($width / 2)
    $midY = $wr.Top + [int]($height / 2)

    # The window's own hit test turns the edges into resize borders.
    $leftHit = [Win]::HitTest($hwnd, $wr.Left + 2, $midY)
    $bottomHit = [Win]::HitTest($hwnd, $midX, $wr.Bottom - 2)
    $clientHit = [Win]::HitTest($hwnd, $midX, $midY)
    if ($leftHit -ne 10) { $problems += "window left edge hit test returned $leftHit instead of HTLEFT" }
    if ($bottomHit -ne 15) { $problems += "window bottom edge hit test returned $bottomHit instead of HTBOTTOM" }
    if ($clientHit -ne 1) { $problems += "window content hit test returned $clientHit instead of HTCLIENT" }
    "window hit test: left=$leftHit bottom=$bottomHit content=$clientHit"

    # The page covers those pixels, so it must hand them back (HTTRANSPARENT)
    # and keep HTCLIENT in its middle.
    if ($view -ne [IntPtr]::Zero) {
        $viewLeft = [Win]::HitTest($view, $wr.Left + 2, $midY)
        $viewBottom = [Win]::HitTest($view, $midX, $wr.Bottom - 2)
        $viewClient = [Win]::HitTest($view, $midX, $midY)
        if ($viewLeft -ne -1) { $problems += "page left edge returned $viewLeft instead of HTTRANSPARENT" }
        if ($viewBottom -ne -1) { $problems += "page bottom edge returned $viewBottom instead of HTTRANSPARENT" }
        if ($viewClient -ne 1) { $problems += "page content returned $viewClient instead of HTCLIENT" }
        "page hit test: left=$viewLeft bottom=$viewBottom content=$viewClient"
    }
} finally {
    if (-not $shell.HasExited) { Stop-Process -Id $shell.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item Env:AGB_SHELL_HIDDEN -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

if ($problems.Count -gt 0) {
    'FAIL: ' + ($problems -join '; ')
    exit 1
}

'PASS: the page is full-bleed and the window resize edges win the hit test'
exit 0
