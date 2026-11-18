// Application glue: load a file from disk, decide what it is, hand it to the
// right reader. Kept free of any UI dependency so it can be driven from tests.
#pragma once

#include <string>

#include "model/info_document.h"

namespace apkinfo {

// Maps `utf8_path`, detects the container and parses it. Never throws; on
// failure the returned document has ok == false and `error` set.
InfoDocument load_package(const std::string& utf8_path, double* elapsed_ms = nullptr);

// Native "open file" dialog. Returns a UTF-8 path, or "" when cancelled.
std::string open_package_dialog();

// Native "save file" dialog for the info dump. `suggested_name` seeds the file
// name; the chosen extension decides the format (.json vs anything else = text).
// Returns a UTF-8 path, or "" when cancelled.
std::string save_report_dialog(const std::string& suggested_name);

// Native "save file" dialog for the extracted launcher icon (.png).
std::string save_icon_dialog(const std::string& suggested_name);

}  // namespace apkinfo
