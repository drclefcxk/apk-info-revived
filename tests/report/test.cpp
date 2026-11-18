// Tests for the dump writers (src/model/report.cpp): the text report and the JSON
// report. The document is built by hand, so the checks are about the *format*
// (complete sections, no truncated rows, correct escaping) rather than parsing.
#include <cstdio>
#include <string>

#include "i18n.h"
#include "model/info_document.h"
#include "model/report.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what) {
    ++checks;
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

apkinfo::InfoDocument sample_document() {
    apkinfo::InfoDocument document;
    document.ok = true;
    document.kind = apkinfo::ContainerKind::AndroidApk;
    document.file_path = "C:\\packages\\demo.apk";
    document.file_name = "demo.apk";
    document.file_size = 12 * 1024 * 1024;
    document.title = "Demo \"quoted\" app";
    document.subtitle = "com.example.demo · 1.2.3 (123)";
    document.badge = "Android APK";
    document.file_hash = "abcdef0123456789";
    document.icon_notes = {"android:icon", "adaptive icon"};
    document.icon = apkinfo::Bitmap::solid(4, 4, 1, 2, 3);

    apkinfo::InfoSection package;
    package.title = "Package";
    package.add("Application name", "Demo \"quoted\" app");
    package.add("Package name", "com.example.demo");
    package.add("File size", "12.00 MiB (12,582,912 bytes)");
    document.sections.push_back(std::move(package));

    // a long list: every entry must survive, and no "… N more" placeholder
    apkinfo::InfoSection permissions;
    permissions.title = "Permissions";
    permissions.count = 300;
    permissions.collapsible = true;
    for (int i = 0; i < 300; ++i) permissions.add("", "android.permission.P" + std::to_string(i));
    document.sections.push_back(std::move(permissions));

    apkinfo::InfoSection names;
    names.title = "Localized names";
    names.count = 60;
    names.collapsible = true;
    for (int i = 0; i < 60; ++i) names.add("", "xx-rYY: Name " + std::to_string(i));
    document.sections.push_back(std::move(names));

    apkinfo::TreeNode root;
    root.name = "demo.apk";
    root.is_dir = true;
    root.children = 2;
    root.first_child = 1;
    root.subtree = 4;
    document.tree.push_back(root);

    apkinfo::TreeNode lib;
    lib.name = "lib";
    lib.path = "lib";
    lib.is_dir = true;
    lib.depth = 1;
    lib.children = 1;
    lib.first_child = 2;
    lib.subtree = 2;
    document.tree.push_back(lib);

    apkinfo::TreeNode so;
    so.name = "libdemo.so";
    so.path = "lib/libdemo.so";
    so.depth = 2;
    so.size = 4096;
    so.subtree = 1;
    document.tree.push_back(so);

    apkinfo::TreeNode manifest;
    manifest.name = "AndroidManifest.xml";
    manifest.path = "AndroidManifest.xml";
    manifest.depth = 1;
    manifest.size = 1024;
    manifest.subtree = 1;
    document.tree.push_back(manifest);
    document.contents_kind = apkinfo::ContentsKind::Files;
    document.file_count = 2;

    document.warnings.push_back("resources.arsc could not be parsed");
    return document;
}

void test_text_report() {
    const apkinfo::InfoDocument document = sample_document();
    const std::string text = apkinfo::build_text_report(document, false);

    check(contains(text, "APK Info"), "header");
    check(contains(text, "C:\\packages\\demo.apk"), "file path");
    check(contains(text, "12.00 MiB (12,582,912 bytes)"), "size with raw bytes");
    check(contains(text, "abcdef0123456789"), "sha-256");
    check(contains(text, "-- Package"), "package section");
    check(contains(text, "-- Permissions (300)"), "section count");
    check(contains(text, "android.permission.P299"), "last permission entry present");
    check(!contains(text, " more\n"), "no '... N more' placeholder");
    check(contains(text, "-- Localized names (60)"), "localized names section");
    check(contains(text, "xx-rYY: Name 59"), "last localized name present");
    check(contains(text, "-- Files (2)"), "file listing header");
    check(contains(text, "lib/libdemo.so") || contains(text, "libdemo.so"), "file listing entry");
    check(contains(text, "resources.arsc could not be parsed"), "warnings");

    // every padded label is on its own line (a long path once swallowed the newline)
    size_t lines = 1;
    for (char c : text) {
        if (c == '\n') ++lines;
    }
    check(lines > 300, "report has one line per row (" + std::to_string(lines) + " lines)");

    // the localised report must actually translate
    apkinfo::i18n::set_language(apkinfo::i18n::Language::Chinese);
    const std::string chinese = apkinfo::build_text_report(document, true);
    check(chinese != text, "the localised report differs from the English one");
    check(contains(chinese, "\u8f6f\u4ef6\u5305"), "Chinese report contains a translated label");
    const std::string english = apkinfo::build_text_report(document, false);
    check(!contains(english, "\u8f6f\u4ef6\u5305"), "the English report is not translated");
    apkinfo::i18n::set_language(apkinfo::i18n::Language::English);
}

void test_json_report() {
    const apkinfo::InfoDocument document = sample_document();
    const std::string json = apkinfo::build_json_report(document);

    check(contains(json, "\"fileName\": \"demo.apk\""), "file name field");
    check(contains(json, "\"fileSize\": 12582912"), "numeric file size");
    check(contains(json, "\"sha256\": \"abcdef0123456789\""), "sha field");
    check(contains(json, "\"ok\": true"), "ok flag");

    // escaping: the title contains quotes
    check(contains(json, "\\\"quoted\\\""), "quotes are escaped");
    check(!contains(json, "\"title\": \"Demo \"quoted\""), "raw quotes would break JSON");

    check(contains(json, "\"title\": \"Permissions\""), "section title");
    check(contains(json, "\"count\": 300"), "section count");
    check(contains(json, "android.permission.P299"), "last permission in JSON");
    check(contains(json, "\"path\": \"lib/libdemo.so\""), "file path in JSON");
    check(contains(json, "\"dir\": false"), "file/dir flag in JSON");

    // brace balance is a cheap sanity check that the document is well formed
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    bool balanced = true;
    for (char c : json) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            if (--depth < 0) balanced = false;
        }
    }
    check(balanced && depth == 0, "braces and brackets balance outside strings");
}

}  // namespace

int main() {
    test_text_report();
    test_json_report();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
