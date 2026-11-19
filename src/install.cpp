#include "install.h"

#ifndef NOMINMAX
#define NOMINMAX  // windows.h's min/max macros would break std::min / std::max
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <system_error>

#include "container/file_map.h"
#include "container/zip_file.h"
#include "util/process.h"

namespace apkinfo::install {
namespace {

std::string to_string(uint64_t value) { return std::to_string(value); }

// Looks a value up by its English row label ("Package name", "Version name", ...).
std::string find_value(const InfoDocument& document, const std::string& label) {
    for (const InfoSection& section : document.sections) {
        for (const InfoRow& row : section.rows) {
            if (row.label == label && !row.value.empty() && row.value != "(none)") return row.value;
        }
    }
    return {};
}
std::string file_size_text(uint64_t bytes) { return std::to_string(bytes); }

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

// The program's own location: a command may want to run a script that ships next to
// ApkInfo.exe, which an absolute placeholder makes possible.
std::wstring module_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) return {};
        if (written < buffer.size()) {
            buffer.resize(written);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);   // long path: grow and ask again
    }
}

std::string unique_temp_dir() {
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    if (error) return {};
    static int counter = 0;
    const std::filesystem::path directory =
        base / ("apkinfo-install-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++counter));
    std::filesystem::create_directories(directory, error);
    if (error) return {};
    return directory.string();
}

std::string path_file_name(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

std::vector<std::pair<std::string, std::string>> placeholder_tokens() {
    return {
        {"{path}", "full path of the package"},
        {"{dir}", "the directory that contains it"},
        {"{file_name}", "file name with extension"},
        {"{base_name}", "file name without extension"},
        {"{extension}", "extension without the dot"},
        {"{size}", "file size in bytes"},
        {"{kind}", "package kind: android, apks or alpine"},
        {"{app_name}", "application label / Alpine package name"},
        {"{package_name}", "application id / Alpine pkgname"},
        {"{version}", "version name / pkgver"},
        {"{version_code}", "Android version code"},
        {"{arch}", "ABIs / Alpine architecture"},
        {"{device}", "selected device serial"},
        {"{adb}", "the adb executable in use"},
        {"{exe}", "full path of ApkInfo.exe"},
        {"{exe_dir}", "the directory that contains ApkInfo.exe"},
    };
}

std::string expand(const std::string& command, const Context& context) {
    const std::pair<const char*, const std::string*> values[] = {
        {"path", &context.path},
        {"dir", &context.dir},
        {"file_name", &context.file_name},
        {"base_name", &context.base_name},
        {"extension", &context.extension},
        {"size", &context.size},
        {"kind", &context.kind},
        {"app_name", &context.app_name},
        {"package_name", &context.package_name},
        {"version", &context.version},
        {"version_code", &context.version_code},
        {"arch", &context.arch},
        {"device", &context.device},
        {"adb", &context.adb},
        {"exe", &context.exe},
        {"exe_dir", &context.exe_dir},
    };

    std::string out;
    out.reserve(command.size() + 32);
    for (size_t i = 0; i < command.size(); ++i) {
        if (command[i] == '{' && i + 1 < command.size() && command[i + 1] == '{') {
            out.push_back('{');  // "{{" is a literal brace
            ++i;
            continue;
        }
        if (command[i] == '}' && i + 1 < command.size() && command[i + 1] == '}') {
            out.push_back('}');  // "}}" closes the escape
            ++i;
            continue;
        }
        if (command[i] != '{') {
            out.push_back(command[i]);
            continue;
        }
        const size_t close = command.find('}', i + 1);
        if (close == std::string::npos) {
            out.append(command, i, std::string::npos);
            break;
        }
        const std::string token = command.substr(i + 1, close - i - 1);
        bool replaced = false;
        for (const auto& entry : values) {
            if (token == entry.first) {
                out += *entry.second;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            // leave the token visible: a typo must not silently disappear
            out.append(command, i, close - i + 1);
        }
        i = close;
    }
    return out;
}

Context make_context(const InfoDocument& document, const std::string& adb_path, const std::string& device) {
    Context context;
    context.path = document.file_path.empty() ? std::string() : document.file_path;
    const size_t slash = context.path.find_last_of("\\/");
    context.dir = slash == std::string::npos ? std::string() : context.path.substr(0, slash);
    context.file_name = document.file_name.empty() ? path_file_name(context.path) : document.file_name;
    const size_t dot = context.file_name.find_last_of('.');
    context.base_name = dot == std::string::npos ? context.file_name : context.file_name.substr(0, dot);
    context.extension = dot == std::string::npos ? std::string() : context.file_name.substr(dot + 1);
    context.size = file_size_text(document.file_size);

    switch (document.kind) {
        case ContainerKind::AndroidApk:
            context.kind = "android";
            break;
        case ContainerKind::AndroidSplitBundle:
            context.kind = "apks";
            break;
        case ContainerKind::AlpinePackage:
            context.kind = "alpine";
            break;
        case ContainerKind::Unknown:
        default:
            context.kind = "unknown";
            break;
    }

    context.app_name = document.title;
    context.package_name = find_value(document, "Package name");
    context.version = find_value(document, "Version name");
    if (context.version.empty()) context.version = find_value(document, "Version");
    context.version_code = find_value(document, "Version code");
    context.arch = find_value(document, "Architecture");
    if (context.arch.empty()) context.arch = find_value(document, "ABIs");
    context.device = device;
    context.adb = adb_path;

    const std::wstring module = module_path();
    context.exe = to_utf8(module);
    const size_t exe_slash = context.exe.find_last_of("\\/");
    context.exe_dir = exe_slash == std::string::npos ? std::string() : context.exe.substr(0, exe_slash);
    return context;
}

Prepared prepare(const std::string& path, ContainerKind kind) {
    Prepared prepared;
    if (path.empty()) {
        prepared.error = "no package is open";
        return prepared;
    }
    if (kind == ContainerKind::AndroidApk) {
        prepared.files.push_back(path);
        return prepared;
    }
    if (kind != ContainerKind::AndroidSplitBundle) {
        prepared.error = "this is not an Android package";
        return prepared;
    }

    // A bundle: adb wants base.apk and the splits as separate files.
    std::string error;
    auto mapped = FileMap::open(path, &error);
    if (!mapped) {
        prepared.error = "cannot read the bundle: " + error;
        return prepared;
    }
    auto zip = ZipFile::open(mapped->bytes(), &error);
    if (!zip) {
        prepared.error = "cannot read the bundle: " + error;
        return prepared;
    }

    prepared.temp_dir = unique_temp_dir();
    if (prepared.temp_dir.empty()) {
        prepared.error = "cannot create a temporary directory";
        return prepared;
    }

    std::vector<const ZipFile::Entry*> members;
    for (const ZipFile::Entry& entry : zip->entries()) {
        if (entry.is_dir) continue;
        if (entry.name.size() < 4) continue;
        const std::string tail = entry.name.substr(entry.name.size() - 4);
        if (tail != ".apk" && tail != ".APK") continue;
        members.push_back(&entry);
    }
    if (members.empty()) {
        prepared.error = "the bundle contains no .apk members";
        cleanup(prepared);
        return prepared;
    }
    // base.apk first, then the splits, which is the order adb expects.
    std::stable_sort(members.begin(), members.end(), [](const ZipFile::Entry* lhs, const ZipFile::Entry* rhs) {
        const bool left_base = lhs->name == "base.apk" || lhs->name.find("/base.apk") != std::string::npos;
        const bool right_base = rhs->name == "base.apk" || rhs->name.find("/base.apk") != std::string::npos;
        if (left_base != right_base) return left_base;
        return lhs->name < rhs->name;
    });

    for (const ZipFile::Entry* entry : members) {
        const std::string name = path_file_name(entry->name);
        const std::wstring target = std::wstring(prepared.temp_dir.begin(), prepared.temp_dir.end()) + L"\\" +
                                    std::wstring(name.begin(), name.end());
        if (!zip->extract_to_file(*entry, target, &error)) {
            prepared.error = "cannot extract " + name + ": " + error;
            cleanup(prepared);
            return prepared;
        }
        prepared.files.push_back(prepared.temp_dir + "\\" + name);
    }
    return prepared;
}

void cleanup(const Prepared& prepared) {
    if (prepared.temp_dir.empty()) return;
    std::error_code error;
    std::filesystem::remove_all(std::filesystem::path(prepared.temp_dir), error);
}

Outcome run_builtin(const Settings& settings, const std::string& adb, const std::string& serial,
                    const std::string& path, ContainerKind kind) {
    Outcome outcome;
    if (adb.empty()) {
        outcome.output = "adb was not found - set its path in the settings (or put it on PATH)";
        return outcome;
    }
    if (serial.empty()) {
        outcome.output = "select a device first";
        return outcome;
    }

    const Prepared prepared = prepare(path, kind);
    if (!prepared.ok()) {
        outcome.output = prepared.error;
        return outcome;
    }
    const adb::Result result = adb::install(adb, serial, prepared.files, settings.options, 900000);
    cleanup(prepared);

    outcome.command = "adb -s " + serial + (prepared.files.size() > 1 ? " install-multiple" : " install");
    outcome.output = result.output;
    if (!result.started && !result.error.empty()) outcome.output = result.error;
    if (result.timed_out) outcome.output += "\n(the install timed out)";
    outcome.success = result.exit_code == 0 && result.output.find("Success") != std::string::npos;
    return outcome;
}

std::vector<std::string> split_steps(const std::string& command) {
    std::vector<std::string> steps;
    std::string current;
    bool in_quotes = false;
    for (size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (c == '"') {
            in_quotes = !in_quotes;
            current.push_back(c);
            continue;
        }
        if (!in_quotes && c == '&' && i + 1 < command.size() && command[i + 1] == '&') {
            steps.push_back(current);   // an empty step is dropped below
            current.clear();
            ++i;
            continue;
        }
        current.push_back(c);
    }
    steps.push_back(current);

    // trim and drop the empty pieces ("a && && b")
    std::vector<std::string> trimmed;
    for (std::string& step : steps) {
        const size_t first = step.find_first_not_of(" \t\r\n");
        const size_t last = step.find_last_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        trimmed.push_back(step.substr(first, last - first + 1));
    }
    return trimmed;
}

Outcome run_command(const std::string& command, const Context& context) {
    Outcome outcome;
    const std::string expanded = expand(command, context);
    outcome.command = expanded;
    if (expanded.empty()) {
        outcome.output = "no command is configured";
        return outcome;
    }
    // Each step runs in the package's own directory - including a UNC one such as
    // \\wsl.localhost\<distro>\..., which CreateProcessW starts in happily and cmd.exe
    // never could.
    const std::vector<std::string> steps = split_steps(expanded);
    const bool many = steps.size() > 1;
    std::string output;
    int exit_code = 0;
    bool timed_out = false;
    for (const std::string& step : steps) {
        const process::Result result = process::run(step, 900000, context.dir);
        if (many) output += (output.empty() ? "" : "\n") + std::string("> ") + step + "\n";
        if (!result.started && !result.error.empty()) {
            output += result.error;
            outcome.output = output;
            return outcome;
        }
        output += result.output;
        exit_code = result.exit_code;
        if (result.timed_out) {
            timed_out = true;
            break;
        }
        if (exit_code != 0) break;   // && semantics: stop at the first failure
    }
    outcome.output = output;
    if (timed_out) outcome.output += "\n(the command timed out)";
    outcome.success = exit_code == 0 && !timed_out;
    if (outcome.output.empty()) {
        outcome.output = outcome.success ? "done" : "the command failed with exit code " + std::to_string(exit_code);
    }
    return outcome;
}

std::string command_for_package(const Settings& settings, const InfoDocument& document, const std::string& adb_path,
                                const std::string& device) {
    const bool alpine = document.kind == ContainerKind::AlpinePackage;
    const std::string& command = alpine ? settings.alpine_command : settings.android_command;
    return expand(command, make_context(document, adb_path, device));
}

}  // namespace apkinfo::install
