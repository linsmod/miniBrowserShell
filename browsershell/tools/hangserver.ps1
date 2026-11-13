# tools/hangserver.ps1 - a TCP server that accepts and never answers.
#
# Reproduces the "load never commits" state the nav guard rebuilds views for:
# miniblink sends the request, the socket stays open and the page never commits.
#
#   pwsh -File tools/hangserver.ps1 -Port 18080
#   pwsh -File tools/hangserver.ps1 -Port 18080 -CloseAfterMs 6000
#   (stop with Ctrl+C or kill the process)
#
# -CloseAfterMs closes every accepted socket after N ms without answering. Use it
# to make a load hang past the nav guard, then fail: the engine gets a network
# completion for a control the guard has already destroyed.
param(
    [int]$Port = 18080,
    [int]$CloseAfterMs = 0
)

$ErrorActionPreference = 'Stop'
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()
Write-Host "hangserver: listening on 127.0.0.1:$Port (accepts, never responds)"
$clients = [System.Collections.Generic.List[object]]::new()

while ($true) {
    if ($listener.Pending()) {
        $client = $listener.AcceptTcpClient()
        $clients.Add([pscustomobject]@{
            Client = $client
            At     = [DateTime]::UtcNow
        })
        Write-Host "hangserver: accepted client $($clients.Count)"
    }

    if ($CloseAfterMs -gt 0) {
        for ($i = $clients.Count - 1; $i -ge 0; $i--) {
            if (([DateTime]::UtcNow - $clients[$i].At).TotalMilliseconds -ge $CloseAfterMs) {
                try { $clients[$i].Client.Close() } catch {}
                Write-Host "hangserver: closed client after $CloseAfterMs ms"
                $clients.RemoveAt($i)
            }
        }
    }

    Start-Sleep -Milliseconds 100
}
