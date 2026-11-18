// Tests for the configuration file (src/config.cpp + util/json.cpp).
//
// The config holds the Android version table and the language preference; it is
// hand-edited, so a broken file must never take the program down and must never be
// silently overwritten.
#include <cstdio>
#include <fstream>
#include <string>

#include "android/versions.h"
#include "config.h"
#include "i18n.h"
#include "util/json.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what) {
    ++checks;
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
}

void write_file(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void test_json() {
    using apkinfo::json::Value;

    const auto document = apkinfo::json::parse(R"({"a": 1, "b": [true, null, "x"], "c": {"d": 2.5}})");
    check(document.has_value(), "valid JSON parses");
    if (!document) return;
    check(document->is_object(), "top level is an object");
    check(document->find("a") && document->find("a")->number_or() == 1.0, "numbers");
    check(document->find("b") && document->find("b")->is_array(), "arrays");
    check(document->find("b")->items.size() == 3, "array size");
    check(document->find("b")->items[0].bool_or(false), "true literal");
    check(document->find("b")->items[1].type == Value::Type::Null, "null literal");
    check(document->find("b")->items[2].string_or() == "x", "strings");
    check(document->find("c")->find("d")->number_or() == 2.5, "nested objects");
    check(document->find("missing") == nullptr, "missing member");

    const auto escapes = apkinfo::json::parse(R"({"s": "a\"b\\c\u00e9\u4e2d"})");
    check(escapes.has_value(), "escape sequences parse");
    if (escapes) {
        const std::string value = escapes->find("s")->string_or();
        check(value.find("\"") != std::string::npos && value.find("\\") != std::string::npos, "quotes and slashes");
        check(value.find("\u4e2d") != std::string::npos, "\\u escapes decode to UTF-8");
    }

    check(!apkinfo::json::parse("{\"a\": }").has_value(), "malformed document is rejected");
    check(!apkinfo::json::parse("{\"a\": 1} trailing").has_value(), "trailing text is rejected");
    check(!apkinfo::json::parse("[1, 2").has_value(), "unterminated array is rejected");
    check(apkinfo::json::quote("a\"b").size() == 6, "quote() escapes");

    apkinfo::json::Value number;
    check(number.string_or("fallback") == "fallback", "string_or falls back");
    check(apkinfo::json::Value{}.bool_or(true), "bool_or falls back");
}

void test_creation_and_reading() {
    const std::string path = apkinfo::config_path();
    check(!path.empty(), "the config path is known");
    check(path.find("apkinfo-config.json") != std::string::npos, "the file is apkinfo-config.json");
    check(path.find(apkinfo::versions::executable_directory()) == 0, "it lives next to the exe");

    std::remove(path.c_str());
    apkinfo::reload_config();
    check(apkinfo::config().language == apkinfo::LanguagePreference::Auto, "default language is auto");
    check(!apkinfo::config().android_versions.empty(), "defaults carry the version table");

    const std::string created = read_file(path);
    check(!created.empty(), "the config file was created on first use");
    check(created.find("\"language\"") != std::string::npos, "it contains the language field");
    check(created.find("\"androidVersions\"") != std::string::npos, "it contains the version table");
    check(created.find("_readme") != std::string::npos, "it carries instructions");

    // a hand-edited file wins
    write_file(path,
               "{\n  \"language\": \"en\",\n  \"androidVersions\": { \"34\": \"14-custom\", \"99\": \"99-custom\" }\n}\n");
    apkinfo::reload_config();
    check(apkinfo::config().language == apkinfo::LanguagePreference::English, "language is read from the file");
    check(apkinfo::config().android_versions.size() == 2, "the version table is read from the file");
    check(apkinfo::versions::android_version_name(34) == "14-custom", "a table entry overrides the built-in name");
    check(apkinfo::versions::android_version_name(99) == "99-custom", "a new level can be added at runtime");
    check(apkinfo::versions::android_version_name(35) == "15", "levels missing from the file use the built-in table");

    // numeric shorthand ("34": 14) is accepted
    write_file(path, "{\"androidVersions\": {\"34\": 14}}");
    apkinfo::reload_config();
    check(apkinfo::versions::android_version_name(34) == "14", "numeric names are accepted");

    // a broken file keeps the defaults and reports a warning, without being overwritten
    write_file(path, "{ this is not json ");
    apkinfo::reload_config();
    check(apkinfo::config().language == apkinfo::LanguagePreference::Auto, "a broken file falls back to defaults");
    check(!apkinfo::config_warning().empty(), "a broken file reports a warning");
    check(read_file(path).find("not json") != std::string::npos, "the broken file is left untouched");

    // saving writes the current state
    apkinfo::reload_config();
    apkinfo::mutable_config().language = apkinfo::LanguagePreference::Chinese;
    check(apkinfo::save_config(), "save_config writes the file");
    check(read_file(path).find("\"language\": \"zh\"") != std::string::npos, "the saved language is Chinese");
    apkinfo::reload_config();
    check(apkinfo::config().language == apkinfo::LanguagePreference::Chinese, "the saved language round-trips");

    std::remove(path.c_str());
    apkinfo::reload_config();
}

void test_migration() {
    const std::string directory = apkinfo::versions::executable_directory();
    const std::string config = apkinfo::config_path();
    const std::string legacy = directory + "\\android-versions.txt";
    std::remove(config.c_str());

    // a leftover table from the pre-JSON version of the program is imported
    write_file(legacy, "# old table\n34 14-legacy\n35 15-legacy\n");
    apkinfo::reload_config();
    check(apkinfo::versions::android_version_name(34) == "14-legacy", "the legacy table is imported");
    check(apkinfo::versions::android_version_name(35) == "15-legacy", "every legacy row is imported");
    std::remove(legacy.c_str());
    std::remove(config.c_str());
    apkinfo::reload_config();
}

void test_language_preference_codes() {
    apkinfo::LanguagePreference preference = apkinfo::LanguagePreference::English;
    check(apkinfo::language_preference_from_code("auto", &preference), "auto parses");
    check(preference == apkinfo::LanguagePreference::Auto, "auto maps to Auto");
    check(apkinfo::language_preference_from_code("zh-Hans", &preference), "zh-Hans parses");
    check(preference == apkinfo::LanguagePreference::Chinese, "zh-Hans maps to Chinese");
    check(apkinfo::language_preference_from_code("EN", &preference), "case insensitive");
    check(preference == apkinfo::LanguagePreference::English, "EN maps to English");
    check(!apkinfo::language_preference_from_code("klingon", &preference), "unknown codes are rejected");
    check(std::string(apkinfo::language_preference_code(apkinfo::LanguagePreference::Chinese)) == "zh", "code(zh)");
}

void test_i18n_preference() {
    apkinfo::i18n::set_preference(apkinfo::i18n::Preference::English);
    check(apkinfo::i18n::current() == apkinfo::i18n::Language::English, "forcing English works");
    apkinfo::i18n::set_preference(apkinfo::i18n::Preference::Chinese);
    check(apkinfo::i18n::current() == apkinfo::i18n::Language::Chinese, "forcing Chinese works");
    apkinfo::i18n::set_preference(apkinfo::i18n::Preference::Auto);
    check(apkinfo::i18n::preference() == apkinfo::i18n::Preference::Auto, "auto is stored");
    // auto resolves to the system language, which is what detect_from_system reports
    check(apkinfo::i18n::current() == apkinfo::i18n::detect_from_system(), "auto follows the system language");
}

}  // namespace

int main() {
    test_json();
    test_creation_and_reading();
    test_migration();
    test_language_preference_codes();
    test_i18n_preference();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
