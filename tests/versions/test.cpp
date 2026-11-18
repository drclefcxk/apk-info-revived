// Tests for the runtime Android version table (src/android/versions.cpp).
//
// The point of the feature is that the table lives in a text file next to the exe,
// so these checks cover both the parser (a hand-edited file must never break the
// app) and the loader (file in the exe directory wins, missing file is recreated).
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "android/versions.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what) {
    ++checks;
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
}

void test_parser() {
    using apkinfo::versions::AndroidVersion;
    using apkinfo::versions::parse_android_versions;

    const std::vector<AndroidVersion> basic = parse_android_versions("34 14\n35 15\n");
    check(basic.size() == 2, "two plain lines");
    check(basic[0].sdk == 34 && basic[0].name == "14", "34 -> 14");
    check(basic[1].sdk == 35 && basic[1].name == "15", "35 -> 15");

    // a UTF-8 BOM (the file is written with one so Notepad shows it correctly)
    const std::vector<AndroidVersion> bom = parse_android_versions("\xEF\xBB\xBF" "36 16\n");
    check(bom.size() == 1 && bom[0].name == "16", "UTF-8 BOM is skipped");

    // comments, blank lines and CRLF
    const std::vector<AndroidVersion> noisy =
        parse_android_versions("# header\r\n\r\n   \r\n# 34 1.0\r\n34 14\r\n");
    check(noisy.size() == 1 && noisy[0].sdk == 34, "comments and blank lines are ignored");

    // malformed lines are skipped instead of breaking the table
    const std::vector<AndroidVersion> broken =
        parse_android_versions("abc def\n42\n0 zero\n-3 neg\n999999 huge\n37 17\n");
    check(broken.size() == 1 && broken[0].sdk == 37, "malformed lines are skipped");

    // the name is everything after the first separator
    const std::vector<AndroidVersion> spaced = parse_android_versions("36 16 (Baklava)\n37\t17\tPreview\n");
    check(spaced.size() == 2, "spaced names parse");
    check(spaced[0].name == "16 (Baklava)", "spaces inside the name are kept");
    check(spaced[1].name == "17\tPreview" || spaced[1].name == "17 Preview", "tab separator tolerated");

    // a later line overrides an earlier one, so users can append fixes
    const std::vector<AndroidVersion> override = parse_android_versions("34 14\n34 14-fixed\n");
    check(override.size() == 1 && override[0].name == "14-fixed", "last entry wins");

    check(parse_android_versions("").empty(), "empty text yields no entries");
    check(apkinfo::versions::builtin_android_versions().size() > 30, "built-in table is populated");
}

}  // namespace

int main() {
    test_parser();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
