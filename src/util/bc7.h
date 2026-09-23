/*
 * bc7.h — a small BC7 (BPTC) encoder for opaque colour maps.
 *
 * Uncompressed RGB textures cost 4 bytes a texel on the GPU (RGB8 is padded
 * to RGBA8): the 16k Earth alone was 683 MB with mips on a 4 GB card. BC7 is
 * 1 byte a texel and close to transparent on photographic imagery.
 *
 * Mode 6 only: one colour line per 4x4 block, 7-bit endpoints plus a p-bit
 * (8 effective bits), 16 interpolation levels. It is the mode fast encoders
 * lean on for natural images; blocks with two distinct colours (sharp
 * coastlines) lose a little to the partitioned modes, which this skips.
 * Endpoints: principal axis, then least-squares refinement against the
 * chosen indices, every p-bit pair tried. Values are encoded as stored
 * (sRGB bytes), which is what the hardware interpolates.
 */
#pragma once
#include <stdint.h>

/* Encode one 4x4 block of RGBA8 texels (row-major, 16 x 4 bytes; alpha is
 * written as opaque) into 16 bytes. */
void bc7_encode_block(const uint8_t px[64], uint8_t out[16]);

/* Decode a mode-6 block back to 16 RGBA8 texels (for tests). Returns 0 if the
 * block is not mode 6. */
int  bc7_decode_block6(const uint8_t in[16], uint8_t px[64]);

/* Encode a whole RGBA8 image (w x h, any size; edges padded by clamping) into
 * ((w+3)/4) * ((h+3)/4) blocks. Parallel over block rows. */
void bc7_encode_image(const uint8_t *rgba, int w, int h, uint8_t *out);

/* Round-trip quality check on a synthetic image (--selftest-bc7). */
int  bc7_selftest(void);
