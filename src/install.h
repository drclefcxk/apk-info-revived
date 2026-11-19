// "Install" behaviour: the built-in adb installer, and the configurable command line.
//
// Two worlds, three cases:
//   * Android package + built-in mode -> the install page drives adb itself
//     (device list, wireless connect/pair, install, log).
//   * Android package + command mode  -> the configured command line is run through
//     cmd.exe, with {placeholders} expanded.
//   * Alpine package                  -> always the configured command line.
//
// The placeholder expansion and the bundle preparation are pure functions on
// purpose: they are what the tests exercise, without an adb or a device.
#pragma once

#include <string>
#include <vector>

#include "android/adb.h"
#include "model/info_document.h"

namespace apkinfo::install {

enum class Mode {
    Builtin = 0,  // the install page (Android only)
    Command = 1,  // run the configured command line
};

struct Settings {
    Mode android_mode = Mode::Builtin;
    std::string android_command;  // used when android_mode == Command
    std::string alpine_command;   // always used for Alpine packages
    std::string adb_path;         // "" = look for adb automatically
    adb::InstallOptions options{};
    std::string device;           // last used serial, remembered across runs
};

// Values available to an external command as {name}.
struct Context {
    std::string path;          // full path of the package
    std::string dir;           // its directory
    std::string file_name;     // "app.apk"
    std::string base_name;     // "app"
    std::string extension;     // "apk" (no dot)
    std::string size;          // file size in bytes
    std::string kind;          // "android" / "apks" / "alpine"
    std::string app_name;      // application label / Alpine package name
    std::string package_name;  // Android application id / Alpine pkgname
    std::string version;       // version name / pkgver
    std::string version_code;  // Android version code ("" for Alpine)
    std::string arch;          // ABIs / Alpine arch
    std::string device;        // the selected adb serial ("" when none)
    std::string adb;           // the adb executable in use ("" when not found)
};

// Defaults written into a fresh config file. Inline so that a translation unit which
// only includes config.h does not need to link the whole install implementation.
inline std::string default_android_command() { return "adb install -r \"{path}\""; }

inline std::string default_alpine_command() {
    // Push the package, then install it with apk-tools; && so the install only runs
    // when the push succeeded.
    return "adb push \"{path}\" /tmp/{file_name} && adb shell apk add --allow-untrusted /tmp/{file_name}";
}

// Replaces {name} with the matching value. Unknown {tokens} are left as they are, so
// a typo in the settings page is visible instead of silently turning into nothing.
// "{{" produces a literal "{".
std::string expand(const std::string& command, const Context& context);

// The token list the settings page shows: one "token<TAB>meaning" per line.
std::vector<std::pair<std::string, std::string>> placeholder_tokens();

// Builds the context from a parsed document.
Context make_context(const InfoDocument& document, const std::string& adb_path, const std::string& device);

// What adb gets: a single package is installed as it is; a bundle (.apks/.apkm/.xapk)
// is extracted into a temporary directory, because adb needs the base apk and the
// splits as separate files (install-multiple).
struct Prepared {
    std::vector<std::string> files;
    std::string temp_dir;  // "" unless something was extracted
    std::string error;

    bool ok() const { return error.empty() && !files.empty(); }
};

Prepared prepare(const std::string& path, ContainerKind kind);
void cleanup(const Prepared& prepared);

struct Outcome {
    bool success = false;
    std::string output;   // what adb / the command printed
    std::string command;  // the command line that ran (for the log)
};

// adb install / install-multiple, with the options from `settings`.
Outcome run_builtin(const Settings& settings, const std::string& adb, const std::string& serial,
                    const std::string& path, ContainerKind kind);

// The configured command line, expanded and run through cmd.exe.
// Splits a command line on "&&" into the steps to run in order (separators inside
// quotes are left alone). Each step goes to CreateProcessW as it is - there is no
// shell, which is what makes a UNC working directory such as a WSL share work.
std::vector<std::string> split_steps(const std::string& command);

Outcome run_command(const std::string& command, const Context& context);

// The command a package would run in command mode, already expanded (settings page
// preview and the log both show this).
std::string command_for_package(const Settings& settings, const InfoDocument& document, const std::string& adb_path,
                                const std::string& device);

}  // namespace apkinfo::install
