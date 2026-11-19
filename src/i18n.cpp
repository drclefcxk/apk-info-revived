#include "i18n.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace apkinfo::i18n {
namespace {

struct Entry {
    const char* english;
    const char* chinese;
};

// Keys are the exact English strings used in the code, grouped by area.
// Duplicate keys are a bug: tests/i18n/test.cpp checks the table.
constexpr Entry kDictionary[] = {
    {"package kind: android, apks or alpine", "包类型：android / apks / alpine"},
    {"Install", "安装"},
    {"Settings", "设置"},
    {"Devices", "设备"},
    {"Refresh", "刷新"},
    {"Discover", "发现"},
    {"Wireless debugging", "无线调试"},
    {"Address (host:port)", "地址（host:端口）"},
    {"Pairing code", "配对码"},
    {"Connect", "连接"},
    {"Disconnect", "断开连接"},
    {"Pair", "配对"},
    {"Install options", "安装选项"},
    {"Replace existing (-r)", "替换安装 (-r)"},
    {"Grant all permissions (-g)", "授予全部权限 (-g)"},
    {"Allow downgrade (-d)", "允许降级 (-d)"},
    {"adb output", "adb 输出"},
    {"No device - connect one, or pair over Wi-Fi", "没有设备——请连接数据线，或通过无线配对"},
    {"6-digit code", "6 位配对码"},
    {"adb was not found - set its path in the settings", "找不到 adb——请在设置里指定它的路径"},
    {"no device selected", "尚未选择设备"},
    {"Selected", "已选择"},
    {"Language", "语言"},
    {"Interface language", "界面语言"},
    {"Install button - Android packages", "安装按钮——Android 包"},
    {"Install method", "安装方式"},
    {"Command line", "命令行"},
    {"Install button - Alpine packages", "安装按钮——Alpine 包"},
    {"adb path (empty = search PATH and the SDK)", "adb 路径（留空则搜索 PATH 与 SDK）"},
    {"Placeholders", "占位符"},
    {"Preview for the open package", "当前包的展开预览"},
    {"Save", "保存"},
    {"Cancel", "取消"},
    {"Alpine packages always use this command - the built-in installer is Android only.", "Alpine 包始终使用这条命令——内置安装器仅支持 Android。"},
    {"A split bundle is installed with adb install-multiple in the built-in mode.", "分发包在内置模式下通过 adb install-multiple 安装。"},
    {"(no package open)", "（未打开包）"},
    {"Installing", "正在安装"},
    {"Installed successfully.", "安装成功。"},
    {"select a device first", "请先选择设备"},
    {"no install command is configured", "未配置安装命令"},
    {"running the install command…", "正在执行安装命令…"},
    {"Install command output", "安装命令输出"},
    {"running…", "执行中…"},
    {"pairing", "配对用"},
    {"connect", "连接用"},
    {"no service discovered (wireless debugging must be on)", "未发现服务（需要设备已打开无线调试）"},
    {"enter the address first", "请先填写地址"},
    {"the address and the pairing code are both needed", "地址与配对码都需要填写"},
    {"full path of the package", "包的完整路径"},
    {"the directory that contains it", "包所在目录"},
    {"file name with extension", "带扩展名的文件名"},
    {"file name without extension", "不带扩展名的文件名"},
    {"extension without the dot", "扩展名（不含点）"},
    {"file size in bytes", "文件大小（字节）"},
    {"application label / Alpine package name", "应用名 / Alpine 包名"},
    {"application id / Alpine pkgname", "应用 id / Alpine pkgname"},
    {"version name / pkgver", "版本名 / pkgver"},
    {"Android version code", "Android 版本号"},
    {"ABIs / Alpine architecture", "ABI / Alpine 架构"},
    {"selected device serial", "所选设备的序列号"},
    {"the adb executable in use", "正在使用的 adb 程序"},
    {"lines", "行"},
    {"PNG image (*.png)", "PNG 图像 (*.png)"},
    {"Save the application icon", "保存应用图标"},
    {"Export icon…", "导出图标…"},
    {"(not computed)", "（未计算）"},
    {"none", "无"},
    {"Icon", "图标"},
    {"Subtitle", "副标题"},
    {"Title", "名称"},
    {"Detected", "识别为"},
    {"Format", "格式"},
    {"File", "文件"},
    {"Save the package information", "保存软件包信息"},
    {"JSON report (*.json)", "JSON 报告 (*.json)"},
    {"Text report (*.txt)", "文本报告 (*.txt)"},
    {"Saved", "已保存"},
    {"Copied", "已复制"},
    {"Hide", "收起"},
    {"Dump…", "转储…"},
    {"Copy all", "复制全部"},
    // ---- collapsed long lists (overlay panel)
    {"Show all", "显示全部"},
    {"Close", "关闭"},
    {"items", "项"},
    {"computing…", "计算中…"},
    {"Splits", "分包"},
    {"base", "基础包"},
    {"feature", "功能"},
    {"feature (install time)", "功能 (安装时)"},
    {"feature (on demand)", "功能 (按需下载)"},
    {"feature (fast follow)", "功能 (快速跟进)"},
    {"density", "密度"},
    {"locale", "语言区域"},
    {"ABI", "指令集"},
    // ---- icon notes
    {"adaptive icon (monochrome)", "自适应图标 (含单色层)"},
    // ---- window chrome / tabs
    {"Contents", "内容"},
    {"Contents summary", "内容概要"},
    {"Files", "文件"},
    {"Information", "信息"},
    {"Notes", "备注"},
    {"Open package…", "打开包…"},
    {"Reload", "重新载入"},
    {"no preview", "无预览"},
    {"bytes", "字节"},
    {"entries", "条目"},

    // ---- shared section titles
    {"Application", "应用"},
    {"Device support", "设备支持"},
    {"Package", "软件包"},
    {"Permissions", "权限"},
    {"Features", "特性"},
    {"Localized names", "本地化名称"},
    {"Scripts", "脚本"},
    {"Signatures", "签名"},

    // ---- Android: package / sdk / support / application
    {"Application name", "应用名称"},
    {"Assets", "资产文件"},
    {"Backup allowed", "允许备份"},
    {"Base APK", "基础 APK"},
    {"Compile SDK", "编译 SDK"},
    {"Components", "组件"},
    {"DEB files", "DEX 文件"},
    {"DEX files", "DEX 文件"},
    {"Debuggable", "可调试"},
    {"Entries", "条目数"},
    {"File name", "文件名"},
    {"File size", "文件大小"},
    {"Icon resource", "图标资源"},
    {"Install location", "安装位置"},
    {"Main activity", "主 Activity"},
    {"Minimum SDK", "最低 SDK"},
    {"Native libraries", "原生库"},
    {"Native platforms (ABIs)", "原生平台 (ABI)"},
    {"OpenGL ES", "OpenGL ES"},
    {"Package name", "包名"},
    {"Package type", "包类型"},
    {"Platform build version", "平台构建版本"},
    {"Requires split apks", "需要分包"},
    {"Resources", "资源"},
    {"SDK", "SDK"},
    {"SHA-256", "SHA-256"},
    {"Screen densities", "屏幕密度"},
    {"Signature schemes", "签名方案"},
    {"Supported screen sizes", "支持的屏幕尺寸"},
    {"Target SDK", "目标 SDK"},
    {"Theme", "主题"},
    {"Uncompressed size", "解压后大小"},
    {"Version code", "版本号"},
    {"Version name", "版本名称"},
    {"extractNativeLibs", "解压原生库"},

    // ---- Alpine: overview / relations
    {"Architecture", "架构"},
    {"Build time", "构建时间"},
    {"Dependencies", "依赖"},
    {"Description", "描述"},
    {"Directories", "目录"},
    {"Install if", "安装条件"},
    {"Installed size", "安装后大小"},
    {"License", "许可证"},
    {"Maintainer", "维护者"},
    {"Origin", "源包"},
    {"Package format", "包格式"},
    {"Package hash", "包哈希"},
    {"Package size (pkginfo)", "包大小 (pkginfo)"},
    {"Payload size", "载荷大小"},
    {"Project URL", "项目地址"},
    {"Provides", "提供"},
    {"Provider priority", "提供者优先级"},
    {"Recommends", "推荐"},
    {"Replaces", "替代"},
    {"Repository commit", "仓库提交"},
    {"Symlinks", "符号链接"},
    {"Tags", "标签"},
    {"Total file size", "文件总计"},
    {"Version", "版本"},

    // ---- plural-ish component counts and misc values
    {"Locales", "语言区域"},
    {"activities", "个 Activity"},
    {"aliases", "个别名"},
    {"services", "个服务"},
    {"receivers", "个接收器"},
    {"providers", "个提供者"},
    {"internal only", "仅内部存储"},
    {"prefer external", "优先外部存储"},
    {"auto", "自动"},
    {"(unnamed)", "(未命名)"},
    {"(sdk 23+)", "(SDK 23+)"},
    {"(optional)", "(可选)"},

    // ---- values
    {"(none)", "(无)"},
    {"Alpine package", "Alpine 软件包"},
    {"Android APK", "Android APK"},
    {"Single APK", "单个 APK"},
    {"Split APK bundle", "分包 APK 合集"},
    {"Split bundle", "分包合集"},
    {"Unknown", "未知"},
    {"no", "否"},
    {"none detected", "未检测到"},
    {"yes", "是"},

    // ---- split qualifiers

    // ---- icon notes
    {"adaptive icon", "自适应图标"},
    {"adaptive icon (layers could not be decoded)", "自适应图标（图层无法解码）"},
    {"could not be decoded", "无法解码"},
    {"from", "来自"},
    {", monochrome layer present", "，含单色图层"},

    // ---- status / banner
    {"Nothing loaded", "未加载文件"},
    {"Open an .apk, .apks, .apkm or Alpine .apk package", "请打开 .apk、.apks、.apkm 或 Alpine .apk 包"},
    {"Use “Open package…” or pass a path on the command line.", "使用“打开包…”或通过命令行传入路径。"},
    {"could not read the file", "无法读取文件"},
    {"showing", "仅显示"},

    // ---- file dialog
    {"Open a package", "打开软件包"},
    {"Packages (Android or Alpine)", "软件包（Android 或 Alpine）"},
    {"Android packages", "Android 软件包"},
    {"All files", "所有文件"},

    // ---- detection reasons
    {"ADB container (apk-tools 3.x)", "ADB 容器 (apk-tools 3.x)"},
    {"apk file(s)", "个 apk 文件"},
    {"bundle metadata without an apk payload", "只有分包元数据，没有 apk 载荷"},
    {"detected from the file content", "根据文件内容判断"},
    {"gzip stream that is not an Alpine package", "不是 Alpine 包的 gzip 流"},
    {"gzip stream with an apk control tarball (.PKGINFO), apk-tools 2.x",
     "含 apk 控制归档 (.PKGINFO) 的 gzip 流，apk-tools 2.x"},
    {"not a zip, not a gzip stream and not an ADB container", "既不是 zip，也不是 gzip 流，也不是 ADB 容器"},
    {"zip archive holding", "包含 apk 成员的 zip 归档"},
    {"zip archive with AndroidManifest.xml at the root", "根目录含 AndroidManifest.xml 的 zip 归档"},
    {"zip archive without AndroidManifest.xml and without apk members",
     "zip 归档，既无 AndroidManifest.xml 也无 apk 成员"},
    {"zip archive whose central directory cannot be read", "zip 归档的中心目录无法读取"},

    // ---- warnings / misc messages
    {"capped at", "已截断"},
    {"could not be turned into an image", "无法转换为图像"},
    {"launcher icon resource", "启动图标资源"},
    {"more", "更多"},
    {"resources.arsc could not be parsed", "resources.arsc 解析失败"},
    {"the package has no resources.arsc (resource references cannot be resolved)",
     "该包没有 resources.arsc（无法解析资源引用）"},
    {"unrecognised file", "无法识别的文件"},
    {"cannot open", "无法打开"},
    {"drawable", "可绘制资源"},
    {"cannot be rasterised", "无法栅格化"},
};

// What the user asked for (config file / dropdown).
Preference g_preference = Preference::Auto;
// A language forced for this run only (--lang or APKINFO_LANG).
Language g_language = Language::English;
bool g_language_resolved = false;
bool g_override = false;

const std::unordered_map<std::string_view, const char*>& dictionary() {
    static const std::unordered_map<std::string_view, const char*> map = [] {
        std::unordered_map<std::string_view, const char*> m;
        for (const Entry& entry : kDictionary) m.emplace(entry.english, entry.chinese);
        return m;
    }();
    return map;
}

Language language_from_windows() {
    LANGID lang = GetUserDefaultUILanguage();
    // PRIMARYLANGID == 0x04 is LANG_CHINESE (Hans and Hant both localise to zh).
    return PRIMARYLANGID(lang) == 0x04 ? Language::Chinese : Language::English;
}

Language language_from_env() {
    char buffer[32]{};
    DWORD length = GetEnvironmentVariableA("APKINFO_LANG", buffer, sizeof buffer);
    if (length == 0 || length >= sizeof buffer) return Language::English;
    Language parsed = Language::English;
    return from_code(buffer, &parsed) ? parsed : Language::English;
}

}  // namespace

Preference preference() { return g_preference; }

void set_preference(Preference preference) {
    g_preference = preference;
    // Picking a language in the UI supersedes a command-line override.
    g_override = false;
    g_language_resolved = false;
}

Language current() {
    if (g_override && g_language_resolved) return g_language;
    switch (g_preference) {
        case Preference::English:
            return Language::English;
        case Preference::Chinese:
            return Language::Chinese;
        case Preference::Auto:
        default:
            break;
    }
    if (!g_language_resolved) {
        const char* raw = std::getenv("APKINFO_LANG");
        g_language = raw && *raw ? language_from_env() : language_from_windows();
        g_language_resolved = true;
    }
    return g_language;
}

void set_language(Language language) {
    g_language = language;
    g_language_resolved = true;
    g_override = true;
}

void toggle_language() { set_language(current() == Language::Chinese ? Language::English : Language::Chinese); }

Language detect_from_system() { return language_from_windows(); }

const char* code(Language language) { return language == Language::Chinese ? "zh" : "en"; }

const char* display_name(Language language) { return language == Language::Chinese ? "中文" : "English"; }

bool from_code(std::string_view text, Language* out) {
    if (text.empty()) return false;
    // compare the primary subtag, case-insensitively
    const size_t end = text.find_first_of("-_");
    std::string primary(text.substr(0, end == std::string_view::npos ? text.size() : end));
    for (char& c : primary) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (primary == "en") {
        if (out) *out = Language::English;
        return true;
    }
    if (primary == "zh") {
        if (out) *out = Language::Chinese;
        return true;
    }
    return false;
}

const char* tr(const char* english) {
    if (!english || !*english) return english ? english : "";
    if (current() == Language::English) return english;
    const auto& map = dictionary();
    const auto it = map.find(std::string_view(english));
    return it == map.end() ? english : it->second;
}

std::span<const DictionaryEntry> dictionary_entries() {
    static_assert(sizeof(Entry) == sizeof(DictionaryEntry), "Entry layout must match the public view");
    const auto* base = reinterpret_cast<const DictionaryEntry*>(kDictionary);
    const size_t count = sizeof(kDictionary) / sizeof(kDictionary[0]);
    return std::span<const DictionaryEntry>(base, count);
}

}  // namespace apkinfo::i18n
