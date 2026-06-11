# Launch engine with given args, wait, screenshot the engine window, kill.
param([string]$EngineArgs = "", [string]$Out = "diag.png", [int]$WaitSec = 6)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32 {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$exe = Join-Path $PSScriptRoot "..\build\Release\glfw_vulkan.exe"
$argList = if ($EngineArgs) { $EngineArgs -split ' ' } else { @() }
$p = if ($argList.Count) { Start-Process -FilePath $exe -ArgumentList $argList -PassThru } else { Start-Process -FilePath $exe -PassThru }
Start-Sleep -Seconds $WaitSec

if ($p.HasExited) { "PROCESS EXITED code=$($p.ExitCode)"; exit 1 }

$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { "NO WINDOW HANDLE"; Stop-Process -Id $p.Id -Force; exit 1 }

# HWND_TOPMOST = -1; SWP_NOMOVE|SWP_NOSIZE = 0x0003
[Win32]::SetWindowPos($hwnd, [IntPtr](-1), 0, 0, 0, 0, 0x0003) | Out-Null
[Win32]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 800

$rect = New-Object Win32+RECT
[Win32]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
"window rect: $($rect.Left),$($rect.Top) ${w}x${h}"

$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

Stop-Process -Id $p.Id -Force
"saved $Out"
