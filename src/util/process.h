// Child-process helper: run a command line, capture its merged output.
//
// Two features need this and nothing more: the adb backend (devices, install,
// connect, pair) and the external install command the settings page can configure.
// Both wait for a child, which is why the UI calls them from a worker thread.
#pragma once

#include <string>
#include <vector>

namespace apkinfo::process {

struct Result {
    int exit_code = -1;
    std::string output;      // stdout and stderr merged, as the program printed them
    bool started = false;    // false when the process could not be created
    bool timed_out = false;
    std::string reason;      // why it could not be started, or why the directory changed
    std::string error;       // reason when started == false
};

// Quotes one argument for a Windows command line (CommandLineToArgvW rules): an
// argument is wrapped in quotes when it contains whitespace or a quote, and the
// backslashes in front of an embedded quote are doubled.
std::string quote_argument(const std::string& argument);

// Joins an executable and its arguments into one command line.
std::string build_command_line(const std::string& executable, const std::vector<std::string>& arguments);

// True for "\\server\share" and "//server/share" paths - network shares, including
// the \\wsl$ / \\wsl.localhost shares a WSL distribution is reachable through. The
// \\?\ and \\.\ device prefixes are *not* UNC; they are handled by the file APIs.
bool is_unc_path(const std::string& utf8_path);

// The directory a child should start in ("" = inherit the current one).
//
// cmd.exe cannot use a UNC current directory: it prints "CMD.EXE was started with
// the above path as the current directory. UNC paths are not supported." and falls
// back to C:\Windows, which breaks every relative path in a command and takes the
// install command with it. So a UNC current directory - and a UNC `preferred`
// directory - is replaced by the temporary directory.
std::string working_directory_for(const std::string& preferred);

// Runs `command_line` through CreateProcessW (no console window), captures stdout and
// stderr into `output`, and waits at most `timeout_ms` (0 = no limit; the child is
// killed when the timeout expires and timed_out is set). `working_directory` is the
// directory the child starts in; pass the result of working_directory_for().
Result run(const std::string& command_line, int timeout_ms, const std::string& working_directory = std::string());

// Runs `cmd.exe /c <command>`, which is what the configurable install command needs
// (it may use quotes, && and other cmd syntax). `preferred_directory` is used when it
// is a local directory - a package next to the command's relative paths then works -
// and the temporary directory replaces a UNC one.
Result run_shell(const std::string& command, int timeout_ms,
                 const std::string& preferred_directory = std::string());

}  // namespace apkinfo::process
