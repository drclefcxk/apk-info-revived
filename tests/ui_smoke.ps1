# GUI smoke check: opens a package, captures the window and OCRs it.
#
# There is no UI automation framework here, but Windows' own OCR engine is enough
# to assert that the window really shows the package: text that must appear
# (application name, section headers, tabs) is printed with its position, so a
# regression that makes a panel render empty is visible at a glance.
#
#   .\ui_smoke.ps1                                  # default sample
#   .\ui_smoke.ps1 -Package ..\..\example\x.apk -StartTab 1
param(
    [string]$Package,
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\Release\ApkInfo.exe"),
    [string]$Out = (Join-Path $env:TEMP "apkinfo_ui.png"),
    [ValidateSet(0, 1)][int]$StartTab = 0,
    [int]$WaitMs = 5000
)

$ErrorActionPreference = "Stop"
if (-not $Package) {
    $example = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "example"
    $Package = (Get-ChildItem $example -Filter "*Lawnchair*.apk" | Select-Object -First 1).FullName
}
if (-not (Test-Path $Exe)) { throw "missing $Exe - build first (.\build.ps1)" }

$env:APKINFO_START_TAB = "$StartTab"

# The app is per-monitor DPI aware, so this *caller* must be too: otherwise
# GetWindowRect returns virtualised coordinates and the capture is a scaled,
# cropped picture of the window (which makes a correct layout look broken).
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Runtime.WindowsRuntime
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class UiSmokeWin32 {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
[void][UiSmokeWin32]::SetProcessDPIAware()
$null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Storage.StorageFile, Windows.Foundation, ContentType = WindowsRuntime]

$process = Start-Process -FilePath $Exe -ArgumentList "`"$Package`"" -PassThru
# Wait for a *laid out* window: the handle appears before the first frame, so
# poll until the rectangle has a sane size instead of sleeping a fixed time.
$hwnd = [IntPtr]::Zero
$rect = New-Object UiSmokeWin32+RECT
$deadline = (Get-Date).AddMilliseconds($WaitMs)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
    $process.Refresh()
    if ($process.HasExited) { throw "the app exited with code $($process.ExitCode)" }
    $hwnd = $process.MainWindowHandle
    if ($hwnd -eq [IntPtr]::Zero) { continue }
    [void][UiSmokeWin32]::GetWindowRect($hwnd, [ref]$rect)
    if (($rect.Right - $rect.Left) -gt 400 -and ($rect.Bottom - $rect.Top) -gt 300) { break }
}
if ($hwnd -eq [IntPtr]::Zero) { $process.Kill(); throw "no main window" }
Start-Sleep -Milliseconds 400   # let the first frame paint

$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
"window: ${width}x${height} title='$($process.MainWindowTitle)'"

$bitmap = New-Object System.Drawing.Bitmap($width, $height)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$hdc = $graphics.GetHdc()
[void][UiSmokeWin32]::PrintWindow($hwnd, $hdc, 2)   # PW_RENDERFULLCONTENT
$graphics.ReleaseHdc($hdc)
$graphics.Dispose()
$bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()

if (-not $process.HasExited) { $process.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 500 }
if (-not $process.HasExited) { $process.Kill() }

$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
})[0]
function Await($op, $type) {
    $task = $asTask.MakeGenericMethod($type).Invoke($null, @($op))
    $task.Wait(-1) | Out-Null
    $task.Result
}
$file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($Out)) ([Windows.Storage.StorageFile])
$stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
$decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
$software = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
$engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
if (-not $engine) { throw "no OCR engine available" }
$result = Await ($engine.RecognizeAsync($software)) ([Windows.Media.Ocr.OcrResult])

"recognised text:"
foreach ($line in $result.Lines) {
    $first = ($line.Words | Select-Object -First 1)
    $x = if ($first) { [int]$first.BoundingRect.X } else { -1 }
    $y = if ($first) { [int]$first.BoundingRect.Y } else { -1 }
    "  ({0,4},{1,4})  {2}" -f $x, $y, $line.Text
}
"capture: $Out"
