# Clicks a screen coordinate in another process' window (UI smoke helper).
#   .\click.ps1 -X 1090 -Y 73
param([int]$X, [int]$Y)

Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Clicker {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint cButtons, UIntPtr dwExtraInfo);
}
"@
[void][Clicker]::SetProcessDPIAware()
[void][Clicker]::SetCursorPos($X, $Y)
Start-Sleep -Milliseconds 120
[Clicker]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # left down
Start-Sleep -Milliseconds 60
[Clicker]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # left up
"clicked $X,$Y"
