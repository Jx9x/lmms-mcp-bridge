# start-lmms-with-bridge.ps1 - launch LMMS with the agentbridge plugin
# directory and log path configured via environment.
#
# Refuses to start while any lmms process is running: LMMS treats a second
# instance as a crash-recovery offer via recover.mmp, and the bridge pipe
# name is single-instance anyway.
param(
    [string]$Project,
    [string]$LmmsExe = "C:\Program Files\LMMS\lmms.exe",
    [string]$PluginDirectory
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (Get-Process -Name lmms -ErrorAction SilentlyContinue) {
    [Console]::Error.WriteLine("an lmms process is already running - close it first (bridge pipe is single-instance and a second instance triggers recover.mmp handling)")
    exit 2
}

if (-not (Test-Path $LmmsExe)) {
    [Console]::Error.WriteLine("lmms executable not found: $LmmsExe")
    exit 2
}

$pluginDir = if ($PluginDirectory) { (Resolve-Path -LiteralPath $PluginDirectory).Path } else { Join-Path $root 'build\lmms-plugins' }
if (-not (Test-Path (Join-Path $pluginDir 'agentbridge.dll'))) {
    [Console]::Error.WriteLine("agentbridge.dll not found in $pluginDir - run scripts/build-plugin.sh first")
    exit 2
}

$logDir = Join-Path $root 'runtime\logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$env:LMMS_PLUGIN_DIR = $pluginDir
$env:LMMS_AGENT_BRIDGE_LOG = Join-Path $logDir 'agentbridge.log'

$procArgs = @{
    FilePath         = $LmmsExe
    WorkingDirectory = Split-Path -Parent $LmmsExe
    PassThru         = $true
    # LMMS is an interactive editor; its window must remain available for
    # native close-prompt checks and the user's subsequent work.
    WindowStyle      = 'Normal'
}
if ($Project) {
    # Windows PowerShell 5.1 Start-Process does not quote paths with spaces
    $procArgs.ArgumentList = @('"' + $Project + '"')
}

$p = Start-Process @procArgs
Write-Output "started lmms.exe PID $($p.Id)"
Write-Output "plugin dir: $env:LMMS_PLUGIN_DIR"
Write-Output "bridge log: $env:LMMS_AGENT_BRIDGE_LOG"
