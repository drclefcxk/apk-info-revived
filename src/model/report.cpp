#include "model/report.h"

#include <cstdio>

#include "i18n.h"
#include "util/text.h"

namespace apkinfo {
namespace {

std::string tr(const std::string& text, bool localized) {
    if (!localized) return text;
    return i18n::t(text);
}

// Translates a value the same way the window does: whole-string keys plus the
// "  <key>" suffix the readers append ("com.foo.PERMISSION  (sdk 23+)").
std::string tr_value(const std::string& value, bool localized) {
    if (!localized) return value;
    const std::string whole = i18n::t(value);
    if (whole != value) return whole;
    const size_t at = value.rfind("  ");
    if (at != std::string::npos) {
        const std::string tail = value.substr(at + 2);
        const std::string translated = i18n::t(tail);
        if (translated != tail) return value.substr(0, at + 2) + translated;
    }
    return value;
}

void append_padded(std::string& out, const std::string& label, const std::string& value) {
    if (label.empty()) {
        out += "  " + value + "\n";
        return;
    }
    std::string head = label + ":";
    out += "  " + head;
    // pad to a fixed column; long labels simply run over by one space
    if (head.size() < 26) out.append(26 - head.size(), ' ');
    out += " " + value + "\n";
}

// human_size() localises its own parenthetical, which would leak into an English
// report, so the two parts are assembled here.
std::string size_text(uint64_t bytes, bool localized) {
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    char human[32];
    if (unit == 0) {
        std::snprintf(human, sizeof human, "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(human, sizeof human, "%.2f %s", value, units[unit]);
    }
    return std::string(human) + " (" + with_thousands(bytes) + " " + tr("bytes", localized) + ")";
}

void append_section(std::string& out, const std::string& title, size_t count, bool localized) {
    out += "-- " + tr(title, localized);
    if (count > 0) out += " (" + std::to_string(count) + ")";
    out += "\n";
}

// --- JSON helpers -----------------------------------------------------------

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    out += buffer;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string json_string(const std::string& text) { return "\"" + json_escape(text) + "\""; }

}  // namespace

std::string build_text_report(const InfoDocument& document, bool localized) {
    std::string out;
    out += tr("APK Info — Revived", localized);
    out += "\n\n";

    append_padded(out, tr("File", localized), document.file_path.empty() ? document.file_name : document.file_path);
    append_padded(out, tr("Format", localized), tr(container_kind_name(document.kind), localized));
    append_padded(out, tr("File size", localized), size_text(document.file_size, localized));
    append_padded(out, tr("Detected", localized), tr(document.badge, localized));
    if (!document.ok) {
        append_padded(out, tr("Error", localized), document.error);
    }
    append_padded(out, tr("Title", localized), document.title);
    append_padded(out, tr("Subtitle", localized), document.subtitle);
    if (document.icon && document.icon->valid()) {
        char size[64];
        std::snprintf(size, sizeof size, "%dx%d px", document.icon->width, document.icon->height);
        std::string note;
        for (const std::string& part : document.icon_notes) {
            if (!note.empty()) note += " · ";
            note += part;
        }
        append_padded(out, tr("Icon", localized), std::string(size) + (note.empty() ? "" : "  (" + note + ")"));
    } else {
        append_padded(out, tr("Icon", localized), tr("none", localized));
    }
    append_padded(out, tr("SHA-256", localized),
                  document.file_hash.empty() ? tr("(not computed)", localized) : document.file_hash);
    out += "\n";

    for (const InfoSection& section : document.sections) {
        append_section(out, section.title, section.count, localized);
        for (const InfoRow& row : section.rows) {
            if (row.label.empty()) {
                out += "  " + tr_value(row.value, localized) + "\n";
            } else {
                append_padded(out, tr(row.label, localized), tr_value(row.value, localized));
            }
        }
        out += "\n";
    }

    if (document.contents_kind == ContentsKind::Splits && !document.splits.empty()) {
        append_section(out, "Splits", document.splits.size(), localized);
        for (const ContentItem& item : document.splits) {
            std::string detail = tr(item.detail_key, localized);
            if (!item.detail_extra.empty()) detail += " (" + item.detail_extra + ")";
            char buffer[512];
            std::snprintf(buffer, sizeof buffer, "  %-58.58s %14s  %s\n", item.name.c_str(),
                          size_text(item.size, localized).c_str(), detail.c_str());
            out += buffer;
        }
        out += "\n";
    }

    if (!document.tree.empty()) {
        append_section(out, "Files", document.file_count, localized);
        // The tree is a pre-order flat array; printing it in order gives a readable
        // indented listing without walking it again.
        for (const TreeNode& node : document.tree) {
            if (node.depth == 0) continue;
            std::string indent(static_cast<size_t>(node.depth) * 2, ' ');
            if (node.is_dir) {
                out += "  " + indent + node.name + "/\n";
            } else {
                char buffer[512];
                std::snprintf(buffer, sizeof buffer, "  %s%-56.56s %14s\n", indent.c_str(), node.name.c_str(),
                              size_text(node.size, localized).c_str());
                out += buffer;
            }
        }
        out += "\n";
    }

    if (!document.warnings.empty()) {
        append_section(out, "Notes", document.warnings.size(), localized);
        for (const std::string& warning : document.warnings) out += "  " + warning + "\n";
        out += "\n";
    }
    return out;
}


std::string build_info_text(const InfoDocument& document, bool localized) {
    std::string out;
    if (!document.ok) {
        out += tr("could not read the file", localized);
        out += "\n";
        if (!document.error.empty()) out += "  " + document.error + "\n";
        return out;
    }

    // Proportional font: no column padding, a section header plus one line per row.
    for (const InfoSection& section : document.sections) {
        if (section.rows.empty()) continue;
        std::string head = "== " + tr(section.title, localized);
        if (section.count > 0) head += " (" + std::to_string(section.count) + ")";
        head += " ==";
        out += head + "\n";

        for (const InfoRow& row : section.rows) {
            if (row.label.empty()) {
                out += "  " + tr_value(row.value, localized) + "\n";
            } else {
                out += tr(row.label, localized) + ": " + tr_value(row.value, localized) + "\n";
            }
        }
        out += "\n";
    }

    if (document.contents_kind == ContentsKind::Splits && !document.splits.empty()) {
        out += "== " + tr("Splits", localized) + " (" + std::to_string(document.splits.size()) + ") ==\n";
        for (const ContentItem& item : document.splits) {
            std::string detail = tr(item.detail_key, localized);
            if (!item.detail_extra.empty()) detail += " (" + item.detail_extra + ")";
            out += "  " + item.name + "  " + size_text(item.size, localized) + "  " + detail + "\n";
        }
        out += "\n";
    }

    if (!document.warnings.empty()) {
        out += "== " + tr("Notes", localized) + " ==\n";
        for (const std::string& warning : document.warnings) out += "  " + warning + "\n";
        out += "\n";
    }
    return out;
}

std::string build_json_report(const InfoDocument& document) {
    std::string out;
    out += "{\n";
    out += "  \"file\": " + json_string(document.file_path) + ",\n";
    out += "  \"fileName\": " + json_string(document.file_name) + ",\n";
    out += "  \"format\": " + json_string(container_kind_name(document.kind)) + ",\n";
    out += "  \"badge\": " + json_string(document.badge) + ",\n";
    out += "  \"ok\": " + std::string(document.ok ? "true" : "false") + ",\n";
    out += "  \"error\": " + json_string(document.error) + ",\n";
    out += "  \"fileSize\": " + std::to_string(document.file_size) + ",\n";
    out += "  \"title\": " + json_string(document.title) + ",\n";
    out += "  \"subtitle\": " + json_string(document.subtitle) + ",\n";
    out += "  \"sha256\": " + json_string(document.file_hash) + ",\n";

    out += "  \"icon\": ";
    if (document.icon && document.icon->valid()) {
        out += "{\"width\": " + std::to_string(document.icon->width) + ", \"height\": " +
               std::to_string(document.icon->height) + ", \"notes\": [";
        for (size_t i = 0; i < document.icon_notes.size(); ++i) {
            if (i) out += ", ";
            out += json_string(document.icon_notes[i]);
        }
        out += "]},\n";
    } else {
        out += "null,\n";
    }

    out += "  \"sections\": [\n";
    for (size_t s = 0; s < document.sections.size(); ++s) {
        const InfoSection& section = document.sections[s];
        out += "    {\"title\": " + json_string(section.title) + ", \"count\": " + std::to_string(section.count) +
               ", \"rows\": [";
        if (!section.rows.empty()) out += "\n";
        for (size_t r = 0; r < section.rows.size(); ++r) {
            const InfoRow& row = section.rows[r];
            out += "      {\"label\": " + json_string(row.label) + ", \"value\": " + json_string(row.value);
            if (!row.hint.empty()) out += ", \"hint\": " + json_string(row.hint);
            out += "}";
            if (r + 1 < section.rows.size()) out += ",";
            out += "\n";
        }
        out += "    ]}";
        if (s + 1 < document.sections.size()) out += ",";
        out += "\n";
    }
    out += "  ],\n";

    out += "  \"splits\": [";
    if (!document.splits.empty()) out += "\n";
    for (size_t i = 0; i < document.splits.size(); ++i) {
        const ContentItem& item = document.splits[i];
        out += "    {\"name\": " + json_string(item.name) + ", \"size\": " + std::to_string(item.size) +
               ", \"packedSize\": " + std::to_string(item.packed_size) + ", \"kind\": " + json_string(item.kind) +
               ", \"detail\": " + json_string(item.detail_key) + ", \"qualifier\": " + json_string(item.detail_extra) +
               "}";
        if (i + 1 < document.splits.size()) out += ",";
        out += "\n";
    }
    out += "  ],\n";

    out += "  \"files\": [";
    if (!document.tree.empty()) out += "\n";
    bool first = true;
    for (const TreeNode& node : document.tree) {
        if (node.depth == 0) continue;
        if (!first) out += ",\n";
        first = false;
        out += "    {\"path\": " + json_string(node.path) + ", \"dir\": " +
               std::string(node.is_dir ? "true" : "false") + ", \"size\": " + std::to_string(node.size) + "}";
    }
    out += "\n  ],\n";

    out += "  \"warnings\": [";
    for (size_t i = 0; i < document.warnings.size(); ++i) {
        if (i) out += ", ";
        out += json_string(document.warnings[i]);
    }
    out += "]\n";
    out += "}\n";
    return out;
}

}  // namespace apkinfo
