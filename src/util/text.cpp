#include "util/text.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include "i18n.h"

namespace apkinfo {

std::string join(const std::vector<std::string>& items, std::string_view separator) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out.append(separator);
        out.append(items[i]);
    }
    return out;
}

std::string with_thousands(uint64_t value) {
    std::string digits = std::to_string(value);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const size_t first = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i == first || (i > first && (i - first) % 3 == 0)) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

std::string human_size(uint64_t bytes) {
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    if (unit == 0) {
        std::snprintf(buffer, sizeof buffer, "%llu B (%s %s)", static_cast<unsigned long long>(bytes),
                      with_thousands(bytes).c_str(), TR("bytes"));
    } else {
        std::snprintf(buffer, sizeof buffer, "%.2f %s (%s %s)", value, units[unit],
                      with_thousands(bytes).c_str(), TR("bytes"));
    }
    return buffer;
}

std::string trim(std::string_view text) {
    size_t start = 0, end = text.size();
    while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n')) ++start;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) --end;
    return std::string(text.substr(start, end - start));
}

std::string to_lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string basename(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string dirname(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash));
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return suffix.size() <= text.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return prefix.size() <= text.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t pos = text.find(separator, start);
        if (pos == std::string_view::npos) {
            out.emplace_back(text.substr(start));
            break;
        }
        out.emplace_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::string one_line(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    // Collapse runs of spaces.
    std::string collapsed;
    collapsed.reserve(out.size());
    bool last_space = false;
    for (char c : out) {
        if (c == ' ') {
            if (!last_space) collapsed.push_back(c);
            last_space = true;
        } else {
            collapsed.push_back(c);
            last_space = false;
        }
    }
    return trim(collapsed);
}

std::string format_timestamp(int64_t unix_seconds) {
    if (unix_seconds <= 0) return {};
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm utc{};
    if (gmtime_s(&utc, &t) != 0) return {};
    char buffer[64];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M:%S UTC", &utc);
    return buffer;
}

}  // namespace apkinfo
