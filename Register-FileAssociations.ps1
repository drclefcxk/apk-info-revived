# Registers ApkInfo as the handler for package files (*per user*, no admin needed)
# and gives Explorer the matching icons:
#
#   .apk              -> ApkInfo.apk  (blue document)
#   .apks/.apkm/.xapk -> ApkInfo.apks (purple stacked document)
#
# Why only two icons? Windows picks a file-type icon from the *extension*, so it
# cannot tell an Android .apk from an Alpine .apk before opening it. The app
# itself does know (it sniffs the content) and switches its window/taskbar icon
# accordingly; only split bundles get their own extension-level icon.
#
#   .\Register-FileAssociations.ps1                    # become the default handler
#   .\Register-FileAssociations.ps1 -OpenWithOnly      # only add "Open with" entries
#   .\Register-FileAssociations.ps1 -ReplaceUserChoice # also drop a previous
#                                                      # per-user "always open
#                                                      # with" record
#   .\Register-FileAssociations.ps1 -Exe <path>        # register another build
#   .\Register-FileAssociations.ps1 -DryRun            # show what would be registered
#
# The script works both from the source tree (ApkInfo.exe in build\Release, icons in
# assets\icons) and from the release archive (ApkInfo.exe and icons\ beside it), so the
# extracted release can register itself without a checkout.
#
# Windows 10/11 store the user's explicit "always open with" choice in a
# hash-protected UserChoice (or UserChoiceLatest) record that scripts cannot
# forge; while such a record exists it wins over everything below, which would
# leave the old icon in Explorer. -ReplaceUserChoice deletes those records (the
# ProgId is backed up first) so the registration above becomes effective. The
# user's old preference cannot be restored byte-for-byte - Windows will simply
# fall back to the ProgID registered here.
#
# The previous default handlers are saved under HKCU\Software\ApkInfo\Backup and
# restored by Unregister-FileAssociations.ps1.
param(
    [string]$Exe,
    [switch]$OpenWithOnly,
    [switch]$ReplaceUserChoice,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class ShellNotify {
    [DllImport("shell32.dll")] public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);
}
"@

if (-not $Exe) {
    foreach ($candidate in @((Join-Path $PSScriptRoot "ApkInfo.exe"),
                             (Join-Path $PSScriptRoot "build\Release\ApkInfo.exe"),
                             (Join-Path $PSScriptRoot "build-ninja\ApkInfo.exe"))) {
        if (Test-Path $candidate) { $Exe = $candidate; break }
    }
}
if (-not $Exe -or -not (Test-Path $Exe)) {
    throw "ApkInfo.exe not found next to this script - pass -Exe <path> or build it first (.\build.ps1)"
}
$Exe = (Resolve-Path $Exe).Path

# Icons are copied to a stable per-user location so the registration does not
# break when the build directory is cleaned.
$iconSource = $null
foreach ($candidate in @((Join-Path $PSScriptRoot "icons"),
                         (Join-Path $PSScriptRoot "assets\icons"))) {
    if (Test-Path (Join-Path $candidate "app.ico")) { $iconSource = $candidate; break }
}
if (-not $iconSource) {
    throw "no icons found - expected an 'icons' or 'assets\icons' folder next to this script"
}
$iconDir = Join-Path $env:LOCALAPPDATA "ApkInfo\icons"
New-Item -ItemType Directory -Force -Path $iconDir | Out-Null
foreach ($name in "app", "apk", "apks", "android", "alpine") {
    $from = Join-Path $iconSource "$name.ico"
    if (-not (Test-Path $from)) { throw "missing icon: $from (run python tools\make_icons.py)" }
    Copy-Item $from (Join-Path $iconDir "$name.ico") -Force
}

if ($DryRun) {
    Write-Host "ApkInfo.exe : $Exe"
    Write-Host "icons from  : $iconSource"
    Write-Host "icons to    : $iconDir"
    foreach ($ext in @(".apk", ".apks", ".apkm", ".xapk")) {
        Write-Host "would register $ext  (dry run, the registry was not touched)"
    }
    exit 0
}

$backupRoot = "HKCU:\Software\ApkInfo\Backup"
New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null

# ext -> progid, type name, icon file, launch command
$types = @(
    @{ Ext = ".apk";  ProgId = "ApkInfo.apk";  Icon = "apk.ico";
       Name = "Android / Alpine package";       Bundle = $false },
    @{ Ext = ".apks"; ProgId = "ApkInfo.apks"; Icon = "apks.ico";
       Name = "Android split package bundle";   Bundle = $true },
    @{ Ext = ".apkm"; ProgId = "ApkInfo.apks"; Icon = "apks.ico";
       Name = "Android split package bundle";   Bundle = $true },
    @{ Ext = ".xapk"; ProgId = "ApkInfo.apks"; Icon = "apks.ico";
       Name = "Android split package bundle";   Bundle = $true }
)

function Save-Previous([string]$keyPath, [string]$name) {
    if (-not (Test-Path $keyPath)) { return }
    $item = Get-Item $keyPath
    if ($item.GetValueNames() -contains $name) {
        # backup entry name is language- and path-independent: the extension itself
        $leaf = ($keyPath -split '\\')[-1]                 # e.g. ".apk"
        $backupName = "ext" + $leaf.Replace('.', '_')     # e.g. "ext_apk"
        Set-ItemProperty -Path "HKCU:\Software\ApkInfo\Backup" -Name $backupName -Value $item.GetValue($name)
        Write-Host "  saved previous $keyPath default = $($item.GetValue($name))"
    }
}

foreach ($type in $types) {
    $ext = $type.Ext
    $progId = $type.ProgId
    $icon = Join-Path $iconDir $type.Icon

    # --- the ProgID ---------------------------------------------------------
    $progKey = "HKCU:\Software\Classes\$progId"
    New-Item -Path "$progKey\DefaultIcon" -Force | Out-Null
    New-Item -Path "$progKey\shell\open\command" -Force | Out-Null
    Set-ItemProperty -Path $progKey -Name "(default)" -Value $type.Name
    Set-ItemProperty -Path $progKey -Name "FriendlyTypeName" -Value $type.Name
    Set-ItemProperty -Path "$progKey\DefaultIcon" -Name "(default)" -Value "`"$icon`""
    Set-ItemProperty -Path "$progKey\shell\open\command" -Name "(default)" -Value "`"$Exe`" `"%1`""

    # --- the extension ------------------------------------------------------
    $extKey = "HKCU:\Software\Classes\$ext"
    New-Item -Path "$extKey\OpenWithProgids" -Force | Out-Null
    Set-ItemProperty -Path "$extKey\OpenWithProgids" -Name $progId -Value "" -Type String
    if (-not $OpenWithOnly) {
        Save-Previous $extKey "(default)"
        Set-ItemProperty -Path $extKey -Name "(default)" -Value $progId
    }
    # --- a previous explicit user choice would shadow all of the above -------
    $fileExts = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$ext"
    foreach ($record in "UserChoice", "UserChoiceLatest") {
        $recordKey = "$fileExts\$record"
        if (-not (Test-Path $recordKey)) { continue }
        $previous = $null
        if ($record -eq "UserChoice") {
            $previous = (Get-ItemProperty $recordKey -ErrorAction SilentlyContinue).ProgId
        } else {
            $previous = (Get-ItemProperty "$recordKey\ProgId" -ErrorAction SilentlyContinue).ProgId
        }
        if ($ReplaceUserChoice) {
            Set-ItemProperty -Path "HKCU:\Software\ApkInfo\Backup" -Name ("userchoice" + $ext.Replace('.', '_')) -Value $previous
            Remove-Item -Recurse -Force $recordKey
            Write-Host ("  {0,-7} dropped {1} (was {2})" -f $ext, $record, $previous)
        } else {
            Write-Host ("  {0,-7} WARNING: {1} still points at '{2}'" -f $ext, $record, $previous) -ForegroundColor Yellow
            Write-Host ("            run again with -ReplaceUserChoice (or pick this app once in Explorer)" ) -ForegroundColor Yellow
        }
    }

    Write-Host ("  {0,-7} -> {1} ({2})" -f $ext, $progId, $type.Name)
}

# --- appear in "Open with" even before any association exists ----------------
$appsKey = "HKCU:\Software\Classes\Applications\ApkInfo.exe"
New-Item -Path "$appsKey\shell\open\command" -Force | Out-Null
Set-ItemProperty -Path $appsKey -Name "FriendlyAppName" -Value "APK Info - Revived"
Set-ItemProperty -Path "$appsKey\Shell\Open\command" -Name "(default)" -Value "`"$Exe`" `"%1`""
New-Item -Path "$appsKey\SupportedTypes" -Force | Out-Null
foreach ($type in $types) {
    Set-ItemProperty -Path "$appsKey\SupportedTypes" -Name $type.Ext -Value "" -Type String
}

[ShellNotify]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)  # SHCNE_ASSOCCHANGED

$mode = if ($OpenWithOnly) { "registered as an 'Open with' option" } else { "set as the default handler" }
Write-Host ""
Write-Host "Done - ApkInfo $mode for .apk / .apks / .apkm / .xapk." -ForegroundColor Green
Write-Host "Icons: $iconDir (apk.ico for packages, apks.ico for bundles)"
if (-not $OpenWithOnly) {
    Write-Host ""
    Write-Host "Note: if Windows still opens packages with another app, it has a per-file" -ForegroundColor Yellow
    Write-Host "'UserChoice' record that scripts may not overwrite. Right click a package ->" -ForegroundColor Yellow
    Write-Host "Open with -> Choose another app -> APK Info - Revived -> Always." -ForegroundColor Yellow
}
