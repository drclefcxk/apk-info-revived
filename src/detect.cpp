#include "detect.h"

#include <algorithm>
#include <string_view>

#include "alpine/alpine.h"
#include "container/zip_file.h"
#include "i18n.h"

namespace apkinfo {
namespace {

// Picks the base apk out of a split bundle. Conventionally it is
// "base.apk" (bundletool output), but APKMirror/SAI bundles use
// "splits/base-master.apk", "com.foo.apk" or "base-master.apk".
std::string pick_base_apk(const std::vector<const ZipFile::Entry*>& apks) {
    auto exact = [&](std::string_view name) -> std::string {
        for (const auto* e : apks) {
            if (e->name == name) return e->name;
        }
        return {};
    };
    if (auto name = exact("base.apk"); !name.empty()) return name;
    if (auto name = exact("splits/base-master.apk"); !name.empty()) return name;
    if (auto name = exact("base-master.apk"); !name.empty()) return name;

    std::string best;
    for (const auto* e : apks) {
        std::string_view name = e->name;
        const size_t slash = name.find_last_of('/');
        const std::string_view base = slash == std::string_view::npos ? name : name.substr(slash + 1);
        if (base == "base.apk" || base == "base-master.apk") {
            if (best.empty() || e->name.size() < best.size()) best = e->name;
        }
    }
    if (!best.empty()) return best;

    // Last resort: the first non-split apk, else the first apk at all.
    for (const auto* e : apks) {
        const std::string& name = e->name;
        if (name.find("split") == std::string::npos && name.find("config.") == std::string::npos) {
            return name;
        }
    }
    return apks.empty() ? std::string{} : apks.front()->name;
}

Detection detect_zip(std::span<const uint8_t> file) {
    Detection result;
    result.format = "zip";
    std::string error;
    auto zip = ZipFile::open(file, &error);
    if (!zip) {
        result.kind = ContainerKind::Unknown;
        result.reason = std::string(TR("zip archive whose central directory cannot be read")) + " (" + error + ")";
        return result;
    }

    if (zip->contains("AndroidManifest.xml")) {
        result.kind = ContainerKind::AndroidApk;
        result.format = "apk";
        result.reason = TR("zip archive with AndroidManifest.xml at the root");
        return result;
    }

    // A bundle: zips inside a zip, or the metadata files that bundletool /
    // APKMirror / SAI leave behind.
    const auto apks = zip->find_suffix(".apk");
    const bool has_bundle_metadata = zip->contains("toc.pb") || zip->contains("info.json") ||
                                     zip->contains("manifest.json") || zip->contains("meta.sai_v2.json");
    if (!apks.empty() || has_bundle_metadata) {
        result.kind = ContainerKind::AndroidSplitBundle;
        result.base_apk_entry = pick_base_apk(apks);
        if (!apks.empty()) {
            result.format = "split bundle";
            result.reason = std::string(TR("zip archive holding")) + " " + std::to_string(apks.size()) + " " +
                            TR("apk file(s)");
        } else {
            result.format = "split bundle";
            result.reason = TR("bundle metadata without an apk payload");
        }
        return result;
    }

    result.kind = ContainerKind::Unknown;
    result.reason = TR("zip archive without AndroidManifest.xml and without apk members");
    return result;
}

}  // namespace

Detection detect(std::span<const uint8_t> file) {
    Detection result;
    // apk-tools 3.x container: "ADB" followed by the compression selector
    // ('d' deflate, 'z' zstd, 'n' none). Note that "ADB." (with a dot) is the
    // adb object header *inside* the decompressed stream, not the container.
    if (file.size() >= 4 && file[0] == 'A' && file[1] == 'D' && file[2] == 'B' &&
        (file[3] == 'd' || file[3] == 'z' || file[3] == 'n')) {
        result.kind = ContainerKind::AlpinePackage;
        result.format = "v3";
        result.reason = std::string(TR("ADB container (apk-tools 3.x)")) + ", '" +
                        static_cast<char>(file[3]) + "'";
        return result;
    }
    if (looks_like_zip(file)) {
        return detect_zip(file);
    }
    if (file.size() >= 2 && file[0] == 0x1F && file[1] == 0x8B) {
        if (alpine::looks_like_alpine(file)) {
            result.kind = ContainerKind::AlpinePackage;
            result.format = "v2";
            result.reason = TR("gzip stream with an apk control tarball (.PKGINFO), apk-tools 2.x");
            return result;
        }
        result.kind = ContainerKind::Unknown;
        result.format = "gzip";
        result.reason = TR("gzip stream that is not an Alpine package");
        return result;
    }
    result.kind = ContainerKind::Unknown;
    result.format = "binary";
    result.reason = TR("not a zip, not a gzip stream and not an ADB container");
    return result;
}

}  // namespace apkinfo
