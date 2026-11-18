// The program's configuration file: JSON, next to ApkInfo.exe.
//
// It started as a table of Android version names, but it is the place future
// settings go, so it is a small structured document instead of a bare list:
//
//     {
//       "_readme": ["..."],
//       "language": "auto",
//       "androidVersions": { "34": "14", "35": "15" }
//     }
//
// The file is created from the built-in defaults on first start. A hand-edited file
// is never overwritten: a malformed document keeps the defaults in memory and reports
// a warning instead of destroying the user's edits.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace apkinfo {

// "auto" follows the Windows UI language; the others force one.
enum class LanguagePreference { Auto, English, Chinese };

struct Config {
    LanguagePreference language = LanguagePreference::Auto;
    // SDK level -> marketing name. Empty means "use the built-in table", which is
    // also what a config file with no androidVersions object means.
    std::vector<std::pair<int, std::string>> android_versions;
};

// Path of the config file: <exe dir>/apkinfo-config.json ("" when unknown).
std::string config_path();

// The loaded configuration; loads on first use (creating the file when missing).
const Config& config();

// Same as config(), but the mutable view used by the UI when a setting changes.
Config& mutable_config();

// Writes the current configuration next to the exe. Returns false when the file
// cannot be written.
bool save_config();

// Drops the cached configuration so the next access re-reads the file (tests).
void reload_config();

// Non-fatal problem noticed while loading ("" when everything was fine). Shown once
// in the Notes area so a broken file is not silently ignored.
const std::string& config_warning();

// The text written to a fresh config file (defaults + instructions).
std::string config_file_text();

// Language preference <-> JSON string ("auto" / "en" / "zh").
const char* language_preference_code(LanguagePreference preference);
bool language_preference_from_code(const std::string& code, LanguagePreference* out);

}  // namespace apkinfo
