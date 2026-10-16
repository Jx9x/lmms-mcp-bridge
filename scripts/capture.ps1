# capture.ps1 - screenshot the LMMS main window to a PNG.
param([string]$Out = 'lmms-agentbridge.png', [int]$LmmsPid = 0)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class WinApi {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int n);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
'@
$p = Get-Process -Id $LmmsPid -ErrorAction SilentlyContinue
if (-not $p -or $p.MainWindowHandle -eq 0) { [Console]::Error.WriteLine('no window handle'); exit 1 }
$hwnd = $p.MainWindowHandle
if ([WinApi]::IsIconic($hwnd)) { [void][WinApi]::ShowWindow($hwnd, 9) }  # SW_RESTORE
[void][WinApi]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 700
$r = New-Object WinApi+RECT
[void][WinApi]::GetWindowRect($hwnd, [ref]$r)
$w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
if ($w -le 0 -or $h -le 0) { [Console]::Error.WriteLine("bad rect $w x $h"); exit 1 }
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output "saved $Out ($w x $h)"
