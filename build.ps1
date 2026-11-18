# Builds ApkInfo (GUI) and apkinfo-cli (console test driver).
#
#   .\build.ps1                     # Release build into .\build (Visual Studio generator)
#   .\build.ps1 -Clean              # wipe the build directory first
#   .\build.ps1 -Target apkinfo-cli
#   .\build.ps1 -Generator Ninja -BinDir build-ninja
#
# A CMake build directory is tied to the generator that created it. Visual Studio
# Code's CMake Tools defaults to Ninja and will happily create .\build with it,
# which then makes this script fail with "does not match the generator used
# previously". The script detects that and recreates the directory instead of
# making you delete it by hand.
param(
    [string]$Configuration = "Release",
    [switch]$Clean,
    [string]$Target = "",
    [string]$Generator = "Visual Studio 17 2022",
    [string]$BinDir = "build"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root $BinDir

if ($Clean -and (Test-Path $build)) {
    Remove-Item -Recurse -Force $build
}

# --- stale cache guard -------------------------------------------------------
$cacheFile = Join-Path $build "CMakeCache.txt"
if (Test-Path $cacheFile) {
    $cachedGenerator = $null
    Select-String -Path $cacheFile -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$' | ForEach-Object {
        $cachedGenerator = $_.Matches[0].Groups[1].Value.Trim()
    }
    if ($cachedGenerator -and ($cachedGenerator -ne $Generator)) {
        Write-Host "== build dir was configured with '$cachedGenerator', recreating it for '$Generator'" -ForegroundColor Yellow
        Remove-Item -Recurse -Force $build
    }
}

# --- toolchain discovery -----------------------------------------------------
$vcpkg = $env:VCPKG_ROOT
if (-not $vcpkg -or -not (Test-Path (Join-Path $vcpkg "scripts/buildsystems/vcpkg.cmake"))) {
    $vcpkg = $null
    foreach ($candidate in @("D:/SDK/vcpkg", "C:/vcpkg", "$env:USERPROFILE/vcpkg")) {
        if (Test-Path (Join-Path $candidate "scripts/buildsystems/vcpkg.cmake")) {
            $vcpkg = $candidate
            break
        }
    }
}
$slintDir = $env:SLINT_DIR
if (-not $slintDir -or -not (Test-Path (Join-Path $slintDir "lib/cmake/Slint/SlintConfig.cmake"))) {
    $slintDir = $null
    foreach ($candidate in @("D:/SDK/slint", "C:/slint", "$env:USERPROFILE/slint")) {
        if (Test-Path (Join-Path $candidate "lib/cmake/Slint/SlintConfig.cmake")) {
            $slintDir = $candidate
            break
        }
    }
}
if (-not $vcpkg) { Write-Host "warning: vcpkg not found (zlib/libwebp/zstd must be discoverable)" -ForegroundColor Yellow }
if (-not $slintDir) { Write-Host "warning: Slint C++ not found (set SLINT_DIR)" -ForegroundColor Yellow }

$configure = @("-S", $root, "-B", $build, "-G", $Generator)
if ($Generator -like "Visual Studio*") { $configure += @("-A", "x64") }
if ($vcpkg) { $configure += "-DCMAKE_TOOLCHAIN_FILE=$vcpkg/scripts/buildsystems/vcpkg.cmake" }
if ($slintDir) { $configure += "-DSlint_DIR=$slintDir/lib/cmake/Slint" }
$configure += "-DCMAKE_BUILD_TYPE=$Configuration"

Write-Host "== configure ($Generator) ==" -ForegroundColor Cyan
& cmake @configure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$buildArgs = @("--build", $build, "--config", $Configuration)
if ($Target) { $buildArgs += @("--target", $Target) }

Write-Host "== build ==" -ForegroundColor Cyan
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Single-config generators (Ninja) put the binaries straight into build/,
# multi-config ones (Visual Studio) into build/<Configuration>/.
$outDir = if (Test-Path (Join-Path $build $Configuration)) { Join-Path $build $Configuration } else { $build }
Write-Host "== done: $outDir ==" -ForegroundColor Green
