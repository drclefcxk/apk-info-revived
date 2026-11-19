// adb access for the installer page: run adb, list devices, install, connect, pair.
//
// There is no Android SDK dependency here - the module only starts a child process
// and parses what adb prints. `adb` is looked up in the configured path, $ADB, PATH
// and the usual SDK locations, so a user with platform-tools anywhere can use it.
//
// Everything here blocks (it waits for a child process), so the UI calls it from a
// worker thread and hands the result back through the event loop.
#pragma once

#include <string>
#include <vector>

namespace apkinfo::adb {

// One line of `adb devices -l`.
struct Device {
    std::string serial;
    std::string state;    // "device", "offline", "unauthorized", ...
    std::string model;    // "Pixel 7" when adb reports it
    std::string product;
    std::string device;
    std::string transport;  // transport_id

    bool usable() const { return state == "device"; }
    // "Pixel 7 (emulator-5554) - device"
    std::string label() const;
};

struct Result {
    int exit_code = -1;
    std::string output;   // stdout and stderr merged, as adb printed them
    bool timed_out = false;
    bool started = false;  // false when the process could not be created
    std::string error;     // why it could not be started
};

// `adb` lookup: the configured path first, then $ADB, then PATH, then the common
// per-user SDK locations. "" when nothing was found.
std::string find_adb(const std::string& configured = {});

// "Android Debug Bridge version 1.0.41 ..." first line, "" when adb does not run.
std::string version(const std::string& adb);

// Runs `adb <args>`. `timeout_ms` bounds the wait; the child is killed when it
// expires (installing a big package can legitimately take a while, so pass a large
// value for those).
Result run(const std::string& adb, const std::vector<std::string>& args, int timeout_ms);

// `adb devices -l`, parsed. Devices in any state are returned, including
// "unauthorized" and "offline" - the page shows them so the reason is visible.
std::vector<Device> devices(const std::string& adb, Result* raw = nullptr);

// The parsers behind devices() / mdns_services(), exposed for the unit tests.
std::vector<Device> parse_devices(const std::string& output);

struct InstallOptions {
    bool replace = true;      // -r
    bool grant = false;       // -g  (grant all runtime permissions)
    bool downgrade = false;   // -d
    bool test_packages = false;  // -t
};

// `adb -s <serial> install <options> <paths...>`, or install-multiple for a bundle.
Result install(const std::string& adb, const std::string& serial, const std::vector<std::string>& paths,
               const InstallOptions& options, int timeout_ms = 600000);

// Wireless debugging.
Result connect(const std::string& adb, const std::string& endpoint);
Result disconnect(const std::string& adb, const std::string& endpoint);
Result pair(const std::string& adb, const std::string& endpoint, const std::string& code);

// A service advertised over mDNS (`adb mdns services`), used to suggest endpoints.
struct MdnsService {
    std::string name;
    std::string type;    // "_adb-tls-connect._tcp" / "_adb-tls-pairing._tcp"
    std::string endpoint;  // "192.168.1.5:37000"
};
std::vector<MdnsService> mdns_services(const std::string& adb, Result* raw = nullptr);
std::vector<MdnsService> parse_mdns(const std::string& output);

}  // namespace apkinfo::adb
