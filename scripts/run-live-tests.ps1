param(
    [ValidateSet('baseline', 'features')]
    [string]$Mode = 'features',
    [string]$Python = 'python',
    [string]$LmmsExe = 'C:\Program Files\LMMS\lmms.exe',
    [string]$PluginDirectory,
    [string]$RestoreProject
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$testRoot = Join-Path $root ('runtime\test\commands-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$normal = @(Get-Process -Name lmms -ErrorAction SilentlyContinue)
if ($normal.Count -gt 1) { throw 'Multiple LMMS instances are running; leave them untouched.' }
$restoreHash = $null
if ($RestoreProject) {
    $RestoreProject = (Resolve-Path -LiteralPath $RestoreProject).Path
    if (-not (Test-Path -LiteralPath $RestoreProject -PathType Leaf)) { throw 'RestoreProject must be an existing saved project.' }
    $restoreHash = (Get-FileHash -LiteralPath $RestoreProject).Hash
}
$expectedTitle = if ($RestoreProject) { [IO.Path]::GetFileNameWithoutExtension($RestoreProject) + ' - LMMS 1.2.2' } else { 'Untitled - LMMS 1.2.2' }
if ($normal.Count -eq 1 -and ($normal[0].MainWindowTitle.Contains('*') -or $normal[0].MainWindowTitle -ne $expectedTitle)) {
    throw "Only a clean session titled '$expectedTitle' can be restored automatically; leave the current session untouched."
}
New-Item -ItemType Directory -Path $testRoot | Out-Null
$config = Join-Path $HOME '.lmmsrc.xml'
$recoverBak = Join-Path $HOME 'Documents\lmms\recover.mmp.bak'
$recover = Join-Path $HOME 'Documents\lmms\recover.mmp'
$configBackup = Join-Path $testRoot 'lmmsrc-original.xml'
$recoverBackup = Join-Path $testRoot 'recover-original.mmp.bak'
$configExisted = Test-Path $config
$recoverBakExisted = Test-Path $recoverBak
if ($configExisted) { Copy-Item -LiteralPath $config -Destination $configBackup }
if ($recoverBakExisted) { Copy-Item -LiteralPath $recoverBak -Destination $recoverBackup }
$oldPlugin = $env:LMMS_PLUGIN_DIR
$oldLog = $env:LMMS_AGENT_BRIDGE_LOG
$oldPipe = $env:LMMS_AGENT_BRIDGE_PIPE
$testProcess = $null
$testExit = 1
$restoreSafe = $true

function Get-InstallState {
    @(Get-ChildItem -LiteralPath (Split-Path -Parent $LmmsExe) -Recurse -File | Sort-Object FullName | ForEach-Object {
        $_.FullName + '|' + $_.Length + '|' + $_.LastWriteTimeUtc.Ticks
    })
}

function Invoke-Bridge([string]$Command) {
    $output = & $Python (Join-Path $PSScriptRoot 'bridge_client.py') --timeout 5 $Command
    if ($LASTEXITCODE -ne 0) { throw "Bridge command failed: $Command" }
    $output | ConvertFrom-Json
}

function Wait-Exit($Process) {
    if (-not $Process.WaitForExit(10000)) { throw "LMMS PID $($Process.Id) did not close normally; do not kill it." }
}

$installBefore = Get-InstallState
$installBefore | Set-Content -LiteralPath (Join-Path $testRoot 'install-before.txt')
try {
    if ($normal.Count -eq 1) {
        $normal[0].Refresh()
        if ($normal[0].MainWindowTitle.Contains('*') -or $normal[0].MainWindowTitle -ne $expectedTitle) { throw 'The user changed LMMS during setup; leave it untouched.' }
        if (-not $normal[0].CloseMainWindow()) { throw 'Could not request a normal LMMS close.' }
        Wait-Exit $normal[0]
    }
    if (Test-Path $recover) { throw "Recovery data exists at $recover; leave it untouched instead of launching a second/recovery session." }
    $project = Join-Path $testRoot 'crunk-demo.mmp'
    Copy-Item -LiteralPath (Join-Path $root 'tests\fixtures\crunk-demo.mmp') -Destination $project
    # Only the disposable fixture copy changes. Keep test startup at safe gain.
    $projectText = [IO.File]::ReadAllText($project)
    $projectText = [regex]::Replace($projectText, 'mastervol="[^"]*"', 'mastervol="8"')
    [IO.File]::WriteAllText($project, $projectText, (New-Object Text.UTF8Encoding($false)))
    $env:LMMS_AGENT_BRIDGE_PIPE = 'lmms-agent-bridge'
    & (Join-Path $PSScriptRoot 'start-lmms-with-bridge.ps1') -Project $project -LmmsExe $LmmsExe -PluginDirectory $PluginDirectory
    $deadline = (Get-Date).AddSeconds(35)
    do {
        Start-Sleep -Milliseconds 250
        $testProcess = Get-Process -Name lmms -ErrorAction SilentlyContinue
        if ($testProcess -and $testProcess.MainWindowTitle -match '^crunk-demo') { break }
    } while ((Get-Date) -lt $deadline)
    if (-not $testProcess) { throw 'Test LMMS did not start.' }
    $ready = Invoke-Bridge 'ping'
    if ($ready.pid -ne $testProcess.Id) { throw 'The bridge is not owned by the protected test LMMS process.' }
    $testProcess.Id | Set-Content -LiteralPath (Join-Path $testRoot 'test-pid.txt')
    & $Python (Join-Path $PSScriptRoot 'test_bridge_live.py') --mode $Mode --artifacts $testRoot
    $testExit = $LASTEXITCODE
}
finally {
    try {
        if ($testProcess -and -not $testProcess.HasExited) {
            $song = Invoke-Bridge 'song'
            if ($song.modified) {
                $commandPath = Join-Path $testRoot 'shutdown-command.jsonl'
                @{cmd='save_project'; args=@{path=(Join-Path $testRoot 'shutdown-saved.mmp')}} | ConvertTo-Json -Compress -Depth 5 | Set-Content -LiteralPath $commandPath -Encoding UTF8
                & $Python (Join-Path $PSScriptRoot 'bridge_client.py') --command-file $commandPath
                if ($LASTEXITCODE -ne 0) { throw 'Could not save protected test edits; leave LMMS running rather than discard them.' }
            }
            $testProcess.Refresh()
            if ($testProcess.MainWindowTitle.Contains('*')) { throw 'Protected test LMMS is still modified; leave it running.' }
            if (-not $testProcess.CloseMainWindow()) { throw 'Could not request protected test LMMS close.' }
            Wait-Exit $testProcess
        }
    }
    catch {
        $restoreSafe = $false
        [Console]::Error.WriteLine($_.Exception.Message)
        [Console]::Error.WriteLine("Test artifacts and configuration backups are retained at $testRoot. Restoration is blocked while test LMMS is running.")
        $testExit = 1
    }
    if ($restoreSafe) {
        if ($RestoreProject -and (Get-FileHash -LiteralPath $RestoreProject).Hash -ne $restoreHash) { throw 'The saved user project changed during testing; preserve backups and inspect it before reopening.' }
        if ($configExisted) {
            Copy-Item -LiteralPath $configBackup -Destination $config -Force
            if ((Get-FileHash -LiteralPath $config).Hash -ne (Get-FileHash -LiteralPath $configBackup).Hash) { throw 'LMMS configuration restoration hash mismatch.' }
        }
        if ($recoverBakExisted) {
            Copy-Item -LiteralPath $recoverBackup -Destination $recoverBak -Force
            if ((Get-FileHash -LiteralPath $recoverBak).Hash -ne (Get-FileHash -LiteralPath $recoverBackup).Hash) { throw 'Recovery backup restoration hash mismatch.' }
        }
        $installAfter = Get-InstallState
        $installAfter | Set-Content -LiteralPath (Join-Path $testRoot 'install-after.txt')
        if (Compare-Object $installBefore $installAfter) { throw 'Installed LMMS file listing changed during tests.' }
        $env:LMMS_PLUGIN_DIR = $null
        $env:LMMS_AGENT_BRIDGE_LOG = $null
        $env:LMMS_AGENT_BRIDGE_PIPE = $null
        if ($normal.Count -eq 1) {
            $restoreArgs = @{FilePath=$LmmsExe; WorkingDirectory=(Split-Path -Parent $LmmsExe); PassThru=$true; WindowStyle='Normal'}
            if ($RestoreProject) { $restoreArgs.ArgumentList = @('"' + $RestoreProject + '"') }
            $restored = Start-Process @restoreArgs
            $restoreDeadline = (Get-Date).AddSeconds(30)
            do {
                Start-Sleep -Milliseconds 250
                $restored.Refresh()
                if ($restored.MainWindowTitle -eq $expectedTitle) { break }
            } while (-not $restored.HasExited -and (Get-Date) -lt $restoreDeadline)
            Write-Output "Restored normal LMMS PID $($restored.Id): $($restored.MainWindowTitle)"
            if ($restored.MainWindowTitle -ne $expectedTitle) { throw 'Normal LMMS did not restore the expected saved project.' }
            $ErrorActionPreference = 'Continue'
            & $Python (Join-Path $PSScriptRoot 'bridge_client.py') --timeout 0.3 ping 2>$null
            $offlineExit = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            if ($offlineExit -eq 0) { throw 'The bridge unexpectedly remained active in normal LMMS.' }
        }
        $env:LMMS_PLUGIN_DIR = $oldPlugin
        $env:LMMS_AGENT_BRIDGE_LOG = $oldLog
        $env:LMMS_AGENT_BRIDGE_PIPE = $oldPipe
        'Configuration SHA256 restored; installed LMMS listing unchanged; normal session has no bridge.' | Set-Content -LiteralPath (Join-Path $testRoot 'restoration.txt')
    }
    Write-Output "Test artifacts: $testRoot"
}
exit $testExit
