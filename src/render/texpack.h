/*
 * texpack.h — colour maps as BC7 on the GPU, baked once and cached.
 *
 * texpack_load_srgb() loads an equirectangular JPG/PNG as a BC7 sRGB texture
 * with a full mip chain. The first load decodes the image, builds the mips in
 * linear light, encodes every level (bc7.h) and writes `<path>.bc7.dds` next to
 * the source -- a standard DX10 DDS (BC7_UNORM_SRGB) any texture tool opens.
 * Later loads read that file and upload it as is: a quarter of the VRAM of
 * RGB8 (which drivers pad to RGBA8), and no decode or mip build at startup.
 * The cache is rebuilt when the source is newer or the encoder changes.
 *
 * max_px caps the width: the upload starts at the first mip level that fits
 * (and fits GL_MAX_TEXTURE_SIZE), so a lower cap costs nothing extra.
 * Returns 0 without BPTC support or on any failure; the caller then falls
 * back to an uncompressed upload.
 */
#pragma once

unsigned int texpack_load_srgb(const char *path, int max_px, const char *what);
