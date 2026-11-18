// Alpine Package Keeper (.apk) package readers.
//
// Two on-disk formats exist and both must be handled:
//
//  * v2 (apk-tools 2.x, classic Alpine, also used by OpenWrt < 25):
//      concatenated gzip streams  [ .SIGN.RSA.* ][ .PKGINFO + scripts ][ data.tar.gz ]
//  * v3 (apk-tools 3.x, Alpine "edge", OpenWrt 25+):
//      ADB container: 4-byte magic "ADB" + compression char ('d' deflate,
//      'z' zstd, 'n' none), then adb blocks: one ADB block (the binary object
//      tree holding pkginfo + the complete file index), optional SIG blocks,
//      and DATA blocks with the raw file payload.
//
// Note that an Alpine package keeps the ".apk" extension and is completely
// unrelated to an Android .apk (which is a zip). Detection is by content, see
// looks_like_alpine().
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apkinfo::alpine {

enum class Format { Unknown, V2, V3 };

const char* format_name(Format f);  // "v2" / "v3" / "unknown"

// A single dependency expression, e.g. "musl>=1.2.4-r0" or "so:libc.musl-x86_64.so.1".
struct Dependency {
    std::string name;     // "musl"
    std::string op;       // ">=", "<", "=", "~=", "" when unconstrained
    std::string version;  // "1.2.4-r0"
    std::string to_string() const;  // name + op + version
};

// One file (or directory, or symlink) inside the package.
struct FileEntry {
    std::string path;      // absolute path inside the package, e.g. "/usr/bin/busybox"
                           // directories end with '/'
    bool is_dir = false;
    bool is_symlink = false;
    uint64_t size = 0;
    uint64_t mtime = 0;    // unix seconds
    uint32_t mode = 0;     // permission bits, e.g. 0755
    std::string user;      // owner name
    std::string group;
    std::string sha1;      // hex digest of contents: SHA-1 for v2 packages, SHA-256
                           // (64 hex chars) for v3 packages, "" for dirs/symlinks
    std::string target;    // symlink target
};

struct Script {
    std::string name;   // "post-install", "trigger", "pre-upgrade", ...
    std::string body;
};

struct Signature {
    std::string algorithm;  // "RSA" (v3), "RSA512"/"RSA256" (v2 file name suffix)
    std::string key_id;     // v3: hex key id; v2: file name ".SIGN.RSA.alpine-devel@..."
    uint32_t size = 0;      // signature blob size in bytes
};

struct Package {
    Format format = Format::Unknown;

    // pkginfo
    std::string name;
    std::string version;
    std::string description;
    std::string arch;
    std::string license;
    std::string origin;
    std::string maintainer;
    std::string url;
    std::string commit;        // v2: "commit" / v3: repo commit
    std::string hash;          // content hash, sha1 hex when available
    int64_t build_time = 0;    // unix seconds, 0 unknown
    int64_t installed_size = 0;  // bytes on disk when installed
    int64_t file_size = 0;     // size of the package file itself
    uint32_t provider_priority = 0;

    std::vector<Dependency> depends;
    std::vector<Dependency> provides;
    std::vector<Dependency> replaces;
    std::vector<Dependency> install_if;
    std::vector<Dependency> recommends;
    std::vector<std::string> tags;

    std::vector<Script> scripts;
    std::vector<FileEntry> files;          // sorted by path
    std::vector<Signature> signatures;

    // Bookkeeping useful for the UI
    uint64_t data_section_size = 0;  // bytes of compressed DATA blocks (v3) / data.tar.gz (v2)
    std::vector<std::string> warnings;  // non fatal oddities (e.g. "zstd block skipped")
};

struct Result {
    bool ok = false;
    std::string error;
    Package pkg;
};

// Parse a complete .apk file held in memory. Detects v2 vs v3 by content.
Result parse(std::span<const uint8_t> file);

// Parse only the v3 ADB object tree (a decompressed ADB block stream, i.e. the
// payload that follows the 4-byte "ADBd"/"ADBz"/"ADBn" compression header).
// Exposed separately because it is the interesting reverse-engineered part and
// is handy for tests.
Result parse_v3_adb(std::span<const uint8_t> adb_stream);

// Cheap content sniffing for the file-type dispatcher.
// True when the buffer starts like an Alpine package (v2 gzip-with-.PKGINFO or
// a v3 "ADB?" container). Does not validate the whole file.
bool looks_like_alpine(std::span<const uint8_t> head);

}  // namespace apkinfo::alpine
