/*
 * Copyright (c) 2017 Etnaviv Project
 * Copyright (C) 2017 Zodiac Inflight Innovations
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Wladimir J. van der Laan <laanwj@gmail.com>
 */
#ifndef H_ETNAVIV_BLT
#define H_ETNAVIV_BLT

#include "etnaviv_tiling.h"

#include <stdbool.h>
#include <etnaviv_drmif.h>

/* src/dest info for image operations */
struct blt_imginfo
{
    unsigned compressed:1;
    unsigned use_ts:1;
    struct etna_reloc addr;
    struct etna_reloc ts_addr;
    uint32_t format; /* BLT_FORMAT_* */
    uint32_t stride;
    uint32_t compress_fmt; /* COLOR_COMPRESSION_FORMAT_* */
    enum etna_surface_layout tiling; /* ETNA_LAYOUT_* */
    uint32_t ts_clear_value[2];
    uint8_t swizzle[4]; /* TEXTURE_SWIZZLE_* */
    uint8_t cache_mode; /* TS_CACHE_MODE_* */
    uint8_t endian_mode; /* ENDIAN_MODE_* */
    uint8_t bpp; /* # bytes per pixel 1/2/4/8 - only used for CLEAR_IMAGE */
};

/** (Partial) image clear operation.
 */
struct blt_clear_op
{
    struct blt_imginfo dest;
    uint32_t clear_value[2];
    uint32_t clear_bits[2]; /* bit mask of bits to clear */
    uint16_t rect_x;
    uint16_t rect_y;
    uint16_t rect_w;
    uint16_t rect_h;
};

/** Copy image operation.
 */
struct blt_imgcopy_op
{
    struct blt_imginfo src;
    struct blt_imginfo dest;
    uint16_t src_x;
    uint16_t src_y;
    uint16_t dest_x;
    uint16_t dest_y;
    uint16_t rect_w;
    uint16_t rect_h;
};

/** Resolve-in-place operation.
 * Fills unfilled tiles.
 */
struct blt_inplace_op
{
    struct etna_reloc addr;
    struct etna_reloc ts_addr;
    uint32_t ts_clear_value[2];
    uint32_t num_tiles;
};

/** Generate a series of mipmaps for a texture.
 */
struct blt_genmipmaps_op
{
    struct blt_imginfo src;
    struct blt_imginfo dest; /* address is not used, stride must be equal to src stride */
    uint16_t rect_w; /* width of source image */
    uint16_t rect_h; /* height of source image */
    uint32_t num_mips; /* number of mipmaps to generate */
    struct etna_reloc mip_addr[16];
    uint32_t mip_stride[16];
};

/** Clear (part of) an image.
 */
void emit_blt_clearimage(struct etna_cmd_stream *stream, const struct blt_clear_op *op);

/** Copy (a subset of) a linear buffer to another buffer.
 */
void emit_blt_copybuffer(struct etna_cmd_stream *stream, const struct etna_reloc *dest, const struct etna_reloc *src, uint32_t size);

/** Copy (a subset of) an image to another image.
 */
void emit_blt_copyimage(struct etna_cmd_stream *stream, const struct blt_imgcopy_op *op);

/** Emit in-place resolve using BLT.
 */
void emit_blt_inplace(struct etna_cmd_stream *stream, const struct blt_inplace_op *op);

/**
 * Emit command to generate mipmap chain using BLT.
 */
void emit_blt_genmipmaps(struct etna_cmd_stream *stream, const struct blt_genmipmaps_op *op);

/** Make frontend wait for BLT operation.
 */
void emit_blt_sync_fe(struct etna_cmd_stream *stream);

/** Make rasterizer wait for BLT operation.
 */
void emit_blt_sync_ra(struct etna_cmd_stream *stream);

#endif
