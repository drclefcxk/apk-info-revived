# Regenerates the screenshots used by README.md / README.en.md.
#
#   .\make_screenshots.ps1                  # writes docs\screenshots
#   .\make_screenshots.ps1 -OutDir C:\tmp\shots
#
# The Alpine shots pick samples by name from the sibling example/ directory (which is
# not part of the repository):
#   *luci-app-acl*    a noarch package (an OpenWrt LuCI module)
#   *Alpine_busybox*  an x86_64 v2 package (Alpine main/busybox)
#   *OpenWrt_alpine-* an aarch64 v3 package (OpenWrt's busybox)
#
# The app is driven the way a user drives it (OCR finds the text, the mouse clicks),
# because Slint exposes no accessibility tree. Every click first raises the target
# window and verifies it is the foreground window: a blind click would land on
# whatever window happens to be there.
param(
    [string]$Package,
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\Release\ApkInfo.exe"),
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) "docs\screenshots")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Runtime.WindowsRuntime

$example = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "example"
$Bundle = $Package
if (-not $Bundle) {
    $Bundle = (Get-ChildItem $example -Filter "*.apks" | Select-Object -First 1).FullName
}
# a single package is what has a real file tree; a bundle shows its splits there
$Single = (Get-ChildItem $example -Filter "*.apk" | Where-Object { $_.Name -notmatch "alpine" } |
           Select-Object -First 1).FullName
if (-not (Test-Path $Exe)) { throw "missing $Exe - build first (.\build.ps1)" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# The screenshots are of the default state: language "follow the system", which here is
# Chinese for the -zh shots; the -en shots override it for one run via APKINFO_LANG.
# The screenshots show the default state, so the configuration is moved aside for the
# run and restored afterwards (the test suite leaves its own commands in there).
$configFile = Join-Path (Split-Path -Parent $Exe) "apkinfo-config.json"
$configBackup = Join-Path $env:TEMP "apkinfo-config-backup.json"
if (Test-Path $configFile) {
    Copy-Item $configFile $configBackup -Force
    Remove-Item $configFile -Force
}
Remove-Item Env:\APKINFO_AUTO_SWITCH_MS -ErrorAction SilentlyContinue

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Shot {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, int data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
[void][Shot]::SetProcessDPIAware()

$null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Storage.StorageFile, Windows.Foundation, ContentType = WindowsRuntime]
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]

function Await($operation, $type) {
    $task = $asTask.MakeGenericMethod($type).Invoke($null, @($operation))
    $task.Wait(-1) | Out-Null
    $task.Result
}

function Get-AppWindow([int]$processId) {
    $script:found = [IntPtr]::Zero
    $callback = [Shot+EnumProc] {
        param($h, $l)
        [uint32]$owner = 0
        [void][Shot]::GetWindowThreadProcessId($h, [ref]$owner)
        if ($owner -eq $processId -and [Shot]::IsWindowVisible($h)) {
            $text = New-Object System.Text.StringBuilder 512
            [void][Shot]::GetWindowTextW($h, $text, 512)
            if ($text.Length -gt 0) { $script:found = $h; return $false }
        }
        return $true
    }
    [void][Shot]::EnumWindows($callback, [IntPtr]::Zero)
    return $script:found
}

function Start-App([string]$path, [string]$language, [string]$startTab = "0") {
    $env:APKINFO_LANG = $language
    $env:APKINFO_START_TAB = $startTab
    $process = Start-Process -FilePath $Exe -ArgumentList "`"$path`"" -PassThru
    $handle = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $deadline -and $handle -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 500
        $handle = Get-AppWindow $process.Id
    }
    if ($handle -eq [IntPtr]::Zero) { throw "the window did not appear" }
    [void][Shot]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 1500
    return @{ Process = $process; Handle = $handle }
}

function Stop-App($app) {
    if ($app -and -not $app.Process.HasExited) {
        $app.Process.Kill()
        $app.Process.WaitForExit(5000) | Out-Null
    }
    Start-Sleep -Milliseconds 400
}

function Capture([IntPtr]$hwnd, [string]$name) {
    $rect = New-Object Shot+RECT
    [void][Shot]::GetWindowRect($hwnd, [ref]$rect)
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    [void][Shot]::PrintWindow($hwnd, $hdc, 2)
    $graphics.ReleaseHdc($hdc)
    $graphics.Dispose()
    $path = Join-Path $OutDir "$name.png"
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    return @{ Path = $path; Rect = $rect; Width = $width; Height = $height }
}

# The combo box popup is its own window, so PrintWindow would miss it: grab the screen
# region instead (the app has just been raised, so nothing covers it).
function Capture-Screen([IntPtr]$hwnd, [string]$name) {
    $rect = New-Object Shot+RECT
    [void][Shot]::GetWindowRect($hwnd, [ref]$rect)
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size($width, $height)))
    $graphics.Dispose()
    # inset by the window shadow so no desktop pixels leak into the screenshot
    $inset = 8
    $cropped = New-Object System.Drawing.Bitmap(($width - 2 * $inset), ($height - 2 * $inset))
    $cropGraphics = [System.Drawing.Graphics]::FromImage($cropped)
    $cropGraphics.DrawImage($bitmap, (New-Object System.Drawing.Rectangle(0, 0, ($width - 2 * $inset), ($height - 2 * $inset))),
                            (New-Object System.Drawing.Rectangle($inset, $inset, ($width - 2 * $inset), ($height - 2 * $inset))),
                            [System.Drawing.GraphicsUnit]::Pixel)
    $cropGraphics.Dispose()
    $bitmap.Dispose()
    $bitmap = $cropped
    $path = Join-Path $OutDir "$name.png"
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    return @{ Path = $path; Rect = $rect; Width = $width; Height = $height }
}

# Covers the device rows of the install page with a fictional one: the screenshot is
# published, the serial of the phone that happened to be plugged in is not.
function Protect-DeviceList([string]$path, $shot) {
    # FromFile keeps the file locked, so work on a copy
    $source = [System.Drawing.Image]::FromFile($path)
    $bitmap = New-Object System.Drawing.Bitmap($source)
    $source.Dispose()
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $background = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(250, 251, 252))
    # the device list sits in the first card of the left column
    $left = 24
    $top = 100
    $width = [int]($shot.Width * 0.36)
    $height = 230
    $graphics.FillRectangle($background, $left, $top, $width, $height)
    $font = New-Object System.Drawing.Font("Consolas", 9)
    $ink = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(29, 35, 48))
    $graphics.DrawString("O  Pixel 7  (emulator-5554)  ·  device", $font, $ink, ($left + 12), ($top + 10))
    $graphics.DrawString("O  Pixel 4a (R58M12345)       ·  unauthorized", $font, $ink, ($left + 12), ($top + 40))
    $graphics.Dispose()
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    $font.Dispose()
    $ink.Dispose()
    $background.Dispose()
}

function Get-Lines([string]$path) {
    $file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($path)) ([Windows.Storage.StorageFile])
    $stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
    $decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
    $software = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
    $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
    $result = Await ($engine.RecognizeAsync($software)) ([Windows.Media.Ocr.OcrResult])
    $lines = @()
    foreach ($line in $result.Lines) {
        $words = @($line.Words)
        if ($words.Count -eq 0) { continue }
        $x0 = ($words | ForEach-Object { $_.BoundingRect.X } | Measure-Object -Minimum).Minimum
        $y0 = ($words | ForEach-Object { $_.BoundingRect.Y } | Measure-Object -Minimum).Minimum
        $x1 = ($words | ForEach-Object { $_.BoundingRect.X + $_.BoundingRect.Width } | Measure-Object -Maximum).Maximum
        $y1 = ($words | ForEach-Object { $_.BoundingRect.Y + $_.BoundingRect.Height } | Measure-Object -Maximum).Maximum
        $lines += [pscustomobject]@{ Text = $line.Text; X = [int]$x0; Y = [int]$y0; W = [int]($x1 - $x0); H = [int]($y1 - $y0) }
    }
    return $lines
}

function Click-At([int]$screenX, [int]$screenY, [IntPtr]$hwnd) {
    [void][Shot]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 150
    if ([Shot]::GetForegroundWindow() -ne $hwnd) {
        throw "refusing to click: the target window is not in the foreground"
    }
    [void][Shot]::SetCursorPos($screenX, $screenY)
    Start-Sleep -Milliseconds 120
    [Shot]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [Shot]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 500
}

function Click-Line($shot, $line, [IntPtr]$hwnd) {
    Click-At ($shot.Rect.Left + $line.X + [int]($line.W / 2)) ($shot.Rect.Top + $line.Y + [int]($line.H / 2)) $hwnd
}

function Scroll-By([IntPtr]$hwnd, [int]$x, [int]$y, [int]$notches) {
    [void][Shot]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 120
    [void][Shot]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 100
    # a positive notch count scrolls down, i.e. a negative wheel delta
    [Shot]::mouse_event(0x0800, 0, 0, [int](-120 * $notches), [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 400
}

function Move-Away([IntPtr]$hwnd) {
    $rect = New-Object Shot+RECT
    [void][Shot]::GetWindowRect($hwnd, [ref]$rect)
    [void][Shot]::SetCursorPos(($rect.Left - 60), ($rect.Top - 60))
    Start-Sleep -Milliseconds 250
}

# --------------------------------------------------------------------------- shots
Write-Output "package: $(Split-Path -Leaf $Bundle)"

$app = Start-App $Bundle "zh"
try {
    # 1. the main window, information tab, as it opens
    Move-Away $app.Handle
    $main = Capture $app.Handle "main-window-zh"
    Write-Output "  main-window-zh.png  ($($main.Width)x$($main.Height))"

    # 2. scroll until a section ends with its "显示全部 (N)" button
    $shot = $main
    $button = $null
    for ($i = 0; $i -lt 30 -and $null -eq $button; $i++) {
        $button = (Get-Lines $shot.Path) | Where-Object { $_.Text -match "显\s*示\s*全\s*部|Show\s*all" } | Select-Object -First 1
        if ($null -ne $button) { break }
        # positive = scroll the list down
        Scroll-By $app.Handle ($shot.Rect.Left + [int]($shot.Width * 0.6)) ($shot.Rect.Top + [int]($shot.Height * 0.55)) 5
        $shot = Capture $app.Handle "rows-with-show-all-zh"
    }
    if ($null -eq $button) { throw "no '显示全部' button found while scrolling" }
    # one notch up so the button and the rows above it are both visible
    Scroll-By $app.Handle ($shot.Rect.Left + [int]($shot.Width * 0.6)) ($shot.Rect.Top + [int]($shot.Height * 0.55)) -1
    Move-Away $app.Handle
    $rows = Capture $app.Handle "rows-with-show-all-zh"
    Write-Output "  rows-with-show-all-zh.png"

    # 3. the viewer window the button opens
    $button = (Get-Lines $rows.Path) | Where-Object { $_.Text -match "显\s*示\s*全\s*部|Show\s*all" } | Select-Object -First 1
    if ($null -eq $button) { throw "the button moved out of view" }
    Click-Line $rows $button $app.Handle
    Start-Sleep -Milliseconds 1200
    $viewer = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(8)
    while ((Get-Date) -lt $deadline -and $viewer -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 400
        $script:candidates = @()
        $callback = [Shot+EnumProc] {
            param($h, $l)
            [uint32]$owner = 0
            [void][Shot]::GetWindowThreadProcessId($h, [ref]$owner)
            if ($owner -eq $app.Process.Id -and [Shot]::IsWindowVisible($h) -and $h -ne $app.Handle) {
                $text = New-Object System.Text.StringBuilder 512
                [void][Shot]::GetWindowTextW($h, $text, 512)
                if ($text.Length -gt 0) { $script:candidates += $h }
            }
            return $true
        }
        [void][Shot]::EnumWindows($callback, [IntPtr]::Zero)
        if ($script:candidates.Count -gt 0) { $viewer = $script:candidates[0] }
    }
    if ($viewer -eq [IntPtr]::Zero) { throw "the viewer window did not appear" }
    Move-Away $viewer
    $viewerShot = Capture $viewer "viewer-zh"
    Write-Output "  viewer-zh.png  ($($viewerShot.Width)x$($viewerShot.Height))"
    [void][Shot]::SendMessageW($viewer, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
    Start-Sleep -Milliseconds 900

}
finally { Stop-App $app }

# 4. the file tree of a single package, one directory expanded
#    (started on the contents tab: the tab label is Chinese and OCR mangles it)
$app = Start-App $Single "zh" "1"
try {
    $shot = Capture $app.Handle "temp-tree"
    $directory = (Get-Lines $shot.Path) | Where-Object { $_.Text -match "/\s*$" } | Select-Object -First 1
    if ($null -ne $directory) {
        Click-Line $shot $directory $app.Handle
        Start-Sleep -Milliseconds 1000
    }
    Move-Away $app.Handle
    $tree = Capture $app.Handle "tree-zh"
    Write-Output "  tree-zh.png"
}
finally { Stop-App $app }

# 6. the same window in English
$app = Start-App $Bundle "en"
try {
    Move-Away $app.Handle
    $english = Capture $app.Handle "main-window-en"
    Write-Output "  main-window-en.png  ($($english.Width)x$($english.Height))"
}
finally { Stop-App $app }

# 7. Alpine packages: arch-independent, x86_64 and aarch64, plus a file index
$alpineJobs = @(
    @{ Pattern = "*luci-app-acl*";    Name = "alpine-noarch-zh" },
    @{ Pattern = "*Alpine_busybox*";  Name = "alpine-x86_64-zh" },
    @{ Pattern = "*OpenWrt_alpine-*"; Name = "alpine-aarch64-zh" }
)
$alpineTree = $null
foreach ($job in $alpineJobs) {
    $sample = Get-ChildItem $example -Filter "*.apk" | Where-Object { $_.Name -like $job.Pattern } |
              Select-Object -First 1
    if ($null -eq $sample) {
        Write-Warning "no sample for $($job.Name) (looked for $($job.Pattern) in $example)"
        continue
    }
    $app = Start-App $sample.FullName "zh"
    try {
        Move-Away $app.Handle
        [void](Capture $app.Handle $job.Name)
        Write-Output "  $($job.Name).png  ($($sample.Name))"
    }
    finally { Stop-App $app }
    if ($job.Name -eq "alpine-x86_64-zh") { $alpineTree = $sample.FullName }
}

# the file index of an Alpine package (started on the contents tab)
if ($null -ne $alpineTree) {
    $app = Start-App $alpineTree "zh" "1"
    try {
        $shot = Capture $app.Handle "temp-alpine-tree"
        # one click on the caret of a top-level directory: clicking the same row twice
        # would collapse it again, which is what an earlier version of this script did
        $directory = (Get-Lines $shot.Path) |
                     Where-Object { $_.Text -match "^\s*(bin|etc|usr|lib|sbin|var)\s*$" } |
                     Select-Object -First 1
        if ($null -ne $directory) {
            Click-At ($shot.Rect.Left + $directory.X + 8) ($shot.Rect.Top + $directory.Y + [int]($directory.H / 2)) $app.Handle
            Start-Sleep -Milliseconds 1000
        }
        Move-Away $app.Handle
        [void](Capture $app.Handle "alpine-tree-zh")
        Write-Output "  alpine-tree-zh.png"
    }
    finally { Stop-App $app }
}

# 8. the install page and the settings page
$app = Start-App $Bundle "zh"
try {
    # the toolbar cluster is explicit geometry; Settings is the first button
    $client = New-Object Shot+RECT
    [void][Shot]::GetClientRect($app.Handle, [ref]$client)
    $scale = 1.25
    $clusterStart = ($client.Right - $client.Left) / $scale - 664
    $rect = New-Object Shot+RECT
    [void][Shot]::GetWindowRect($app.Handle, [ref]$rect)

    $installX = [int]($rect.Left + ($clusterStart + 392 + 48) * $scale)
    Click-At $installX ($rect.Top + 70) $app.Handle
    $page = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(12)
    while ((Get-Date) -lt $deadline -and $page -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 400
        $callback = [Shot+EnumProc] {
            param($h, $l)
            [uint32]$owner = 0
            [void][Shot]::GetWindowThreadProcessId($h, [ref]$owner)
            if ($owner -eq $app.Process.Id -and [Shot]::IsWindowVisible($h) -and $h -ne $app.Handle) {
                $text = New-Object System.Text.StringBuilder 256
                [void][Shot]::GetWindowTextW($h, $text, 256)
                if ($text.Length -gt 0) { $script:foundPage = $h }
            }
            return $true
        }
        $script:foundPage = [IntPtr]::Zero
        [void][Shot]::EnumWindows($callback, [IntPtr]::Zero)
        $page = $script:foundPage
    }
    if ($page -ne [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 1500
        Move-Away $page
        $installShot = Capture $page "install-page-zh"
        Protect-DeviceList $installShot.Path $installShot
        Write-Output "  install-page-zh.png"
        [void][Shot]::SendMessageW($page, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 800
    }

    $settingsX = [int]($rect.Left + ($clusterStart + 44) * $scale)
    Click-At $settingsX ($rect.Top + 70) $app.Handle
    Start-Sleep -Seconds 2
    $script:foundPage = [IntPtr]::Zero
    [void][Shot]::EnumWindows($callback, [IntPtr]::Zero)
    if ($script:foundPage -ne [IntPtr]::Zero) {
        Move-Away $script:foundPage
        [void](Capture $script:foundPage "settings-zh")
        Write-Output "  settings-zh.png"
        [void][Shot]::SendMessageW($script:foundPage, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    }
}
finally { Stop-App $app }

# put the previous configuration back
if (Test-Path $configBackup) {
    Copy-Item $configBackup $configFile -Force
    Remove-Item $configBackup -Force
}

Remove-Item (Join-Path $OutDir "temp-*.png") -ErrorAction SilentlyContinue
Write-Output "screenshots written to $OutDir"
Get-ChildItem $OutDir -Filter "*.png" | Sort-Object Name | ForEach-Object { "  {0,7:N1} KB  {1}" -f ($_.Length/1KB), $_.Name }
