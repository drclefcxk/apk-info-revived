#include "android/versions.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "config.h"
#include "util/text.h"

namespace apkinfo {
namespace versions {
namespace {

constexpr const char* kFileName = "android-versions.txt";

// The table as shipped. Kept sorted by SDK so the generated file reads naturally.
constexpr AndroidVersion kBuiltin[] = {
    {1, "1.0"},   {2, "1.1"},  {3, "1.5"},    {4, "1.6"},   {5, "2.0"},   {6, "2.0.1"},
    {7, "2.1"},   {8, "2.2"},  {9, "2.3"},    {10, "2.3.3"}, {11, "3.0"}, {12, "3.1"},
    {13, "3.2"},  {14, "4.0"}, {15, "4.0.3"}, {16, "4.1"},  {17, "4.2"},  {18, "4.3"},
    {19, "4.4"},  {20, "4.4W"}, {21, "5.0"},  {22, "5.1"},  {23, "6.0"},  {24, "7.0"},
    {25, "7.1"},  {26, "8.0"}, {27, "8.1"},   {28, "9"},    {29, "10"},   {30, "11"},
    {31, "12"},   {32, "12L"}, {33, "13"},    {34, "14"},   {35, "15"},   {36, "16"},
    {37, "17"},
};

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

// Reads the whole file as bytes; empty when it cannot be opened.
std::string read_file(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
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

// Writes with a UTF-8 BOM: the file is meant to be edited in Notepad.
bool write_file(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    bool ok = WriteFile(file, bom, 3, &written, nullptr) != FALSE;
    if (ok) ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE;
    CloseHandle(file);
    return ok;
}

}  // namespace

std::string executable_directory() {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) return {};
    std::wstring path(buffer, length);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return to_utf8(path.substr(0, slash));
}

std::span<const AndroidVersion> builtin_android_versions() {
    return std::span<const AndroidVersion>(kBuiltin, std::size(kBuiltin));
}

std::string android_versions_path() {
    const std::string directory = executable_directory();
    if (directory.empty()) return {};
    return directory + "\\" + kFileName;
}

std::string android_versions_file_text() {
    std::string out;
    out += "# APK Info - Revived: Android version names\n";
    out += "#\n";
    out += "# Maps an SDK level to the name shown after it, e.g.\n";
    out += "#     34 14            ->  \"34 (Android 14)\"\n";
    out += "#     36 16 (Baklava)  ->  \"36 (Android 16 (Baklava))\"\n";
    out += "#\n";
    out += "# One entry per line: the SDK level first, then the name (everything after\n";
    out += "# the first space). Blank lines and lines starting with '#' are ignored, and\n";
    out += "# a level that is not listed simply shows no name. Edit this file to teach\n";
    out += "# the program about future Android releases - it is read on every start,\n";
    out += "# so no rebuild is needed. Deleting the file restores the built-in table.\n";
    out += "#\n";
    out += "# This file lives next to ApkInfo.exe:\n";
    out += "#     " + android_versions_path() + "\n";
    out += "#\n";
    out += "# sdk  name\n";
    for (const AndroidVersion& entry : kBuiltin) {
        out += std::to_string(entry.sdk) + " " + entry.name + "\n";
    }
    return out;
}

std::vector<AndroidVersion> parse_android_versions(std::string_view text) {
    // tolerate a UTF-8 BOM (the file is written with one so Notepad shows it right)
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }

    std::vector<AndroidVersion> out;
    size_t position = 0;
    while (position <= text.size()) {
        const size_t end = text.find('\n', position);
        std::string line(text.substr(position, end == std::string_view::npos ? std::string_view::npos : end - position));
        position = (end == std::string_view::npos) ? text.size() + 1 : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        const size_t space = trimmed.find_first_of(" \t");
        if (space == std::string::npos) continue;  // "34" alone says nothing
        const std::string key = trimmed.substr(0, space);
        char* end_of_number = nullptr;
        const long sdk = std::strtol(key.c_str(), &end_of_number, 10);
        if (!end_of_number || *end_of_number != '\0' || sdk <= 0 || sdk > 10000) continue;

        const std::string name = trim(trimmed.substr(space + 1));
        if (name.empty()) continue;

        // last entry wins, so a user can append an override at the bottom
        const auto existing = std::find_if(out.begin(), out.end(),
                                           [sdk](const AndroidVersion& entry) { return entry.sdk == sdk; });
        if (existing != out.end()) {
            existing->name = name;
        } else {
            out.push_back(AndroidVersion{static_cast<int>(sdk), name});
        }
    }
    return out;
}

namespace {

// The configuration file owns the file format; this module only answers lookups.
std::string lookup(int64_t sdk) {
    const Config& current = config();
    for (const auto& entry : current.android_versions) {
        if (entry.first == sdk) return entry.second;
    }
    for (const AndroidVersion& entry : kBuiltin) {
        if (entry.sdk == sdk) return entry.name;
    }
    return {};
}

}  // namespace

std::string android_version_name(int64_t sdk) { return lookup(sdk); }

void reload_android_versions() { reload_config(); }

}  // namespace versions
}  // namespace apkinfo
