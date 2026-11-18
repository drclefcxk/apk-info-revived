// Test harness for ApkInfo/src/alpine/alpine.cpp (built outside src/).
//
//   g++ -std=c++20 -O2 -Wall -Wextra -I<src/alpine> test.cpp <alpine.cpp> -o test.exe -lz
//
// Verifies the real v3 fixture, the pre-extracted v3 adb stream, a synthetic
// v2 package, and runs a truncation fuzz over all of them.
#include "alpine.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void check_eq(const std::string& got, const std::string& want, const char* what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("FAIL: %s: got \"%s\" want \"%s\"\n", what, got.c_str(), want.c_str());
    }
}

std::vector<uint8_t> read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("cannot open %s\n", path);
        return {};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

bool has_dep(const std::vector<apkinfo::alpine::Dependency>& deps, const std::string& name) {
    for (const auto& d : deps)
        if (d.name == name) return true;
    return false;
}

const apkinfo::alpine::FileEntry* find_file(const apkinfo::alpine::Package& pkg,
                                            const std::string& path) {
    for (const auto& f : pkg.files)
        if (f.path == path) return &f;
    return nullptr;
}

std::string to_string_list(const std::vector<apkinfo::alpine::Dependency>& deps) {
    std::string s;
    for (const auto& d : deps) {
        if (!s.empty()) s += ' ';
        s += d.to_string();
    }
    return s;
}

void dump_warnings(const apkinfo::alpine::Package& pkg) {
    for (const auto& w : pkg.warnings) std::printf("    warn: %s\n", w.c_str());
}

// --------------------------------------------------------------------------
void test_v3_stream(const char* path) {
    using namespace apkinfo::alpine;
    const auto data = read_file(path);
    if (data.empty()) { ++g_failures; return; }

    const auto t0 = std::chrono::steady_clock::now();
    Result r = parse_v3_adb(data);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("v3 adb stream (%zu bytes) parsed in %.2f ms: ok=%d err=%s\n", data.size(), ms,
                int(r.ok), r.error.c_str());
    dump_warnings(r.pkg);

    check(r.ok, "v3 stream parses");
    if (!r.ok) return;
    const Package& p = r.pkg;
    check(p.format == Format::V3, "format == V3");
    check_eq(p.name, "busybox", "name");
    check_eq(p.version, "1.37.0-r6", "version");
    check_eq(p.description,
             "The Swiss Army Knife of embedded Linux. It slices, it dices, it makes Julian Fries.",
             "description");
    check_eq(p.arch, "aarch64_cortex-a53", "arch");
    check_eq(p.license, "GPL-2.0", "license");
    check_eq(p.origin, "feeds/base/utils/busybox", "origin");
    check_eq(p.maintainer, "Felix Fietkau <nbd@nbd.name>", "maintainer");
    check_eq(p.url, "http://busybox.net/", "url");
    check(has_dep(p.depends, "libc"), "depends contains libc");
    check(has_dep(p.provides, "busybox-any"), "provides contains busybox-any");
    std::printf("    depends: %s\n    provides: %s\n", to_string_list(p.depends).c_str(),
                to_string_list(p.provides).c_str());
    check(p.hash.size() == 40, "pkginfo hash is a 40-char sha1 hex");
    check(p.files.size() >= 140 && p.files.size() <= 200,
          "files.size() plausible: " + std::to_string(p.files.size()));
    check(p.scripts.size() >= 2, "scripts present: " + std::to_string(p.scripts.size()));
    std::printf("    scripts:");
    for (const auto& s : p.scripts) std::printf(" %s(%zu)", s.name.c_str(), s.body.size());
    std::printf("\n");
    check(p.installed_size == 465465, "installed_size 465465, got " + std::to_string(p.installed_size));
    check(p.provider_priority == 100, "provider_priority 100");
    check(p.build_time == 0, "build_time absent in this package => 0 (got " +
                                 std::to_string(p.build_time) + ")");
    check(p.signatures.size() == 1, "one signature");
    if (!p.signatures.empty()) {
        check_eq(p.signatures[0].algorithm, "RSA", "signature algorithm");
        check(p.signatures[0].key_id == "a18d037a9207dd46f135dbb967d154b7",
              "signature key id, got " + p.signatures[0].key_id);
        check(p.signatures[0].size == 89, "signature size 89, got " +
                                               std::to_string(p.signatures[0].size));
    }
    check(p.data_section_size > 400000, "data_section_size " +
                                            std::to_string(p.data_section_size));

    // NOTE: this OpenWrt build installs the real binary as /bin/busybox and
    // everything else (including all of /usr/bin) as symlinks to it, so
    // "/usr/bin/busybox" does not exist in this particular package.
    const FileEntry* busybox = find_file(p, "/bin/busybox");
    check(busybox != nullptr, "file /bin/busybox present");
    if (busybox) {
        std::printf("    /bin/busybox: size=%llu mode=0%o mtime=%llu user=%s sha1=%s\n",
                    (unsigned long long)busybox->size, busybox->mode,
                    (unsigned long long)busybox->mtime, busybox->user.c_str(),
                    busybox->sha1.c_str());
        check(busybox->size == 458773, "busybox size 458773");
        check((busybox->mode & 0111) != 0, "busybox is executable");
        check(busybox->sha1.size() == 64 || busybox->sha1.size() == 40,
              "busybox content digest hex (" + std::to_string(busybox->sha1.size()) + " chars)");
        check(busybox->mtime > 1600000000ull && busybox->mtime < 2000000000ull,
              "busybox mtime plausible");
        check(busybox->user == "root" && busybox->group == "root", "busybox owner root:root");
    }
    const FileEntry* link = find_file(p, "/bin/ash");
    check(link != nullptr && link->is_symlink && link->target == "busybox",
          "symlink /bin/ash -> busybox");
    const FileEntry* ubin = find_file(p, "/usr/bin/awk");
    check(ubin != nullptr && ubin->is_symlink && ubin->target == "../../bin/busybox",
          "/usr/bin/awk is a relative symlink to ../../bin/busybox");
    const FileEntry* dir = find_file(p, "/usr/bin/");
    check(dir != nullptr && dir->is_dir, "directory /usr/bin/ listed");

    bool sorted = true;
    for (size_t i = 1; i < p.files.size(); ++i)
        if (p.files[i - 1].path > p.files[i].path) sorted = false;
    check(sorted, "files sorted by path");

    size_t dirs = 0, links = 0, regs = 0;
    uint64_t reg_bytes = 0;
    for (const auto& f : p.files) {
        if (f.is_dir) ++dirs;
        else if (f.is_symlink) ++links;
        else { ++regs; reg_bytes += f.size; }
    }
    std::printf("    %zu entries = %zu dirs + %zu symlinks + %zu regular; regular bytes=%llu\n",
                p.files.size(), dirs, links, regs, (unsigned long long)reg_bytes);
    check(reg_bytes == uint64_t(p.installed_size), "sum of regular file sizes == installed_size");
}

// --------------------------------------------------------------------------
void test_v3_file(const char* path) {
    using namespace apkinfo::alpine;
    const auto data = read_file(path);
    if (data.empty()) { ++g_failures; return; }
    const auto t0 = std::chrono::steady_clock::now();
    Result r = parse(data);
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("v3 .apk (%zu bytes) parsed in %.2f ms: ok=%d err=%s\n", data.size(),
                std::chrono::duration<double, std::milli>(t1 - t0).count(), int(r.ok),
                r.error.c_str());
    check(r.ok, "v3 file parses");
    if (!r.ok) return;
    check(r.pkg.format == Format::V3, "v3 format");
    check(r.pkg.file_size == int64_t(data.size()),
          "file_size == real file size (" + std::to_string(r.pkg.file_size) + ")");
    check(r.pkg.files.size() >= 140, "v3 file listing");
    check(r.pkg.name == "busybox", "v3 file name");
    std::printf("    data_section_size=%llu, %zu files, %zu warnings\n",
                (unsigned long long)r.pkg.data_section_size, r.pkg.files.size(),
                r.pkg.warnings.size());

    // A truncated .apk must still yield the (intact) metadata, but say so.
    {
        std::vector<uint8_t> cut(data.begin(), data.begin() + data.size() * 3 / 4);
        const Result tr = parse(cut);
        bool warned = false;
        for (const auto& w : tr.pkg.warnings)
            if (w.find("truncated") != std::string::npos ||
                w.find("smaller than") != std::string::npos)
                warned = true;
        std::printf("    3/4-truncated .apk: ok=%d files=%zu warned=%d first warn=%s\n", int(tr.ok),
                    tr.pkg.files.size(), int(warned),
                    tr.pkg.warnings.empty() ? "-" : tr.pkg.warnings[0].c_str());
        check(tr.ok && tr.pkg.name == "busybox", "truncated v3 .apk still yields metadata");
        check(warned, "truncated v3 .apk reports the truncation");
    }
}

// --------------------------------------------------------------------------
void test_v2(const char* path) {
    using namespace apkinfo::alpine;
    const auto data = read_file(path);
    if (data.empty()) { ++g_failures; return; }
    const auto t0 = std::chrono::steady_clock::now();
    Result r = parse(data);
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("v2 synthetic (%zu bytes) parsed in %.2f ms: ok=%d err=%s\n", data.size(),
                std::chrono::duration<double, std::milli>(t1 - t0).count(), int(r.ok),
                r.error.c_str());
    dump_warnings(r.pkg);
    check(r.ok, "v2 parses");
    if (!r.ok) return;
    const Package& p = r.pkg;
    check(p.format == Format::V2, "v2 format");
    check_eq(p.name, "testpkg", "v2 name");
    check_eq(p.version, "2.3.4-r1", "v2 version");
    check_eq(p.description, "A synthetic test package for the alpine reader.", "v2 description");
    check_eq(p.url, "https://example.org/testpkg", "v2 url");
    check_eq(p.arch, "x86_64", "v2 arch");
    check_eq(p.license, "MIT", "v2 license");
    check_eq(p.origin, "testpkg", "v2 origin");
    check_eq(p.maintainer, "Test Maintainer <maint@example.org>", "v2 maintainer");
    check_eq(p.commit, "0123456789abcdef0123456789abcdef01234567", "v2 commit");
    check_eq(p.hash, "0123456789abcdef0123456789abcdef", "v2 datahash");
    check(p.build_time == 1700000000, "v2 builddate");
    check(p.installed_size == 4096, "v2 size");
    check(p.provider_priority == 7, "v2 provider_priority");
    check(p.file_size == int64_t(data.size()), "v2 file_size == file size");

    check(p.depends.size() == 4, "v2 depends count " + std::to_string(p.depends.size()));
    if (p.depends.size() == 4) {
        check_eq(p.depends[0].name, "musl", "dep0 name");
        check_eq(p.depends[0].op, ">=", "dep0 op");
        check_eq(p.depends[0].version, "1.2.4-r0", "dep0 version");
        check_eq(p.depends[0].to_string(), "musl>=1.2.4-r0", "dep0 round-trip");
        check_eq(p.depends[1].name, "so:libc.musl-x86_64.so.1", "dep1 keeps prefix:");
        check_eq(p.depends[1].op, "", "dep1 op empty");
        check_eq(p.depends[1].to_string(), "so:libc.musl-x86_64.so.1", "dep1 round-trip");
        check_eq(p.depends[2].to_string(), "!conflicting-pkg", "dep2 conflict round-trip");
        check_eq(p.depends[3].to_string(), "fuzzy-pkg~=1.2.3", "dep3 fuzzy round-trip");
        check_eq(p.depends[3].op, "~=", "dep3 op ~=");
    }
    check(p.provides.size() == 1 && p.provides[0].to_string() == "test-any=2.3.4-r1",
          "provides round-trip: " + to_string_list(p.provides));
    check(p.replaces.size() == 1 && p.replaces[0].to_string() == "oldtestpkg<1.0",
          "replaces round-trip");
    check(p.install_if.size() == 1 && p.install_if[0].to_string() == "base-files",
          "install_if round-trip");
    check(p.recommends.size() == 1 && p.recommends[0].name == "suggest-pkg",
          "recommends round-trip");

    check(p.scripts.size() == 3, "v2 scripts: " + std::to_string(p.scripts.size()));
    bool have_post = false, have_pre = false, have_trig = false;
    for (const auto& s : p.scripts) {
        if (s.name == "post-install" && s.body.find("post-install ran") != std::string::npos)
            have_post = true;
        if (s.name == "pre-install" && s.body.find("pre-install") != std::string::npos)
            have_pre = true;
        if (s.name == "trigger" && s.body.find("trigger") != std::string::npos)
            have_trig = true;
    }
    check(have_post, "v2 post-install script body");
    check(have_pre, "v2 pre-install script body");
    check(have_trig, "v2 trigger script body");

    check(p.signatures.size() == 1, "v2 one signature");
    if (!p.signatures.empty()) {
        check_eq(p.signatures[0].algorithm, "RSA", "v2 signature alg");
        check_eq(p.signatures[0].key_id, "test@example.org", "v2 signature key");
        check(p.signatures[0].size == 512, "v2 signature size");
    }

    std::printf("    v2 files (%zu):", p.files.size());
    for (const auto& f : p.files) std::printf(" %s", f.path.c_str());
    std::printf("\n");
    check(p.files.size() == 6, "v2 file count");
    const FileEntry* bin = find_file(p, "/usr/bin/testpkg");
    check(bin != nullptr && bin->size == 21 && bin->mode == 0755 && !bin->is_dir &&
              !bin->is_symlink,
          "v2 regular file entry");
    const FileEntry* conf = find_file(p, "/etc/testpkg.conf");
    check(conf != nullptr && conf->size == 12 && conf->user == "root", "v2 config file entry");
    const FileEntry* lnk = find_file(p, "/usr/bin/testlink");
    check(lnk != nullptr && lnk->is_symlink && lnk->target == "testpkg", "v2 symlink entry");
    const FileEntry* dir = find_file(p, "/usr/bin/");
    check(dir != nullptr && dir->is_dir, "v2 directory entry");
    check(p.data_section_size == 235, "v2 data_section_size is the data stream size (" +
                                          std::to_string(p.data_section_size) + ")");
}

// --------------------------------------------------------------------------
// truncation fuzz: every prefix of every file must not crash
// --------------------------------------------------------------------------
void fuzz(const char* label, const std::vector<uint8_t>& data, size_t step) {
    using namespace apkinfo::alpine;
    int ok = 0, bad = 0;
    for (size_t n = 0; n <= data.size(); n += step) {
        const Result r = parse(std::span<const uint8_t>(data.data(), n));
        if (r.ok) ++ok; else ++bad;
        if (!r.ok && r.error.empty()) {
            ++g_failures;
            std::printf("FAIL: %s truncation at %zu produced no error message\n", label, n);
            return;
        }
    }
    std::printf("fuzz %s: %zu prefixes (step %zu), %d ok, %d rejected, no crash\n", label,
                data.size() / step + 1, step, ok, bad);
}

// byte corruption fuzz: flip bytes, must never crash
void fuzz_corrupt(const char* label, const std::vector<uint8_t>& data, int iterations) {
    using namespace apkinfo::alpine;
    int ok = 0;
    std::vector<uint8_t> copy;
    uint32_t seed = 12345;
    for (int i = 0; i < iterations; ++i) {
        copy = data;
        for (int k = 0; k < 4; ++k) {
            seed = seed * 1103515245u + 12345u;
            const size_t at = (seed >> 8) % copy.size();
            seed = seed * 1103515245u + 12345u;
            copy[at] = static_cast<uint8_t>(seed >> 16);
        }
        const Result r = parse(copy);
        if (r.ok) ++ok;
    }
    std::printf("fuzz %s: %d corrupted inputs, %d still parsed, no crash\n", label, iterations, ok);
}

// --------------------------------------------------------------------------
// every compression marker variant must reach the same adb stream
// --------------------------------------------------------------------------
void test_containers(const char* apk_path, const char* stream_path) {
    using namespace apkinfo::alpine;
    const auto apk = read_file(apk_path);
    const auto adb = read_file(stream_path);
    if (apk.empty() || adb.empty()) { ++g_failures; return; }

    std::vector<uint8_t> buf;

    // 1) uncompressed: "ADB." is both the marker and the adb file header
    buf.assign(adb.begin(), adb.end());
    {
        const Result r = parse(buf);
        check(r.ok && r.pkg.name == "busybox" && r.pkg.format == Format::V3,
              "uncompressed container (ADB. magic) parses");
        std::printf("    uncompressed container: ok=%d files=%zu\n", int(r.ok), r.pkg.files.size());
    }

    // 2) 'n' marker + stream at +4
    buf = {'A', 'D', 'B', 'n'};
    buf.insert(buf.end(), adb.begin(), adb.end());
    {
        const Result r = parse(buf);
        check(r.ok && r.pkg.name == "busybox", "'n' (no compression) container parses");
    }

    // 3) 'c' marker with alg 0 (none) + level
    buf = {'A', 'D', 'B', 'c', 0, 0};
    buf.insert(buf.end(), adb.begin(), adb.end());
    {
        const Result r = parse(buf);
        check(r.ok && r.pkg.name == "busybox", "'c' container with alg 0 parses");
    }

    // 4) 'c' marker with alg 1 (deflate), reusing the real file's deflate body
    buf = {'A', 'D', 'B', 'c', 1, 0};
    buf.insert(buf.end(), apk.begin() + 4, apk.end());
    {
        const Result r = parse(buf);
        check(r.ok && r.pkg.name == "busybox" && r.pkg.files.size() >= 140,
              "'c' container with alg 1 (deflate) parses");
        check(r.pkg.data_section_size > 400000, "'c' container data_section_size");
    }

    // 5) a 'z' marker in front of non-zstd bytes: must fail cleanly either way
    buf = {'A', 'D', 'B', 'z'};
    buf.insert(buf.end(), apk.begin() + 4, apk.end());
    {
        const Result r = parse(buf);
        check(!r.ok && r.error.find("zstd") != std::string::npos,
              "deflate body behind a 'z' marker fails with a zstd message: " + r.error);
        std::printf("    zstd-garbage container: ok=%d err=%s\n", int(r.ok), r.error.c_str());
    }

    // 6) garbage / unrelated formats and the zstd variants built as fixtures
    {
        const uint8_t junk[] = {'P', 'K', 3, 4, 20, 0, 0, 0};
        const Result r = parse(std::span<const uint8_t>(junk, sizeof junk));
        check(!r.ok && !r.error.empty(), "zip input rejected");
        const Result r2 = parse(std::span<const uint8_t>());
        check(!r2.ok, "empty input rejected");
        const uint8_t adbonly[] = {'A', 'D', 'B', 'X', 0, 0, 0, 0};
        const Result r3 = parse(std::span<const uint8_t>(adbonly, sizeof adbonly));
        check(!r3.ok && r3.error.find("compression marker") != std::string::npos,
              "unknown compression marker rejected: " + r3.error);
    }
}

// Real zstd containers ("ADBz..." and "ADBc" + spec), built from the same adb
// stream with the zstd CLI.  Only meaningful when APKINFO_HAVE_ZSTD is set.
void test_zstd_containers(const char* dir) {
    using namespace apkinfo::alpine;
    const char* names[] = {"/synthetic_v3_zstd.apk", "/synthetic_v3_zstc.apk"};
    for (const char* name : names) {
        const auto data = read_file((std::string(dir) + name).c_str());
        if (data.empty()) {
            ++g_failures;
            std::printf("FAIL: missing %s\n", name);
            continue;
        }
        const Result r = parse(data);
#ifdef APKINFO_HAVE_ZSTD
        check(r.ok && r.pkg.name == "busybox" && r.pkg.files.size() >= 140
                  && r.pkg.data_section_size > 400000,
              std::string("zstd container ") + name + " parses: " + r.error);
        std::printf("    %s: ok=%d files=%zu data=%llu\n", name, int(r.ok), r.pkg.files.size(),
                    (unsigned long long)r.pkg.data_section_size);
#else
        check(!r.ok && r.error.find("zstd") != std::string::npos,
              std::string("zstd container ") + name + " fails with a zstd message");
        std::printf("    %s: ok=%d err=%s\n", name, int(r.ok), r.error.c_str());
#endif
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string base = argc > 1 ? argv[1] : ".";
    const std::string apk = base + "/alpine-busybox.apk";
    const std::string stream = base + "/alpine_v3_payload.bin";
    const std::string v2 = base + "/synthetic_v2.apk";

    test_v3_stream(stream.c_str());
    test_v3_file(apk.c_str());
    test_v2(v2.c_str());
    test_containers(apk.c_str(), stream.c_str());
    test_zstd_containers(base.c_str());

    // v2 variant without a signature stream
    {
        using namespace apkinfo::alpine;
        const auto nosig = read_file((base + "/synthetic_v2_nosig.apk").c_str());
        if (nosig.empty()) {
            ++g_failures;
            std::printf("FAIL: missing synthetic_v2_nosig.apk\n");
        } else {
            const Result r = parse(nosig);
            std::printf("v2 without signature stream parsed: ok=%d err=%s\n", int(r.ok),
                        r.error.c_str());
            check(r.ok && r.pkg.name == "testpkg" && r.pkg.signatures.empty() &&
                      r.pkg.files.size() == 6 && r.pkg.scripts.size() == 3,
                  "v2 without a signature stream (control stream detected by .PKGINFO)");
            check(r.pkg.data_section_size == 235, "v2 nosig data_section_size");
        }
    }

    // sniffing
    {
        using namespace apkinfo::alpine;
        const auto a = read_file(apk.c_str());
        const auto v = read_file(v2.c_str());
        check(looks_like_alpine(std::span<const uint8_t>(a.data(), std::min<size_t>(a.size(), 64))),
              "looks_like_alpine(v3)");
        check(looks_like_alpine(std::span<const uint8_t>(v.data(), std::min<size_t>(v.size(), 64))),
              "looks_like_alpine(v2)");
        const uint8_t zip[4] = {'P', 'K', 3, 4};
        check(!looks_like_alpine(std::span<const uint8_t>(zip, 4)), "looks_like_alpine(zip)=false");
        check(format_name(Format::V3) == std::string("v3"), "format_name(v3)");
        check(format_name(Format::Unknown) == std::string("unknown"), "format_name(unknown)");
    }

    // fuzzers
    {
        const auto a = read_file(apk.c_str());
        const auto s = read_file(stream.c_str());
        const auto v = read_file(v2.c_str());
        fuzz("v3 .apk", a, 251);
        fuzz("v3 adb stream", s, 997);
        fuzz("v2 synthetic", v, 1);
        fuzz_corrupt("v3 .apk", a, 300);
        fuzz_corrupt("v2 synthetic", v, 300);
        // direct parse_v3_adb truncation
        for (size_t n = 0; n < s.size(); n += 7919) {
            const auto r = apkinfo::alpine::parse_v3_adb(std::span<const uint8_t>(s.data(), n));
            (void)r;
        }
        std::printf("fuzz parse_v3_adb truncation: no crash\n");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
