# Undoes Register-FileAssociations.ps1: removes the ApkInfo ProgIDs, the "Open
# with" entry, and restores whatever was registered for these extensions before.
#
#   .\Unregister-FileAssociations.ps1

$ErrorActionPreference = "Stop"
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class ShellNotify2 {
    [DllImport("shell32.dll")] public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);
}
"@

$backupKey = "HKCU:\Software\ApkInfo\Backup"
$ourProgIds = @("ApkInfo.apk", "ApkInfo.apks")

function Get-DefaultValue([string]$path) {
    if (-not (Test-Path $path)) { return $null }
    $names = (Get-Item $path).GetValueNames()
    if ($names -notcontains "") { return $null }
    return (Get-Item $path).GetValue("")
}

foreach ($progId in $ourProgIds + @("ApkInfo.android", "ApkInfo.alpine")) {
    $progKey = "HKCU:\Software\Classes\$progId"
    if (Test-Path $progKey) {
        Remove-Item -Recurse -Force $progKey
        Write-Host "  removed ProgID $progId"
    }
}

foreach ($ext in ".apk", ".apks", ".apkm", ".xapk") {
    $extKey = "HKCU:\Software\Classes\$ext"
    if (-not (Test-Path $extKey)) { continue }

    $openWith = "$extKey\OpenWithProgids"
    if (Test-Path $openWith) {
        $names = (Get-Item $openWith).GetValueNames()
        foreach ($progId in $ourProgIds) {
            if ($names -contains $progId) { Remove-ItemProperty -Path $openWith -Name $progId }
        }
    }

    # restore the previous default handler if one was saved
    $backupName = "ext" + $ext.Replace('.', '_')
    $restored = $false
    if (Test-Path $backupKey) {
        $names = (Get-Item $backupKey).GetValueNames()
        if ($names -contains $backupName) {
            Set-ItemProperty -Path $extKey -Name "(default)" -Value (Get-Item $backupKey).GetValue($backupName)
            Write-Host "  $ext restored to $(Get-Item $backupKey).GetValue($backupName)"
            $restored = $true
        }
    }
    if (-not $restored) {
        $current = Get-DefaultValue $extKey
        if ($current -and ($ourProgIds -contains $current)) {
            Remove-ItemProperty -Path $extKey -Name "(default)" -ErrorAction SilentlyContinue
            Write-Host "  $ext default cleared"
        }
    }

    # drop the key entirely when nothing but leftovers remain
    $item = Get-Item $extKey
    $children = @(Get-ChildItem $extKey -ErrorAction SilentlyContinue)
    if ($item.ValueCount -eq 0 -and $children.Count -eq 0) {
        Remove-Item -Recurse -Force $extKey
        Write-Host "  removed empty $ext key"
    }
}

$appsKey = "HKCU:\Software\Classes\Applications\ApkInfo.exe"
if (Test-Path $appsKey) {
    Remove-Item -Recurse -Force $appsKey
    Write-Host "  removed Applications\ApkInfo.exe"
}

[ShellNotify2]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)

Write-Host ""
Write-Host "Done - ApkInfo file associations removed." -ForegroundColor Green
Write-Host "The copied icons in $env:LOCALAPPDATA\ApkInfo\icons can be deleted safely."
