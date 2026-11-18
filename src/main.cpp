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

#include "app.h"
#include "config.h"
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
};

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
    // Dropdown: 0 = follow the system, 1 = 中文, 2 = English.
    int index = 0;
    switch (apkinfo::i18n::preference()) {
        case apkinfo::i18n::Preference::Chinese:
            index = 1;
            break;
        case apkinfo::i18n::Preference::English:
            index = 2;
            break;
        case apkinfo::i18n::Preference::Auto:
        default:
            index = 0;
            break;
    }
    // The dropdown's model is constant (its entries are the language names, with the
    // system entry written bilingually), so only the selection is set here. Rebuilding
    // the model on a language change reset the control to its first entry, which is what
    // made it look stuck on "follow the system".
    ui.set_language_index(index);
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
    // --lang zh|en (also honoured through APKINFO_LANG) picks the UI language;
    // anything else is treated as the package path to open.
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

    // The config file decides the language preference; --lang / APKINFO_LANG still win
    // for this run because set_language() marks itself as an override.
    apkinfo::i18n::set_preference(static_cast<apkinfo::i18n::Preference>(apkinfo::config().language));

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
    // stored in the config file and the cached document is re-rendered (never re-read).
    handle->on_language_selected([ui, state](int index) {
        const auto started = std::chrono::steady_clock::now();
        apkinfo::i18n::Preference preference = apkinfo::i18n::Preference::Auto;
        if (index == 1) preference = apkinfo::i18n::Preference::Chinese;
        if (index == 2) preference = apkinfo::i18n::Preference::English;

        apkinfo::i18n::set_preference(preference);
        apkinfo::mutable_config().language = static_cast<apkinfo::LanguagePreference>(preference);
        apkinfo::save_config();

        apply_chrome(*ui);
        if (state->loaded) apply_view(*ui, *state);
        if (std::getenv("APKINFO_DEBUG_TIMING")) {
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            std::fprintf(stderr, "language switch (%s): %.2f ms\n", apkinfo::i18n::code(apkinfo::i18n::current()), ms);
        }
    });

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
        ui->set_status_text(slint::SharedString(tr("Open an .apk, .apks, .apkm or Alpine .apk package")));
        ui->set_app_title(slint::SharedString(tr("Nothing loaded")));
        ui->set_app_subtitle(slint::SharedString(tr("Use “Open package…” or pass a path on the command line.")));

        ui->set_has_icon(false);
        ui->set_rows(std::make_shared<slint::VectorModel<InfoRowItem>>(std::vector<InfoRowItem>{}));
        set_window_icon(ContainerKind::Unknown);
    }

    handle->run();
    return 0;
}

// Win32 entry point: the wide command line is parsed by run() above.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return run(__argc, __wargv); }
