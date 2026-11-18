#include "model/info_document.h"

namespace apkinfo {

const char* container_kind_name(ContainerKind kind) {
    switch (kind) {
        case ContainerKind::AndroidApk:
            return "Android APK";
        case ContainerKind::AndroidSplitBundle:
            return "Split bundle";
        case ContainerKind::AlpinePackage:
            return "Alpine package";
        case ContainerKind::Unknown:
        default:
            return "Unknown";
    }
}

Bitmap Bitmap::solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    Bitmap bitmap;
    if (w <= 0 || h <= 0) return bitmap;
    bitmap.width = w;
    bitmap.height = h;
    bitmap.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (size_t i = 0; i + 3 < bitmap.rgba.size(); i += 4) {
        bitmap.rgba[i + 0] = r;
        bitmap.rgba[i + 1] = g;
        bitmap.rgba[i + 2] = b;
        bitmap.rgba[i + 3] = a;
    }
    return bitmap;
}

}  // namespace apkinfo
