# APK Info — Revived

**Inspect Android installation packages and Alpine Linux packages on Windows.**

[![GitHub Release](https://img.shields.io/github/v/release/drclefcxk/apk-info-revived)](https://github.com/drclefcxk/apk-info-revived/releases/latest)
[![Windows x64](https://img.shields.io/badge/Windows-x64-0078D4)](https://github.com/drclefcxk/apk-info-revived/releases/latest)
[![GPL-3.0-only](https://img.shields.io/badge/license-GPL--3.0--only-blue)](LICENSE)

[简体中文](README.md) · **English**

[Download for Windows](https://github.com/drclefcxk/apk-info-revived/releases/latest) · [Supported formats](#supported-formats) · [Build from source](#build-from-source) · [Report an issue](https://github.com/drclefcxk/apk-info-revived/issues)

Android and Alpine Linux both use the `.apk` extension for different package formats. APK Info — Revived identifies the format from the file's contents and lets you inspect metadata, browse files, and export reports in one window. Inspection runs locally and requires no package installation, `adb`, `aapt`, or 7-Zip.

![APK Info in English showing an Android launcher icon, package metadata, and a contents breakdown](docs/screenshots/main-window-en.png)

## Quick start

1. Open [GitHub Releases](https://github.com/drclefcxk/apk-info-revived/releases/latest) and download `APK-Info-Revived-<version>-win64.zip` from **Assets**.
2. Extract the whole archive, keep the DLLs beside the executable, and run `ApkInfo.exe`.
3. Click **Open package…** to select a file, or pass its path when launching the app. Click a directory to expand the file tree, or use **Show all (N)** to view and copy a complete list.

Use **Copy all** to copy the report to the clipboard, or **Dump…** to save it as TXT or JSON. You can also export the Android launcher icon as a PNG. The interface follows the Windows display language by default; choose English or Simplified Chinese in **Settings**.

## Supported formats

| Format | What you can inspect |
| --- | --- |
| Android APK (`.apk`) | App name, package ID, version, SDK levels, ABIs, screen densities, locales, permissions, components, launcher icon, signature schemes, and file listing. |
| Android split archives (`.apks`, `.apkm`, `.xapk`) | App details from the base APK and split classifications by ABI, density, language, or feature. |
| Alpine Linux APK (`.apk`) | Package name, version, architecture, licence, maintainer, dependencies, install scripts, signatures, and file listing. Supports apk-tools 2.x gzip/tar and 3.x ADB formats. |

OpenWrt uses `apk` starting with version 25.12. The screenshot shows the BusyBox package from OpenWrt 25.12.6 (`aarch64_cortex-a53`, Alpine APK v3) open in APK Info:

![APK Info showing the version, architecture, and dependencies of the OpenWrt 25.12.6 BusyBox package](docs/screenshots/openwrt-25.12-apk.png)

The viewer reports signature information without cryptographically verifying it. Some Android icon effects, including gradients, clipping, and theme colours, may differ from what you see on a device.

More screenshots: [Alpine package](docs/screenshots/alpine-x86_64-zh.png) · [File tree](docs/screenshots/tree-zh.png) · [Android install page](docs/screenshots/install-page-zh.png) · [Settings](docs/screenshots/settings-zh.png)

## Command line

The release archive includes `apkinfo-cli.exe`, which prints package details and can save reports. Replace `sample.apk` with your package path:

```powershell
.\apkinfo-cli.exe .\sample.apk
.\apkinfo-cli.exe --dump-json .\report.json .\sample.apk
.\apkinfo-cli.exe --dump-txt .\report.txt .\sample.apk
```

Use `--icon-out <directory>` to save any resolved launcher icons to an existing directory. CLI output is in English for scripting.

## Optional features

### Install on a device

Open an Android package and click **Install** in the toolbar. The built-in installer uses `adb` from [Android SDK Platform-Tools](https://developer.android.com/tools/releases/platform-tools) to list devices, discover, pair with, and connect to devices using wireless debugging, and display the installation log. It uses `adb install` for a single APK and `adb install-multiple` for a split archive containing multiple APKs. Options include replacing an existing app, granting permissions, allowing downgrades, and allowing test packages.

You can also configure an external command for Android in **Settings**. Alpine installation always uses an external command; configure it for your target environment. Settings lists the available placeholders and previews the command for the open package.

Packages on a network share install too, including builds made inside WSL (`\\wsl.localhost\<distro>\...`): an external command runs in the temporary directory, and the built-in installer copies the package out of the share first.

<details>
<summary>Configuration file, adb lookup, and command placeholders</summary>

The app creates `apkinfo-config.json` next to `ApkInfo.exe` on its first run. The file stores language, Android version display names, and installation options. Changes saved in Settings take effect immediately. To customise the names displayed for Android SDK levels, edit `androidVersions` in the file.

The app looks for `adb` in this order: the path in Settings, `ADB`, `ANDROID_HOME`, `ANDROID_SDK_ROOT`, `PATH`, and common SDK directories.

External commands are started directly (no shell), in the package's own directory, so a UNC path - a WSL share such as `\\wsl.localhost\...` - installs fine. Common placeholders are `{path}` (full package path), `{file_name}` (file name), `{package_name}` (package name), `{version}` (version), `{device}` (selected device serial), and `{adb}` (adb path). Use `"{path}"` in your command to handle paths with spaces.

</details>

### Open from File Explorer

The release includes two PowerShell scripts. Run them from the extracted folder to register or remove file associations for `.apk`, `.apks`, `.apkm`, and `.xapk` for the current user:

```powershell
.\Register-FileAssociations.ps1 -DryRun
.\Register-FileAssociations.ps1
.\Unregister-FileAssociations.ps1
```

`-DryRun` previews the registration changes.

## Build from source

Install Visual Studio 2022 (the **Desktop development with C++** workload, including the Windows SDK), CMake 3.21 or later, [Slint C++ 1.13.1 for Windows MSVC x64](https://github.com/slint-ui/slint/releases/tag/v1.13.1), and [vcpkg](https://github.com/microsoft/vcpkg). Replace the example SDK paths with your own:

```powershell
git clone https://github.com/drclefcxk/apk-info-revived.git
Set-Location apk-info-revived

$env:SLINT_DIR = 'D:\SDK\slint'
$env:VCPKG_ROOT = 'D:\SDK\vcpkg'
& "$env:VCPKG_ROOT\vcpkg.exe" install zlib:x64-windows libwebp:x64-windows zstd:x64-windows
.\build.ps1
```

The default output directory is `build\Release\`, containing `ApkInfo.exe`, `apkinfo-cli.exe`, and their runtime DLLs. Point `SLINT_DIR` at the root of the Slint installation and `VCPKG_ROOT` at the bootstrapped vcpkg directory.

<details>
<summary>Editor setup, tests, and releases</summary>

- The [CMake presets](CMakePresets.json) include `vs2022` and `ninja`. To generate `compile_commands.json` for clangd, run `cmake --preset ninja` and then `cmake --build build-ninja` from a VS 2022 x64 developer shell. The presets use SDK paths under `D:/SDK`; override them with `-D` options as needed.
- The full parser suite needs Python 3, `aapt2.exe`, `7z.exe`, and sample packages. Place `*Lawnchair*.apk`, `*alpine-busybox*.apk` (a deflate-compressed Alpine v3 package), and an `.apks` archive containing `base.apk` in `example/` at the repository root. Set `APKINFO_TOOLS` to the directory containing the two tools, then run `.\tests\prepare_fixtures.ps1 -Root (Get-Location).Path` and `.\tests\run_tests.ps1`.
- The [release workflow](.github/workflows/release.yml) builds and packages the Windows x64 release and smoke-tests the CLI with a synthetic Alpine package.

</details>

## Contributing and licence

[Issues](https://github.com/drclefcxk/apk-info-revived/issues) and pull requests are welcome. When reporting a parser issue, include the app version, package format, steps to reproduce, and a sample or download link you can share publicly.

The project is licensed under [GPL-3.0-only](LICENSE). Thanks to the [Slint](https://slint.dev/), [stb_image](https://github.com/nothings/stb), [libwebp](https://chromium.googlesource.com/webm/libwebp), [zlib](https://zlib.net/), and [zstd](https://github.com/facebook/zstd) projects.
