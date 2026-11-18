// Localisation ("汉化") for every user-visible string.
//
// The English source string is the key: `TR("Version code")` returns the
// translation for the current language, or the source string itself when there
// is none. That keeps the call sites readable and makes a missing translation
// harmless instead of a build error.
//
// Scope: the *preference* is auto (follow the Windows UI language), or a forced
// language picked from the toolbar dropdown and stored in the config file. --lang /
// APKINFO_LANG still override the preference for one run, which is what the tests use.
//
// Strings produced by the Alpine parser (its free-form `warnings`) and by the
// Android parser's low-level error paths intentionally stay in English: they are
// diagnostics about the file being read, not UI chrome.
#pragma once

#include <string>
#include <span>
#include <string_view>

namespace apkinfo::i18n {

enum class Language { English, Chinese };

// What the user asked for: follow the system, or force one language.
enum class Preference { Auto, English, Chinese };

Preference preference();
void set_preference(Preference preference);  // takes effect immediately

// The language actually in use (the resolved preference or a --lang override).
Language current();
// Forces a language for this run only (command line / APKINFO_LANG).
void set_language(Language language);
// Flips the language in use (used by the perf hook; the UI uses set_preference).
void toggle_language();

// Windows UI language -> Language ("zh-Hans-CN" etc. maps to Chinese).
Language detect_from_system();

// "en" / "zh". Also accepted by from_code(), which understands en, en-*,
// zh, zh-*, and falls back to the current language for anything unknown.
const char* code(Language language);
const char* display_name(Language language);  // "English" / "中文"
bool from_code(std::string_view text, Language* out);

// Translate an English source string. Never returns nullptr.
const char* tr(const char* english);

// The raw dictionary, exposed so a test can assert there are no duplicate keys
// and that the table stays sorted by the English key.
struct DictionaryEntry {
    const char* english;
    const char* chinese;
};
std::span<const DictionaryEntry> dictionary_entries();

// Convenience: translate and return a std::string (for concatenation).
inline std::string t(std::string_view english) { return std::string(tr(std::string(english).c_str())); }

}  // namespace apkinfo::i18n

// Marks a string literal for translation at the call site:
//   section.add(TR("Version code"), ...)
#define TR(text) ::apkinfo::i18n::tr(text)
