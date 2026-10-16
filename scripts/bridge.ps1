# bridge.ps1 - send text commands to the agentbridge named pipe and print
# the one-line JSON responses.
#
#   .\bridge.ps1 ping song "tempo 140"
#   .\bridge.ps1 -Pipe lmms-agent-bridge -TimeoutMs 5000 selftest
param(
    [Parameter(Mandatory=$true, Position=0, ValueFromRemainingArguments=$true)]
    [string[]]$Command,
    [string]$Pipe = 'lmms-agent-bridge',
    [int]$TimeoutMs = 5000
)

$ErrorActionPreference = 'Stop'

$client = New-Object System.IO.Pipes.NamedPipeClientStream(
    '.', $Pipe, [System.IO.Pipes.PipeDirection]::InOut)
try {
    $client.Connect($TimeoutMs)
}
catch {
    [Console]::Error.WriteLine("cannot connect to \\.\pipe\$Pipe : $($_.Exception.Message)")
    exit 1
}

$utf8 = New-Object System.Text.UTF8Encoding($false)
$writer = New-Object System.IO.StreamWriter($client, $utf8)
$writer.AutoFlush = $true
$reader = New-Object System.IO.StreamReader($client, $utf8)

foreach ($cmd in $Command) {
    try {
        $writer.Write($cmd + "`n")
        $line = $reader.ReadLine()
        if ($null -eq $line) {
            [Console]::Error.WriteLine("pipe closed by server while waiting for reply to '$cmd'")
            $client.Dispose()
            exit 1
        }
        Write-Output $line
    }
    catch {
        [Console]::Error.WriteLine("pipe error on '$cmd': $($_.Exception.Message)")
        $client.Dispose()
        exit 1
    }
}

$client.Dispose()
exit 0
