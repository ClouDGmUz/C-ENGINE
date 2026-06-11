# Drag the X-axis gizmo arrow and report cube centroid displacement.
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W3 {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
    [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rt, B; }
}
"@

function Get-Centroid($file) {
    $b = [System.Drawing.Bitmap]::FromFile($file)
    $sx = 0.0; $sy = 0.0; $n = 0
    for ($x = 260; $x -lt 1100; $x += 3) {
        for ($y = 60; $y -lt 800; $y += 3) {
            $p = $b.GetPixel($x, $y)
            if ($p.R -gt 140 -and $p.G -gt 80 -and $p.R -gt $p.G -and $p.G -gt $p.B -and ($p.R - $p.B) -gt 25) {
                $sx += $x; $sy += $y; $n++
            }
        }
    }
    $b.Dispose()
    if ($n -eq 0) { return $null }
    return @([int]($sx / $n), [int]($sy / $n), $n)
}

function Capture($hwnd, $file) {
    $r = New-Object W3+R
    [W3]::GetWindowRect($hwnd, [ref]$r) | Out-Null
    $w = $r.Rt - $r.L; $h = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $bmp.Save($file)
    $g.Dispose(); $bmp.Dispose()
    return $r
}

$p = Start-Process -FilePath "build\Release\glfw_vulkan.exe" -PassThru
Start-Sleep 5
$p.Refresh()
$hwnd = $p.MainWindowHandle
[W3]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 600

$r = Capture $hwnd "drag_before.png"
$c0 = Get-Centroid "drag_before.png"
if ($null -eq $c0) { "NO CUBE FOUND"; Stop-Process -Id $p.Id -Force; exit 1 }
"before: centroid=($($c0[0]),$($c0[1])) px=$($c0[2])"

# grab X arrow shaft: starts at cube centroid, points right-down on screen.
# shaft sample point = centroid + (90,42); drag a further (140,65).
$gx = $r.L + $c0[0] + 90; $gy = $r.T + $c0[1] + 42
[W3]::SetCursorPos($gx, $gy) | Out-Null
Start-Sleep -Milliseconds 250
[W3]::mouse_event(0x02, 0, 0, 0, [UIntPtr]::Zero)   # LMB down
Start-Sleep -Milliseconds 120
for ($i = 1; $i -le 20; $i++) {
    [W3]::SetCursorPos($gx + 7 * $i, $gy + 3 * $i) | Out-Null
    Start-Sleep -Milliseconds 16
}
Start-Sleep -Milliseconds 120
[W3]::mouse_event(0x04, 0, 0, 0, [UIntPtr]::Zero)   # LMB up
Start-Sleep -Milliseconds 400

Capture $hwnd "drag_after.png" | Out-Null
$c1 = Get-Centroid "drag_after.png"
Stop-Process -Id $p.Id -Force
if ($null -eq $c1) { "NO CUBE AFTER"; exit 1 }
"after:  centroid=($($c1[0]),$($c1[1])) px=$($c1[2])"
$dx = $c1[0] - $c0[0]; $dy = $c1[1] - $c0[1]
"displacement: dx=$dx dy=$dy  (drag dir was +140,+60)"
if ($dx -gt 15) { "RESULT: cube followed the drag — direction CORRECT" }
elseif ($dx -lt -15) { "RESULT: cube moved OPPOSITE — direction INVERTED" }
else { "RESULT: cube barely moved — drag likely missed the arrow" }
