// "Dump" output: turn a parsed package into text a human can paste into a bug
// report, or into JSON a script can read.
//
// The text report is localised exactly like the window (it reuses the same
// dictionary), the JSON one is not: its keys are stable English, so tooling can
// rely on them whatever language the app is running in.
#pragma once

#include <string>

#include "model/info_document.h"

namespace apkinfo {

// Human readable report. `localized` picks the current UI language; pass false
// for a reproducible English dump (tests do).
std::string build_text_report(const InfoDocument& document, bool localized);

// Machine readable dump: full fidelity, English keys, 2-space indented.
std::string build_json_report(const InfoDocument& document);

// The text the Information pane shows: every section and every row, nothing
// collapsed or truncated. It is displayed in a read-only text control, so the
// user can select and copy any part of it.
std::string build_info_text(const InfoDocument& document, bool localized);

}  // namespace apkinfo
