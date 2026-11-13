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
        # Only a thin shell-owned strip (1-2 DIP, scaled) around the page: the
        # rest of the window is the page, but the strip is what keeps the window
        # border draggable. A wider inset would eat rendering area; 0 would
        # leave the resize border to the page control, which swallows it.
        $leftInset = $vr.Left - $wr.Left
        $rightInset = $wr.Right - $vr.Right
        $bottomInset = $wr.Bottom - $vr.Bottom

        if ($leftInset -lt 1 -or $leftInset -gt 6) { $problems += "page left inset $leftInset (want 1..6)" }
        if ($rightInset -lt 1 -or $rightInset -gt 6) { $problems += "page right inset $rightInset (want 1..6)" }
        if ($bottomInset -lt 1 -or $bottomInset -gt 6) { $problems += "page bottom inset $bottomInset (want 1..6)" }
        if ($vr.Top -le $wr.Top) { $problems += "page starts at the window's top edge ($($vr.Top))" }
        "page inset: left=$leftInset right=$rightInset bottom=$bottomInset top=$($vr.Top - $wr.Top)"
    }

    $midX = $wr.Left + [int]($width / 2)
    $midY = $wr.Top + [int]($height / 2)

    # The window's own hit test turns the shell-owned strip into resize borders
    # (point 1 px inside the window edge, inside the 1-2 px strip).
    $leftHit = [Win]::HitTest($hwnd, $wr.Left + 1, $midY)
    $bottomHit = [Win]::HitTest($hwnd, $midX, $wr.Bottom - 1)
    $clientHit = [Win]::HitTest($hwnd, $midX, $midY)
    if ($leftHit -ne 10) { $problems += "window left edge hit test returned $leftHit instead of HTLEFT" }
    if ($bottomHit -ne 15) { $problems += "window bottom edge hit test returned $bottomHit instead of HTBOTTOM" }
    if ($clientHit -ne 1) { $problems += "window content hit test returned $clientHit instead of HTCLIENT" }
    "window hit test: left=$leftHit bottom=$bottomHit content=$clientHit"

    # The control also reports its own edge strip as HTTRANSPARENT (a help when
    # the host and the control share a thread) and keeps HTCLIENT in the middle.
    if ($view -ne [IntPtr]::Zero) {
        $viewLeft = [Win]::HitTest($view, $vr.Left + 1, $midY)
        $viewBottom = [Win]::HitTest($view, $midX, $vr.Bottom - 1)
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
