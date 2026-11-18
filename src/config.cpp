#include "config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "android/versions.h"
#include "util/json.h"
#include "util/text.h"

namespace apkinfo {
namespace {

constexpr const char* kConfigFileName = "apkinfo-config.json";
constexpr const char* kLegacyFileName = "android-versions.txt";

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    wide.resize(static_cast<size_t>(length) - 1);
    return wide;
}

std::string read_file(const std::string& path) {
    const std::wstring wide = to_wide(path);
    if (wide.empty()) return {};
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string data;
    char buffer[8192];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer, sizeof buffer, &read, nullptr) || read == 0) break;
        data.append(buffer, read);
    }
    CloseHandle(file);
    return data;
}

bool write_file(const std::string& path, const std::string& text) {
    const std::wstring wide = to_wide(path);
    if (wide.empty()) return false;
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    // UTF-8 BOM: the file is meant to be opened in Notepad.
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    bool ok = WriteFile(file, bom, 3, &written, nullptr) != FALSE;
    if (ok) ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE;
    CloseHandle(file);
    return ok;
}

std::string strip_bom(std::string text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

Config defaults() {
    Config result;
    result.language = LanguagePreference::Auto;
    for (const auto& entry : versions::builtin_android_versions()) {
        result.android_versions.emplace_back(entry.sdk, entry.name);
    }
    return result;
}

// Reads the pre-JSON table (one "<sdk> <name>" per line), so a user who edited it
// keeps their entries after the format change.
std::vector<std::pair<int, std::string>> parse_legacy_versions(const std::string& text) {
    std::vector<std::pair<int, std::string>> out;
    size_t position = 0;
    while (position <= text.size()) {
        const size_t end = text.find('\n', position);
        std::string line = text.substr(position, end == std::string::npos ? std::string::npos : end - position);
        position = (end == std::string::npos) ? text.size() + 1 : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        const size_t space = trimmed.find_first_of(" \t");
        if (space == std::string::npos) continue;
        const long sdk = std::strtol(trimmed.substr(0, space).c_str(), nullptr, 10);
        if (sdk <= 0 || sdk > 10000) continue;
        const std::string name = trim(trimmed.substr(space + 1));
        if (name.empty()) continue;
        out.emplace_back(static_cast<int>(sdk), name);
    }
    return out;
}

struct State {
    Config config;
    bool loaded = false;
    std::string warning;
};

State& state() {
    static State instance;
    return instance;
}

std::mutex& state_mutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace

std::string config_path() {
    const std::string directory = versions::executable_directory();
    if (directory.empty()) return {};
    return directory + "\\" + kConfigFileName;
}

std::string config_file_text() {
    const Config current = defaults();
    std::string out;
    out += "{\n";
    out += "  \"_readme\": [\n";
    out += "    \"APK Info - Revived configuration. Edit the values below; the file is read on every start.\",\n";
    out += "    \"language: \\\"auto\\\" follows the Windows UI language, or force \\\"en\\\" / \\\"zh\\\".\",\n";
    out += "    \"androidVersions: SDK level -> the name shown after it, e.g. \\\"36\\\": \\\"16 (Baklava)\\\".\",\n";
    out += "    \"Add future Android releases here - no rebuild needed. Delete the file to restore defaults.\",\n";
    out += "    \"This file lives next to ApkInfo.exe: " + config_path() + "\"\n";
    out += "  ],\n";
    out += "  \"language\": \"auto\",\n";
    out += "  \"androidVersions\": {\n";
    for (size_t i = 0; i < current.android_versions.size(); ++i) {
        const auto& entry = current.android_versions[i];
        out += "    \"" + std::to_string(entry.first) + "\": " + json::quote(entry.second);
        if (i + 1 < current.android_versions.size()) out += ",";
        out += "\n";
    }
    out += "  }\n";
    out += "}\n";
    return out;
}

const char* language_preference_code(LanguagePreference preference) {
    switch (preference) {
        case LanguagePreference::English:
            return "en";
        case LanguagePreference::Chinese:
            return "zh";
        case LanguagePreference::Auto:
        default:
            return "auto";
    }
}

bool language_preference_from_code(const std::string& code, LanguagePreference* out) {
    const std::string lowered = to_lower(trim(code));
    if (lowered.empty() || lowered == "auto" || lowered == "system") {
        *out = LanguagePreference::Auto;
        return true;
    }
    if (lowered.rfind("en", 0) == 0) {
        *out = LanguagePreference::English;
        return true;
    }
    if (lowered.rfind("zh", 0) == 0) {
        *out = LanguagePreference::Chinese;
        return true;
    }
    return false;
}

const Config& config() {
    std::lock_guard<std::mutex> guard(state_mutex());
    State& current = state();
    if (current.loaded) return current.config;

    current.loaded = true;
    current.config = defaults();

    const std::string path = config_path();
    if (path.empty()) return current.config;

    const std::string raw = strip_bom(read_file(path));
    if (raw.empty()) {
        // First start: write the defaults so there is something to edit. Also import a
        // table left behind by the pre-JSON versions of the program.
        const std::string directory = versions::executable_directory();
        if (!directory.empty()) {
            const std::string legacy = strip_bom(read_file(directory + "\\" + kLegacyFileName));
            if (!legacy.empty()) {
                auto migrated = parse_legacy_versions(legacy);
                if (!migrated.empty()) current.config.android_versions = std::move(migrated);
            }
        }
        write_file(path, config_file_text());
        return current.config;
    }

    std::string error;
    const auto document = json::parse(raw, &error);
    if (!document) {
        current.warning = "apkinfo-config.json: " + error + " (using defaults)";
        return current.config;
    }
    if (!document->is_object()) {
        current.warning = "apkinfo-config.json: the top level is not an object (using defaults)";
        return current.config;
    }

    if (const json::Value* language = document->find("language")) {
        LanguagePreference preference = LanguagePreference::Auto;
        if (language_preference_from_code(language->string_or("auto"), &preference)) {
            current.config.language = preference;
        } else {
            current.warning = "apkinfo-config.json: unknown language \"" + language->string_or("") + "\"";
        }
    }

    if (const json::Value* table = document->find("androidVersions")) {
        if (table->is_object()) {
            std::vector<std::pair<int, std::string>> entries;
            for (const auto& member : table->members) {
                const long sdk = std::strtol(member.first.c_str(), nullptr, 10);
                if (sdk <= 0 || sdk > 10000) continue;
                std::string name = member.second.string_or("");
                if (name.empty() && member.second.is_number()) {
                    // "34": 14 is a convenient shorthand for plain names
                    const double value = member.second.number;
                    if (value == static_cast<double>(static_cast<long long>(value))) {
                        name = std::to_string(static_cast<long long>(value));
                    } else {
                        name = std::to_string(value);
                    }
                }
                if (name.empty()) continue;
                entries.emplace_back(static_cast<int>(sdk), std::move(name));
            }
            if (!entries.empty()) {
                std::sort(entries.begin(), entries.end(),
                          [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
                current.config.android_versions = std::move(entries);
            }
        } else {
            current.warning = "apkinfo-config.json: androidVersions is not an object";
        }
    }
    return current.config;
}

Config& mutable_config() {
    // Ensure the file has been read at least once.
    (void)config();
    return state().config;
}

bool save_config() {
    const std::string path = config_path();
    if (path.empty()) return false;
    const Config& current = state().config;
    std::string out;
    out += "{\n";
    out += "  \"_readme\": [\n";
    out += "    \"APK Info - Revived configuration. Edit the values below; the file is read on every start.\",\n";
    out += "    \"language: \\\"auto\\\" follows the Windows UI language, or force \\\"en\\\" / \\\"zh\\\".\",\n";
    out += "    \"androidVersions: SDK level -> the name shown after it, e.g. \\\"36\\\": \\\"16 (Baklava)\\\".\",\n";
    out += "    \"Add future Android releases here - no rebuild needed. Delete the file to restore defaults.\"\n";
    out += "  ],\n";
    out += "  \"language\": " + json::quote(language_preference_code(current.language)) + ",\n";
    out += "  \"androidVersions\": {\n";
    for (size_t i = 0; i < current.android_versions.size(); ++i) {
        const auto& entry = current.android_versions[i];
        out += "    \"" + std::to_string(entry.first) + "\": " + json::quote(entry.second);
        if (i + 1 < current.android_versions.size()) out += ",";
        out += "\n";
    }
    out += "  }\n";
    out += "}\n";
    return write_file(path, out);
}

void reload_config() {
    std::lock_guard<std::mutex> guard(state_mutex());
    State& current = state();
    current.config = Config{};
    current.loaded = false;
    current.warning.clear();
}

const std::string& config_warning() {
    (void)config();
    return state().warning;
}

}  // namespace apkinfo
