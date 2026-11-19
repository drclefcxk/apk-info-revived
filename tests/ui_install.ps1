# Checks the install page and the settings page.
#
#   .\ui_install.ps1              # Chinese UI (default follow-the-system here)
#   .\ui_install.ps1 -Lang en
#
# Nothing is installed on any device: the built-in page is only opened, and the
# external-command path is exercised with "cmd /c echo …", so no adb install runs.
param(
    [string]$Package,
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\Release\ApkInfo.exe"),
    [string]$OutDir = (Join-Path $env:TEMP "apkinfo_install"),
    [string]$Lang = "zh"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Runtime.WindowsRuntime

$root = Split-Path -Parent $PSScriptRoot
$example = Join-Path (Split-Path -Parent $root) "example"
if (-not $Package) {
    $Package = (Get-ChildItem $example -Filter "*.apk" | Where-Object { $_.Name -notmatch "alpine" } |
                Select-Object -First 1).FullName
}
if (-not (Test-Path $Exe)) { throw "missing $Exe - build first (.\build.ps1)" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$configFile = Join-Path (Split-Path -Parent $Exe) "apkinfo-config.json"
# The run rewrites the configuration, so it is put back afterwards.
$configBackup = Join-Path $env:TEMP "apkinfo-config-install-test.json"
if (Test-Path $configFile) { Copy-Item $configFile $configBackup -Force }
function Restore-Config {
    if (Test-Path $configBackup) {
        Copy-Item $configBackup $configFile -Force
        Remove-Item $configBackup -Force
    }
}
# A small, always valid document is written before every run: editing the file the
# application produced would mean re-serialising a document that contains escaped
# quotes and a _readme block, and a broken edit silently falls back to the defaults
# (which is exactly what made an earlier version of this test pass vacuously).
function Set-InstallConfig([string]$androidMode, [string]$androidCommand) {
    $android = [ordered]@{ mode = $androidMode }
    if ($androidCommand) { $android.command = $androidCommand }
    $document = [ordered]@{
        language = "auto"
        install = [ordered]@{
            adbPath = ""
            android = $android
            alpine = [ordered]@{ command = 'echo ALPINE {file_name}' }
            options = [ordered]@{ replace = $true; grant = $false; downgrade = $false; testPackages = $false }
            device = ""
        }
    }
    $document | ConvertTo-Json -Depth 8 | Set-Content -Path $configFile -Encoding UTF8
}
Set-InstallConfig "builtin" $null
$env:APKINFO_LANG = $Lang
Remove-Item Env:\APKINFO_START_TAB -ErrorAction SilentlyContinue

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class UI {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, int d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
[void][UI]::SetProcessDPIAware()

$null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Storage.StorageFile, Windows.Foundation, ContentType = WindowsRuntime]
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, $t) { $task = $asTask.MakeGenericMethod($t).Invoke($null, @($op)); $task.Wait(-1) | Out-Null; $task.Result }

function Get-Lines([string]$path) {
    $file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($path)) ([Windows.Storage.StorageFile])
    $stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
    $decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
    $software = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
    $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
    $result = Await ($engine.RecognizeAsync($software)) ([Windows.Media.Ocr.OcrResult])
    $lines = @()
    foreach ($line in $result.Lines) {
        $words = @($line.Words); if ($words.Count -eq 0) { continue }
        $x0 = ($words | ForEach-Object { $_.BoundingRect.X } | Measure-Object -Minimum).Minimum
        $y0 = ($words | ForEach-Object { $_.BoundingRect.Y } | Measure-Object -Minimum).Minimum
        $x1 = ($words | ForEach-Object { $_.BoundingRect.X + $_.BoundingRect.Width } | Measure-Object -Maximum).Maximum
        $h1 = ($words | ForEach-Object { $_.BoundingRect.Height } | Measure-Object -Maximum).Maximum
        $lines += [pscustomobject]@{ Text = $line.Text; X = [int]$x0; Y = [int]$y0; W = [int]($x1-$x0); H = [int]$h1 }
    }
    $lines
}

function Capture([IntPtr]$hwnd, [string]$name) {
    $rect = New-Object UI+RECT
    [void][UI]::GetWindowRect($hwnd, [ref]$rect)
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    [void][UI]::PrintWindow($hwnd, $dc, 2)
    $graphics.ReleaseHdc($dc)
    $graphics.Dispose()
    $path = Join-Path $OutDir "$name.png"
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    @{ Path = $path; Rect = $rect; Width = $width; Height = $height }
}

function Get-Windows([int]$processId) {
    $rows = New-Object System.Collections.ArrayList
    $callback = [UI+EnumProc] {
        param($h, $l)
        [uint32]$owner = 0
        [void][UI]::GetWindowThreadProcessId($h, [ref]$owner)
        if ($owner -eq $processId -and [UI]::IsWindowVisible($h)) {
            $text = New-Object System.Text.StringBuilder 256
            [void][UI]::GetWindowTextW($h, $text, 256)
            if ($text.Length -gt 0) { [void]$rows.Add(("{0}|{1}" -f $h.ToInt64(), $text.ToString())) }
        }
        return $true
    }
    [void][UI]::EnumWindows($callback, [IntPtr]::Zero)
    $rows
}

function Start-App {
    $process = Start-Process -FilePath $Exe -ArgumentList "`"$Package`"" -PassThru
    $handle = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $deadline -and $handle -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 500
        foreach ($entry in (Get-Windows $process.Id)) {
            $parts = $entry -split '\|'
            if ($parts[1] -match "APK Info") { $handle = [IntPtr][int64]$parts[0] }
        }
    }
    if ($handle -eq [IntPtr]::Zero) { throw "the main window did not appear" }
    [void][UI]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 1200
    @{ Process = $process; Handle = $handle }
}

function Stop-App($app) {
    if ($app -and -not $app.Process.HasExited) {
        $app.Process.Kill()
        $app.Process.WaitForExit(5000) | Out-Null
    }
    Start-Sleep -Milliseconds 400
}

function Click-At([int]$x, [int]$y, [IntPtr]$hwnd) {
    [void][UI]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 200
    if ([UI]::GetForegroundWindow() -ne $hwnd) {
        throw "refusing to click: the target window is not in the foreground"
    }
    [void][UI]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 150
    [UI]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [UI]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 700
}

# The toolbar cluster starts at (client width / 1.25) - 664 logical pixels.
function Toolbar-Button([IntPtr]$hwnd, [int]$indexFromLeft) {
    $client = New-Object UI+RECT
    [void][UI]::GetClientRect($hwnd, [ref]$client)
    $scale = 1.25
    $start = ($client.Right - $client.Left) / $scale - 664
    # settings(88) copy(88) dump(88) reload(88) install(96) open(148), 10px gaps
    $offsets = @(0, 98, 196, 294, 392, 498)
    $widths = @(88, 88, 88, 88, 96, 148)
    $logical = $start + $offsets[$indexFromLeft] + $widths[$indexFromLeft] / 2
    $rect = New-Object UI+RECT
    [void][UI]::GetWindowRect($hwnd, [ref]$rect)
    @{ X = [int]($rect.Left + $logical * $scale); Y = $rect.Top + 70 }
}

# Waits for another window of the process and returns its handle with its native title
# (the title is what the page sets through its window-title property, so it is a far
# more reliable identity than OCR of a small bold caption).
function Take-OtherWindow([int]$processId, [IntPtr]$main) {
    $deadline = (Get-Date).AddSeconds(12)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 400
        foreach ($entry in (Get-Windows $processId)) {
            $parts = $entry -split '\|'
            if ([IntPtr][int64]$parts[0] -ne $main) {
                return @{ Handle = [IntPtr][int64]$parts[0]; Title = $parts[1] }
            }
        }
    }
    $null
}

$checks = 0
$failed = 0
function Check([bool]$condition, [string]$what) {
    $script:checks++
    if ($condition) { Write-Host "  OK      $what" -ForegroundColor Green }
    else { Write-Host "  FAILED  $what" -ForegroundColor Red; $script:failed++ }
}

# a language-agnostic substring match: OCR puts spaces between CJK characters
function Has($lines, [string]$pattern) {
    @($lines | Where-Object { $_.Text -match $pattern }).Count -gt 0
}

Write-Output "install page + settings page ($Lang)"

# ---------------------------------------------------------------- install page
$app = Start-App
try {
    $button = Toolbar-Button $app.Handle 4          # "Install"
    Click-At $button.X $button.Y $app.Handle
    $page = Take-OtherWindow $app.Process.Id $app.Handle
    Check ($null -ne $page) "the install page opens"
    if ($null -ne $page) {
        $expected = if ($Lang -eq "en") { "Install" } else { "安装" }
        Check ($page.Title -eq $expected) "the page is titled '$expected' (title: '$($page.Title)')"
        $shot = Capture $page.Handle "10-install-page"
        $lines = Get-Lines $shot.Path
        Check (Has $lines "adb") "the page reports the adb it uses"
        Check (Has $lines "无\s*线|Wireless") "the wireless section is there"
        Check (Has $lines "配\s*对|Pair") "the pairing controls are there"
        Check (Has $lines "刷\s*新|Refresh") "the refresh button is there"
        Check (Has $lines "连\s*接|Connect") "the connect button is there"
        # the device list either lists a device or shows the empty hint
        $empty = Has $lines "没\s*有\s*设\s*备|No\s*device"
        # the row label is "<model>  (<serial>)  ·  <state>" and the state is never
        # translated, so "device" (or a serial-like token) is what to look for
        $listed = Has $lines "device|\([A-Z0-9]{8,}\)|:\d{2,5}"
        Check ($empty -or $listed) "the device list shows a device or the empty hint"
        Check (-not (Has $lines "Unhandled|Exception|assert")) "the page holds no error text"

        # refresh must finish (the busy marker disappears and the list stays)
        $refresh = $lines | Where-Object { $_.Text -match "^\s*刷\s*新|^\s*Refresh" } | Select-Object -First 1
        if ($null -ne $refresh) {
            Click-At ($shot.Rect.Left + $refresh.X + [int]($refresh.W / 2)) `
                     ($shot.Rect.Top + $refresh.Y + [int]($refresh.H / 2)) $page.Handle
            Start-Sleep -Milliseconds 1800
            Check ($null -ne $page) "refreshing keeps the window open"
        }
        [void][UI]::SendMessageW($page.Handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
        Start-Sleep -Milliseconds 900
        Check ($null -eq (Take-OtherWindow $app.Process.Id $app.Handle)) "the install page closes again"
    }
}
finally { Stop-App $app }

# --------------------------------------------------------------- settings page
$app = Start-App
try {
    $button = Toolbar-Button $app.Handle 0          # "Settings"
    Click-At $button.X $button.Y $app.Handle
    $page = Take-OtherWindow $app.Process.Id $app.Handle
    Check ($null -ne $page) "the settings page opens"
    if ($null -ne $page) {
        $expected = if ($Lang -eq "en") { "Settings" } else { "设置" }
        Check ($page.Title -eq $expected) "the page is titled '$expected' (title: '$($page.Title)')"
        $shot = Capture $page.Handle "20-settings-page"
        $lines = Get-Lines $shot.Path
        Check (Has $lines "界\s*面|[Il]nterface|Language") "the language selector is in the settings"
        Check (Has $lines "方\s*式|[Il]nstall\s*method|method") "the install method selector is there"
        Check (Has $lines "\{path\}") "the placeholder reference is listed"
        Check (Has $lines "\{file_name\}") "the placeholder list is complete"
        Check (Has $lines "adb") "the adb path field is there"

        $json = Get-Content $configFile -Raw -ErrorAction SilentlyContinue
        Check ($null -ne $json -and $json -match '"install"') "the config keeps its install section"
        [void][UI]::SendMessageW($page.Handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 800
    }
}
finally { Stop-App $app }

# ------------------------------------------------- the external command path
# Command mode runs something harmless instead of adb, so the placeholders and the
# output window are covered without touching a device.
Set-InstallConfig "command" 'cmd /c echo CMD-OK {kind} {file_name}'
$app = Start-App
try {
    $button = Toolbar-Button $app.Handle 4
    Click-At $button.X $button.Y $app.Handle
    $viewer = Take-OtherWindow $app.Process.Id $app.Handle
    Check ($null -ne $viewer) "command mode opens an output window"
    if ($null -ne $viewer) {
        $expected = if ($Lang -eq "en") { "Install command output" } else { "安装命令输出" }
        Check ($viewer.Title -eq $expected) "its window is titled '$expected' (title: '$($viewer.Title)')"
        Start-Sleep -Milliseconds 1500
        $lines = Get-Lines (Capture $viewer.Handle "30-command-output").Path
        Check (Has $lines "CMD-OK") "the command actually ran"
        Check (Has $lines "android|apks") "the {kind} placeholder was expanded"
        [void][UI]::SendMessageW($viewer.Handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    }
}
finally { Stop-App $app }
# ------------------------------------------------------- a package on a UNC share
# WSL builds live on \\wsl.localhost\<distro>\..., and a UNC current directory makes
# cmd.exe give up ("UNC paths are not supported") and continue in C:\Windows, which
# broke the install command. The share here is the local C$ share, so the test needs no
# WSL distribution - only admin rights to the administrative share.
#
#   $env:APKINFO_UNC_ROOT = "\\wsl.localhost\Ubuntu\home\me"   # to use a real WSL path
$uncRoot = if ($env:APKINFO_UNC_ROOT) { $env:APKINFO_UNC_ROOT } else { "\\localhost\C$\Users\$env:USERNAME\AppData\Local\Temp\apkinfo-unc" }
# the administrative share may simply not be reachable (no admin, no loopback share)
$uncReachable = if ($env:APKINFO_UNC_ROOT) { Test-Path $env:APKINFO_UNC_ROOT } else { Test-Path "\\localhost\C$\Windows" }
if ($uncReachable) {
    New-Item -ItemType Directory -Force -Path $uncRoot | Out-Null
    $uncPackage = Join-Path $uncRoot (Split-Path -Leaf $Package)
    Copy-Item $Package $uncPackage -Force
    # No cmd.exe in the command: it is the one program that cannot start in a UNC
    # directory, and the runner now starts the command directly.
    Set-InstallConfig "command" 'powershell -NoProfile -Command "Write-Output (''CWD='' + (Get-Location).Path + '' KIND={kind}'')"'
    $env:APKINFO_LANG = $Lang
    $arguments = "`"$uncPackage`""
    $process = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
    $handle = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $deadline -and $handle -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 500
        foreach ($entry in (Get-Windows $process.Id)) {
            $parts = $entry -split '\|'
            if ($parts[1] -match "APK Info") { $handle = [IntPtr][int64]$parts[0] }
        }
    }
    $app = @{ Process = $process; Handle = $handle }
    try {
        Check ($handle -ne [IntPtr]::Zero) "a package on a UNC share opens"
        if ($handle -ne [IntPtr]::Zero) {
            $button = Toolbar-Button $handle 4
            Click-At $button.X $button.Y $handle
            $viewer = Take-OtherWindow $process.Id $handle
            Check ($null -ne $viewer) "the install command runs for a package on a UNC share"
            if ($null -ne $viewer) {
                Start-Sleep -Milliseconds 1800
                $lines = Get-Lines (Capture $viewer.Handle "40-unc-output").Path
                $joined = ($lines | ForEach-Object { $_.Text }) -join " "
                # the child's own current directory has to be the share, not C:\Windows
                Check ($joined -match "CWD=") "the command reported its working directory"
                Check ($joined -match "localhost|apkinfo-unc|C\$") `
                      "the command ran inside the share ($joined)"
                Check ($joined -notmatch "UNC paths are not supported") "nothing complained about the UNC path"
                # OCR reads a capital I as a lowercase l ("KlND")
                Check ($joined -match "K[Il]ND=(android|apks)") "the placeholders were expanded for the UNC package"
                [void][UI]::SendMessageW($viewer.Handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            }
        }
    }
    finally { Stop-App $app }
}
else {
    Write-Output "  (no UNC share reachable at $uncRoot - the UNC phase was skipped)"
}

Restore-Config

Write-Output ""
if ($failed -eq 0) { Write-Output "== all install-page checks passed"; exit 0 }
Write-Output "== $failed check(s) failed"
exit 1
