# repro-destroy-inflight.ps1 - minimal reproducer for the mb132 crash where
# mbDestroyWebView() is called while a request is still in flight.
#
# What happens:
#   1. a local server accepts the connection and never answers (page load hangs,
#      nothing commits - progress stays at "navigate started");
#   2. at t=4 s the host destroys the webview (the shell's nav guard does this
#      after its 4 s timeout);
#   3. at t=6 s the server closes the socket, i.e. the request fails;
#   4. the engine dispatches that network completion into the destroyed webview
#      and dies with an access violation:
#
#        mb132_x64.dll+0x5e666f   cmp qword ptr [r14+0F0h],0     r14 = 0
#        read of 0xF0, EXCEPTION_ACCESS_VIOLATION
#
# Only the DLL is needed (no shell, no agave layer). Run it in an *x64* PowerShell
# next to mb132_x64.dll:
#
#   pwsh -File repro-destroy-inflight.ps1 -Dll .\mb132_x64.dll
#
# If the process dies around step 3 with exit code -1073741819 (0xC0000005) the
# bug reproduced. If it reaches "no crash" the engine survived.
param(
    [string]$Dll = '',
    [int]$Port = 18095,
    [int]$DestroyAtMs = 4000,
    [int]$CloseAtMs = 6000,
    [int]$RunMs = 12000
)

$ErrorActionPreference = 'Stop'

if (-not $Dll) {
    foreach ($candidate in @(
            (Join-Path $PSScriptRoot 'mb132_x64.dll'),
            (Join-Path $PSScriptRoot '..\mb132_x64.dll'),
            (Join-Path $PSScriptRoot '..\..\mb132_x64.dll'))) {
        if (Test-Path $candidate) { $Dll = $candidate; break }
    }
}
if (-not $Dll -or -not (Test-Path $Dll)) {
    throw 'mb132_x64.dll not found; put this script next to it or pass -Dll <path>'
}
$Dll = (Resolve-Path $Dll).Path
if ([IntPtr]::Size -ne 8) { throw 'run this under 64-bit PowerShell (x64 build of the DLL)' }

$work = Join-Path $env:TEMP ('mb-repro-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
$log = Join-Path $work 'repro.log'
Set-Location $work

function Say([string]$text)
{
    $line = '{0:HH:mm:ss.fff}  {1}' -f (Get-Date), $text
    Write-Host $line
    Add-Content -Path $log -Value $line
}

Say "dll:  $Dll"
Say "log:  $log"

# ---- Win32 + miniblink ------------------------------------------------------
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class M {
    [StructLayout(LayoutKind.Sequential)]
    public struct MSG {
        public IntPtr hwnd;
        public uint message;
        public IntPtr wParam;
        public IntPtr lParam;
        public uint time;
        public int ptX;
        public int ptY;
        public uint lPrivate;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern bool SetDllDirectoryW(string path);
    [DllImport("kernel32.dll")]
    public static extern IntPtr GetModuleHandleW(IntPtr name);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateWindowExW(uint exStyle, string cls, string title,
        uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu,
        IntPtr instance, IntPtr param);
    [DllImport("user32.dll")]
    public static extern bool DestroyWindow(IntPtr hwnd);
    [DllImport("user32.dll")]
    public static extern bool PeekMessageW(ref MSG msg, IntPtr hwnd, uint min, uint max, uint remove);
    [DllImport("user32.dll")]
    public static extern bool TranslateMessage(ref MSG msg);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr DispatchMessageW(ref MSG msg);

    [DllImport("mb132_x64.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern void mbInit(IntPtr settings);
    [DllImport("mb132_x64.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr mbCreateWebWindow(int type, IntPtr parent, int x, int y, int w, int h);
    [DllImport("mb132_x64.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern void mbLoadURL(IntPtr webview, [MarshalAs(UnmanagedType.LPStr)] string url);
    [DllImport("mb132_x64.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern void mbDestroyWebView(IntPtr webview);
    [DllImport("mb132_x64.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern void mbUninit();
}
'@

[void][M]::SetDllDirectoryW((Split-Path $Dll -Parent))

# ---- hang server: accept, never answer, close when asked -------------------
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()
Say "hang server listening on 127.0.0.1:$Port"

# ---- host window + webview ------------------------------------------------
[M]::mbInit([IntPtr]::Zero)
$WS_POPUP = [uint32]2147483648          # 0x80000000 does not fit in an Int32
$parent = [M]::CreateWindowExW(0, 'STATIC', 'repro', $WS_POPUP, 0, 0, 900, 700,
                               [IntPtr]::Zero, [IntPtr]::Zero, [M]::GetModuleHandleW([IntPtr]::Zero), [IntPtr]::Zero)
if ($parent -eq [IntPtr]::Zero) { throw 'CreateWindowEx failed' }

# MB_WINDOW_TYPE_CONTROL = 2
$view = [M]::mbCreateWebWindow(2, $parent, 0, 0, 900, 700)
if ($view -eq [IntPtr]::Zero) { throw 'mbCreateWebWindow failed' }

Say 'webview created; loading http://127.0.0.1/ (the page will never commit)'
[M]::mbLoadURL($view, "http://127.0.0.1:$Port/")

# ---- pump messages and run the timeline -----------------------------------
$start = [Environment]::TickCount64
$destroyed = $false
$closed = $false
$client = $null
$clientAt = 0
$accept = $listener.AcceptTcpClientAsync()
$msg = New-Object M+MSG

while (($now = [Environment]::TickCount64) - $start -lt $RunMs) {
    while ([M]::PeekMessageW([ref]$msg, [IntPtr]::Zero, 0, 0, 1)) {
        [void][M]::TranslateMessage([ref]$msg)
        [void][M]::DispatchMessageW([ref]$msg)
    }

    $elapsed = $now - $start

    if ($accept.IsCompleted -and -not $client) {
        $client = $accept.Result
        $clientAt = [Environment]::TickCount64
        Say "request arrived (t=${elapsed}ms); not answering"
    }

    if (-not $destroyed -and $elapsed -ge $DestroyAtMs) {
        $destroyed = $true
        Say "t=${elapsed}ms  mbDestroyWebView() while the request is in flight"
        [M]::mbDestroyWebView($view)
    }

    if ($client -and -not $closed -and ([Environment]::TickCount64 - $clientAt) -ge $CloseAtMs) {
        $closed = $true
        Say "server closes the socket (request now fails)"
        $client.Close()
    }

    Start-Sleep -Milliseconds 5
}

Say 'no crash: the process survived (bug fixed or not reproduced)'

[M]::mbUninit()
$listener.Stop()
[void][M]::DestroyWindow($parent)
