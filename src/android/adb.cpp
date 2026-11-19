// adb access for the installer page: run adb, list devices, install, connect, pair.
//
// There is no Android SDK dependency here - the module only starts a child process
// and parses what adb prints. `adb` is looked up in the configured path, $ADB/PATH
// and the usual SDK locations, so a user with platform-tools anywhere can use it.
//
// Everything here blocks (it waits for a child process), so the UI calls it from a
// worker thread and hands the result back through the event loop.
#include "android/adb.h"

#ifndef NOMINMAX
#define NOMINMAX  // windows.h's min/max macros would break std::min / std::max
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <optional>

#include "util/process.h"

namespace apkinfo::adb {
namespace {

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    wide.resize(static_cast<size_t>(length) - 1);
    return wide;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), length, nullptr, nullptr);
    out.resize(static_cast<size_t>(length) - 1);
    return out;
}

bool is_executable(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool is_directory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::optional<std::wstring> environment(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return std::nullopt;
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0) return std::nullopt;
    value.resize(written);
    return value;
}

// $ADB / $ANDROID_HOME / $ANDROID_SDK_ROOT, then PATH, then the layout every SDK has.
std::vector<std::wstring> adb_candidates() {
    std::vector<std::wstring> out;
    for (const wchar_t* name : {L"ADB", L"ANDROID_HOME", L"ANDROID_SDK_ROOT"}) {
        const auto value = environment(name);
        if (!value || value->empty()) continue;
        if (is_directory(*value)) {
            out.push_back(*value + L"\\platform-tools\\adb.exe");
            out.push_back(*value + L"\\adb.exe");
        } else {
            out.push_back(*value);
        }
    }
    if (const auto path = environment(L"PATH"); path && !path->empty()) {
        size_t start = 0;
        while (start <= path->size()) {
            const size_t end = path->find(L';', start);
            const std::wstring entry = path->substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!entry.empty()) {
                out.push_back(entry + (entry.back() == L'\\' ? L"" : L"\\") + L"adb.exe");
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
    }
    for (const wchar_t* root : {L"LOCALAPPDATA", L"USERPROFILE"}) {
        if (const auto value = environment(root); value && !value->empty()) {
            out.push_back(*value + L"\\Android\\Sdk\\platform-tools\\adb.exe");
            out.push_back(*value + L"\\AppData\\Local\\Android\\Sdk\\platform-tools\\adb.exe");
        }
    }
    for (const wchar_t* fixed : {L"C:\\Android\\platform-tools\\adb.exe", L"C:\\platform-tools\\adb.exe",
                                 L"D:\\Android\\platform-tools\\adb.exe", L"D:\\platform-tools\\adb.exe"}) {
        out.emplace_back(fixed);
    }
    return out;
}

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        const size_t stop = (end == std::string::npos) ? text.size() : end;
        lines.push_back(text.substr(start, stop - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}

// Splits on runs of spaces (adb separates the `devices -l` fields that way).
std::vector<std::string> split_fields(const std::string& text) {
    std::vector<std::string> fields;
    size_t position = 0;
    while (position < text.size()) {
        while (position < text.size() && (text[position] == ' ' || text[position] == '\t')) ++position;
        const size_t start = position;
        while (position < text.size() && text[position] != ' ' && text[position] != '\t') ++position;
        if (start == position) break;
        fields.push_back(text.substr(start, position - start));
    }
    return fields;
}

Result invoke(const std::string& adb, const std::vector<std::string>& arguments, int timeout_ms) {
    Result result;
    if (adb.empty()) {
        result.error = "adb was not found";
        return result;
    }
    // Never let adb inherit a UNC current directory: cmd.exe (and so anything adb
    // shells out to) refuses one, and a package opened from a \\wsl$\ share is
    // exactly that case.
    const process::Result raw = process::run(process::build_command_line(adb, arguments), timeout_ms,
                                            process::working_directory_for(std::string()));
    result.exit_code = raw.exit_code;
    result.output = raw.output;
    result.timed_out = raw.timed_out;
    result.started = raw.started;
    result.error = raw.error;
    return result;
}

}  // namespace

std::string Device::label() const {
    std::string text;
    if (!model.empty()) text = model + "  ";
    text += "(" + serial + ")";
    if (!state.empty()) text += "  ·  " + state;
    return text;
}

std::string find_adb(const std::string& configured) {
    if (!configured.empty()) {
        const std::wstring wide = to_wide(configured);
        if (is_executable(wide)) return configured;
        if (is_directory(wide)) {
            const std::wstring flat = wide + L"\\adb.exe";
            if (is_executable(flat)) return to_utf8(flat);
            const std::wstring nested = wide + L"\\platform-tools\\adb.exe";
            if (is_executable(nested)) return to_utf8(nested);
        }
    }
    for (const std::wstring& candidate : adb_candidates()) {
        if (is_executable(candidate)) return to_utf8(candidate);
    }
    return {};
}

std::string version(const std::string& adb) {
    const Result result = invoke(adb, {"version"}, 15000);
    const std::vector<std::string> lines = split_lines(result.output);
    for (const std::string& line : lines) {
        const std::string text = trim(line);
        if (!text.empty()) return text;
    }
    return {};
}

Result run(const std::string& adb, const std::vector<std::string>& arguments, int timeout_ms) {
    return invoke(adb, arguments, timeout_ms);
}

std::vector<Device> devices(const std::string& adb, Result* raw) {
    const Result result = invoke(adb, {"devices", "-l"}, 20000);
    if (raw) *raw = result;
    return parse_devices(result.output);
}

std::vector<Device> parse_devices(const std::string& output) {
    std::vector<Device> out;
    bool in_list = false;
    for (const std::string& line : split_lines(output)) {
        const std::string text = trim(line);
        if (text.rfind("List of devices attached", 0) == 0) {
            in_list = true;
            continue;
        }
        if (!in_list || text.empty() || text[0] == '*') continue;

        // "<serial> <state> key:value key:value"
        const std::vector<std::string> fields = split_fields(text);
        if (fields.empty()) continue;
        Device device;
        device.serial = fields[0];
        if (fields.size() > 1) device.state = fields[1];
        for (size_t i = 2; i < fields.size(); ++i) {
            const size_t colon = fields[i].find(':');
            if (colon == std::string::npos) continue;
            const std::string key = fields[i].substr(0, colon);
            const std::string value = fields[i].substr(colon + 1);
            if (key == "model") device.model = value;
            else if (key == "product") device.product = value;
            else if (key == "device") device.device = value;
            else if (key == "transport_id") device.transport = value;
        }
        // "no permissions (missing udev rules?)" arrives as extra bare words
        if (device.state == "no" && fields.size() > 2) device.state = "no permissions";
        out.push_back(std::move(device));
    }
    return out;
}

Result install(const std::string& adb, const std::string& serial, const std::vector<std::string>& paths,
               const InstallOptions& options, int timeout_ms) {
    if (paths.empty()) {
        Result result;
        result.error = "nothing to install";
        return result;
    }
    std::vector<std::string> arguments;
    if (!serial.empty()) {
        arguments.push_back("-s");
        arguments.push_back(serial);
    }
    arguments.push_back(paths.size() > 1 ? "install-multiple" : "install");
    if (options.replace) arguments.push_back("-r");
    if (options.grant) arguments.push_back("-g");
    if (options.downgrade) arguments.push_back("-d");
    if (options.test_packages) arguments.push_back("-t");
    for (const std::string& path : paths) arguments.push_back(path);
    return invoke(adb, arguments, timeout_ms);
}

Result connect(const std::string& adb, const std::string& endpoint) {
    return invoke(adb, {"connect", endpoint}, 30000);
}

Result disconnect(const std::string& adb, const std::string& endpoint) {
    return invoke(adb, {"disconnect", endpoint}, 30000);
}

Result pair(const std::string& adb, const std::string& endpoint, const std::string& code) {
    // adb 30.0+ accepts the pairing code on the command line, which keeps this
    // non-interactive (it would otherwise prompt on stdin).
    return invoke(adb, {"pair", endpoint, code}, 60000);
}

std::vector<MdnsService> mdns_services(const std::string& adb, Result* raw) {
    const Result result = invoke(adb, {"mdns", "services"}, 15000);
    if (raw) *raw = result;
    return parse_mdns(result.output);
}

std::vector<MdnsService> parse_mdns(const std::string& output) {
    std::vector<MdnsService> out;
    for (const std::string& line : split_lines(output)) {
        const std::string text = trim(line);
        if (text.empty() || text.rfind("List of discovered", 0) == 0) continue;

        // "<name>\t<type>\t<endpoint>", but some adb versions use spaces
        std::vector<std::string> fields;
        if (text.find('\t') != std::string::npos) {
            size_t position = 0;
            while (position <= text.size()) {
                const size_t tab = text.find('\t', position);
                fields.push_back(trim(text.substr(position, tab == std::string::npos ? std::string::npos
                                                                                    : tab - position)));
                if (tab == std::string::npos) break;
                position = tab + 1;
            }
        } else {
            fields = split_fields(text);
        }
        if (fields.size() < 3) continue;
        MdnsService service;
        service.name = fields[0];
        service.type = fields[1];
        service.endpoint = fields[2];
        if (service.endpoint.find(':') == std::string::npos) continue;
        out.push_back(std::move(service));
    }
    return out;
}

}  // namespace apkinfo::adb
