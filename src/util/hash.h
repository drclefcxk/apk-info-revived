// SHA-256 helpers (BCrypt), used for the file fingerprint row.
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace apkinfo {

// Lower-case hex SHA-256 of `data`. Returns "" when the hash cannot be computed.
std::string sha256_hex(std::span<const uint8_t> data);

// Same digest, but streamed from a file handle. Used for the *lazy* fingerprint:
// the app hashes the package on a worker thread after the window is up, so a
// 100 MB+ file is never kept mapped just to be hashed.
std::string sha256_hex_file(const std::string& path);

}  // namespace apkinfo
