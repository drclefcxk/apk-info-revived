#include "util/clipboard.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace apkinfo {
namespace {

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    wide.resize(static_cast<size_t>(length) - 1);  // drop the terminating NUL
    return wide;
}

}  // namespace

bool set_clipboard_text(const std::string& utf8) {
    const std::wstring wide = to_wide(utf8);
    if (wide.empty()) return false;

    // Another process can hold the clipboard for a moment; a couple of retries is
    // what every text editor does here.
    bool opened = false;
    // A dead owner, a busy one, or the shell mid-update can hold the clipboard for
    // a while; retry for about a second before giving up.
    for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
        opened = OpenClipboard(nullptr) != FALSE;
        if (!opened) Sleep(20);
    }
    if (!opened) return false;

    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* target = GlobalLock(block)) {
                std::memcpy(target, wide.c_str(), bytes);
                GlobalUnlock(block);
                // SetClipboardData takes ownership of the block on success.
                ok = SetClipboardData(CF_UNICODETEXT, block) != nullptr;
                if (!ok) GlobalFree(block);
            } else {
                GlobalFree(block);
            }
        }
    }
    CloseClipboard();
    return ok;
}

}  // namespace apkinfo
