// APK Info — Revived, GUI entry point.
//
// The window itself is declared in ui/main.slint (compiled to main.h). This file
// turns an InfoDocument into what the view consumes, and owns the things that
// make the UI fast and useful:
//
//   * the document is parsed **once** and kept; switching language only
//     re-renders (the readers emit English keys, translation happens here),
//   * the Information tab is one read-only text control holding the whole
//     document - selectable, Ctrl+C-able, nothing collapsed behind a button,
//   * the Contents tab is a collapsed tree: only visible nodes become rows,
//   * SHA-256 is computed on a worker thread after the window is up,
//   * the text leaves the window through the text control itself, "Copy all"
//     and "Dump…" (the same report written to a file).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <slint.h>
#include <slint_timer.h>

#include "android/adb.h"
#include "app.h"
#include "config.h"
#include "install.h"
#include "i18n.h"
#include "main.h"
#include "model/info_document.h"
#include "model/report.h"
#include "util/clipboard.h"
#include "util/hash.h"
#include "util/png_writer.h"
#include "util/text.h"

namespace {

using apkinfo::ContainerKind;
using apkinfo::ContentsKind;
using apkinfo::InfoDocument;

// How long the "Copied" confirmation stays on the button.
constexpr int kCopiedFeedbackMs = 1200;

// A section with more rows than this shows its first kPreviewRows entries (rendered
// exactly like the basic information) plus a "show all (N)" button that opens the
// viewer window.
constexpr size_t kLongSection = 12;
constexpr size_t kPreviewRows = 8;

std::string to_utf8(const wchar_t* text) {
    if (!text) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::string compact_size(uint64_t bytes) {
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[32];
    if (unit == 0) {
        std::snprintf(buffer, sizeof buffer, "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buffer, sizeof buffer, "%.2f %s", value, units[unit]);
    }
    return buffer;
}

// Asks Windows to trim the process working set. Parsing a bundle touches a lot of
// mapped pages once; without this the reported memory stays high until the OS
// feels like reclaiming them.
void trim_working_set() {
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

// ---------------------------------------------------------------------------
// Window / taskbar icon
//
// Slint's C++ API has no window-icon setter, so the icon is applied through
// Win32: find our own top-level window and send WM_SETICON. The icons are
// compiled into the exe (assets/icons/app.rc), which is what makes the fast,
// content-based switch possible: the shell can only pick an icon by file
// extension, but once we have read the file we know whether it is an Android
// package, a split bundle or an Alpine package.
// ---------------------------------------------------------------------------

// Resource ids; must match assets/icons/app.rc
constexpr int kIconApp = 101;
constexpr int kIconApk = 102;
constexpr int kIconApks = 103;
constexpr int kIconAndroid = 104;
constexpr int kIconAlpine = 105;

struct WindowSearch {
    DWORD process_id = 0;
    HWND found = nullptr;
};

// The main window handle, remembered so a secondary window can be centred on the
// window's *current* position (the first show used to land where the main window had
// been at start-up).
HWND g_main_window = nullptr;

BOOL CALLBACK find_own_window(HWND hwnd, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(hwnd, &process_id);
    if (process_id != search->process_id) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;  // skip owned popups
    // Only the real top-level window counts: the event loop also owns small
    // helper windows (Slint/winit creates a "Winit Thread Event Target"), and
    // sending WM_SETICON to one of those looked exactly like "no icon at all".
    if (GetWindowTextLengthW(hwnd) == 0) return TRUE;
    search->found = hwnd;
    return FALSE;
}

// The main window handle. Resolved once (while it is the only titled window) and kept,
// because EnumWindows returns the topmost window first - which becomes the viewer as
// soon as one is open.
HWND main_window_handle() {
    if (g_main_window && IsWindow(g_main_window) && IsWindowVisible(g_main_window)) return g_main_window;
    WindowSearch search{GetCurrentProcessId(), nullptr};
    EnumWindows(find_own_window, reinterpret_cast<LPARAM>(&search));
    if (search.found) g_main_window = search.found;
    return g_main_window;
}

void set_window_icon(ContainerKind kind) {
    static HICON current_big = nullptr;
    static HICON current_small = nullptr;
    static HMODULE instance = GetModuleHandleW(nullptr);

    int resource = kIconApp;
    switch (kind) {
        case ContainerKind::AndroidApk:
            resource = kIconAndroid;
            break;
        case ContainerKind::AndroidSplitBundle:
            resource = kIconApks;
            break;
        case ContainerKind::AlpinePackage:
            resource = kIconAlpine;
            break;
        default:
            resource = kIconApp;
            break;
    }

    const int big_size = GetSystemMetrics(SM_CXICON);
    const int small_size = GetSystemMetrics(SM_CXSMICON);

    const HWND target = main_window_handle();
    if (!target) {
        // Slint creates the native window when the event loop starts, so the very
        // first call (while loading the command line package) finds nothing yet.
        static int attempts = 0;
        if (attempts++ < 50) {
            slint::Timer::single_shot(std::chrono::milliseconds(100), [kind]() { set_window_icon(kind); });
        }
        return;
    }

    HICON big = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(resource), IMAGE_ICON, big_size, big_size,
                                              LR_DEFAULTCOLOR));
    HICON small = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(resource), IMAGE_ICON, small_size,
                                                small_size, LR_DEFAULTCOLOR));
    if (big) SendMessageW(target, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    if (small) SendMessageW(target, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));

    if (current_big) DestroyIcon(current_big);
    if (current_small) DestroyIcon(current_small);
    current_big = big;
    current_small = small;
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

std::string tr(const std::string& text) { return apkinfo::i18n::t(text); }

// Translates a value: whole-string keys ("yes", "(none)", "Single APK", ...) and
// the "  <key>" suffix the readers append ("com.foo.BAR  (sdk 23+)").
std::string tr_value(const std::string& value) {
    const std::string whole = apkinfo::i18n::t(value);
    if (whole != value) return whole;
    const size_t split_at = value.rfind("  ");
    if (split_at != std::string::npos) {
        const std::string tail = value.substr(split_at + 2);
        const std::string translated = apkinfo::i18n::t(tail);
        if (translated != tail) return value.substr(0, split_at + 2) + translated;
    }
    return value;
}

InfoRowItem make_row(int kind) {
    InfoRowItem row;
    row.kind = kind;
    row.label = slint::SharedString();
    row.value = slint::SharedString();
    row.hint = slint::SharedString();
    row.mono = false;
    row.indent = 0;
    row.toggle = 0;
    row.payload = -1;
    return row;
}

// One line of a section row, as the box shows it.
std::string row_text(const apkinfo::InfoRow& row) {
    if (row.label.empty()) return tr_value(row.value);
    return tr(row.label) + ": " + tr_value(row.value);
}

// Every entry of a section, one per line - the body of the viewer window.
std::string section_text(const apkinfo::InfoSection& section) {
    std::string body;
    body.reserve(section.rows.size() * 40);
    for (const apkinfo::InfoRow& row : section.rows) {
        body += row_text(row);
        body += '\n';
    }
    return body;
}

struct ViewState {
    InfoDocument document;
    double elapsed_ms = 0;
    std::string path;
    bool loaded = false;

    std::vector<char> expanded;  // per tree node
    std::vector<InfoRowItem> info_rows;
    std::vector<InfoRowItem> contents_rows;
    std::shared_ptr<slint::VectorModel<InfoRowItem>> info_model;
    std::shared_ptr<slint::VectorModel<InfoRowItem>> contents_model;
    // The section viewer window, created on demand and reused afterwards.
    std::optional<slint::ComponentHandle<TextViewer>> viewer;
    int viewer_section = -1;

    // Install page + settings page. Both are created once and then reused, so the
    // callbacks registered on them (and the raw pointers captured by the worker
    // threads) stay valid for the lifetime of the application.
    std::optional<slint::ComponentHandle<InstallWindow>> install_window;
    std::optional<slint::ComponentHandle<SettingsWindow>> settings_window;
    std::string adb_path;     // resolved once per process
    std::string device;       // serial selected on the install page
    bool install_busy = false;
    std::string install_log;  // accumulates, so earlier steps stay visible
};

// Appends one block to the install page log and scrolls it to the end.
void install_log(const InstallWindow& window, ViewState& state, const std::string& text) {
    state.install_log += text;
    if (!state.install_log.empty() && state.install_log.back() != '\n') state.install_log += '\n';
    window.set_log_text(slint::SharedString(state.install_log));
}

// Diagnostic: APKINFO_DEBUG_ROWS=<file> appends the rendered contents model, which
// is how the tree behaviour is checked without a UI driver.
void dump_rows(const std::vector<InfoRowItem>& rows, const char* tag) {
    if (const char* path = std::getenv("APKINFO_DEBUG_ROWS")) {
        if (FILE* file = std::fopen(path, "ab")) {
            std::fprintf(file, "%s rows=%zu\n", tag, rows.size());
            for (size_t i = 0; i < rows.size(); ++i) {
                const InfoRowItem& row = rows[i];
                const std::string value(row.value);
                size_t lines = 0;
                for (char c : value) lines += (c == '\n') ? 1 : 0;
                std::fprintf(file, "%4zu kind=%d payload=%d ind=%d tog=%d lines=%zu |%s|%.40s|\n", i, row.kind,
                             row.payload, row.indent, row.toggle, lines, std::string(row.label).c_str(),
                             value.c_str());
            }
            std::fclose(file);
        }
    }
}

// Renders one tree node and, when expanded, its subtree, returning the index just
// past that subtree. The flat tree is pre-order with contiguous subtrees, so a
// collapsed node can be skipped with its known size instead of walking it - that
// is what keeps a 10 000 entry package cheap.
size_t append_subtree(const InfoDocument& document, const ViewState& state, size_t index, int depth,
                      std::vector<InfoRowItem>& out) {
    const apkinfo::TreeNode& node = document.tree[index];

    InfoRowItem row = make_row(5);
    row.label = slint::SharedString(node.name.empty() ? document.file_name : node.name);
    row.hint = slint::SharedString(node.path);
    row.indent = depth;
    row.payload = static_cast<int>(index);
    if (node.is_dir) {
        row.value = slint::SharedString(std::to_string(node.children) + " " + tr("items"));
        row.toggle = (index < state.expanded.size() && state.expanded[index]) ? 2 : 1;
    } else {
        row.value = slint::SharedString(compact_size(node.size));
        row.mono = true;
    }
    out.push_back(std::move(row));

    const bool is_expanded = node.is_dir && index < state.expanded.size() && state.expanded[index];
    if (!is_expanded || node.children == 0) {
        return index + (node.subtree ? node.subtree : 1);
    }
    size_t child = index + 1;
    for (uint32_t i = 0; i < node.children; ++i) {
        if (child >= document.tree.size()) break;
        child = append_subtree(document, state, child, depth + 1, out);
    }
    return child;
}

std::string contents_tab_title(const InfoDocument& document) {
    if (document.contents_kind == ContentsKind::Splits) {
        return tr("Splits") + " (" + std::to_string(document.splits.size()) + ")";
    }
    return tr("Contents") + " (" + apkinfo::with_thousands(document.file_count) + " " + tr("entries") + ")";
}

void build_contents_rows(ViewState& state) {
    const InfoDocument& document = state.document;
    std::vector<InfoRowItem> rows;
    if (document.contents_kind == ContentsKind::Splits) {
        rows.reserve(document.splits.size());
        for (const apkinfo::ContentItem& item : document.splits) {
            InfoRowItem row = make_row(6);
            row.label = slint::SharedString(apkinfo::basename(item.name));
            row.value = slint::SharedString(compact_size(item.size));
            std::string hint = tr(item.detail_key);
            if (!item.detail_extra.empty()) hint += " · " + item.detail_extra;
            row.hint = slint::SharedString(hint);
            rows.push_back(std::move(row));
        }
    } else if (!document.tree.empty()) {
        rows.reserve(64);
        size_t index = 0;
        while (index < document.tree.size()) index = append_subtree(document, state, index, 0, rows);
    }
    state.contents_rows = std::move(rows);
    dump_rows(state.contents_rows, "contents");
}

// Information rows: section header, label/value rows, monospace list lines, and one
// embedded text box per long section.
void build_info_rows(ViewState& state) {
    const InfoDocument& document = state.document;
    std::vector<InfoRowItem> rows;
    rows.reserve(96);

    for (size_t section_index = 0; section_index < document.sections.size(); ++section_index) {
        const apkinfo::InfoSection& section = document.sections[section_index];
        const bool is_package_section = section_index == 0;
        if (section.rows.empty() && !is_package_section) continue;

        InfoRowItem header = make_row(0);
        std::string title = tr(section.title);
        if (section.count > 0) title += " (" + std::to_string(section.count) + ")";
        header.value = slint::SharedString(title);
        rows.push_back(std::move(header));

        if (section.collapsible && section.rows.size() > kLongSection) {
            // Too many entries for the page: the first few stay ordinary rows, the
            // rest lives behind a button that opens the viewer window.
            size_t emitted = 0;
            for (const apkinfo::InfoRow& source : section.rows) {
                if (emitted++ >= kPreviewRows) break;
                InfoRowItem row = make_row(source.label.empty() ? 2 : 1);
                row.label = slint::SharedString(source.label.empty() ? std::string() : tr(source.label));
                row.value = slint::SharedString(tr_value(source.value));
                row.hint = slint::SharedString(source.hint);
                row.mono = source.monospace;
                rows.push_back(std::move(row));
            }
            InfoRowItem more = make_row(4);
            more.payload = static_cast<int>(section_index);
            more.label = slint::SharedString(tr("Show all") + " (" + std::to_string(section.rows.size()) + ")");
            rows.push_back(std::move(more));
        } else {
            for (const apkinfo::InfoRow& source : section.rows) {
                InfoRowItem row = make_row(source.label.empty() ? 2 : 1);
                row.label = slint::SharedString(source.label.empty() ? std::string() : tr(source.label));
                row.value = slint::SharedString(tr_value(source.value));
                row.hint = slint::SharedString(source.hint);
                row.mono = source.monospace;
                rows.push_back(std::move(row));
            }
        }

        // The digest is appended by hand: it arrives after parsing, from a worker
        // thread, so it is not part of the parsed document.
        if (is_package_section) {
            InfoRowItem row = make_row(1);
            row.label = slint::SharedString(tr("SHA-256"));
            row.value = slint::SharedString(document.file_hash.empty() ? tr("computing…") : document.file_hash);
            row.mono = true;
            rows.push_back(std::move(row));
        }
    }
    state.info_rows = std::move(rows);
    dump_rows(state.info_rows, "info");
}

void apply_view(MainWindow& ui, ViewState& state) {
    const InfoDocument& document = state.document;

    ui.set_app_title(slint::SharedString(tr_value(document.title)));
    ui.set_app_subtitle(slint::SharedString(document.subtitle));
    ui.set_app_badge(slint::SharedString(tr(document.badge)));
    ui.set_file_name(slint::SharedString(document.file_name));
    ui.set_contents_title(slint::SharedString(contents_tab_title(document)));
    ui.set_has_error(!document.ok && !document.error.empty());
    ui.set_error_text(slint::SharedString(tr(document.error)));
    // "Install" only means something for a package that could be handed to a device.
    ui.set_has_install_target(document.ok && document.kind != ContainerKind::Unknown);

    if (document.icon && document.icon->valid()) {
        slint::SharedPixelBuffer<slint::Rgba8Pixel> buffer(static_cast<uint32_t>(document.icon->width),
                                                          static_cast<uint32_t>(document.icon->height));
        std::memcpy(buffer.begin(), document.icon->rgba.data(), document.icon->rgba.size());
        ui.set_app_icon(slint::Image(buffer));
        ui.set_has_icon(true);
    } else {
        ui.set_has_icon(false);
    }

    build_info_rows(state);
    state.info_model = std::make_shared<slint::VectorModel<InfoRowItem>>(state.info_rows);
    ui.set_rows(state.info_model);

    build_contents_rows(state);
    state.contents_model = std::make_shared<slint::VectorModel<InfoRowItem>>(state.contents_rows);
    ui.set_contents_rows(state.contents_model);

    std::vector<slint::SharedString> warnings;
    warnings.reserve(document.warnings.size());
    for (const std::string& warning : document.warnings) warnings.emplace_back(warning);
    ui.set_warnings(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(warnings)));

    char status[256];
    if (document.ok) {
        std::snprintf(status, sizeof status, "%s · %s · %.0f ms", tr(apkinfo::container_kind_name(document.kind)).c_str(),
                      compact_size(document.file_size).c_str(), state.elapsed_ms);
    } else {
        std::snprintf(status, sizeof status, "%s", tr("could not read the file").c_str());
    }
    ui.set_status_text(slint::SharedString(status));

    set_window_icon(document.ok ? document.kind : ContainerKind::Unknown);
}

// Finds the viewer's own native window (the process's titled window that is not the
// main one).
HWND find_viewer_window(HWND main_window) {
    struct Search {
        DWORD process_id;
        HWND skip;
        HWND found;
    } search{GetCurrentProcessId(), main_window, nullptr};

    static const auto callback = [](HWND hwnd, LPARAM parameter) -> BOOL {
        auto* state = reinterpret_cast<Search*>(parameter);
        DWORD process_id = 0;
        GetWindowThreadProcessId(hwnd, &process_id);
        if (process_id != state->process_id || hwnd == state->skip) return TRUE;
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (GetWindowTextLengthW(hwnd) == 0) return TRUE;
        state->found = hwnd;
        return FALSE;
    };
    EnumWindows(callback, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// Centres the viewer over the main window's *current* rectangle. Called after show():
// a window has no size before it is mapped, and some platforms reposition a window
// once more when it appears, so the placement is verified and re-applied.
void position_viewer_over_main(slint::ComponentHandle<TextViewer>& viewer, int attempt = 0) {
    const HWND main_window = main_window_handle();
    if (!main_window) return;

    HWND viewer_window = find_viewer_window(main_window);
    RECT main_rect{};
    RECT viewer_rect{};
    if (!viewer_window || !GetWindowRect(main_window, &main_rect) || !GetWindowRect(viewer_window, &viewer_rect)) {
        if (attempt < 20) {
            slint::Timer::single_shot(std::chrono::milliseconds(50),
                                      [&viewer, attempt]() { position_viewer_over_main(viewer, attempt + 1); });
        }
        return;
    }

    // Use the real rectangles: right after show() the window still has its minimum
    // size, so Slint's reported size would centre the pre-layout geometry.
    const LONG width = viewer_rect.right - viewer_rect.left;
    const LONG height = viewer_rect.bottom - viewer_rect.top;
    const LONG main_width = main_rect.right - main_rect.left;
    const LONG main_height = main_rect.bottom - main_rect.top;
    if (width <= 0 || height <= 0 || width >= main_width || height >= main_height) {
        // still growing into its layout
        if (attempt < 20) {
            slint::Timer::single_shot(std::chrono::milliseconds(50),
                                      [&viewer, attempt]() { position_viewer_over_main(viewer, attempt + 1); });
        }
        return;
    }

    LONG x = main_rect.left + (main_width - width) / 2;
    LONG y = main_rect.top + (main_height - height) / 2;

    MONITORINFO monitor{};
    monitor.cbSize = sizeof monitor;
    if (GetMonitorInfoW(MonitorFromWindow(main_window, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const LONG max_x = std::max(monitor.rcWork.left, monitor.rcWork.right - width);
        const LONG max_y = std::max(monitor.rcWork.top, monitor.rcWork.bottom - height);
        x = std::min(std::max(x, monitor.rcWork.left), max_x);
        y = std::min(std::max(y, monitor.rcWork.top), max_y);
    }

    if (const char* trace = std::getenv("APKINFO_DEBUG_PLACE")) {
        if (FILE* file = std::fopen(trace, "ab")) {
            std::fprintf(file,
                         "main=(%ld,%ld)-(%ld,%ld) viewer=(%ld,%ld)-(%ld,%ld) target=(%ld,%ld) attempt=%d\n",
                         main_rect.left, main_rect.top, main_rect.right, main_rect.bottom, viewer_rect.left,
                         viewer_rect.top, viewer_rect.right, viewer_rect.bottom, static_cast<long>(x),
                         static_cast<long>(y), attempt);
            std::fclose(file);
        }
    }
    if (std::abs(viewer_rect.left - x) <= 4 && std::abs(viewer_rect.top - y) <= 4) {
        // Even when the position is already right, make sure the window is in front:
        // a viewer that opens behind the main window looks broken.
        BringWindowToTop(viewer_window);
        SetForegroundWindow(viewer_window);
        SetFocus(viewer_window);
        return;
    }

    // Move the native window: Slint's own set_position() happens before the window is
    // mapped and gets overridden by the platform's initial placement. SWP_NOACTIVATE is
    // deliberately absent so the new window also comes to the front.
    SetWindowPos(viewer_window, HWND_TOP, static_cast<int>(x), static_cast<int>(y), 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(viewer_window);
    SetForegroundWindow(viewer_window);
    SetFocus(viewer_window);
    if (attempt < 10) {
        slint::Timer::single_shot(std::chrono::milliseconds(60),
                                  [&viewer, attempt]() { position_viewer_over_main(viewer, attempt + 1); });
    }
}

// apply_chrome / apply_empty_state are defined further down; the install and settings
// helpers below use them.
void apply_chrome(MainWindow& ui);
void apply_empty_state(MainWindow& ui);

// ---------------------------------------------------------------------------
// Install page and settings page
//
// Both windows exist for the lifetime of the process (created on first use), so the
// callbacks and the worker threads below can hold on to them. Every adb call runs on
// a worker thread and hands its result back through the event loop: installing a
// 250 MB package takes long enough that a blocked UI would look like a crash.
// ---------------------------------------------------------------------------

// Placeholder reference shown in the settings page.
std::string placeholder_help_text() {
    std::string text;
    for (const auto& token : apkinfo::install::placeholder_tokens()) {
        text += token.first;
        // align the meanings in a column
        text += std::string(token.first.size() < 16 ? 16 - token.first.size() : 1, ' ');
        text += "  " + tr(token.second) + "\n";
    }
    return text;
}

void apply_install_chrome(InstallWindow& window, const ViewState& state) {
    window.set_window_title(slint::SharedString(tr("Install")));
    window.set_label_devices(slint::SharedString(tr("Devices")));
    window.set_label_refresh(slint::SharedString(tr("Refresh")));
    window.set_label_discover(slint::SharedString(tr("Discover")));
    window.set_label_wireless(slint::SharedString(tr("Wireless debugging")));
    window.set_label_endpoint(slint::SharedString(tr("Address (host:port)")));
    window.set_label_code(slint::SharedString(tr("Pairing code")));
    window.set_label_connect(slint::SharedString(tr("Connect")));
    window.set_label_disconnect(slint::SharedString(tr("Disconnect")));
    window.set_label_pair(slint::SharedString(tr("Pair")));
    window.set_label_options(slint::SharedString(tr("Install options")));
    window.set_label_replace(slint::SharedString(tr("Replace existing (-r)")));
    window.set_label_grant(slint::SharedString(tr("Grant all permissions (-g)")));
    window.set_label_downgrade(slint::SharedString(tr("Allow downgrade (-d)")));
    window.set_label_log(slint::SharedString(tr("adb output")));
    window.set_label_install(slint::SharedString(tr("Install")));
    window.set_label_close(slint::SharedString(tr("Close")));
    window.set_label_empty(slint::SharedString(tr("No device - connect one, or pair over Wi-Fi")));
    window.set_hint_endpoint(slint::SharedString("192.168.1.5:5555"));
    window.set_hint_code(slint::SharedString(tr("6-digit code")));

    const apkinfo::install::Settings& settings = apkinfo::config().install;
    window.set_replace(settings.options.replace);
    window.set_grant(settings.options.grant);
    window.set_downgrade(settings.options.downgrade);

    if (state.adb_path.empty()) {
        window.set_adb_line(slint::SharedString(tr("adb was not found - set its path in the settings")));
    } else {
        window.set_adb_line(slint::SharedString("adb: " + state.adb_path));
    }
    if (state.device.empty()) {
        window.set_device_hint(slint::SharedString(tr("no device selected")));
    } else {
        window.set_device_hint(slint::SharedString(tr("Selected") + ": " + state.device));
    }
}

void apply_settings_chrome(SettingsWindow& window, const ViewState& state) {
    window.set_window_title(slint::SharedString(tr("Settings")));
    window.set_label_language_section(slint::SharedString(tr("Language")));
    window.set_label_language(slint::SharedString(tr("Interface language")));
    window.set_label_android_section(slint::SharedString(tr("Install button - Android packages")));
    window.set_label_android_mode(slint::SharedString(tr("Install method")));
    window.set_label_android_command(slint::SharedString(tr("Command line")));
    window.set_label_alpine_section(slint::SharedString(tr("Install button - Alpine packages")));
    window.set_label_alpine_command(slint::SharedString(tr("Command line")));
    window.set_label_adb(slint::SharedString(tr("adb path (empty = search PATH and the SDK)")));
    window.set_label_placeholders(slint::SharedString(tr("Placeholders")));
    window.set_label_preview(slint::SharedString(tr("Preview for the open package")));
    window.set_label_save(slint::SharedString(tr("Save")));
    window.set_label_cancel(slint::SharedString(tr("Cancel")));
    window.set_placeholder_help(slint::SharedString(placeholder_help_text()));

    const apkinfo::install::Settings& settings = apkinfo::config().install;
    window.set_android_mode_index(settings.android_mode == apkinfo::install::Mode::Command ? 1 : 0);
    window.set_android_command(slint::SharedString(settings.android_command));
    window.set_alpine_command(slint::SharedString(settings.alpine_command));
    window.set_adb_path(slint::SharedString(settings.adb_path));

    int language_index = 0;
    switch (apkinfo::i18n::preference()) {
        case apkinfo::i18n::Preference::Chinese:
            language_index = 1;
            break;
        case apkinfo::i18n::Preference::English:
            language_index = 2;
            break;
        default:
            break;
    }
    window.set_language_index(language_index);

    if (state.loaded) {
        const std::string command =
            apkinfo::install::command_for_package(settings, state.document, state.adb_path, state.device);
        const bool alpine = state.document.kind == ContainerKind::AlpinePackage;
        window.set_preview_text(slint::SharedString(command));
        // A command is not run through a shell, so what works - and what needs an
        // explicit cmd /c - is said right under the preview.
        std::string note = tr("A command is started directly (no cmd.exe), in the package's directory. "
                              "\"&&\" chains steps; pipes, redirection and %VAR% need \"cmd /c ...\".");
        if (alpine) {
            note += "\n" + tr("Alpine packages always use this command - the built-in installer is Android only.");
        } else if (state.document.kind == ContainerKind::AndroidSplitBundle) {
            note += "\n" + tr("A split bundle is installed with adb install-multiple in the built-in mode.");
        }
        window.set_note_text(slint::SharedString(note));
    } else {
        window.set_preview_text(slint::SharedString(tr("(no package open)")));
        window.set_note_text(slint::SharedString(""));
    }
}

// Re-reads the device list on a worker thread.
void refresh_devices(std::shared_ptr<ViewState> state, slint::ComponentHandle<InstallWindow> window) {
    if (state->install_busy) return;
    state->install_busy = true;
    window->set_busy(true);
    const std::string adb = state->adb_path;
    std::thread([state, window, adb]() {
        const std::vector<apkinfo::adb::Device> devices = apkinfo::adb::devices(adb);
        slint::invoke_from_event_loop([state, window, devices]() {
            std::vector<DeviceItem> items;
            items.reserve(devices.size());
            int selected = -1;
            for (size_t i = 0; i < devices.size(); ++i) {
                DeviceItem item;
                item.label = slint::SharedString(devices[i].label());
                item.serial = slint::SharedString(devices[i].serial);
                item.state = slint::SharedString(devices[i].state);
                item.usable = devices[i].usable();
                items.push_back(std::move(item));
                // keep the previous choice when the device is still there
                if (!state->device.empty() && devices[i].serial == state->device) {
                    selected = static_cast<int>(i);
                } else if (selected < 0 && devices[i].usable()) {
                    selected = static_cast<int>(i);
                }
            }
            window->set_devices(std::make_shared<slint::VectorModel<DeviceItem>>(std::move(items)));
            if (selected >= 0) {
                state->device = std::string(window->get_devices()->row_data(static_cast<size_t>(selected))->serial);
                window->set_device_index(selected);
                window->set_device_hint(
                    slint::SharedString(tr("Selected") + ": " + std::string(std::string(state->device))));
            } else {
                state->device.clear();
                window->set_device_index(-1);
                window->set_device_hint(slint::SharedString(tr("no device selected")));
            }
            state->install_busy = false;
            window->set_busy(false);
        });
    }).detach();
}

// adb connect / disconnect / pair, then refresh the list so the result is visible.
void run_adb_step(std::shared_ptr<ViewState> state, slint::ComponentHandle<InstallWindow> window,
                  std::string title, std::function<apkinfo::adb::Result(const std::string& adb)> action) {
    if (state->install_busy) return;
    state->install_busy = true;
    window->set_busy(true);
    const std::string adb = state->adb_path;
    install_log(*window, *state, "> " + title);
    std::thread([state, window, adb, title, action = std::move(action)]() {
        const apkinfo::adb::Result result = action(adb);
        slint::invoke_from_event_loop([state, window, result]() {
            std::string text = result.output;
            if (text.empty()) text = result.started ? "(no output)" : result.error;
            install_log(*window, *state, text);
            state->install_busy = false;
            window->set_busy(false);
            // the device list usually changed (a new wireless device appeared)
            refresh_devices(state, window);
        });
    }).detach();
}

// The Android built-in install: adb install / install-multiple on a worker thread.
void run_install(std::shared_ptr<ViewState> state, slint::ComponentHandle<InstallWindow> window) {
    if (state->install_busy) return;
    if (state->adb_path.empty()) {
        install_log(*window, *state, tr("adb was not found - set its path in the settings"));
        return;
    }
    if (state->device.empty()) {
        install_log(*window, *state, tr("select a device first"));
        return;
    }

    apkinfo::install::Settings settings = apkinfo::config().install;
    settings.options.replace = window->get_replace();
    settings.options.grant = window->get_grant();
    settings.options.downgrade = window->get_downgrade();

    state->install_busy = true;
    window->set_busy(true);

    const std::string adb = state->adb_path;
    const std::string device = state->device;
    const std::string path = state->path;
    const ContainerKind kind = state->document.kind;
    install_log(*window, *state, tr("Installing") + ": " + state->document.file_name);

    std::thread([state, window, adb, device, path, kind, settings]() {
        const apkinfo::install::Outcome outcome =
            apkinfo::install::run_builtin(settings, adb, device, path, kind);
        slint::invoke_from_event_loop([state, window, outcome, device]() {
            install_log(*window, *state, outcome.output);
            if (outcome.success) install_log(*window, *state, tr("Installed successfully."));
            // remember the device that was used, so it is preselected next time
            if (!device.empty() && apkinfo::config().install.device != device) {
                apkinfo::mutable_config().install.device = device;
                apkinfo::save_config();
            }
            state->install_busy = false;
            window->set_busy(false);
            refresh_devices(state, window);
        });
    }).detach();
}

// Runs the configured command line for whatever package is open.
void run_install_command(std::shared_ptr<ViewState> state, MainWindow* ui) {
    const apkinfo::install::Settings& settings = apkinfo::config().install;
    const std::string command =
        apkinfo::install::command_for_package(settings, state->document, state->adb_path, state->device);
    if (command.empty()) {
        ui->set_status_text(slint::SharedString(tr("no install command is configured")));
        return;
    }
    ui->set_status_text(slint::SharedString(tr("running the install command…")));

    // The output goes into the viewer window, which already shows long selectable text.
    if (!state->viewer.has_value()) state->viewer.emplace(TextViewer::create());
    auto viewer = *state->viewer;
    viewer->set_viewer_title(slint::SharedString(tr("Install command output")));
    viewer->set_viewer_hint(slint::SharedString(""));
    viewer->set_label_close(slint::SharedString(tr("Close")));
    viewer->set_viewer_text(slint::SharedString(command + "\n\n" + tr("running…")));
    viewer->show();

    const apkinfo::install::Context context =
        apkinfo::install::make_context(state->document, state->adb_path, state->device);
    std::thread([state, viewer, command, context]() {
        const apkinfo::install::Outcome outcome = apkinfo::install::run_command(command, context);
        const std::string text = outcome.command + "\n\n" + outcome.output;
        slint::invoke_from_event_loop([viewer, text]() mutable {
            viewer->set_viewer_text(slint::SharedString(text));
            viewer->show();
        });
    }).detach();
}

// Centres a secondary window over the main window: the window is mapped first (so it
// has a real size), then moved. Retried because the size is still 0 right after
// show() on the first event-loop turn.
template <typename Component>
void centre_over_main(slint::ComponentHandle<Component> component, std::shared_ptr<int> attempts = nullptr) {
    if (!attempts) attempts = std::make_shared<int>(0);
    const HWND main_window = main_window_handle();
    if (!main_window) return;

    RECT main_rect{};
    if (!GetWindowRect(main_window, &main_rect)) return;

    // The window that was just shown is the foreground one (Slint activates it); fall
    // back to "any other titled window of this process" when focus went elsewhere.
    HWND self = GetForegroundWindow();
    DWORD owner = 0;
    if (self) GetWindowThreadProcessId(self, &owner);
    if (!self || owner != GetCurrentProcessId() || self == main_window) {
        self = find_viewer_window(main_window);
    }
    RECT self_rect{};
    const bool have = self && self != main_window && GetWindowRect(self, &self_rect);
    const LONG width = have ? self_rect.right - self_rect.left : 0;
    const LONG height = have ? self_rect.bottom - self_rect.top : 0;
    if (!have || width < 240 || height < 200) {
        if (*attempts < 60) {
            ++*attempts;
            slint::Timer::single_shot(std::chrono::milliseconds(50), [component, attempts]() {
                centre_over_main(component, attempts);
            });
        }
        return;
    }

    const LONG main_width = main_rect.right - main_rect.left;
    const LONG main_height = main_rect.bottom - main_rect.top;
    LONG x = main_rect.left + (main_width - width) / 2;
    LONG y = main_rect.top + (main_height - height) / 2;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof monitor;
    if (GetMonitorInfoW(MonitorFromWindow(main_window, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const LONG max_x = std::max(monitor.rcWork.left, monitor.rcWork.right - width);
        const LONG max_y = std::max(monitor.rcWork.top, monitor.rcWork.bottom - height);
        x = std::min(std::max(x, monitor.rcWork.left), max_x);
        y = std::min(std::max(y, monitor.rcWork.top), max_y);
    }
    SetWindowPos(self, HWND_TOP, static_cast<int>(x), static_cast<int>(y), 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(self);
    SetForegroundWindow(self);
}

// Builds and shows the install page (created once, then reused).
void show_install_window(std::shared_ptr<ViewState> state, slint::ComponentHandle<MainWindow> host) {
    if (!state->install_window) {
        state->install_window.emplace(InstallWindow::create());
        slint::ComponentHandle<InstallWindow> window = *state->install_window;

        window->on_close_requested([window]() mutable { window->hide(); });
        window->on_endpoint_edited([window](slint::SharedString text) { window->set_endpoint(text); });
        window->on_code_edited([window](slint::SharedString text) { window->set_pairing_code(text); });
        window->on_device_selected([state, window](int index) {
            const auto model = window->get_devices();
            if (index < 0 || static_cast<size_t>(index) >= model->row_count()) return;
            state->device = std::string(model->row_data(static_cast<size_t>(index))->serial);
            window->set_device_index(index);
            window->set_device_hint(slint::SharedString(tr("Selected") + ": " + state->device));
        });
        window->on_refresh_requested([state, window]() { refresh_devices(state, window); });
        window->on_discover_requested([state, window]() {
            if (state->install_busy) return;
            state->install_busy = true;
            window->set_busy(true);
            const std::string adb = state->adb_path;
            std::thread([state, window, adb]() {
                const std::vector<apkinfo::adb::MdnsService> services = apkinfo::adb::mdns_services(adb);
                slint::invoke_from_event_loop([state, window, services]() {
                    std::string text;
                    std::string pairing;
                    std::string connecting;
                    for (const apkinfo::adb::MdnsService& service : services) {
                        const bool is_pairing = service.type.find("pairing") != std::string::npos;
                        text += (is_pairing ? tr("pairing") : tr("connect")) + ":  " + service.endpoint + "  (" +
                                service.name + ")\n";
                        if (is_pairing && pairing.empty()) pairing = service.endpoint;
                        if (!is_pairing && connecting.empty()) connecting = service.endpoint;
                    }
                    if (text.empty()) text = tr("no service discovered (wireless debugging must be on)");
                    window->set_mdns_text(slint::SharedString(text));
                    // fill the field with what the next step needs
                    if (std::string(window->get_endpoint()).empty()) {
                        const std::string suggestion = pairing.empty() ? connecting : pairing;
                        if (!suggestion.empty()) window->set_endpoint(slint::SharedString(suggestion));
                    }
                    state->install_busy = false;
                    window->set_busy(false);
                });
            }).detach();
        });
        window->on_connect_requested([state, window]() {
            const std::string endpoint(window->get_endpoint());
            if (endpoint.empty()) {
                install_log(*window, *state, tr("enter the address first"));
                return;
            }
            run_adb_step(state, window, "adb connect " + endpoint,
                         [endpoint](const std::string& adb) { return apkinfo::adb::connect(adb, endpoint); });
        });
        window->on_disconnect_requested([state, window]() {
            const std::string endpoint(window->get_endpoint());
            run_adb_step(state, window, "adb disconnect " + endpoint,
                         [endpoint](const std::string& adb) { return apkinfo::adb::disconnect(adb, endpoint); });
        });
        window->on_pair_requested([state, window]() {
            const std::string endpoint(window->get_endpoint());
            const std::string code(window->get_pairing_code());
            if (endpoint.empty() || code.empty()) {
                install_log(*window, *state, tr("the address and the pairing code are both needed"));
                return;
            }
            run_adb_step(state, window, "adb pair " + endpoint,
                         [endpoint, code](const std::string& adb) { return apkinfo::adb::pair(adb, endpoint, code); });
        });
        window->on_install_requested([state, window]() { run_install(state, window); });
    }

    slint::ComponentHandle<InstallWindow> window = *state->install_window;
    if (state->adb_path.empty()) {
        state->adb_path = apkinfo::adb::find_adb(apkinfo::config().install.adb_path);
    }
    if (state->device.empty() && !apkinfo::config().install.device.empty()) {
        state->device = apkinfo::config().install.device;
    }
    apply_install_chrome(*window, *state);
    std::string package_line = tr_value(state->document.title);
    if (!state->document.subtitle.empty()) package_line += "  ·  " + state->document.subtitle;
    window->set_package_line(slint::SharedString(package_line));
    window->show();
    centre_over_main(*state->install_window);
    refresh_devices(state, *state->install_window);
}

// Builds and shows the settings page.
void show_settings_window(std::shared_ptr<ViewState> state, slint::ComponentHandle<MainWindow> host) {
    if (!state->settings_window) {
        state->settings_window.emplace(SettingsWindow::create());
        slint::ComponentHandle<SettingsWindow> window = *state->settings_window;

        window->on_cancel_requested([window]() mutable { window->hide(); });
        window->on_android_command_edited([window](slint::SharedString text) { window->set_android_command(text); });
        window->on_alpine_command_edited([window](slint::SharedString text) { window->set_alpine_command(text); });
        window->on_adb_path_edited([window](slint::SharedString text) { window->set_adb_path(text); });
        window->on_save_requested([window, state, host]() mutable {
            const int language_index = window->get_language_index();
            apkinfo::i18n::Preference preference = apkinfo::i18n::Preference::Auto;
            if (language_index == 1) preference = apkinfo::i18n::Preference::Chinese;
            if (language_index == 2) preference = apkinfo::i18n::Preference::English;
            apkinfo::i18n::set_preference(preference);

            apkinfo::Config& config = apkinfo::mutable_config();
            config.language = static_cast<apkinfo::LanguagePreference>(preference);
            config.install.android_mode = window->get_android_mode_index() == 1 ? apkinfo::install::Mode::Command
                                                                                : apkinfo::install::Mode::Builtin;
            config.install.android_command = std::string(window->get_android_command());
            config.install.alpine_command = std::string(window->get_alpine_command());
            config.install.adb_path = std::string(window->get_adb_path());
            apkinfo::save_config();

            // the adb path may have changed
            state->adb_path = apkinfo::adb::find_adb(config.install.adb_path);

            apply_chrome(*host);
            if (state->loaded) {
                apply_view(*host, *state);
            } else {
                apply_empty_state(*host);
            }
            if (state->install_window) apply_install_chrome(**state->install_window, *state);
            apply_settings_chrome(*window, *state);
            window->hide();
        });
    }

    slint::ComponentHandle<SettingsWindow> window = *state->settings_window;
    apply_settings_chrome(*window, *state);
    window->show();
    centre_over_main(*state->settings_window);
}

// Opens (or refreshes and re-shows) the viewer window for one section.
void show_section_window(ViewState& state, size_t section_index) {
    if (section_index >= state.document.sections.size()) return;
    const apkinfo::InfoSection& section = state.document.sections[section_index];

    if (!state.viewer.has_value()) {
        slint::ComponentHandle<TextViewer> created = TextViewer::create();
        // The window's own close button and its X both just hide it: the section is
        // still in memory, so reopening is instant.
        TextViewer* raw = &*created;
        created->on_close_requested([raw]() { raw->hide(); });
        state.viewer = created;
    }
    state.viewer_section = static_cast<int>(section_index);

    std::string title = tr(section.title);
    if (section.count > 0) title += " (" + std::to_string(section.count) + ")";
    std::string hint = std::to_string(section.rows.size()) + " " + tr("lines");

    auto& window = *state.viewer;
    window->set_viewer_title(slint::SharedString(title));
    window->set_viewer_hint(slint::SharedString(hint));
    window->set_label_close(slint::SharedString(tr("Close")));
    window->set_viewer_text(slint::SharedString(section_text(section)));

    // Show first, then position: before show() the window has no size yet, and Slint
    // would place it itself. The retry covers the case where the size is still 0 on
    // the first event-loop iteration.
    window->show();
    position_viewer_over_main(*state.viewer);
}

// Refresh the Information rows (digest arrival, language switch).
void rebuild_info_rows(MainWindow& ui, ViewState& state) {
    build_info_rows(state);
    state.info_model = std::make_shared<slint::VectorModel<InfoRowItem>>(state.info_rows);
    ui.set_rows(state.info_model);
    // keep an open viewer in sync with the new language
    if (state.viewer.has_value() && state.viewer_section >= 0) {
        show_section_window(state, static_cast<size_t>(state.viewer_section));
    }
}

void apply_chrome(MainWindow& ui) {
    ui.set_label_open(slint::SharedString(tr("Open package…")));
    ui.set_label_reload(slint::SharedString(tr("Reload")));
    ui.set_label_information(slint::SharedString(tr("Information")));
    ui.set_label_notes(slint::SharedString(tr("Notes")));
    ui.set_label_no_preview(slint::SharedString(tr("no preview")));
    ui.set_label_copy_all(slint::SharedString(tr("Copy all")));
    ui.set_label_dump(slint::SharedString(tr("Dump…")));
    ui.set_label_export_icon(slint::SharedString(tr("Export icon…")));
    ui.set_label_install(slint::SharedString(tr("Install")));
    ui.set_label_settings(slint::SharedString(tr("Settings")));
}

// The texts of the "no package" state. Kept in one place because they have to be
// re-applied when the language changes while nothing is loaded: apply_view is what
// normally pushes localised text, and it does not run without a document.
void apply_empty_state(MainWindow& ui) {
    ui.set_status_text(slint::SharedString(tr("Open an .apk, .apks, .apkm or Alpine .apk package")));
    ui.set_app_title(slint::SharedString(tr("Nothing loaded")));
    ui.set_app_subtitle(slint::SharedString(tr("Use “Open package…” or pass a path on the command line.")));
    // the tab label would otherwise stay at its English default
    ui.set_contents_title(slint::SharedString(tr("Contents")));
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

// Fills in SHA-256 without blocking the UI: the file is hashed on a worker
// thread and the text is refreshed through the event loop when it is ready.
void start_hash(ViewState* state, MainWindow* ui) {
    if (state->path.empty()) return;
    const std::string path = state->path;
    std::thread([state, ui, path]() {
        const std::string hash = apkinfo::sha256_hex_file(path);
        slint::invoke_from_event_loop([state, ui, hash]() {
            if (hash.empty() || state->path.empty()) return;
            state->document.file_hash = hash;
            rebuild_info_rows(*ui, *state);
            trim_working_set();
        });
    }).detach();
}

}  // namespace

int run(int argument_count, wchar_t** arguments) {
    // The config file holds the language preference, and it has to be applied *first*:
    // set_preference() clears the "forced for this run" flag, so applying it afterwards
    // would silently undo --lang / APKINFO_LANG.
    apkinfo::i18n::set_preference(static_cast<apkinfo::i18n::Preference>(apkinfo::config().language));

    // APKINFO_LANG forces the language for this run, then --lang overrides even that;
    // anything else is treated as the package path to open.
    if (const char* forced = std::getenv("APKINFO_LANG"); forced && *forced) {
        apkinfo::i18n::Language parsed = apkinfo::i18n::current();
        if (apkinfo::i18n::from_code(forced, &parsed)) apkinfo::i18n::set_language(parsed);
    }

    std::string path_to_open;
    for (int i = 1; i < argument_count; ++i) {
        const std::string argument = to_utf8(arguments[i]);
        if (argument == "--lang" && i + 1 < argument_count) {
            apkinfo::i18n::Language parsed = apkinfo::i18n::current();
            if (apkinfo::i18n::from_code(to_utf8(arguments[++i]), &parsed)) {
                apkinfo::i18n::set_language(parsed);
            }
            continue;
        }
        if (argument.rfind("--lang=", 0) == 0) {
            apkinfo::i18n::Language parsed = apkinfo::i18n::current();
            if (apkinfo::i18n::from_code(argument.substr(7), &parsed)) {
                apkinfo::i18n::set_language(parsed);
            }
            continue;
        }
        if (path_to_open.empty()) path_to_open = argument;
    }

    auto handle = MainWindow::create();
    MainWindow* ui = &*handle;
    auto state = std::make_shared<ViewState>();

    // Parse once; everything after this (language switch included) is re-render.
    auto load = [ui, state](const std::string& path) {
        double elapsed_ms = 0;
        state->document = apkinfo::load_package(path, &elapsed_ms);
        state->elapsed_ms = elapsed_ms;
        state->path = path;
        state->loaded = true;
        state->expanded.assign(state->document.tree.size(), 0);
        if (!state->expanded.empty()) state->expanded[0] = 1;  // show the top level
        ui->set_tab(0);
        apply_view(*ui, *state);
        trim_working_set();
        start_hash(state.get(), ui);
        // Hidden switch for the automated UI smoke check: start on the contents tab.
        wchar_t start_tab[8]{};
        if (GetEnvironmentVariableW(L"APKINFO_START_TAB", start_tab, 8) > 0 && start_tab[0] == L'1') {
            ui->set_tab(1);
        }
    };

    handle->on_open_requested([load]() {
        const std::string path = apkinfo::open_package_dialog();
        if (!path.empty()) load(path);
    });

    handle->on_reload_requested([load, state]() {
        if (!state->path.empty()) load(state->path);
    });

    // Language dropdown: 0 = follow the system, 1 = 中文, 2 = English. The choice is
    // "Install": the built-in page for Android packages, the configured command line for
    // Alpine packages (and for Android packages when the settings say so).
    handle->on_install_requested([ui, handle, state]() {
        if (!state->loaded || state->document.kind == ContainerKind::Unknown) return;
        const apkinfo::install::Settings& settings = apkinfo::config().install;
        const bool use_builtin = state->document.kind != ContainerKind::AlpinePackage &&
                                 settings.android_mode == apkinfo::install::Mode::Builtin;
        if (!use_builtin) {
            run_install_command(state, &*ui);
            return;
        }
        show_install_window(state, handle);
    });

    // "Settings": language, install behaviour, adb path.
    handle->on_settings_requested([handle, state]() { show_settings_window(state, handle); });
    // "Copy all" puts the whole localised report on the clipboard and confirms on
    // the button itself - the status bar keeps showing the package information.
    handle->on_copy_all_requested([ui, state]() {
        if (!state->loaded) return;
        if (!apkinfo::set_clipboard_text(apkinfo::build_text_report(state->document, true))) return;
        ui->set_label_copy_all(slint::SharedString(tr("Copied")));
        MainWindow* window = ui;
        slint::Timer::single_shot(std::chrono::milliseconds(kCopiedFeedbackMs),
                                  [window]() { apply_chrome(*window); });
    });

    // "Dump…" writes the same report to a file; the extension picks the format.
    handle->on_dump_requested([ui, state]() {
        if (!state->loaded) return;
        std::string base = state->document.file_name;
        if (base.empty()) base = "package";
        const std::string path = apkinfo::save_report_dialog(base + ".txt");
        if (path.empty()) return;

        // A dump should be complete: finish the digest now if the worker thread
        // has not produced it yet.
        if (state->document.file_hash.empty() && !state->path.empty()) {
            const std::string hash = apkinfo::sha256_hex_file(state->path);
            if (!hash.empty()) {
                state->document.file_hash = hash;
                rebuild_info_rows(*ui, *state);
            }
        }
        const bool as_json = apkinfo::ends_with(apkinfo::to_lower(path), ".json");
        const std::string body = as_json ? apkinfo::build_json_report(state->document)
                                         : apkinfo::build_text_report(state->document, true);
        if (FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fwrite(body.data(), 1, body.size(), file);
            std::fclose(file);
        }
    });

    // The "show all (N)" button opens the viewer window for that section.
    handle->on_info_row_clicked([state](int index) {
        if (index < 0 || static_cast<size_t>(index) >= state->info_rows.size()) return;
        const InfoRowItem& row = state->info_rows[static_cast<size_t>(index)];
        if (row.kind != 4 || row.payload < 0) return;
        show_section_window(*state, static_cast<size_t>(row.payload));
    });

    // "Export icon…" writes the extracted launcher icon as a PNG.
    handle->on_export_icon_requested([ui, state]() {
        if (!state->loaded || !state->document.icon || !state->document.icon->valid()) return;
        std::string base = state->document.title.empty() ? state->document.file_name : state->document.title;
        // strip characters Windows does not accept in a file name
        for (char& c : base) {
            if (std::strchr("\\/:*?\"<>|", c) != nullptr || static_cast<unsigned char>(c) < 0x20) c = '_';
        }
        if (base.empty()) base = "icon";
        const std::string path = apkinfo::save_icon_dialog(base + ".png");
        if (path.empty()) return;

        std::wstring wide(path.begin(), path.end());
        const int length = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (length > 0) {
            wide.assign(static_cast<size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), length);
            wide.resize(static_cast<size_t>(length) - 1);
        }
        if (!apkinfo::write_png(*state->document.icon, wide)) {
            std::fprintf(stderr, "could not write %s\n", path.c_str());
        }
    });

    // Tree rows expand/collapse: only the affected slice of the model changes.
    handle->on_contents_row_clicked([ui, state](int index) {
        if (index < 0 || static_cast<size_t>(index) >= state->contents_rows.size()) return;
        const InfoRowItem row = state->contents_rows[static_cast<size_t>(index)];
        if (row.kind != 5 || row.toggle == 0 || row.payload < 0) return;
        const size_t node = static_cast<size_t>(row.payload);
        if (node >= state->expanded.size() || !state->contents_model) return;
        const bool was_expanded = state->expanded[node] != 0;
        state->expanded[node] = was_expanded ? 0 : 1;
        const size_t at = static_cast<size_t>(index);
        const int depth = row.indent;

        // Rows of this node's visible descendants, exactly as append_subtree would
        // produce them - used both to insert and to remove.
        std::vector<InfoRowItem> children;
        const apkinfo::TreeNode& tree_node = state->document.tree[node];
        size_t child = tree_node.first_child;
        for (uint32_t i = 0; i < tree_node.children; ++i) {
            if (child >= state->document.tree.size()) break;
            child = append_subtree(state->document, *state, child, depth + 1, children);
        }

        if (was_expanded) {
            if (!children.empty()) {
                for (size_t k = children.size(); k > 0; --k) state->contents_model->erase(at + k);
                state->contents_rows.erase(state->contents_rows.begin() + static_cast<long>(at) + 1,
                                           state->contents_rows.begin() + static_cast<long>(at + 1 + children.size()));
            }
        } else {
            size_t insert_at = at + 1;
            for (const InfoRowItem& child_row : children) {
                state->contents_model->insert(insert_at, child_row);
                state->contents_rows.insert(state->contents_rows.begin() + static_cast<long>(insert_at), child_row);
                ++insert_at;
            }
        }
        // refresh the expander glyph of the row itself
        InfoRowItem updated = row;
        updated.toggle = was_expanded ? 1 : 2;
        state->contents_model->set_row_data(at, updated);
        state->contents_rows[at] = updated;
        dump_rows(state->contents_rows, "contents-after-toggle");
    });

    // Hidden switch for the perf check: cycle the language a few times by itself,
    // so the re-render cost can be measured without driving the mouse.
    if (const char* auto_switch = std::getenv("APKINFO_AUTO_SWITCH_MS")) {
        const int delay = std::atoi(auto_switch);
        if (delay > 0) {
            auto toggles_left = std::make_shared<int>(4);
            auto cycle = std::make_shared<std::function<void()>>();
            *cycle = [ui, state, delay, toggles_left, cycle]() {
                if ((*toggles_left)-- <= 0) return;
                const auto started = std::chrono::steady_clock::now();
                apkinfo::i18n::toggle_language();
                apply_chrome(*ui);
                if (state->loaded) apply_view(*ui, *state);
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                std::fprintf(stderr, "language switch (%s): %.2f ms\n",
                             apkinfo::i18n::code(apkinfo::i18n::current()), ms);
                slint::Timer::single_shot(std::chrono::milliseconds(delay), *cycle);
            };
            slint::Timer::single_shot(std::chrono::milliseconds(delay), *cycle);
        }
    }

    apply_chrome(*ui);
    if (!path_to_open.empty()) {
        load(path_to_open);
    } else {
        apply_empty_state(*ui);
        ui->set_has_icon(false);
        ui->set_rows(std::make_shared<slint::VectorModel<InfoRowItem>>(std::vector<InfoRowItem>{}));
        set_window_icon(ContainerKind::Unknown);
    }

    handle->run();
    return 0;
}

// Win32 entry point: the wide command line is parsed by run() above.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return run(__argc, __wargv); }
