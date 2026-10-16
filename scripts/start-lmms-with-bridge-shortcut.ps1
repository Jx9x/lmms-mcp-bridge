# start-lmms-with-bridge-shortcut.ps1 - target for shortcuts/launchers.
# Delegates to start-lmms-with-bridge.ps1 in a child process so its exit code
# is reliable (an exit inside an &-invoked script would end this script too,
# and $LASTEXITCODE is stale after a script that finishes without exit).
# Success is the bridge pipe appearing; failures show a dialog, since a
# shortcut launch has no console to display the script's stderr.
$inner = Join-Path $PSScriptRoot 'start-lmms-with-bridge.ps1'
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$output = & $powershell -NoProfile -ExecutionPolicy Bypass -File $inner 2>&1
$launchExitCode = $LASTEXITCODE

$pipeName = $env:LMMS_AGENT_BRIDGE_PIPE
if ([string]::IsNullOrEmpty($pipeName)) {
    $pipeName = 'lmms-agent-bridge'
}
$pipe = '\\.\pipe\' + $pipeName
if ($launchExitCode -eq 0) {
    # LMMS launched; the pipe appears once the plugin finishes loading.
    $deadline = (Get-Date).AddSeconds(20)
    while (-not (Test-Path -LiteralPath $pipe) -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
    }
}
# If LMMS was already running, the script exits nonzero: stay silent when the
# existing instance already serves the bridge, otherwise surface the error.
if (-not (Test-Path -LiteralPath $pipe)) {
    $message = ($output | Out-String).Trim()
    if ($launchExitCode -eq 0) {
        $message = "LMMS started, but the agent bridge pipe did not appear within 20 seconds.`nPipe: $pipe`n`n$message"
    }
    Add-Type -AssemblyName PresentationFramework
    [System.Windows.MessageBox]::Show(
        $message, 'LMMS (agent bridge)', 'OK', 'Warning') | Out-Null
    exit 1
}
