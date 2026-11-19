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
    std::string error;       // reason when started == false
};

// Quotes one argument for a Windows command line (CommandLineToArgvW rules): an
// argument is wrapped in quotes when it contains whitespace or a quote, and the
// backslashes in front of an embedded quote are doubled.
std::string quote_argument(const std::string& argument);

// Joins an executable and its arguments into one command line.
std::string build_command_line(const std::string& executable, const std::vector<std::string>& arguments);

// Runs `command_line` through CreateProcessW (no console window), captures stdout and
// stderr into `output`, and waits at most `timeout_ms` (0 = no limit; the child is
// killed when the timeout expires and timed_out is set).
Result run(const std::string& command_line, int timeout_ms);

// Runs `cmd.exe /c <command>`, which is what the configurable install command needs
// (it may use quotes, && and other cmd syntax).
Result run_shell(const std::string& command, int timeout_ms);

}  // namespace apkinfo::process
