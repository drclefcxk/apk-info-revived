# OCR an image with the Windows OCR engine and print the recognised lines.
# Used by ui_smoke.ps1 and for eyeballing captures; must run under Windows
# PowerShell 5.1 (WinRT type loading), so invoke it as:
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\ocr_text.ps1 -Image shot.png
param([Parameter(Mandatory = $true)][string]$Image)

Add-Type -AssemblyName System.Runtime.WindowsRuntime
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

$file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($Image)) ([Windows.Storage.StorageFile])
$stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
$decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
$bitmap = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
$engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
if (-not $engine) { throw "no OCR engine available" }
$result = Await ($engine.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])

$index = 0
foreach ($line in $result.Lines) {
    $first = ($line.Words | Select-Object -First 1)
    $x = if ($first) { [int]$first.BoundingRect.X } else { -1 }
    $y = if ($first) { [int]$first.BoundingRect.Y } else { -1 }
    "{0,3}: ({1,4},{2,4}) {3}" -f $index, $x, $y, $line.Text
    $index++
}
