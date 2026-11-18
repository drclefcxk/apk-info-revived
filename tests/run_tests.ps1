# Builds and runs the parser regression tests.
#
#   .\run_tests.ps1                 # all modules
#   .\run_tests.ps1 -Module axml    # axml | arsc | alpine | i18n | tree
#   .\run_tests.ps1 -Compiler gcc   # msvc (default) or g++ from MSYS2
#
# Fixtures must exist first: .\prepare_fixtures.ps1
param(
    [ValidateSet("all", "axml", "arsc", "alpine", "i18n", "tree", "report", "versions", "vector", "config")] [string]$Module = "all",
    [ValidateSet("msvc", "gcc")] [string]$Compiler = "msvc",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$src = Join-Path (Split-Path -Parent $here) "src"
$fixtures = Join-Path $here "fixtures"
$out = Join-Path $here "bin"
New-Item -ItemType Directory -Force -Path $out | Out-Null

if (-not (Test-Path (Join-Path $fixtures "lawnchair\resources.arsc"))) {
    throw "fixtures missing - run .\prepare_fixtures.ps1 first"
}

# module -> include directory (under src/) and translation units (as a single
# amalgamated source is not used, each module is one .cpp)
$modules = [ordered]@{
    axml   = @{ include = "android"; sources = @("android\axml.cpp") }
    arsc   = @{ include = "android"; sources = @("android\arsc.cpp") }
    alpine = @{ include = "alpine";  sources = @("alpine\alpine.cpp") }
    i18n   = @{ include = ".";       sources = @("i18n.cpp") }
    tree   = @{ include = ".";       sources = @("model\tree_builder.cpp") }
    report = @{ include = ".";       sources = @("model\report.cpp", "model\info_document.cpp", "util\text.cpp", "i18n.cpp") }
    versions = @{ include = ".";     sources = @("android\versions.cpp", "util\text.cpp", "i18n.cpp", "config.cpp", "util\json.cpp") }
    vector = @{ include = ".";       sources = @("image\vector.cpp", "model\info_document.cpp") }
    config = @{ include = ".";       sources = @("config.cpp", "util\json.cpp", "util\text.cpp", "i18n.cpp", "android\versions.cpp") }
}

function Build-Test {
    param([string]$Name, [string]$TestSrc, [string]$Exe, [string[]]$Sources, [string]$IncludeDir)

    if ($Compiler -eq "gcc") {
        $gxx = "D:\SDK\msys64\ucrt64\bin\g++.exe"
        $arguments = @("-std=c++20", "-O2", "-Wall", "-Wextra", "-I", $IncludeDir, $TestSrc) + $Sources +
                     @("-o", $Exe, "-lz")
        & $gxx @arguments 2>&1 | Out-Null
    } else {
        $vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        # zlib comes from vcpkg (the app links the same one)
        $vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "D:\SDK\vcpkg" }
        $vcpkgInc = Join-Path $vcpkgRoot "installed\x64-windows\include"
        $vcpkgLib = Join-Path $vcpkgRoot "installed\x64-windows\lib"
        $quoted = ($Sources | ForEach-Object { "`"$_`"" }) -join " "
        $line = "call `"$vcvars`" >nul 2>&1 && cl /nologo /std:c++20 /EHsc /O2 /W3 /utf-8 " +
                "/I `"$IncludeDir`" /I `"$vcpkgInc`" `"$TestSrc`" $quoted /Fe:`"$Exe`" /Fo:`"$out\\`" " +
                "/link zlib.lib /LIBPATH:`"$vcpkgLib`""
        cmd /c $line 2>&1 | Out-Null
    }
    return (Test-Path $Exe)
}

$failures = 0
foreach ($name in $modules.Keys) {
    if ($Module -ne "all" -and $Module -ne $name) { continue }
    $testSrc = Join-Path $here "$name\test.cpp"
    $exe = Join-Path $out "test_$name.exe"
    $includeDir = Join-Path $src $modules[$name].include
    $moduleSources = $modules[$name].sources | ForEach-Object { Join-Path $src $_ }

    if (-not $SkipBuild) {
        Write-Host "== building $name" -ForegroundColor Cyan
        if (-not (Build-Test -Name $name -TestSrc $testSrc -Exe $exe -Sources $moduleSources -IncludeDir $includeDir)) {
            Write-Host "BUILD FAILED: $name" -ForegroundColor Red
            $failures++
            continue
        }
    }

    Write-Host "== running $name" -ForegroundColor Cyan
    if ($name -eq "axml") {
        & $exe cmp (Join-Path $fixtures "lawnchair\AndroidManifest.xml") (Join-Path $fixtures "gt_lawnchair_xmltree.txt")
        $a = $LASTEXITCODE
        & $exe cmp (Join-Path $fixtures "lawnchair\s31.xml") (Join-Path $fixtures "gt_lawnchair_icon_xml.txt")
        $b = $LASTEXITCODE
        & $exe check icon (Join-Path $fixtures "lawnchair\s31.xml")
        $c = $LASTEXITCODE
        if ($a -ne 0 -or $b -ne 0 -or $c -ne 0) { $failures++ }
    } else {
        & $exe $fixtures
        if ($LASTEXITCODE -ne 0) { $failures++ }
    }
}

if ($failures -eq 0) {
    Write-Host "== all parser tests passed" -ForegroundColor Green
    exit 0
}
Write-Host "== $failures module(s) failed" -ForegroundColor Red
exit 1
