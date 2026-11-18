// Translation unit that instantiates stb_image (single-header library).
// Kept separate so the (slow) implementation is compiled once.
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
