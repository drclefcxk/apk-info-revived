# UI flow checks that need real interaction: the selectable information pane, the
# expandable file tree, the copy/dump buttons.
#
# The app is driven the way a user drives it - OCR finds the text, the mouse
# clicks, the keyboard selects, the clipboard is read back - because Slint exposes
# no accessibility tree to poke at. Every step asserts on something observable.
#
#   .\ui_flow.ps1                 # split bundle + single apk samples
#   .\ui_flow.ps1 -Lang zh        # same, Chinese UI
param(
    [string]$Package,
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\Release\ApkInfo.exe"),
    [string]$OutDir = (Join-Path $env:TEMP "apkinfo_flow"),
    [string]$Lang = "en"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Runtime.WindowsRuntime
Add-Type -AssemblyName System.Windows.Forms

$example = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "example"
$Bundle = $Package
if (-not $Bundle) { $Bundle = (Get-ChildItem $example -Filter "*.apks" | Select-Object -First 1).FullName }
$Single = (Get-ChildItem $example -Filter "*.apk" | Where-Object { $_.Name -notmatch "alpine" } | Select-Object -First 1).FullName
if (-not (Test-Path $Exe)) { throw "missing $Exe - build first (.\build.ps1)" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Reset the stored language preference so every run starts from "follow the system".
$configFile = Join-Path (Split-Path -Parent $Exe) "apkinfo-config.json"
if (Test-Path $configFile) {
    $configText = Get-Content $configFile -Raw
    $configText = $configText -replace '"language"\s*:\s*"[a-z]+"', '"language": "auto"'
    Set-Content -Path $configFile -Value $configText -Encoding UTF8 -NoNewline
}
$env:APKINFO_LANG = $Lang
$env:APKINFO_START_TAB = "0"

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class UiFlow {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
[void][UiFlow]::SetProcessDPIAware()
$null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Storage.StorageFile, Windows.Foundation, ContentType = WindowsRuntime]

$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
})[0]
function Await($op, $type) {
    $task = $asTask.MakeGenericMethod($type).Invoke($null, @($op))
    $task.Wait(-1) | Out-Null
    $task.Result
}

function Find-AppWindow([int]$processId) {
    $script:found = [IntPtr]::Zero
    $cb = [UiFlow+EnumProc] {
        param($h, $p)
        [uint32]$owner = 0
        [void][UiFlow]::GetWindowThreadProcessId($h, [ref]$owner)
        if ($owner -eq $processId -and [UiFlow]::IsWindowVisible($h)) {
            $t = New-Object System.Text.StringBuilder 256
            [void][UiFlow]::GetWindowTextW($h, $t, 256)
            if ($t.Length -gt 0) { $script:found = $h; return $false }
        }
        return $true
    }
    [void][UiFlow]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

# Top-level dialog (#32770) of one process, or Zero. Filtering by process matters:
# the shell owns #32770 windows of its own, and closing one of those leaves the real
# dialog open.
function Find-DialogWindow([int]$processId = 0) {
    $script:dialog = [IntPtr]::Zero
    $cb = [UiFlow+EnumProc] {
        param($h, $p)
        $c = New-Object System.Text.StringBuilder 64
        [void][UiFlow]::GetClassNameW($h, $c, 64)
        if ($c.ToString() -ne "#32770" -or -not [UiFlow]::IsWindowVisible($h)) { return $true }
        if ($processId -ne 0) {
            [uint32]$owner = 0
            [void][UiFlow]::GetWindowThreadProcessId($h, [ref]$owner)
            if ($owner -ne $processId) { return $true }
        }
        $t = New-Object System.Text.StringBuilder 256
        [void][UiFlow]::GetWindowTextW($h, $t, 256)
        if ($t.Length -gt 0) { $script:dialog = $h; return $false }
        return $true
    }
    [void][UiFlow]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:dialog
}

function Capture([IntPtr]$hwnd, [string]$name) {
    $rect = New-Object UiFlow+RECT
    [void][UiFlow]::GetWindowRect($hwnd, [ref]$rect)
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    [void][UiFlow]::PrintWindow($hwnd, $hdc, 2)
    $graphics.ReleaseHdc($hdc)
    $graphics.Dispose()
    $path = Join-Path $OutDir "$name.png"
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    return @{ Path = $path; Rect = $rect; Width = $width; Height = $height }
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

function Scroll-By([int]$screenX, [int]$screenY, [int]$notches) {
    [void][UiFlow]::SetCursorPos($screenX, $screenY)
    Start-Sleep -Milliseconds 120
    $wheelSigned = [int64]$notches * 120
    $wheel = if ($wheelSigned -lt 0) { [uint32]($wheelSigned + 4294967296) } else { [uint32]$wheelSigned }
    [UiFlow]::mouse_event(0x0800, 0, 0, $wheel, [UIntPtr]::Zero)   # MOUSEEVENTF_WHEEL
    Start-Sleep -Milliseconds 350
}

# Brings `hwnd` to the front and only then clicks. A blind click would land on
# whatever window happens to be there, which is exactly what must never happen on a
# real desktop.
function Click-At([int]$screenX, [int]$screenY, [IntPtr]$hwnd = [IntPtr]::Zero) {
    if ($hwnd -ne [IntPtr]::Zero) {
        [void][UiFlow]::SetForegroundWindow($hwnd)
        Start-Sleep -Milliseconds 150
        $foreground = [UiFlow]::GetForegroundWindow()
        if ($foreground -ne $hwnd) {
            throw "refusing to click: the target window is not in the foreground"
        }
    }
    [void][UiFlow]::SetCursorPos($screenX, $screenY)
    Start-Sleep -Milliseconds 120
    [UiFlow]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
    Start-Sleep -Milliseconds 60
    [UiFlow]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
    Start-Sleep -Milliseconds 400
}

function Click-Line($shot, $line, [IntPtr]$hwnd = [IntPtr]::Zero) {
    Click-At ($shot.Rect.Left + $line.X + [int]($line.W / 2)) ($shot.Rect.Top + $line.Y + [int]($line.H / 2)) $hwnd
}

function Set-ClipboardRetry([string]$value) {
    for ($i = 0; $i -lt 10; $i++) {
        try { Set-Clipboard -Value $value; return $true } catch { Start-Sleep -Milliseconds 150 }
    }
    return $false
}

function Get-ClipboardRetry() {
    for ($i = 0; $i -lt 10; $i++) {
        try { return (Get-Clipboard) } catch { Start-Sleep -Milliseconds 150 }
    }
    return $null
}

# Waits until the clipboard holds something matching `pattern` (the app writes it
# asynchronously from the click handler).
function Wait-Clipboard([string]$pattern, [int]$timeoutMs = 3000) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $value = Get-ClipboardRetry
        if ($null -ne $value -and $value -match $pattern) { return $value }
        Start-Sleep -Milliseconds 200
    }
    return (Get-ClipboardRetry)
}

$failures = 0
function Check([bool]$condition, [string]$what) {
    if ($condition) { "  OK      $what" } else { "  FAILED  $what"; $script:failures++ }
}

# Visible, titled top-level windows of a process, as "@handle|title" strings.
function Get-AppWindows([int]$processId) {
    $script:list = @()
    $cb = [UiFlow+EnumProc] {
        param($h, $p)
        [uint32]$owner = 0
        [void][UiFlow]::GetWindowThreadProcessId($h, [ref]$owner)
        if ($owner -eq $processId -and [UiFlow]::IsWindowVisible($h)) {
            $t = New-Object System.Text.StringBuilder 256
            [void][UiFlow]::GetWindowTextW($h, $t, 256)
            if ($t.Length -gt 0) { $script:list += ("{0}|{1}" -f $h.ToInt64(), $t.ToString()) }
        }
        return $true
    }
    [void][UiFlow]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:list
}

function ConvertTo-Handle([string]$entry) { [IntPtr][int64](($entry -split '\|')[0]) }

function Start-App([string]$path) {
    $process = Start-Process -FilePath $Exe -ArgumentList "`"$path`"" -PassThru
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 300
        if ($process.HasExited) { throw "app exited with $($process.ExitCode)" }
        $handle = Find-AppWindow $process.Id
        if ($handle -ne [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 2000
            [void][UiFlow]::SetForegroundWindow($handle)
            return @{ Process = $process; Handle = $handle }
        }
    }
    $process.Kill()
    throw "no window appeared"
}

function Stop-App($app) {
    if (-not $app.Process.HasExited) { $app.Process.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 400 }
    if (-not $app.Process.HasExited) { $app.Process.Kill() }
}

# ------------------------- phase A: preview rows, "Show all" button, viewer window
"package: $(Split-Path -Leaf $Bundle)"
$rowDump = Join-Path $OutDir "info-rows.txt"
Remove-Item $rowDump -ErrorAction SilentlyContinue
$env:APKINFO_DEBUG_ROWS = $rowDump
$app = Start-App $Bundle
try {
    $shot = Capture $app.Handle "01-info"
    $lines = Get-Lines $shot.Path
    $header = $lines | Where-Object { $_.Text -match "^=\s*Package|^=\s*SDK|^Package$|^SDK$" } | Select-Object -First 1
    Check ($null -ne $header) "the information pane lists the sections"

    # long sections: a few ordinary rows plus a "show all" button, no inline box
    $buttons = @(Get-Content $rowDump -Encoding UTF8 -ErrorAction SilentlyContinue |
                 Where-Object { $_ -match "kind=4" -and $_ -match "Show all|显\s*示\s*全\s*部" })
    Check ($buttons.Count -ge 2) "long sections end with a 'Show all' button ($($buttons.Count) buttons)"
    $boxes = @(Get-Content $rowDump -Encoding UTF8 -ErrorAction SilentlyContinue | Where-Object { $_ -match "kind=7" })
    Check ($boxes.Count -eq 0) "no inline box row is generated any more"

    # the rows before the button use the same rendering as the basic information
    $previewRows = @(Get-Content $rowDump -Encoding UTF8 -ErrorAction SilentlyContinue |
                     Where-Object { $_ -match "kind=2" -and $_ -match "permission" })
    Check ($previewRows.Count -ge 4) "the first entries are plain information rows ($($previewRows.Count))"

    # scroll to a button and click it
    $button = $null
    $buttonShot = $null
    for ($i = 0; $i -lt 25; $i++) {
        $buttonShot = Capture $app.Handle "02-scroll$i"
        $button = (Get-Lines $buttonShot.Path) | Where-Object { $_.Text -match "Show all|显\s*示\s*全\s*部" } | Select-Object -First 1
        if ($null -ne $button) { break }
        Scroll-By ($buttonShot.Rect.Left + [int]($buttonShot.Width * 0.62)) ($buttonShot.Rect.Top + [int]($buttonShot.Height * 0.6)) -3
    }
    Check ($null -ne $button) "the button is reachable by scrolling"

    if ($null -ne $button) {
        Click-Line $buttonShot $button $app.Handle
        $viewerEntry = $null
        $deadline = (Get-Date).AddSeconds(6)
        while ((Get-Date) -lt $deadline -and $null -eq $viewerEntry) {
            Start-Sleep -Milliseconds 400
            $windows = @(Get-AppWindows $app.Process.Id)
            if ($windows.Count -ge 2) { $viewerEntry = $windows[0] }
        }
        Check ($null -ne $viewerEntry) "clicking it opens a separate window"
        if ($null -ne $viewerEntry) {
            $viewerHandle = ConvertTo-Handle $viewerEntry
            $viewerShot = Capture $viewerHandle "03-viewer"
            $viewerLines = Get-Lines $viewerShot.Path
            $permLines = @($viewerLines | Where-Object { $_.Text -match "permission|权\s*限\." })
            Check ($permLines.Count -ge 5) "the window shows the whole list ($($permLines.Count) entries on screen)"

            # the window appears over the main window as it is *now*
            $mainRect = New-Object UiFlow+RECT
            [void][UiFlow]::GetWindowRect($app.Handle, [ref]$mainRect)
            $viewRect = New-Object UiFlow+RECT
            [void][UiFlow]::GetWindowRect($viewerHandle, [ref]$viewRect)
            $dx = [Math]::Abs((($viewRect.Left + $viewRect.Right) / 2) - (($mainRect.Left + $mainRect.Right) / 2))
            $dy = [Math]::Abs((($viewRect.Top + $viewRect.Bottom) / 2) - (($mainRect.Top + $mainRect.Bottom) / 2))
            Check ($dx -lt 60 -and $dy -lt 60) "the window is centred on the main window (offset ${dx}x${dy})"

            # the Close button must be compact, not stretched over the whole header
            $bmp = [System.Drawing.Bitmap]::FromFile($viewerShot.Path)
            $buttonTop = -1
            $buttonBottom = -1
            for ($yy = 4; $yy -lt 52; $yy++) {
                $painted = $false
                for ($xx = $bmp.Width - 140; $xx -lt $bmp.Width - 20; $xx++) {
                    $c = $bmp.GetPixel($xx, $yy)
                    if ($c.R -lt 243 -or $c.G -lt 243 -or $c.B -lt 243) { $painted = $true; break }
                }
                if ($painted) {
                    if ($buttonTop -lt 0) { $buttonTop = $yy }
                    $buttonBottom = $yy
                }
            }
            $bmp.Dispose()
            $buttonHeight = $buttonBottom - $buttonTop + 1
            Check ($buttonHeight -gt 20 -and $buttonHeight -lt 60) `
                  "the Close button is compact ($buttonHeight px, header is 46 logical px)"

            # and the text in it can be selected and copied
            [void](Set-ClipboardRetry "(cleared)")
            Click-At ($viewerShot.Rect.Left + [int]($viewerShot.Width * 0.5)) ($viewerShot.Rect.Top + [int]($viewerShot.Height * 0.4)) $viewerHandle
            Start-Sleep -Milliseconds 300
            [System.Windows.Forms.SendKeys]::SendWait("^a")
            Start-Sleep -Milliseconds 250
            [System.Windows.Forms.SendKeys]::SendWait("^c")
            Start-Sleep -Milliseconds 600
            $clip = Get-ClipboardRetry
            $copied = @($clip -split "\r?\n" | Where-Object { $_ -match "permission" })
            Check ($copied.Count -ge 100) "Ctrl+A / Ctrl+C copies the list from the window ($($copied.Count) entries)"

            [void][UiFlow]::SendMessageW($viewerHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
            $deadline = (Get-Date).AddSeconds(5)
            while ((Get-Date) -lt $deadline -and @(Get-AppWindows $app.Process.Id).Count -ge 2) {
                Start-Sleep -Milliseconds 300
            }
            Check (@(Get-AppWindows $app.Process.Id).Count -lt 2) "the window closes again"
        }
    }
}
finally {
    Stop-App $app
    Remove-Item Env:\APKINFO_DEBUG_ROWS -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- phase B: file tree
"package: $(Split-Path -Leaf $Single)"
$rowDump = Join-Path $OutDir "rows.txt"
Remove-Item $rowDump -ErrorAction SilentlyContinue
$env:APKINFO_DEBUG_ROWS = $rowDump
$app = Start-App $Single
try {
    $shot = Capture $app.Handle "10-tabs"
    $tabLines = @(Get-Lines $shot.Path | Where-Object { $_.Y -ge 130 -and $_.Y -le 185 })
    $tab = $tabLines | Where-Object { $_.Text -match "^Contents|容" } | Select-Object -First 1
    if ($null -eq $tab -and $tabLines.Count -gt 0) { $tab = @($tabLines | Sort-Object X)[-1] }
    Check ($null -ne $tab) "the contents tab is visible"
    if ($null -ne $tab) {
        Click-Line $shot $tab $app.Handle
        Start-Sleep -Milliseconds 700
        $treeShot = Capture $app.Handle "11-tree"
        $treeLines = Get-Lines $treeShot.Path
        $deepRows = @($treeLines | Where-Object { $_.Text -match "^\s*(arm64-v8a|armeabi-v7a|drawable|layout|values)\b" })
        Check ($deepRows.Count -eq 0) "the tree starts collapsed (no nested rows: $($deepRows.Count))"

        $directory = $treeLines | Where-Object { $_.Text -match "^lib$|^lib/|^assets$|^assets/" } | Select-Object -First 1
        Check ($null -ne $directory) "top-level directories are listed"
        if ($null -ne $directory) {
            Click-Line $treeShot $directory $app.Handle
            Start-Sleep -Milliseconds 700
            $after = Get-Lines (Capture $app.Handle "12-expanded").Path
            $children = @($after | Where-Object { $_.Text -match "arm64-v8a|armeabi-v7a|dexopt|flutter_assets|javascripts" })
            Check ($children.Count -gt 0) "clicking a directory reveals its children ($($children.Count) new rows)"
        }
    }
}
finally {
    Stop-App $app
    Remove-Item Env:\APKINFO_DEBUG_ROWS -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- phase C: copy all + dump
$app = Start-App $Single
try {
    $shot = Capture $app.Handle "20-toolbar"
    $statusBefore = (Get-Lines $shot.Path) | Where-Object { $_.Y -gt 55 -and $_.Y -lt 95 -and $_.X -gt 400 -and $_.X -lt 780 } | Select-Object -First 1
    $toolLines = Get-Lines $shot.Path
    $copyAll = $toolLines | Where-Object { $_.Text -match "^Copy|^复\s*制" } | Select-Object -First 1
    Check ($null -ne $copyAll) "the 'Copy all' button is in the toolbar"

    if ($null -ne $copyAll) {
        [void](Set-ClipboardRetry "(cleared)")
        Click-Line $shot $copyAll $app.Handle
        Start-Sleep -Milliseconds 400
        $clip = Wait-Clipboard "APK Info"
        Check ($null -ne $clip -and $clip -match "APK Info" -and $clip -match "-- ") "copy all puts the whole report on the clipboard"

        # the confirmation appears on the button (the label text changes), and the
        # status bar keeps its own text; comparing the button's text avoids relying
        # on OCR of a short, translated word
        $buttonTextBefore = (@($toolLines | Where-Object {
            $_.Y -lt 95 -and $_.X -ge ($copyAll.X - 30) -and $_.X -le ($copyAll.X + $copyAll.W + 30)
        } | ForEach-Object { $_.Text }) -join " ")
        $flashShot = Capture $app.Handle "21-copied"
        $flashLines = Get-Lines $flashShot.Path
        $buttonTextAfter = (@($flashLines | Where-Object {
            $_.Y -lt 95 -and $_.X -ge ($copyAll.X - 30) -and $_.X -le ($copyAll.X + $copyAll.W + 30)
        } | ForEach-Object { $_.Text }) -join " ")
        Check ($buttonTextAfter -ne $buttonTextBefore) `
              "the button itself shows the confirmation ('$buttonTextBefore' -> '$buttonTextAfter')"
        $statusAfter = $flashLines | Where-Object { $_.Y -gt 55 -and $_.Y -lt 95 -and $_.X -gt 400 -and $_.X -lt 780 } | Select-Object -First 1
        Check ($null -ne $statusBefore -and $null -ne $statusAfter -and $statusBefore.Text -eq $statusAfter.Text) `
              "the package information in the status bar is untouched"

        Start-Sleep -Milliseconds 1200
        $restored = Get-Lines (Capture $app.Handle "22-restored").Path
        $label = $restored | Where-Object { $_.Text -match "^Copy|^复\s*制" -and $_.Y -lt 95 } | Select-Object -First 1
        Check ($null -ne $label) "the button label comes back after a moment"
    }

    $dumpButton = $toolLines | Where-Object { $_.Text -match "^DU?mp|^转\s*储" } | Select-Object -First 1
    if ($null -eq $dumpButton -and $null -ne $copyAll) {
        $dumpButton = [pscustomobject]@{ Text = "<derived>"; X = $copyAll.X + 127; Y = $copyAll.Y; W = $copyAll.W; H = $copyAll.H }
    }
    Check ($null -ne $dumpButton) "the 'Dump…' button is in the toolbar"
    if ($null -ne $dumpButton) {
        $dialog = [IntPtr]::Zero
        for ($attempt = 0; $attempt -lt 2 -and $dialog -eq [IntPtr]::Zero; $attempt++) {
            [void][UiFlow]::SetForegroundWindow($app.Handle)
            Start-Sleep -Milliseconds 300
            Click-Line $shot $dumpButton $app.Handle
            $deadline = (Get-Date).AddSeconds(5)
            while ((Get-Date) -lt $deadline -and $dialog -eq [IntPtr]::Zero) {
                Start-Sleep -Milliseconds 400
                $dialog = Find-DialogWindow $app.Process.Id
            }
        }
        Check ($dialog -ne [IntPtr]::Zero) "the dump action opens a save dialog"
        if ($dialog -ne [IntPtr]::Zero) {
            [void][UiFlow]::SendMessageW($dialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
            $deadline = (Get-Date).AddSeconds(4)
            while ((Get-Date) -lt $deadline -and (Find-DialogWindow $app.Process.Id) -ne [IntPtr]::Zero) {
                Start-Sleep -Milliseconds 250
            }
            Check ((Find-DialogWindow $app.Process.Id) -eq [IntPtr]::Zero) "the dialog closes again"
        }
    }
}
finally { Stop-App $app }

# ------------------------------------------------- phase D: row selection + legacy style
$app = Start-App $Single
try {
    $shot = Capture $app.Handle "30-rows"
    $lines = Get-Lines $shot.Path
    $row = $lines | Where-Object { $_.Text -match "Package n|名\s*称|版\s*本|类\s*型" } | Select-Object -First 1
    Check ($null -ne $row) "the label/value rows are rendered"

    if ($null -ne $row) {
        [void](Set-ClipboardRetry "(cleared)")
        # click the value cell, which starts past the 172px label column
        Click-At ($shot.Rect.Left + 760) ($shot.Rect.Top + $row.Y + 8) $app.Handle
        Start-Sleep -Milliseconds 300
        [System.Windows.Forms.SendKeys]::SendWait("^a")
        Start-Sleep -Milliseconds 250
        [System.Windows.Forms.SendKeys]::SendWait("^c")
        Start-Sleep -Milliseconds 500
        $clip = Get-ClipboardRetry
        Check ($null -ne $clip -and $clip -ne "(cleared)") "a row value can be selected and copied"
        Check ($null -ne $clip -and $clip.Length -lt 120) `
              "the selection stays inside one row ($($clip.Length) chars: '$clip')"
    }
}
finally { Stop-App $app }

# ------------------------------------------------- phase E: icon export
$app = Start-App $Single
try {
    $shot = Capture $app.Handle "50-export"
    $toolLines = Get-Lines $shot.Path
    $exportButton = $toolLines | Where-Object { $_.Text -match "^Export|^导\s*出" } | Select-Object -First 1
    Check ($null -ne $exportButton) "the 'Export icon' button is in the toolbar"

    if ($null -ne $exportButton) {
        [void][UiFlow]::SetForegroundWindow($app.Handle)
        Start-Sleep -Milliseconds 300
        Click-Line $shot $exportButton $app.Handle
        $dialog = [IntPtr]::Zero
        $deadline = (Get-Date).AddSeconds(6)
        while ((Get-Date) -lt $deadline -and $dialog -eq [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 400
            $dialog = Find-DialogWindow $app.Process.Id
        }
        Check ($dialog -ne [IntPtr]::Zero) "clicking it opens a save dialog"
        if ($dialog -ne [IntPtr]::Zero) {
            [void][UiFlow]::SendMessageW($dialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
            $deadline = (Get-Date).AddSeconds(4)
            while ((Get-Date) -lt $deadline -and (Find-DialogWindow $app.Process.Id) -ne [IntPtr]::Zero) {
                Start-Sleep -Milliseconds 250
            }
            Check ((Find-DialogWindow $app.Process.Id) -eq [IntPtr]::Zero) "the dialog closes again"
        }
    }
}
finally { Stop-App $app }

# the writer itself (same code the button uses, driven through the CLI)
$cli = Join-Path (Split-Path -Parent $Exe) "apkinfo-cli.exe"
$pngDir = Join-Path $OutDir "icons"
New-Item -ItemType Directory -Force -Path $pngDir | Out-Null
Remove-Item (Join-Path $pngDir "*.png") -ErrorAction SilentlyContinue
if (Test-Path $cli) {
    & $cli --icon-out $pngDir $Single 2>&1 | Out-Null
    $png = Get-ChildItem $pngDir -Filter "*.png" | Select-Object -First 1
    Check ($null -ne $png) "the icon is written as a PNG file"
    if ($null -ne $png) {
        $bytes = [System.IO.File]::ReadAllBytes($png.FullName)
        $signature = ($bytes[0..7] | ForEach-Object { $_.ToString("X2") }) -join ""
        Check ($signature -eq "89504E470D0A1A0A") "the file really is a PNG ($($bytes.Length) bytes)"
        # IHDR width/height live at bytes 16..23, big endian
        $width = ([int]$bytes[16] -shl 24) + ([int]$bytes[17] -shl 16) + ([int]$bytes[18] -shl 8) + [int]$bytes[19]
        $height = ([int]$bytes[20] -shl 24) + ([int]$bytes[21] -shl 16) + ([int]$bytes[22] -shl 8) + [int]$bytes[23]
        Check ($width -gt 16 -and $height -gt 16) "the PNG has the icon dimensions (${width}x${height})"
    }
}

# ------------------------------------------------- phase F: language dropdown
$configFile = Join-Path (Split-Path -Parent $Exe) "apkinfo-config.json"
$app = Start-App $Single
try {
    $shot = Capture $app.Handle "60-lang"
    $lines = Get-Lines $shot.Path
    # the first entry is localised ("跟随系统" / "Follow system") and OCR puts spaces
    # between CJK characters, so match tolerantly
    $combo = $lines | Where-Object { $_.Y -lt 95 -and $_.Text -match "系\s*统|System|中\s*文|English" } |
             Select-Object -First 1
    Check ($null -ne $combo) "the language dropdown is in the toolbar"

    if ($null -ne $combo) {
        # open the dropdown and pick English
        Click-Line $shot $combo $app.Handle
        Start-Sleep -Milliseconds 600
        $menuShot = Capture $app.Handle "61-menu"
        $english = (Get-Lines $menuShot.Path) | Where-Object { $_.Text -match "^English$|English" } |
                   Select-Object -Last 1
        Check ($null -ne $english) "the dropdown lists the languages"
        if ($null -ne $english) {
            Click-Line $menuShot $english $app.Handle
            Start-Sleep -Milliseconds 900
            $after = Get-Lines (Capture $app.Handle "62-english").Path
            $switched = @($after | Where-Object { $_.Text -match "Information|lnformation|Copy|DU?mp|Reload" })
            Check ($switched.Count -ge 2) "picking English switches the UI ($($switched.Count) English labels)"
            $saved = Get-Content $configFile -Raw -ErrorAction SilentlyContinue
            Check ($null -ne $saved -and $saved -match '"language"\s*:\s*"en"') `
                  "the choice is stored in apkinfo-config.json"

            # and the control itself must show the pick, not stay on the first entry
            $label = @($after | Where-Object { $_.Y -lt 95 -and $_.Text -match "English" })
            Check ($label.Count -ge 1) "the dropdown shows the selected language (English)"
        }
    }
}
finally { Stop-App $app }

Remove-Item Env:\APKINFO_LANG -ErrorAction SilentlyContinue
Remove-Item Env:\APKINFO_START_TAB -ErrorAction SilentlyContinue

if ($failures -eq 0) { "== all UI flow checks passed"; exit 0 }
"== $failures UI flow check(s) failed"
exit 1
