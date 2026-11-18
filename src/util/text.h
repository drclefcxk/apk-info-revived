// Small string formatting helpers shared by both package readers.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace apkinfo {

std::string join(const std::vector<std::string>& items, std::string_view separator);
// "1.4 MiB (1,485,832 bytes)" style, plus the raw byte count.
std::string human_size(uint64_t bytes);
// "1,234,567"
std::string with_thousands(uint64_t value);
std::string trim(std::string_view text);
std::string to_lower(std::string_view text);
std::string basename(std::string_view path);
std::string dirname(std::string_view path);
bool ends_with(std::string_view text, std::string_view suffix);
bool starts_with(std::string_view text, std::string_view prefix);
std::vector<std::string> split(std::string_view text, char separator);
// Replaces control characters so a value can be rendered in one line.
std::string one_line(std::string_view text);
// Formats a unix timestamp as "2024-04-05 12:34:56 UTC" ("" when 0).
std::string format_timestamp(int64_t unix_seconds);

}  // namespace apkinfo
