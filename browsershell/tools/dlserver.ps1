param([int]$Port = 8768, [int]$Seconds = 40)

$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://127.0.0.1:$Port/")
$listener.Start()
$deadline = (Get-Date).AddSeconds($Seconds)

while ((Get-Date) -lt $deadline) {
    $task = $listener.GetContextAsync()
    while (-not $task.AsyncWaitHandle.WaitOne(250)) {
        if ((Get-Date) -ge $deadline) { break }
    }
    if (-not $task.IsCompleted) { continue }
    $ctx = $task.Result
    $path = $ctx.Request.Url.AbsolutePath
    if ($path -eq '/page') {
        $html = '<html><body style="font:16px sans-serif"><h1>download test</h1><a id="d" href="/file.bin" download="file.bin">download link</a></body></html>'
        $bytes = [Text.Encoding]::UTF8.GetBytes($html)
        $ctx.Response.ContentType = 'text/html'
        $ctx.Response.ContentLength64 = $bytes.Length
        $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
    } else {
        $bytes = New-Object byte[] 1048576
        for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = 65 }
        $ctx.Response.ContentType = 'application/octet-stream'
        $ctx.Response.AddHeader('Content-Disposition', 'attachment; filename="file.bin"')
        $ctx.Response.ContentLength64 = $bytes.Length
        $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
    }
    $ctx.Response.Close()
}
$listener.Stop()
