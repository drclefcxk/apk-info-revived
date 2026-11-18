// Clipboard helper (Win32) for the "copy" affordances.
#pragma once

#include <string>

namespace apkinfo {

// Puts `utf8` on the clipboard as CF_UNICODETEXT. Returns false when another
// process holds the clipboard or the allocation fails.
bool set_clipboard_text(const std::string& utf8);

}  // namespace apkinfo
