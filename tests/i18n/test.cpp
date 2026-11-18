// Test harness for src/i18n.cpp — dictionary hygiene and lookup behaviour.
//
//   g++ -std=c++20 -O2 -I<src> test.cpp <src>/i18n.cpp -o test.exe
#include "i18n.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// Translations that are legitimately identical to the English source (proper
// nouns / acronyms that are not translated).
bool is_allowed_identity(const char* key) {
    static const std::set<std::string> allowed = {
        "SHA-256", "SDK", "OpenGL ES", "Android APK", "DEX files",
    };
    return allowed.count(key) > 0;
}

}  // namespace

int main() {
    using namespace apkinfo::i18n;

    const auto entries = dictionary_entries();
    std::printf("dictionary entries: %zu\n", entries.size());
    check(entries.size() > 100, "dictionary has a useful number of entries");

    std::set<std::string> seen;
    std::vector<std::string> duplicates;
    for (const DictionaryEntry& entry : entries) {
        check(entry.english && *entry.english, "every entry has an English key");
        check(entry.chinese && *entry.chinese, std::string("entry has a translation: ") + entry.english);
        if (!seen.insert(entry.english).second) duplicates.push_back(entry.english);
        if (!is_allowed_identity(entry.english) && std::strcmp(entry.english, entry.chinese) == 0) {
            check(false, std::string("translation differs from the English key: ") + entry.english);
        }
    }
    check(duplicates.empty(), "no duplicate keys" +
                                  (duplicates.empty() ? std::string() : " (first: " + duplicates.front() + ")"));
    if (!duplicates.empty()) {
        for (const std::string& name : duplicates) std::printf("  duplicate: %s\n", name.c_str());
    }

    // Lookup behaviour
    set_language(Language::English);
    check(std::strcmp(tr("Version code"), "Version code") == 0, "English returns the source string");
    set_language(Language::Chinese);
    check(std::strcmp(tr("Version code"), "版本号") == 0, "Chinese translates Version code");
    check(std::strcmp(tr("Package"), "软件包") == 0, "Chinese translates Package");
    check(std::strcmp(tr("a string that is not in the dictionary"), "a string that is not in the dictionary") == 0,
          "unknown strings fall back to English");

    // Language parsing
    Language parsed = Language::English;
    check(from_code("zh", &parsed) && parsed == Language::Chinese, "from_code(zh)");
    check(from_code("zh-CN", &parsed) && parsed == Language::Chinese, "from_code(zh-CN)");
    check(from_code("en-GB", &parsed) && parsed == Language::English, "from_code(en-GB)");
    check(!from_code("de", &parsed), "from_code rejects unknown languages");
    check(std::strcmp(code(Language::Chinese), "zh") == 0, "code(zh)");

    std::printf("%d checks passed, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
