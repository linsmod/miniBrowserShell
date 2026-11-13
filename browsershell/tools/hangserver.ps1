# tools/hangserver.ps1 - a TCP server that accepts and never answers.
#
# Reproduces the "load never commits" state the nav guard rebuilds views for:
# miniblink sends the request, the socket stays open and the page never commits.
#
#   pwsh -File tools/hangserver.ps1 -Port 18080
#   (stop with Ctrl+C or kill the process)
param(
    [int]$Port = 18080
)

$ErrorActionPreference = 'Stop'
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()
Write-Host "hangserver: listening on 127.0.0.1:$Port (accepts, never responds)"
$clients = [System.Collections.Generic.List[System.Net.Sockets.TcpClient]]::new()

while ($true) {
    if ($listener.Pending()) {
        $clients.Add($listener.AcceptTcpClient())
        Write-Host "hangserver: accepted client $($clients.Count)"
    }
    Start-Sleep -Milliseconds 100
}
