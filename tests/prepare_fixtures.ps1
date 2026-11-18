# Prepares the fixtures used by the parser tests in this folder.
#
# Everything is derived from the packages in ..\..\example (nothing is checked
# in): the binary AndroidManifest.xml / resources.arsc / adaptive-icon XML are
# extracted from the sample packages, the Alpine v3 adb stream is decompressed,
# the synthetic Alpine v2 packages are generated with python, and the aapt2
# ground-truth dumps come from the real Android toolchain.
#
#   .\prepare_fixtures.ps1
param(
    [string]$Root = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$fixtures = Join-Path $here "fixtures"
$example = Join-Path $Root "example"
# aapt2 and 7z are needed only to build the ground-truth fixtures. Point APKINFO_TOOLS
# at a folder containing them, or drop them into <repo>\tools.
$tools = if ($env:APKINFO_TOOLS) { $env:APKINFO_TOOLS } else { Join-Path (Split-Path -Parent $PSScriptRoot) "tools" }
$sevenzip = Join-Path $tools "7z.exe"
$aapt2 = Join-Path $tools "aapt2.exe"
if (-not (Test-Path $aapt2) -or -not (Test-Path $sevenzip)) {
    throw "aapt2.exe / 7z.exe not found in $tools - set APKINFO_TOOLS to the folder that has them"
}

New-Item -ItemType Directory -Force -Path $fixtures, (Join-Path $fixtures "lawnchair"),
    (Join-Path $fixtures "gms"), (Join-Path $fixtures "tests\arsc") | Out-Null

# aapt2 writes UTF-8; redirect through a process so the file keeps its raw bytes
# (a PowerShell ">" would re-encode it as UTF-16).
function Save-Aapt2Dump {
    param([string[]]$Aapt2Args, [string]$OutFile)
    $process = Start-Process -FilePath $aapt2 -ArgumentList $Aapt2Args -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $OutFile
    if ($process.ExitCode -ne 0) { throw "aapt2 $($Aapt2Args -join ' ') failed" }
}

$lawnchair = Get-ChildItem $example -Filter "*Lawnchair*.apk" | Select-Object -First 1
$alpine = Get-ChildItem $example -Filter "*alpine-busybox*.apk" | Select-Object -First 1
$bundle = Get-ChildItem $example -Filter "*.apks" | Select-Object -First 1
if (-not $lawnchair -or -not $alpine -or -not $bundle) {
    throw "sample packages not found in $example"
}

Write-Host "== extracting from $($lawnchair.Name)" -ForegroundColor Cyan
& $sevenzip e $lawnchair.FullName -o"$fixtures\lawnchair" "AndroidManifest.xml" "resources.arsc" "res/s31.xml" -y | Out-Null
Save-Aapt2Dump -Aapt2Args @("dump", "xmltree", $lawnchair.FullName, "--file", "AndroidManifest.xml") `
    -OutFile (Join-Path $fixtures "gt_lawnchair_xmltree.txt")
Save-Aapt2Dump -Aapt2Args @("dump", "xmltree", $lawnchair.FullName, "--file", "res/s31.xml") `
    -OutFile (Join-Path $fixtures "gt_lawnchair_icon_xml.txt")
Save-Aapt2Dump -Aapt2Args @("dump", "resources", $lawnchair.FullName) `
    -OutFile (Join-Path $fixtures "tests\arsc\lwn_dump.txt")

Write-Host "== extracting base apk resources from $($bundle.Name)" -ForegroundColor Cyan
$temp = Join-Path $env:TEMP ("apkinfo_fixtures_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
& $sevenzip e $bundle.FullName -o"$temp" "base.apk" -y | Out-Null
$baseApk = Join-Path $temp "base.apk"
& $sevenzip e $baseApk -o"$fixtures\gms" "AndroidManifest.xml" "resources.arsc" -y | Out-Null
Save-Aapt2Dump -Aapt2Args @("dump", "resources", $baseApk) -OutFile (Join-Path $fixtures "tests\arsc\gms_dump.txt")
Remove-Item -Recurse -Force $temp

Write-Host "== copying the Alpine v3 sample" -ForegroundColor Cyan
Copy-Item $alpine.FullName (Join-Path $fixtures "alpine-busybox.apk") -Force

Write-Host "== decompressing the Alpine v3 adb stream" -ForegroundColor Cyan
$payload = Join-Path $fixtures "alpine_v3_payload.bin"
python -c "import sys, zlib; d=open(sys.argv[1],'rb').read(); o=zlib.decompressobj(-15); out=o.decompress(d[4:]); open(sys.argv[2],'wb').write(out); print('payload', len(out), 'bytes')" $alpine.FullName $payload

Write-Host "== generating synthetic Alpine v2 packages" -ForegroundColor Cyan
python (Join-Path $here "alpine\make_v2.py") | Out-Null
Get-ChildItem (Join-Path $here "alpine") -Filter "synthetic_*.apk" | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $fixtures $_.Name) -Force
}
# optional zstd containers (only exercised when the parser is built with zstd)
python (Join-Path $here "alpine\make_v3_zstd.py") 2>$null | Out-Null
Get-ChildItem (Join-Path $here "alpine") -Filter "synthetic_v3_zst*.apk" -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $fixtures $_.Name) -Force
}

Write-Host "== fixtures ready in $fixtures" -ForegroundColor Green
Get-ChildItem -Recurse -File $fixtures | ForEach-Object { "{0,10}  {1}" -f $_.Length, $_.FullName.Substring($fixtures.Length + 1) }
