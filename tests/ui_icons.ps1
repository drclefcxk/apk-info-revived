# Verifies the icon wiring:
#   * the exe carries the application icon
#   * the window/taskbar icon switches per detected package format
#
# Icons are read back through Win32 (LoadImageW / WM_GETICON) and painted with
# DrawIconEx, which works for both .ico files and resource handles; the mean
# colour of the result identifies which icon is in place.
#
#   .\ui_icons.ps1
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\Release\ApkInfo.exe"),
    [string]$Icons = (Join-Path (Split-Path -Parent $PSScriptRoot) "assets\icons"),
    [string]$ExampleDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "example")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class IconProbe {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr LoadImageW(IntPtr instance, string name, uint type, int cx, int cy, uint load);
    [DllImport("user32.dll")] public static extern bool DrawIconEx(IntPtr hdc, int x, int y, IntPtr icon,
                                                                  int cx, int cy, uint step, IntPtr brush, uint flags);
    [DllImport("user32.dll")] public static extern bool DestroyIcon(IntPtr icon);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    public static extern uint ExtractIconExW(string file, int index, out IntPtr large, out IntPtr small, uint count);
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, System.Text.StringBuilder text, int count);
}
"@
[void][IconProbe]::SetProcessDPIAware()

$WM_GETICON = 0x007F
$ICON_BIG = 1
$IMAGE_ICON = 1
$LR_LOADFROMFILE = 0x0010
$DI_NORMAL = 0x0003
$SIZE = 64

function Get-MeanColorFromHandle([IntPtr]$hIcon) {
    if ($hIcon -eq [IntPtr]::Zero) { return $null }
    $bitmap = New-Object System.Drawing.Bitmap($SIZE, $SIZE)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $hdc = $graphics.GetHdc()
    [void][IconProbe]::DrawIconEx($hdc, 0, 0, $hIcon, $SIZE, $SIZE, 0, [IntPtr]::Zero, $DI_NORMAL)
    $graphics.ReleaseHdc($hdc)
    $graphics.Dispose()
    $sum = @(0.0, 0.0, 0.0)
    $count = 0
    for ($y = 0; $y -lt $SIZE; $y += 2) {
        for ($x = 0; $x -lt $SIZE; $x += 2) {
            $c = $bitmap.GetPixel($x, $y)
            if ($c.A -lt 128) { continue }
            $sum[0] += $c.R; $sum[1] += $c.G; $sum[2] += $c.B; $count++
        }
    }
    $bitmap.Dispose()
    if ($count -eq 0) { return $null }
    return @(($sum[0] / $count), ($sum[1] / $count), ($sum[2] / $count))
}

function Get-IconMeanColorFromFile([string]$Path) {
    $h = [IconProbe]::LoadImageW([IntPtr]::Zero, $Path, $IMAGE_ICON, $SIZE, $SIZE, $LR_LOADFROMFILE)
    if ($h -eq [IntPtr]::Zero) { throw "cannot load icon file $Path" }
    try { return Get-MeanColorFromHandle $h } finally { [void][IconProbe]::DestroyIcon($h) }
}

function Get-ExeIconMeanColor([string]$Path) {
    # an .exe cannot be opened with LR_LOADFROMFILE; take its first icon group
    $large = [IntPtr]::Zero
    $small = [IntPtr]::Zero
    $count = [IconProbe]::ExtractIconExW($Path, 0, [ref]$large, [ref]$small, 1)
    if ($count -lt 1 -or $large -eq [IntPtr]::Zero) { throw "no icon resource in $Path" }
    try { return Get-MeanColorFromHandle $large } finally {
        [void][IconProbe]::DestroyIcon($large)
        [void][IconProbe]::DestroyIcon($small)
    }
}

function Get-Distance($a, $b) {
    # Compare chromaticity, not absolute brightness: the window icon comes from a
    # 32 px frame while the .ico reference is rendered at 64 px, which shifts the
    # mean brightness but not the hue.
    if ($null -eq $a -or $null -eq $b) { return 999 }
    $sa = $a[0] + $a[1] + $a[2]
    $sb = $b[0] + $b[1] + $b[2]
    if ($sa -le 0 -or $sb -le 0) { return 999 }
    $na = @(($a[0] / $sa * 255), ($a[1] / $sa * 255), ($a[2] / $sa * 255))
    $nb = @(($b[0] / $sb * 255), ($b[1] / $sb * 255), ($b[2] / $sb * 255))
    return [Math]::Sqrt([Math]::Pow($na[0] - $nb[0], 2) + [Math]::Pow($na[1] - $nb[1], 2) + [Math]::Pow($na[2] - $nb[2], 2))
}

$failures = 0

# 1) application icon embedded in the exe (LoadImage loads an exe's first icon group)
$exeColor = Get-ExeIconMeanColor $Exe
$appColor = Get-IconMeanColorFromFile (Join-Path $Icons "app.ico")
$d = Get-Distance $exeColor $appColor
"exe application icon        rgb({0:N0},{1:N0},{2:N0}) vs app.ico rgb({3:N0},{4:N0},{5:N0})  d={6:N1}  {7}" -f `
    $exeColor[0], $exeColor[1], $exeColor[2], $appColor[0], $appColor[1], $appColor[2], $d,
    $(if ($d -lt 6) { "OK" } else { "MISMATCH" })
if ($d -ge 6) { $failures++ }

# 2) window icon per package format
function Test-WindowIcon([string]$Package, [string]$ExpectedName) {
    $process = Start-Process -FilePath $Exe -ArgumentList "`"$Package`"" -PassThru
    $ok = $false
    try {
        $processId = [uint32]$process.Id
        $hwnd = [IntPtr]::Zero
        $deadline = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 300
            $process.Refresh()
            if ($process.HasExited) { throw "app exited with $($process.ExitCode)" }
            $found = [IntPtr]::Zero
            $callback = [IconProbe+EnumProc] {
                param($hWnd, $lParam)
                [uint32]$owner = 0
                [void][IconProbe]::GetWindowThreadProcessId($hWnd, [ref]$owner)
                if ($owner -eq $processId -and [IconProbe]::IsWindowVisible($hWnd)) {
                    $title = New-Object System.Text.StringBuilder 256
                    [void][IconProbe]::GetWindowTextW($hWnd, $title, 256)
                    if ($title.Length -gt 0) { $script:foundWindow = $hWnd; return $false }
                }
                return $true
            }
            $script:foundWindow = [IntPtr]::Zero
            [void][IconProbe]::EnumWindows($callback, [IntPtr]::Zero)
            $found = $script:foundWindow
            if ($found -ne [IntPtr]::Zero) { $hwnd = $found; break }
        }
        if ($hwnd -eq [IntPtr]::Zero) { throw "no window appeared" }

        # the icon is applied once the package has been read; poll for it
        $hIcon = [IntPtr]::Zero
        $iconDeadline = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $iconDeadline) {
            Start-Sleep -Milliseconds 250
            $hIcon = [IconProbe]::SendMessage($hwnd, $WM_GETICON, [IntPtr]$ICON_BIG, [IntPtr]::Zero)
            if ($hIcon -ne [IntPtr]::Zero) { break }
        }
        if ($hIcon -eq [IntPtr]::Zero) { throw "window never got an icon" }
        $actual = Get-MeanColorFromHandle $hIcon
        $expected = Get-IconMeanColorFromFile (Join-Path $Icons "$ExpectedName.ico")
        $distance = Get-Distance $actual $expected
        $ok = ($distance -lt 6)
        "{0,-24} expected {1,-8} rgb({2:N0},{3:N0},{4:N0}) vs ({5:N0},{6:N0},{7:N0})  d={8:N1}  {9}" -f `
            (Split-Path -Leaf $Package), $ExpectedName, $actual[0], $actual[1], $actual[2],
            $expected[0], $expected[1], $expected[2], $distance, $(if ($ok) { "OK" } else { "MISMATCH" })
    } finally {
        if (-not $process.HasExited) { $process.Kill() }
    }
    if (-not $ok) { $script:failures++ }
}

$android = (Get-ChildItem $ExampleDir -Filter "*Lawnchair*.apk" | Select-Object -First 1).FullName
$alpine = (Get-ChildItem $ExampleDir -Filter "*alpine-busybox*.apk" | Select-Object -First 1).FullName
$bundle = (Get-ChildItem $ExampleDir -Filter "*.apks" | Select-Object -First 1).FullName

Test-WindowIcon $android "android"
Test-WindowIcon $alpine "alpine"
Test-WindowIcon $bundle "apks"

if ($failures -eq 0) {
    "== all icon checks passed"
    exit 0
}
"== $failures icon check(s) failed"
exit 1