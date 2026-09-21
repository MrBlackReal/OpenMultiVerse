/*
 * stb_image_impl.c — the one translation unit that compiles stb_image
 * (extern/stb/stb_image.h, public domain, v2.30). JPEG and PNG only: the
 * Earth imagery (render/earth_tex.c) is all this project loads. The library
 * is third-party code, so its warnings are silenced here rather than patched.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG

#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wall"
#  pragma GCC diagnostic ignored "-Wextra"
#  pragma GCC diagnostic ignored "-Wsign-compare"
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
