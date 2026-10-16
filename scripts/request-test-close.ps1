param(
    [Parameter(Mandatory=$true)]
    [int]$ProcessId,
    [Parameter(Mandatory=$true)]
    [string]$ExpectedProject
)
$ErrorActionPreference = 'Stop'
$p = Get-Process -Id $ProcessId
if ($p.ProcessName -ne 'lmms' -or -not $p.MainWindowTitle.StartsWith($ExpectedProject + '*')) {
    throw 'Refusing to close anything except the explicitly identified modified test project.'
}
if (-not $p.CloseMainWindow()) { throw 'The test LMMS close request failed.' }
