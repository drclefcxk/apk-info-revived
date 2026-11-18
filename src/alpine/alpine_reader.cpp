#include "alpine/alpine_reader.h"

#include <algorithm>
#include <cstdio>

#include "alpine/alpine.h"
#include "model/tree_builder.h"
#include "util/text.h"

namespace apkinfo {
namespace {

std::vector<std::string> dependency_strings(const std::vector<alpine::Dependency>& dependencies) {
    std::vector<std::string> out;
    out.reserve(dependencies.size());
    for (const auto& dependency : dependencies) out.push_back(dependency.to_string());
    return out;
}

}  // namespace

InfoDocument read_alpine_package(std::span<const uint8_t> file, const Detection& detection,
                                 const std::string& display_path, const AlpineOptions& options) {
    InfoDocument document;
    document.kind = ContainerKind::AlpinePackage;
    document.file_path = display_path;
    document.file_name = basename(display_path);
    document.file_size = file.size();

    alpine::Result parsed = alpine::parse(file);
    if (!parsed.ok) {
        document.error = parsed.error.empty() ? "the Alpine package could not be read" : parsed.error;
        return document;
    }
    const alpine::Package& package = parsed.pkg;

    document.badge = std::string("Alpine ") + alpine::format_name(package.format);
    document.title = package.name.empty() ? document.file_name : package.name;
    std::string subtitle;
    if (!package.version.empty()) subtitle = package.version;
    if (!package.arch.empty()) {
        if (!subtitle.empty()) subtitle += " · ";
        subtitle += package.arch;
    }
    document.subtitle = subtitle;
    for (const std::string& warning : package.warnings) document.warnings.push_back(warning);

    // Keep references held across later appends valid (see apk_reader.cpp).
    document.sections.reserve(32);

    // --- overview -----------------------------------------------------------
    InfoSection& overview = document.sections.emplace_back();
    overview.title = "Package";
    overview.add("Package name", package.name, "pkgname", true);
    overview.add("Version", package.version, "pkgver");
    overview.add("Description", one_line(package.description), "pkgdesc", false, true);
    overview.add("Architecture", package.arch, "arch");
    overview.add("License", package.license, "license");
    overview.add("Origin", package.origin, "origin (source package)");
    overview.add("Maintainer", package.maintainer, "maintainer");
    overview.add("Project URL", package.url, "url");
    if (!package.commit.empty()) overview.add("Repository commit", package.commit, "commit", true);
    if (package.build_time > 0) {
        overview.add("Build time", format_timestamp(package.build_time),
                     std::to_string(package.build_time) + " unix seconds");
    }
    if (package.installed_size > 0) overview.add("Installed size", human_size(static_cast<uint64_t>(package.installed_size)));
    if (package.file_size > 0) overview.add("Package size (pkginfo)", human_size(static_cast<uint64_t>(package.file_size)));
    if (package.data_section_size > 0) {
        overview.add("Payload size", human_size(package.data_section_size), "compressed file data");
    }
    if (package.provider_priority > 0) {
        overview.add("Provider priority", std::to_string(package.provider_priority));
    }
    if (!package.tags.empty()) overview.add("Tags", join(package.tags, ", "));
    overview.add("Package format", alpine::format_name(package.format),
                 detection.reason.empty() ? "detected from the file content" : detection.reason);
    overview.add("File name", document.file_name);
    overview.add("File size", human_size(document.file_size));
    if (!package.hash.empty()) overview.add("Package hash", package.hash, "sha1 of the package contents", true, true);
    // SHA-256 is computed by the app on a worker thread (see apk_reader.cpp).

    // --- relations ----------------------------------------------------------
    auto add_relation = [&](const char* title, const std::vector<alpine::Dependency>& dependencies,
                            const char* hint) {
        if (dependencies.empty()) return;
        const std::vector<std::string> strings = dependency_strings(dependencies);
        InfoSection& section = document.sections.emplace_back();
        section.title = title;
        section.count = strings.size();
        section.collapsible = true;
        for (const std::string& value : strings) section.add("", value, hint, true);
    };
    add_relation("Dependencies", package.depends, "depend");
    add_relation("Provides", package.provides, "provides");
    add_relation("Replaces", package.replaces, "replaces");
    add_relation("Install if", package.install_if, "install_if");
    add_relation("Recommends", package.recommends, "recommends");

    // --- scripts ------------------------------------------------------------
    if (!package.scripts.empty()) {
        InfoSection& section = document.sections.emplace_back();
        section.title = "Scripts";
        section.count = package.scripts.size();
        for (const alpine::Script& script : package.scripts) {
            section.add(script.name, std::to_string(script.body.size()) + " bytes",
                        one_line(script.body.substr(0, 400)), true);
        }
    }

    // --- signatures ---------------------------------------------------------
    if (!package.signatures.empty()) {
        InfoSection& section = document.sections.emplace_back();
        section.title = "Signatures";
        section.count = package.signatures.size();
        for (const alpine::Signature& signature : package.signatures) {
            std::string value = signature.algorithm;
            if (!signature.key_id.empty()) value += " · key " + signature.key_id;
            if (signature.size) value += " · " + std::to_string(signature.size) + " bytes";
            section.add("", value, "not verified", true, true);
        }
    }

    // --- contents -----------------------------------------------------------
    if (options.read_contents && !package.files.empty()) {
        InfoSection& summary = document.sections.emplace_back();
        summary.title = "Contents summary";
        uint64_t total = 0;
        size_t directories = 0, symlinks = 0;
        for (const alpine::FileEntry& entry : package.files) {
            total += entry.size;
            if (entry.is_dir) ++directories;
            if (entry.is_symlink) ++symlinks;
        }
        summary.add("Entries", with_thousands(package.files.size()));
        if (directories) summary.add("Directories", with_thousands(directories));
        if (symlinks) summary.add("Symlinks", with_thousands(symlinks));
        summary.add("Total file size", human_size(total));

        // Only the tree skeleton is built: node names, sizes and structure. The
        // UI expands a directory on demand instead of materialising 20k rows.
        document.contents_kind = ContentsKind::Files;
        TreeBuilder tree;
        for (const alpine::FileEntry& entry : package.files) {
            if (entry.is_dir) {
                tree.add_dir(entry.path);
                continue;
            }
            tree.add_file(entry.path, entry.size, entry.is_symlink ? 1 : 0);
        }
        document.file_count = tree.file_count();
        document.tree = tree.finish(document.file_name);
    }

    document.ok = true;
    return document;
}

}  // namespace apkinfo
