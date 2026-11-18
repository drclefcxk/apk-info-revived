# APK Info — Revived

**简体中文** · [English](README.en.md)

一个用来查看**两种共用 `.apk` 扩展名、但毫无关系**的包格式的单文件信息阅读器：

| | Android 包 | Alpine 包 |
|---|---|---|
| 容器 | zip | ADB 块容器（apk-tools 3.x）或拼接的 gzip/tar 流（2.x） |
| 读取 | `AndroidManifest.xml`（二进制 XML）+ `resources.arsc` | `adb` 对象树 / `.PKGINFO` + tar 索引 |

文件类型由**内容**判定，从不看扩展名（`src/detect.cpp`）；两种格式各有独立解析器，只在结果界面这一层共享（`model/info_document.h`）。

界面为 **中文 / English** 双语，默认跟随系统语言（见[语言](#语言--localisation)）。

## 功能

* **格式判定靠内容。** Android 包与 Alpine 包共用 `.apk` 扩展名，程序按文件头判断，从不看文件名。
* **Android 包**：读取二进制 `AndroidManifest.xml` 与 `resources.arsc`，给出应用名、包名、版本名/号、包类型、ABI、屏幕密度、语言区域、组件统计、权限（含 `uses-permission`、`uses-permission-sdk-23`、自定义权限）、特性、签名方案、DEX/资源/资产/原生库体积等。
* **Alpine 包**：名称、版本、描述、架构、许可证、origin、维护者、地址、commit、体积、依赖 / provides / replaces / install_if / recommends、脚本、签名以及完整文件索引——v3 `adb` 二进制格式与经典 v2 tar 格式都支持。
* **`.apks` / `.apkm` / `.xapk` 分发包**：解出 `base.apk` 当普通包读取，各 `split_*.apk` 按配置类型（ABI / 密度 / 语言 / 功能）列出。
* **启动图标**：通过资源表解析 `android:icon`，支持自适应图标、图层为 WebP 的图标、矢量图层，以及只存在于配置分包里的图标；图标可一键导出为 PNG。
* **矢量图层自带光栅器**：解析 SVG 路径（含弧线与光滑曲线）、`<group>` 变换、填充/描边/透明度/填充规则，无需外部工具。
* **密度与语言**取自 `resources.arsc` 的配置项，资源名被混淆时同样可见。
* **文件清单是折叠树**，点目录逐层展开；条目数、解压后大小与各分区体积一并给出。
* **长列表不截断**：条目过多的区块只渲染前 8 条，其余进独立窗口，整份列表可全选复制。
* **报告导出**：整份信息可一键复制到剪贴板，或保存为 `.txt` / `.json`。
* **中大文件也不卡**：文档只解析一次，切换语言、展开树、复制报告都是重渲染，不重读文件。
* **界面双语**：中文 / English，默认跟随系统语言，可在工具栏下拉里切换并记住选择。
* **设置文件 `apkinfo-config.json`**：语言偏好与 Android 版本名对照表都在其中，改完即生效，无需重编译。

所有信息都从文件本身解析：**不用 aapt、不用 7z、不联网**。

## 截图

| | |
|---|---|
| ![主窗口](docs/screenshots/main-window-zh.png) | ![英文界面](docs/screenshots/main-window-en.png) |
| 主窗口：左侧图标与应用信息，右侧分区信息（默认跟随系统语言） | 同一界面切到 English，语言下拉随之显示 *Follow system* |
| ![长列表](docs/screenshots/rows-with-show-all-zh.png) | ![独立窗口](docs/screenshots/viewer-zh.png) |
| 长列表只渲染前 8 条，其后是 `显示全部 (403)` 按钮 | 按钮打开独立窗口，整份列表在一个可选中的文本框里 |
| ![文件树](docs/screenshots/tree-zh.png) | ![语言下拉](docs/screenshots/language-menu-zh.png) |
| 文件清单是折叠树（10 561 条目初始 26 行），点目录展开 | 语言下拉：跟随系统 / 中文 / English |
| ![Alpine noarch](docs/screenshots/alpine-noarch-zh.png) | ![Alpine x86_64](docs/screenshots/alpine-x86_64-zh.png) |
| Alpine 包（`noarch`，与架构无关；OpenWrt 的 LuCI 模块） | Alpine 包（`x86_64`，v2 tar 格式；官方 busybox） |
| ![Alpine aarch64](docs/screenshots/alpine-aarch64-zh.png) | ![Alpine 文件索引](docs/screenshots/alpine-tree-zh.png) |
| Alpine 包（`aarch64_cortex-a53`，v3 格式；OpenWrt 的 busybox） | Alpine 包的文件索引：折叠树，点开 `bin` 看到 busybox |

## 构建

依赖：

* Visual Studio 2022（MSVC 14.4x）与 Windows 10/11 SDK
* CMake ≥ 3.21
* 预编译 [Slint C++ 1.13](https://github.com/slint-ui/slint/releases)（`Slint-cpp-1.13.0-win64-MSVC-AMD64.exe`，安装到如 `D:\SDK\slint`）
* vcpkg 端口：`zlib`、`libwebp`、`zstd`

```powershell
.\build.ps1                 # 配置 + 构建 Release 到 .\build
.\build.ps1 -Clean          # 先清空构建目录
.\build.ps1 -Target apkinfo-cli
```

`build.ps1` 会自动查找 Slint 与 vcpkg（`$env:SLINT_DIR`、`$env:VCPKG_ROOT`，然后 `D:\SDK\slint` / `D:\SDK\vcpkg`），传入 `-DSlint_DIR=<slint>/lib/cmake/Slint` 与 vcpkg toolchain，并把运行库（`slint_cpp.dll`、`libwebp.dll`、`zlib1.dll`、`zstd.dll`）复制到可执行文件旁。

### Visual Studio Code

`CMakePresets.json` 提供两个预设：

* **vs2022** — Visual Studio 生成器，目录 `build/`（与 `build.ps1` 相同）
* **ninja** — Ninja 生成到 `build-ninja/`，供 CMake Tools 的默认生成器使用

CMake 构建目录与创建它的生成器绑定，若 CMake Tools 用 Ninja 建了 `build/`，`build.ps1` 会报 *"does not match the generator used previously"*。脚本会检测到这一点并自行重建目录；但仍建议在 VS Code 里选 `vs2022` 预设，让两种方式共用一个目录。命令行构建 Ninja 变体：`.\build.ps1 -Generator Ninja -BinDir build-ninja`。

产物：

* `build\Release\ApkInfo.exe` — 查看器（可在命令行给一个包路径，或用 *打开包…*）
* `build\Release\apkinfo-cli.exe` — 输出解析器找到的全部信息，用于脚本化回归（`--icon-out <目录>` 还会把图标写成 PNG）

## 使用

* **打开包**：命令行传入路径，或点工具栏 *打开包…*；可直接从资源管理器拖进窗口。
* **复制全部**：把整份报告（含文件清单）复制到剪贴板，按钮上会显示"已复制"。
* **转储…**：保存 `.txt` 或 `.json` 报告。
* **导出图标…**：把解析出的启动图标写成 PNG（未加载包或无图标时禁用）。
* **重新载入**：重读当前文件（内容被替换时用）。
* **显示全部 (N)**：长列表（权限、语言等）前 8 条按普通行显示，其余进独立窗口。
* 信息行里的值可以**按行选中复制**（每行一个只读输入框，选区不会跨行）。
* 文件清单是**折叠树**，点目录展开/收起。

### 文件关联

```powershell
.\Register-FileAssociations.ps1     # 为当前用户注册 .apk / .apks
.\Unregister-FileAssociations.ps1   # 撤销（带回滚备份）
```

资源管理器的文件类型图标只能按**扩展名**取，在打开文件之前就要给出，而 `.apk` 同时是 Android 与 Alpine，所以外壳图标保持"格式中立"，真正能区分的格式图标（绿包 / 青山 / 紫束）在**打开之后**切换窗口与任务栏图标。

## 配置文件

`apkinfo-config.json`，**始终位于 `ApkInfo.exe` 同目录**（不读写当前工作目录），存放所有"不该为改一项而重编译"的设置：

```json
{
  "_readme": ["……如何编辑……"],
  "language": "auto",
  "androidVersions": { "34": "14", "35": "15", "36": "16" }
}
```

* 启动时读取；文件不存在则写入默认值（带说明注释），保证永远有东西可改。
* `language`：`auto`（跟随 Windows 界面语言）/ `en` / `zh`。工具栏下拉菜单会写入这个字段，重启后保持。
* `androidVersions`：SDK 级别 → 显示名（`"36": "16 (Baklava)"` 可以，`"34": 14` 这种纯数字也行）。文件里没有的级别回落到内置表；同键后者覆盖前者；格式错误的条目跳过。
* **坏文件不会被覆盖**：保留默认值、在"备注"区提示问题，并原样保留文件让你修。
* 删除该文件即恢复默认。旧格式的 `android-versions.txt` 会在首次启动时自动迁移过来。

## 语言 / Localisation

工具栏的下拉菜单有三项：**跟随系统 / System**、中文、English，默认"跟随系统"（读 Windows 界面语言），选择写入 `apkinfo-config.json` 后持久化。

* 所有面向用户的文案都走 `TR("English source")`（字典在 `src/i18n.cpp`，169 条，测试会校验无重复键且译文与原文不同）。
* `--lang zh|en` 与 `APKINFO_LANG` 环境变量仍可**单次覆盖**（测试即用此方式）。
* 解析器输出的诊断信息（`Package::warnings`、`apk-tools` 层错误）有意保持英文：它们描述的是被读的文件，不是界面。

## 大包：只解析一次，渲染按需

现代包动辄上万条目、几百权限，界面靠四件事保持流畅：

* **解析一次，其余全部重渲染。** 读取器只产出*英文*（行标签、区块标题、枚举值都是字典键），翻译发生在 `src/main.cpp` 建行时。因此切语言只是重渲染缓存文档，不重读包：在 120 MB 的 Play 服务分发包上实测 **约 1 ms**。
* **文件清单是折叠树。** `src/model/tree_builder.cpp` 只保存名称、大小与节点结构：子节点连续存放、每个节点知道自己子树的规模，界面按此逐层展开——10 561 条目的 apk 初始只显示 26 行。
* **条目过多的区块以 `显示全部 (N)` 按钮收尾。** 前 8 条是普通行（与基本信息同样渲染），按钮打开**独立窗口**（`TextViewer`），正文是一个只读文本框，装完整列表：可选、`Ctrl+C`、自带滚动条。不截断任何内容，信息页也不会长得离谱。
* **SHA-256 不阻塞任何东西。** 哈希在后台线程算完再插入那一行，加载大包时界面已经可用；哈希只在真正需要时计算一次。

实测（Play 服务 120 MB 分发包，125% DPI）：常驻工作集约 **25 MB**，峰值约 151 MB（一次性触页），语言切换 0.6–1.0 ms。

## 图标

窗口、任务栏与文件关联图标都是自绘的，`tools/make_icons.py` 生成全部尺寸（16–256 px，9 档）与 `.ico`：

| 图标 | 用途 |
|---|---|
| `app.ico` | 程序本身 |
| `apk.ico` | Android 单包 |
| `apks.ico` | 分发包（`.apks` / `.apkm` / `.xapk`） |
| `android.ico` | 打开 Android 包后的窗口/任务栏图标 |
| `alpine.ico` | 打开 Alpine 包后的窗口/任务栏图标 |

重新生成：

```powershell
python tools\make_icons.py --preview   # 重绘全部图标（并打印 ASCII 预览）
```

### 图标渲染能力与缺口

覆盖：`<adaptive-icon>`、`<vector>`（`M/L/H/V/C/S/Q/T/A/Z` 含弧线与光滑曲线、`<group>` 平移/缩放/旋转/枢轴、填充/描边/透明度/`fillType`、`@color` 引用）、`<scale>`、`<bitmap>`、`<inset>`、`<nine-patch>`、`<layer-list>`、`<color>`，以及位图（PNG/JPEG/GIF/BMP/WebP）。

尚未覆盖（会在特定包上失真）：`<gradient>` 填充退化为起始色、`<clip-path>` 被忽略、`android:tint`/`tintMode` 未应用、`<animated-vector>`/`<rotate>`/`<level-list>`/`<selector>` 不解码、`?attr/...` 主题颜色引用无法解析、`android:roundIcon` 不优先、自适应图标未做 1.5× 放大 + 72dp 安全区裁剪。

## 测试

```powershell
cd tests
.\run_tests.ps1                 # 全部解析/单元测试
.\run_tests.ps1 -Module arsc    # 单个模块
.\ui_flow.ps1                   # 真实界面流程（OCR + 点击）
.\ui_flow.ps1 -Lang zh
.\ui_perf.ps1                   # 内存与语言切换耗时
.\ui_icons.ps1                  # 图标资源与关联检查
```

单元/解析测试（`run_tests.ps1`，共 9 个模块）：

| 模块 | 检查数 | 内容 |
|---|---|---|
| axml | 453 行对拍 | 与 aapt2 的 `xmltree` 输出逐行比对 |
| arsc | 53 | 资源表解析、42 276 个 id 交叉核对、密度/语言配置选择 |
| alpine | 104 | v3 `adb` 与 v2 tar 解析、依赖关系、签名块 |
| i18n | 349 | 字典完整性、无重复键、译文与原文不同 |
| tree | 40 | 树构建：子节点连续、子树规模、路径拼装 |
| report | 29 | 文本与 JSON 报告格式 |
| versions | 12 | 旧格式版本表解析、内置表回落 |
| vector | 22 | 路径解析、圆弧圆度与面积、填充规则、变换顺序 |
| config | 54 | JSON 解析、配置生成/手改/坏文件回退/保存/迁移 |

界面测试用 PrintWindow + Windows OCR，不做像素比对：`ui_flow.ps1` 30 项（两种语言各跑一遍），覆盖窗口居中、Close 按钮高度、树展开、复制/转储、行内选区、语言下拉切换并落盘。

**自动化点击安全**：脚本会用真实鼠标点绝对坐标，所以每次点击前都会把目标窗口置顶并校验它确实是前台窗口，否则直接抛异常中止——绝不会把点击发到别的窗口上。

测试夹具：`tests/fixtures/` 由 `tests/prepare_fixtures.ps1` 从 `example/` 里的样例包生成（重建夹具需要 `aapt2.exe` 与 `7z.exe`：放进 `<repo>\tools`，或用 `APKINFO_TOOLS` 指向存放它们的目录；该目录与 `example/` 都在 `.gitignore` 中，不入库。`example/` 放你自己的样例包即可。

## 目录结构

```
.                        仓库根目录
  src/
    detect.cpp            内容嗅探：Android / Alpine
    android/              axml、arsc、apk_reader、versions
    alpine/               v3 adb、v2 tar
    image/                stb_image/libwebp 解码、PNG 写出、矢量光栅器
    model/                info_document、tree_builder、report
    util/                 json、hash、clipboard、text
    i18n.cpp              中英字典
    config.cpp            配置文件
    app.cpp               Win32 细节（图标、剪贴板、保存对话框）
    main.cpp              Slint 界面与状态
  ui/main.slint           主窗口与 TextViewer
  assets/icons/           图标（svg/png/ico + app.rc）
  tools/                  make_icons.py、cli.cpp
  tests/                  单元测试、界面测试、夹具准备、截图脚本
  docs/screenshots/       截图
```

## 已知限制

* 签名**只展示不校验**（v1 JAR、v2/v3 APK Signature Scheme、Alpine 签名块都会读出来，但不做密码学验证）。
* Alpine v3 用 zstd 压缩的载荷需要 `zstd` 端口（默认已链接）；deflate 与未压缩容器始终可用。
* Alpine v3 没有 `triggers`、`replaces-priority`、`layer` 这类包字段：以解析器警告的形式报告，而不是静默丢弃。
* 图标渲染的缺口见上文"图标渲染能力与缺口"。

## 许可证

本项目的源代码采用 **GNU 通用公共许可证第 3 版（GPL-3.0-only）**，全文见 [LICENSE](LICENSE)：

> This program is free software: you can redistribute it and/or modify it under the terms
> of the GNU General Public License as published by the Free Software Foundation, **version
> 3 only**.

界面框架 [Slint](https://slint.dev/) 采用 GPLv3 / 商业双许可，本程序链接其 GPLv3 版本（`slint_cpp.dll`），因此**发布的可执行文件整体按 GPL-3.0 分发**，与上面的许可证一致。若需在非 GPL 条件下分发二进制，需要另行购买 Slint 商业许可。

## 鸣谢

* [Slint](https://slint.dev/)——界面框架。
* [stb_image](https://github.com/nothings/stb)（Sean Barrett）——PNG/JPEG/BMP/GIF 解码。
* [libwebp](https://chromium.googlesource.com/webm/libwebp)（Google）——WebP 解码。
* [zlib](https://zlib.net/)、[zstd](https://github.com/facebook/zstd)（Meta）——压缩格式支持。
* [aapt2](https://developer.android.com/studio)（Android SDK）——仅测试中用作交叉校验基准。
