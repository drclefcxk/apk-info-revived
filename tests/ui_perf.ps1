# Measures the two things the "improve" round was about: language switching and
# memory once a big package is open.
#
#   .\ui_perf.ps1                 # split bundle (the slow case)
#   .\ui_perf.ps1 -Package ..\..\example\some.apk
param(
    [string]$Package,
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\Release\ApkInfo.exe")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
if (-not $Package) {
    $example = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "example"
    $Package = (Get-ChildItem $example -Filter "*.apks" | Select-Object -First 1).FullName
}

$env:APKINFO_DEBUG_TIMING = "1"
$env:APKINFO_AUTO_SWITCH_MS = "1200"
$env:APKINFO_LANG = "en"
$err = Join-Path $env:TEMP "apkinfo_perf_stderr.txt"
Remove-Item $err -ErrorAction SilentlyContinue

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class PerfWin32 {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

function Find-Window([int]$processId) {
    $script:found = [IntPtr]::Zero
    $cb = [PerfWin32+EnumProc] {
        param($h, $p)
        [uint32]$owner = 0
        [void][PerfWin32]::GetWindowThreadProcessId($h, [ref]$owner)
        if ($owner -eq $processId -and [PerfWin32]::IsWindowVisible($h)) {
            $t = New-Object System.Text.StringBuilder 256
            [void][PerfWin32]::GetWindowTextW($h, $t, 256)
            if ($t.Length -gt 0) { $script:found = $h; return $false }
        }
        return $true
    }
    [void][PerfWin32]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

"package: $(Split-Path -Leaf $Package)"
$process = Start-Process -FilePath $Exe -ArgumentList "`"$Package`"" -PassThru -RedirectStandardError $err
try {
    $hwnd = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
        if ($process.HasExited) { throw "app exited" }
        $hwnd = Find-Window $process.Id
        if ($hwnd -ne [IntPtr]::Zero) { break }
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw "no window" }
    Start-Sleep -Seconds 8      # let the package load and the hash thread finish
    $process.Refresh()
    "memory after load: working set {0:N0} MB, private {1:N0} MB, peak working set {2:N0} MB" -f `
        ($process.WorkingSet64 / 1MB), ($process.PrivateMemorySize64 / 1MB), ($process.PeakWorkingSet64 / 1MB)

    # the language button sits in the top bar; its x depends on the window width
    [void][PerfWin32]::SetForegroundWindow($hwnd)
    $rect = New-Object PerfWin32+RECT
    [void][PerfWin32]::GetWindowRect($hwnd, [ref]$rect)
    # The app cycles the language itself (APKINFO_AUTO_SWITCH_MS), which measures
    # the real re-render without depending on a click landing on a moving button.
    Start-Sleep -Seconds 8
    $process.Refresh()
    "memory after language cycling: working set {0:N0} MB, private {1:N0} MB" -f `
        ($process.WorkingSet64 / 1MB), ($process.PrivateMemorySize64 / 1MB)
}
finally {
    if (-not $process.HasExited) { $process.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 400 }
    if (-not $process.HasExited) { $process.Kill() }
    Remove-Item Env:\APKINFO_LANG -ErrorAction SilentlyContinue
    Remove-Item Env:\APKINFO_DEBUG_TIMING -ErrorAction SilentlyContinue
    Remove-Item Env:\APKINFO_AUTO_SWITCH_MS -ErrorAction SilentlyContinue
}

if (Test-Path $err) {
    $lines = Get-Content $err | Where-Object { $_ -match "language switch" }
    if ($lines) { "language switches:"; $lines | ForEach-Object { "  $_" } }
    else { "no language switch timings captured" }
}
