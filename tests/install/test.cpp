// Tests for the install helpers and the adb output parsers.
//
// Nothing here needs a device: the placeholder expansion, the command defaults and
// the two parsers are pure, and the bundle preparation is exercised on a file given
// on the command line (skipped when absent).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "android/adb.h"
#include "install.h"
#include "model/info_document.h"
#include "util/process.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what) {
    ++checks;
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
}

apkinfo::InfoDocument android_document() {
    apkinfo::InfoDocument document;
    document.ok = true;
    document.kind = apkinfo::ContainerKind::AndroidApk;
    document.file_path = "C:\\roms\\My App\\myapp-1.2.3.apk";
    document.file_name = "myapp-1.2.3.apk";
    document.file_size = 123456;
    document.title = "My App";

    apkinfo::InfoSection package;
    package.title = "Package";
    package.add("Package name", "com.example.myapp");
    package.add("Version name", "1.2.3");
    package.add("Version code", "1002003");
    document.sections.push_back(package);

    apkinfo::InfoSection tech;
    tech.title = "Technical";
    tech.add("ABIs", "arm64-v8a, armeabi-v7a");
    document.sections.push_back(tech);
    return document;
}

void test_defaults() {
    const std::string android = apkinfo::install::default_android_command();
    check(android.find("{path}") != std::string::npos, "the Android default command uses {path}");
    check(android.find("install") != std::string::npos, "the Android default command installs");

    const std::string alpine = apkinfo::install::default_alpine_command();
    check(alpine.find("adb push") != std::string::npos, "the Alpine default pushes the package");
    check(alpine.find("apk add") != std::string::npos, "the Alpine default installs with apk-tools");
    check(alpine.find("{file_name}") != std::string::npos, "the Alpine default uses {file_name}");
}

void test_expand() {
    const apkinfo::install::Context context = apkinfo::install::make_context(android_document(), "C:\\adb.exe", "SERIAL1");

    check(context.file_name == "myapp-1.2.3.apk", "file_name");
    check(context.base_name == "myapp-1.2.3", "base_name");
    check(context.extension == "apk", "extension");
    check(context.dir == "C:\\roms\\My App", "dir");
    check(context.size == "123456", "size");
    check(context.kind == "android", "kind");
    check(context.app_name == "My App", "app_name");
    check(context.package_name == "com.example.myapp", "package_name");
    check(context.version == "1.2.3", "version");
    check(context.version_code == "1002003", "version_code");
    check(context.arch == "arm64-v8a, armeabi-v7a", "arch");

    const std::string expanded = apkinfo::install::expand(
        "adb -s {device} install \"{path}\"  # {app_name} {version} ({version_code}) {kind} {adb}", context);
    check(expanded.find("adb -s SERIAL1 install") == 0, "device is substituted");
    check(expanded.find("C:\\roms\\My App\\myapp-1.2.3.apk") != std::string::npos, "the path keeps its quotes");
    check(expanded.find("# My App 1.2.3 (1002003) android C:\\adb.exe") != std::string::npos,
          "several tokens in one line");

    // unknown tokens stay visible, so a typo in the settings is not silent
    check(apkinfo::install::expand("echo {nonsense}", context) == "echo {nonsense}", "unknown token is kept");
    // "{{" is a literal brace
    check(apkinfo::install::expand("echo {{path}}", context) == "echo {path}", "double braces are literal");
    // an unclosed brace is copied through
    check(apkinfo::install::expand("echo {path", context) == "echo {path", "unclosed brace is kept");
    check(apkinfo::install::expand("", context).empty(), "empty command stays empty");

    // {exe} / {exe_dir}: the program's own location, so a command can start a script
    // that ships next to ApkInfo.exe. Here the "program" is this test binary.
    check(!context.exe.empty() && context.exe.find("test_install") != std::string::npos,
          "exe is the running executable: " + context.exe);
    check(!context.exe_dir.empty(), "exe_dir is known: " + context.exe_dir);
    check(context.exe_dir.back() != '\\' && context.exe_dir.back() != '/', "exe_dir has no trailing separator");
    check(context.exe.rfind(context.exe_dir, 0) == 0, "exe_dir is the directory of exe");
    const std::string with_exe = apkinfo::install::expand("\"{exe_dir}\\scripts\\run.cmd\" {exe}", context);
    check(with_exe.find("test_install.exe") != std::string::npos, "both tokens expand: " + with_exe);
    check(with_exe.find("{exe") == std::string::npos, "no token is left behind: " + with_exe);

    // an Alpine document uses the Alpine command
    apkinfo::InfoDocument alpine = android_document();
    alpine.kind = apkinfo::ContainerKind::AlpinePackage;
    alpine.file_name = "busybox.apk";
    alpine.sections.clear();
    apkinfo::InfoSection overview;
    overview.add("Package name", "busybox");
    overview.add("Version", "1.37.0-r14");
    overview.add("Architecture", "x86_64");
    alpine.sections.push_back(overview);

    const apkinfo::install::Context alpine_context = apkinfo::install::make_context(alpine, "", "");
    check(alpine_context.kind == "alpine", "alpine kind");
    check(alpine_context.package_name == "busybox", "alpine package name");
    check(alpine_context.version == "1.37.0-r14", "alpine version comes from Version");
    check(alpine_context.arch == "x86_64", "alpine arch");

    apkinfo::install::Settings settings;
    settings.android_command = "ANDROID {file_name}";
    settings.alpine_command = "ALPINE {file_name}";
    check(apkinfo::install::command_for_package(settings, alpine, "", "") == "ALPINE busybox.apk",
          "Alpine packages use the Alpine command");
    check(apkinfo::install::command_for_package(settings, android_document(), "", "") == "ANDROID myapp-1.2.3.apk",
          "Android packages use the Android command");
}

void test_placeholders_documented() {
    const auto tokens = apkinfo::install::placeholder_tokens();
    check(tokens.size() >= 12, "the token list is documented");
    for (const auto& token : tokens) {
        check(token.first.front() == '{' && token.first.back() == '}', "token is braced: " + token.first);
        check(!token.second.empty(), "token has a meaning: " + token.first);
    }
}

void test_parse_devices() {
    using apkinfo::adb::parse_devices;
    const std::string output =
        "* daemon not running; starting now at tcp:5037\n"
        "* daemon started successfully\n"
        "List of devices attached\n"
        "emulator-5554          device product:sdk_gphone64_x86_64 model:sdk_gphone64_x86_64 device:emu64xa "
        "transport_id:1\n"
        "R58M12345              unauthorized transport_id:2\n"
        "adb-AWPQUT6618-NVLZo9  device product:MEP-AN00 model:MEP_AN00 device:HNMEP transport_id:3\n"
        "\n";

    const std::vector<apkinfo::adb::Device> devices = parse_devices(output);
    check(devices.size() == 3, "three devices parsed");
    if (devices.size() == 3) {
        check(devices[0].serial == "emulator-5554", "serial of the first device");
        check(devices[0].state == "device", "state of the first device");
        check(devices[0].model == "sdk_gphone64_x86_64", "model is parsed");
        check(devices[0].transport == "1", "transport id is parsed");
        check(devices[0].usable(), "an online device is usable");
        check(devices[1].state == "unauthorized", "unauthorized is reported");
        check(!devices[1].usable(), "an unauthorized device is not usable");
        check(devices[2].serial == "adb-AWPQUT6618-NVLZo9", "wireless serial");
    }
    check(parse_devices("").empty(), "empty output yields no devices");
    check(parse_devices("List of devices attached\n").empty(), "a bare header yields no devices");
}

void test_parse_mdns() {
    using apkinfo::adb::parse_mdns;
    const std::string output =
        "List of discovered mdns services\n"
        "adb-AWPQUT6618-NVLZo9\t_adb-tls-connect._tcp\t192.168.5.102:40039\n"
        "adb-AWPQUT6618-NVLZo9\t_adb._tcp\t192.168.5.102:7243\n";
    const auto services = parse_mdns(output);
    check(services.size() == 2, "two mdns services");
    if (services.size() == 2) {
        check(services[0].endpoint == "192.168.5.102:40039", "endpoint is parsed");
        check(services[0].type.find("tls-connect") != std::string::npos, "type is parsed");
    }

    // some adb versions separate the fields with spaces
    const auto spaced = parse_mdns("adb-X _adb-tls-pairing._tcp 10.0.0.5:37123\n");
    check(spaced.size() == 1 && spaced[0].endpoint == "10.0.0.5:37123", "space separated output works too");
    check(parse_mdns("").empty(), "empty mdns output yields nothing");
}

void test_split_steps() {
    using apkinfo::install::split_steps;

    const std::vector<std::string> two = split_steps("adb push \"C:\\a b\\x.apk\" /tmp/x && adb shell apk add /tmp/x");
    check(two.size() == 2, "two steps are split");
    if (two.size() == 2) {
        check(two[0] == "adb push \"C:\\a b\\x.apk\" /tmp/x", "the first step keeps its quotes");
        check(two[1] == "adb shell apk add /tmp/x", "the second step is trimmed");
    }

    check(split_steps("echo \"a && b\"").size() == 1, "a separator inside quotes is left alone");
    check(split_steps("a && b && c").size() == 3, "three steps");
    check(split_steps("  adb install x  &&  ").size() == 1, "empty steps are dropped");
    check(split_steps("").empty(), "an empty command has no steps");
    check(split_steps("adb install x").size() == 1, "a single command is one step");

    // The defaults really are chains
    check(split_steps(apkinfo::install::default_alpine_command()).size() == 2,
          "the Alpine default means push, then install");
    check(split_steps(apkinfo::install::default_android_command()).size() == 1,
          "the Android default is a single command");
}

void test_unc_working_directory() {
    // The point of dropping cmd.exe: CreateProcessW starts a child in a UNC directory.
    // Windows is asked for this binary's path, which is then started in \\localhost\C$\...
    // with --print-cwd, and the child prints the directory it ended up in.
    wchar_t self[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        std::printf("  (cannot resolve the test binary, UNC check skipped)\n");
        return;
    }
    const std::string share = "\\\\localhost\\C$";
    if (GetFileAttributesW(L"\\\\localhost\\C$\\Windows") == INVALID_FILE_ATTRIBUTES) {
        std::printf("  (no %s share here, UNC check skipped)\n", share.c_str());
        return;
    }

    char self_utf8[MAX_PATH * 2]{};
    const int self_length = WideCharToMultiByte(CP_UTF8, 0, self, -1, self_utf8, sizeof self_utf8, nullptr, nullptr);
    check(self_length > 0, "the test binary path converts");

    const std::string directory = share + "\\Windows";
    const apkinfo::process::Result result =
        apkinfo::process::run(apkinfo::process::build_command_line(self_utf8, {"--print-cwd"}), 30000, directory);
    check(result.started, "a child starts with a UNC working directory: " + result.error);
    check(result.exit_code == 0, "the child finished: " + result.output);
    check(result.output.find("localhost\\C$\\Windows") != std::string::npos,
          "the child reports the UNC directory it was started in (got: " + result.output + ")");
}

void test_prepare(const char* bundle) {
    // a plain package is installed as it is
    const apkinfo::install::Prepared single = apkinfo::install::prepare("C:\\a\\b.apk", apkinfo::ContainerKind::AndroidApk);
    check(single.ok() && single.files.size() == 1 && single.files[0] == "C:\\a\\b.apk",
          "a single apk is passed through");
    check(single.temp_dir.empty(), "nothing is extracted for a single apk");

    // an Alpine package cannot be installed by the built-in installer
    const apkinfo::install::Prepared alpine =
        apkinfo::install::prepare("C:\\a\\b.apk", apkinfo::ContainerKind::AlpinePackage);
    check(!alpine.ok() && !alpine.error.empty(), "Alpine packages are rejected by the built-in installer");

    // A bundle is extracted - but only when the runner happened to hand us one: it
    // passes the fixtures directory, which usually holds no .apks.
    if (!bundle || !*bundle) {
        std::printf("  (no bundle given, extraction not exercised)\n");
        return;
    }
    const std::string candidate = bundle;
    if (candidate.find(".apks") == std::string::npos && candidate.find(".apkm") == std::string::npos &&
        candidate.find(".xapk") == std::string::npos) {
        std::printf("  (%s is not a bundle, extraction not exercised)\n", candidate.c_str());
        return;
    }
    const apkinfo::install::Prepared extracted =
        apkinfo::install::prepare(bundle, apkinfo::ContainerKind::AndroidSplitBundle);
    check(extracted.ok(), "the bundle is extracted: " + extracted.error);
    check(extracted.files.size() >= 2, "the bundle yields base + splits");
    if (!extracted.files.empty()) {
        check(extracted.files[0].find("base.apk") != std::string::npos, "base.apk comes first");
    }
    apkinfo::install::cleanup(extracted);
}

}  // namespace

int main(int argc, char** argv) {
    // the UNC test starts this binary with --print-cwd and reads the directory back
    if (argc > 1 && std::string(argv[1]) == "--print-cwd") {
        char directory[MAX_PATH * 2]{};
        GetCurrentDirectoryA(static_cast<DWORD>(sizeof directory), directory);
        std::printf("%s\n", directory);
        return 0;
    }
    test_defaults();
    test_expand();
    test_placeholders_documented();
    test_parse_devices();
    test_parse_mdns();
    test_split_steps();
    test_unc_working_directory();
    test_prepare(argc > 1 ? argv[1] : nullptr);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
