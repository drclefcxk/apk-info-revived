// Android version names, loaded at runtime from a text file next to the exe.
//
// The table (SDK -> marketing name) changes every year, so it does not live in the
// binary: `android-versions.txt` sits beside ApkInfo.exe and is created from the
// built-in table on first run, with comments that explain the format. Users can add
// future versions without waiting for a rebuild.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apkinfo {
namespace versions {

struct AndroidVersion {
    int sdk = 0;
    std::string name;  // "8.0", "12L", ...
};

// Directory that contains this executable (no trailing separator, UTF-8).
// Empty when it cannot be determined.
std::string executable_directory();

// Parses the config format: "<sdk> <name>" per line, "#" comments, blank lines
// ignored, an optional UTF-8 BOM tolerated. Malformed lines are skipped, so a
// half-edited file still works.
std::vector<AndroidVersion> parse_android_versions(std::string_view text);

// The table shipped with the program, in the order it is written to the file.
std::span<const AndroidVersion> builtin_android_versions();

// Marketing name for an SDK level ("" when unknown). The table comes from the
// configuration file (config.h); a level missing there falls back to built-in.
std::string android_version_name(int64_t sdk);

// Drops the cached table so the next lookup re-reads the configuration (tests).
void reload_android_versions();

}  // namespace versions
}  // namespace apkinfo
