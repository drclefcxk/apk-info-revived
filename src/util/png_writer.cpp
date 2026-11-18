#include "png_writer.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace apkinfo {
namespace {

void put32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void put_chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& payload) {
    put32(out, static_cast<uint32_t>(payload.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    const uint32_t crc = static_cast<uint32_t>(
        crc32(0, out.data() + start, static_cast<uInt>(out.size() - start)));
    put32(out, crc);
}

}  // namespace

bool write_png(const Bitmap& bitmap, const std::wstring& path) {
    if (!bitmap.valid()) return false;

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(bitmap.height) * (1 + static_cast<size_t>(bitmap.width) * 4));
    for (int y = 0; y < bitmap.height; ++y) {
        raw.push_back(0);  // filter type "none"
        const uint8_t* row = bitmap.rgba.data() + static_cast<size_t>(y) * bitmap.width * 4;
        raw.insert(raw.end(), row, row + static_cast<size_t>(bitmap.width) * 4);
    }

    uLongf compressed_size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> compressed(compressed_size);
    if (compress2(compressed.data(), &compressed_size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) {
        return false;
    }
    compressed.resize(compressed_size);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(bitmap.width));
    put32(ihdr, static_cast<uint32_t>(bitmap.height));
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // colour type RGBA
    ihdr.push_back(0);  // compression
    ihdr.push_back(0);  // filter
    ihdr.push_back(0);  // interlace
    put_chunk(png, "IHDR", ihdr);
    put_chunk(png, "IDAT", compressed);
    put_chunk(png, "IEND", {});

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;
    const bool ok = fwrite(png.data(), 1, png.size(), file) == png.size();
    fclose(file);
    return ok;
}

}  // namespace apkinfo
