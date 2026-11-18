// Console test driver: dumps what the parsers found for a package.
//
// Used during development and for regression checks on the example files, so the
// GUI does not have to be driven by hand.
//
//   apkinfo-cli [--icon-out <dir>] [--dump-txt <file>] [--dump-json <file>] <package> [...]
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "app.h"
#include "model/info_document.h"
#include "model/report.h"
#include "util/png_writer.h"
#include "util/text.h"

namespace {

std::string to_utf8(const wchar_t* text) {
    if (!text) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), length);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

void dump(const apkinfo::InfoDocument& document, double elapsed_ms, const std::string& icon_dir,
          size_t index) {
    std::printf("========================================================================\n");
    std::printf("file      : %s\n", document.file_name.c_str());
    std::printf("kind      : %s\n", apkinfo::container_kind_name(document.kind));
    std::printf("time      : %.1f ms\n", elapsed_ms);
    if (!document.ok) {
        std::printf("ERROR     : %s\n", document.error.c_str());
        for (const std::string& warning : document.warnings) {
            std::printf("note      : %s\n", warning.c_str());
        }
        return;
    }
    std::printf("title     : %s\n", document.title.c_str());
    std::printf("subtitle  : %s\n", document.subtitle.c_str());
    std::printf("badge     : %s\n", document.badge.c_str());
    std::string icon_note;
    for (const std::string& part : document.icon_notes) {
        if (!icon_note.empty()) icon_note += " \u00b7 ";
        icon_note += part;
    }
    if (document.icon && document.icon->valid()) {
        std::printf("icon      : %dx%d px  (%s)\n", document.icon->width, document.icon->height,
                    icon_note.c_str());
        if (!icon_dir.empty()) {
            const std::string name = "icon_" + std::to_string(index) + ".png";
            const std::wstring path = to_wide(icon_dir + "\\" + name);
            if (apkinfo::write_png(*document.icon, path)) {
                std::printf("icon file : %s\n", name.c_str());
            }
        }
    } else {
        std::printf("icon      : none  (%s)\n", icon_note.c_str());
    }
    for (const std::string& warning : document.warnings) {
        std::printf("note      : %s\n", warning.c_str());
    }
    for (const apkinfo::InfoSection& section : document.sections) {
        if (section.count > 0) {
            std::printf("-- %s (%zu)\n", section.title.c_str(), section.count);
        } else {
            std::printf("-- %s\n", section.title.c_str());
        }
        for (const apkinfo::InfoRow& row : section.rows) {
            if (row.label.empty()) {
                std::printf("   %s\n", row.value.c_str());
            } else {
                std::printf("   %-26s %s\n", (row.label + ":").c_str(), row.value.c_str());
            }
        }
    }
    if (document.contents_kind == apkinfo::ContentsKind::Splits) {
        std::printf("-- splits (%zu)\n", document.splits.size());
        const size_t shown = std::min<size_t>(document.splits.size(), 20);
        for (size_t k = 0; k < shown; ++k) {
            const apkinfo::ContentItem& item = document.splits[k];
            std::printf("   %-52s %10llu  %s %s\n", item.name.c_str(),
                        static_cast<unsigned long long>(item.size), item.detail_key.c_str(),
                        item.detail_extra.c_str());
        }
        if (document.splits.size() > shown) {
            std::printf("   ... %zu more\n", document.splits.size() - shown);
        }
    } else {
        // The listing is a tree now; the CLI prints the top two levels plus a
        // count so a scripted check can still assert on the structure.
        std::printf("-- tree (%zu nodes, %zu files)\n", document.tree.size(), document.file_count);
        for (size_t k = 0; k < document.tree.size(); ++k) {
            const apkinfo::TreeNode& node = document.tree[k];
            if (node.depth > 1) continue;
            std::printf("   %*s%s%s  %10llu  (%u children)\n", static_cast<int>(node.depth) * 2, "",
                        node.name.c_str(), node.is_dir ? "/" : "", static_cast<unsigned long long>(node.size),
                        node.children);
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // never lose output when a parser crashes

    std::string icon_dir;
    std::string dump_text;
    std::string dump_json;
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = to_utf8(argv[i]);
        if (argument == "--icon-out" && i + 1 < argc) {
            icon_dir = to_utf8(argv[++i]);
            continue;
        }
        if (argument == "--dump-txt" && i + 1 < argc) {
            dump_text = to_utf8(argv[++i]);
            continue;
        }
        if (argument == "--dump-json" && i + 1 < argc) {
            dump_json = to_utf8(argv[++i]);
            continue;
        }
        paths.push_back(argument);
    }
    if (paths.empty()) {
        std::printf("usage: apkinfo-cli [--icon-out <dir>] [--dump-txt <file>] [--dump-json <file>] "
                    "<package.apk|package.apks|alpine.apk> [...]\n");
        return 2;
    }

    int failures = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        double elapsed_ms = 0;
        const apkinfo::InfoDocument document = apkinfo::load_package(paths[i], &elapsed_ms);
        dump(document, elapsed_ms, icon_dir, i + 1);

        // The reports are language independent here: the CLI has no UI language and
        // a stable English dump is what a scripted comparison wants.
        const auto write_report = [&failures](const std::string& path, const std::string& body,
                                              const char* what) {
            if (path.empty()) return;
            FILE* file = std::fopen(path.c_str(), "wb");
            if (!file) {
                std::printf("cannot write %s\n", path.c_str());
                ++failures;
                return;
            }
            std::fwrite(body.data(), 1, body.size(), file);
            std::fclose(file);
            std::printf("%s report: %s (%zu bytes)\n", what, path.c_str(), body.size());
        };
        write_report(dump_text, apkinfo::build_text_report(document, false), "text");
        write_report(dump_json, apkinfo::build_json_report(document), "json");

        if (!document.ok) ++failures;
    }
    return failures == 0 ? 0 : 1;
}
