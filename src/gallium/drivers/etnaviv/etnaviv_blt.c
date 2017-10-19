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
#include "etnaviv_blt.h"

#include "etnaviv_emit.h"

#include "hw/state.xml.h"
#include "hw/common_3d.xml.h"
#include "hw/state_3d.xml.h"
#include "hw/state_blt.xml.h"

#include <assert.h>

static inline uint32_t blt_compute_stride_bits(const struct blt_imginfo *img)
{
    return VIVS_BLT_DEST_STRIDE_TILING(img->tiling == ETNA_LAYOUT_LINEAR ? 0 : 3) |
           VIVS_BLT_DEST_STRIDE_FORMAT(img->format) |
           VIVS_BLT_DEST_STRIDE_STRIDE(img->stride);
}

static inline uint32_t blt_compute_img_config_bits(const struct blt_imginfo *img, bool for_dest)
{
    uint32_t tiling_bits = 0;
    if (img->tiling == ETNA_LAYOUT_SUPER_TILED) {
        tiling_bits |= for_dest ? BLT_IMAGE_CONFIG_TO_SUPER_TILED : BLT_IMAGE_CONFIG_FROM_SUPER_TILED;
    }

    return BLT_IMAGE_CONFIG_CACHE_MODE(img->cache_mode) |
           COND(img->use_ts, BLT_IMAGE_CONFIG_TS) |
           COND(img->compressed, BLT_IMAGE_CONFIG_COMPRESSION) |
           BLT_IMAGE_CONFIG_COMPRESSION_FORMAT(img->compress_fmt) |
           COND(for_dest, BLT_IMAGE_CONFIG_UNK22) |
           BLT_IMAGE_CONFIG_SWIZ_R(0) | /* not used? */
           BLT_IMAGE_CONFIG_SWIZ_G(1) |
           BLT_IMAGE_CONFIG_SWIZ_B(2) |
           BLT_IMAGE_CONFIG_SWIZ_A(3) |
           tiling_bits;
}

static inline uint32_t blt_compute_swizzle_bits(const struct blt_imginfo *img, bool for_dest)
{
    uint32_t swiz = VIVS_BLT_SWIZZLE_SRC_R(img->swizzle[0]) |
                    VIVS_BLT_SWIZZLE_SRC_G(img->swizzle[1]) |
                    VIVS_BLT_SWIZZLE_SRC_B(img->swizzle[2]) |
                    VIVS_BLT_SWIZZLE_SRC_A(img->swizzle[3]);
    return for_dest ? (swiz << 12) : swiz;
}

void emit_blt_clearimage(struct etna_cmd_stream *stream, const struct blt_clear_op *op)
{
    etna_cmd_stream_reserve(stream, 64*2); /* Make sure BLT op doesn't get broken up */

    etna_set_state(stream, VIVS_GL_FLUSH_CACHE, 0x00000c23);
    etna_set_state(stream, VIVS_TS_FLUSH_CACHE, 0x00000001);

    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000001);
    assert(op->dest.bpp);
    etna_set_state(stream, VIVS_BLT_CONFIG, VIVS_BLT_CONFIG_CLEAR_BPP(op->dest.bpp-1));
    /* NB: blob sets format to 1 in dest/src config for clear, and the swizzle to RRRR.
     * does this matter? It seems to just be ignored. But if we run into issues with BLT
     * behaving stragely, it's something to look at.
     */
    etna_set_state(stream, VIVS_BLT_DEST_STRIDE, blt_compute_stride_bits(&op->dest));
    etna_set_state(stream, VIVS_BLT_DEST_CONFIG, blt_compute_img_config_bits(&op->dest, true));
    etna_set_state_reloc(stream, VIVS_BLT_DEST_ADDR, &op->dest.addr);
    etna_set_state(stream, VIVS_BLT_SRC_STRIDE, blt_compute_stride_bits(&op->dest));
    etna_set_state(stream, VIVS_BLT_SRC_CONFIG, blt_compute_img_config_bits(&op->dest, false));
    etna_set_state_reloc(stream, VIVS_BLT_SRC_ADDR, &op->dest.addr);
    etna_set_state(stream, VIVS_BLT_DEST_POS, VIVS_BLT_DEST_POS_X(op->rect_x) | VIVS_BLT_DEST_POS_Y(op->rect_y));
    etna_set_state(stream, VIVS_BLT_IMAGE_SIZE, VIVS_BLT_IMAGE_SIZE_WIDTH(op->rect_w) | VIVS_BLT_IMAGE_SIZE_HEIGHT(op->rect_h));
    etna_set_state(stream, VIVS_BLT_CLEAR_COLOR0, op->clear_value[0]);
    etna_set_state(stream, VIVS_BLT_CLEAR_COLOR1, op->clear_value[1]);
    etna_set_state(stream, VIVS_BLT_CLEAR_BITS0, op->clear_bits[0]);
    etna_set_state(stream, VIVS_BLT_CLEAR_BITS1, op->clear_bits[1]);
    if (op->dest.use_ts) {
        etna_set_state_reloc(stream, VIVS_BLT_DEST_TS, &op->dest.ts_addr);
        etna_set_state_reloc(stream, VIVS_BLT_SRC_TS, &op->dest.ts_addr);
        etna_set_state(stream, VIVS_BLT_DEST_TS_CLEAR_VALUE0, op->dest.ts_clear_value[0]);
        etna_set_state(stream, VIVS_BLT_DEST_TS_CLEAR_VALUE1, op->dest.ts_clear_value[1]);
        etna_set_state(stream, VIVS_BLT_SRC_TS_CLEAR_VALUE0, op->dest.ts_clear_value[0]);
        etna_set_state(stream, VIVS_BLT_SRC_TS_CLEAR_VALUE1, op->dest.ts_clear_value[1]);
    }
    etna_set_state(stream, VIVS_BLT_SET_COMMAND, 0x00000003);
    etna_set_state(stream, VIVS_BLT_COMMAND, VIVS_BLT_COMMAND_COMMAND_CLEAR_IMAGE);
    etna_set_state(stream, VIVS_BLT_SET_COMMAND, 0x00000003);
    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000000);
}

void emit_blt_copybuffer(struct etna_cmd_stream *stream, const struct etna_reloc *dest, const struct etna_reloc *src, uint32_t size)
{
    etna_cmd_stream_reserve(stream, 64*2); /* Make sure BLT op doesn't get broken up */

    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000001);
    etna_set_state_reloc(stream, VIVS_BLT_SRC_ADDR, src);
    etna_set_state_reloc(stream, VIVS_BLT_DEST_ADDR, dest);
    etna_set_state(stream, VIVS_BLT_BUFFER_SIZE, size);
    etna_set_state(stream, VIVS_BLT_SET_COMMAND, 0x00000003);
    etna_set_state(stream, VIVS_BLT_COMMAND, VIVS_BLT_COMMAND_COMMAND_COPY_BUFFER);
    etna_set_state(stream, VIVS_BLT_SET_COMMAND, 0x00000003);
    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000000);

    /* Synchronize FE with BLT, because we want to see result after finishing command buffer */
    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000001);
    etna_set_state(stream, VIVS_GL_SEMAPHORE_TOKEN, 0x30001001);
    etna_cmd_stream_emit(stream, 0x48000000); /* command STALL (9) OP=STALL */
    etna_cmd_stream_emit(stream, 0x30001001); /* command   TOKEN FROM=FE,TO=BLT,UNK28=0x3 */
    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000000);
}

void emit_blt_copyimage(struct etna_cmd_stream *stream, const struct blt_imgcopy_op *op)
{
    etna_cmd_stream_reserve(stream, 64*2); /* Never allow BLT sequences to be broken up */
    etna_set_state(stream, VIVS_GL_FLUSH_CACHE, 0x00000c23);
    etna_set_state(stream, VIVS_TS_FLUSH_CACHE, 0x00000001);

    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000001);
    etna_set_state(stream, VIVS_BLT_CONFIG,
            VIVS_BLT_CONFIG_SRC_ENDIAN(op->src.endian_mode) |
            VIVS_BLT_CONFIG_DEST_ENDIAN(op->dest.endian_mode));
    etna_set_state(stream, VIVS_BLT_SRC_STRIDE, blt_compute_stride_bits(&op->src));
    etna_set_state(stream, VIVS_BLT_SRC_CONFIG, blt_compute_img_config_bits(&op->src, false));
    etna_set_state(stream, VIVS_BLT_SWIZZLE,
            blt_compute_swizzle_bits(&op->src, false) |
            blt_compute_swizzle_bits(&op->dest, true));
    etna_set_state(stream, VIVS_BLT_UNK140A0, 0x00040004);
    etna_set_state(stream, VIVS_BLT_UNK1409C, 0x00400040);
    if (op->src.use_ts) {
        etna_set_state_reloc(stream, VIVS_BLT_SRC_TS, &op->src.ts_addr);
        etna_set_state(stream, VIVS_BLT_SRC_TS_CLEAR_VALUE0, op->src.ts_clear_value[0]);
        etna_set_state(stream, VIVS_BLT_SRC_TS_CLEAR_VALUE1, op->src.ts_clear_value[1]);
    }
    etna_set_state_reloc(stream, VIVS_BLT_SRC_ADDR, &op->src.addr);
    etna_set_state(stream, VIVS_BLT_DEST_STRIDE, blt_compute_stride_bits(&op->dest));
    etna_set_state(stream, VIVS_BLT_DEST_CONFIG, blt_compute_img_config_bits(&op->dest, true));
    assert(!op->dest.use_ts); /* Dest TS path doesn't work for copies? */
    if (op->dest.use_ts) {
        etna_set_state_reloc(stream, VIVS_BLT_DEST_TS, &op->dest.ts_addr);
        etna_set_state(stream, VIVS_BLT_DEST_TS_CLEAR_VALUE0, op->dest.ts_clear_value[0]);
        etna_set_state(stream, VIVS_BLT_DEST_TS_CLEAR_VALUE1, op->dest.ts_clear_value[1]);
    }
    etna_set_state_reloc(stream, VIVS_BLT_DEST_ADDR, &op->dest.addr);
    etna_set_state(stream, VIVS_BLT_SRC_POS, VIVS_BLT_DEST_POS_X(op->src_x) | VIVS_BLT_DEST_POS_Y(op->src_y));
    etna_set_state(stream, VIVS_BLT_DEST_POS, VIVS_BLT_DEST_POS_X(op->dest_x) | VIVS_BLT_DEST_POS_Y(op->dest_y));
    etna_set_state(stream, VIVS_BLT_IMAGE_SIZE, VIVS_BLT_IMAGE_SIZE_WIDTH(op->rect_w) | VIVS_BLT_IMAGE_SIZE_HEIGHT(op->rect_h));
    etna_set_state(stream, VIVS_BLT_UNK14058, 0xffffffff);
    etna_set_state(stream, VIVS_BLT_UNK1405C, 0xffffffff);
    etna_set_state(stream, VIVS_BLT_SET_COMMAND, 0x00000003);
    etna_set_state(stream, VIVS_BLT_COMMAND, VIVS_BLT_COMMAND_COMMAND_COPY_IMAGE);
    etna_set_state(stream, VIVS_BLT_SET_COMMAND, 0x00000003);
    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000000);
}

void emit_blt_inplace(struct etna_cmd_stream *stream, const struct blt_inplace_op *op)
{
    etna_cmd_stream_reserve(stream, 64*2); /* Never allow BLT sequences to be broken up */
    etna_set_state(stream, VIVS_GL_FLUSH_CACHE, 0x00000c23);
    etna_set_state(stream, VIVS_TS_FLUSH_CACHE, 0x00000001);

    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000001);
    etna_set_state(stream, VIVS_BLT_CONFIG, (1<<10) | (1<<11) | (1<<15));
    etna_set_state(stream, VIVS_BLT_DEST_TS_CLEAR_VALUE0, op->ts_clear_value[0]);
    etna_set_state(stream, VIVS_BLT_DEST_TS_CLEAR_VALUE1, op->ts_clear_value[1]);
    etna_set_state_reloc(stream, VIVS_BLT_DEST_ADDR, &op->addr);
    etna_set_state_reloc(stream, VIVS_BLT_DEST_TS, &op->ts_addr);
    etna_set_state(stream, 0x14068, op->num_tiles);
    etna_set_state(stream, VIVS_BLT_SET_COMMAND, 0x00000003);
    etna_set_state(stream, VIVS_BLT_COMMAND, 0x00000004);
    etna_set_state(stream, VIVS_BLT_SET_COMMAND, 0x00000003);
    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000000);
}

void emit_blt_genmipmaps(struct etna_cmd_stream *stream, const struct blt_genmipmaps_op *op)
{
    etna_cmd_stream_reserve(stream, 64*2); /* Never allow BLT sequences to be broken up */
    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000001);
    etna_set_state(stream, VIVS_BLT_SRC_STRIDE, blt_compute_stride_bits(&op->src));
    etna_set_state(stream, VIVS_BLT_SRC_CONFIG, blt_compute_img_config_bits(&op->src, false));
    etna_set_state_reloc(stream, VIVS_BLT_SRC_ADDR, &op->src.addr);
    etna_set_state(stream, VIVS_BLT_DEST_STRIDE, blt_compute_stride_bits(&op->dest));
    etna_set_state(stream, VIVS_BLT_DEST_CONFIG, blt_compute_img_config_bits(&op->dest, true));
    etna_set_state(stream, VIVS_BLT_IMAGE_SIZE, VIVS_BLT_IMAGE_SIZE_WIDTH(op->rect_w) | VIVS_BLT_IMAGE_SIZE_HEIGHT(op->rect_h));
    etna_set_state(stream, VIVS_BLT_SWIZZLE,
            blt_compute_swizzle_bits(&op->src, false) |
            blt_compute_swizzle_bits(&op->dest, true));
    for (unsigned level=0; level<op->num_mips; ++level) {
        etna_set_state_reloc(stream, VIVS_BLT_MIP_ADDR(level), &op->mip_addr[level]);
        etna_set_state(stream, VIVS_BLT_MIP_STRIDE(level), op->mip_stride[level]);
    }
    etna_set_state(stream, VIVS_BLT_MIPMAP_CONFIG, VIVS_BLT_MIPMAP_CONFIG_UNK5 |
            VIVS_BLT_MIPMAP_CONFIG_NUM(op->num_mips+1));
    etna_set_state(stream, VIVS_BLT_CONFIG,
            VIVS_BLT_CONFIG_SRC_ENDIAN(op->src.endian_mode) |
            VIVS_BLT_CONFIG_DEST_ENDIAN(op->dest.endian_mode));
    etna_set_state(stream, VIVS_BLT_SET_COMMAND, 0x00000003);
    etna_set_state(stream, VIVS_BLT_COMMAND, VIVS_BLT_COMMAND_COMMAND_GEN_MIPMAPS);
    etna_set_state(stream, VIVS_BLT_SET_COMMAND, 0x00000003);
    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000000);
}

void emit_blt_sync_fe(struct etna_cmd_stream *stream)
{
    etna_cmd_stream_reserve(stream, 64*2); /* Never allow BLT sequences to be broken up */
    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000001);
    etna_set_state(stream, VIVS_GL_SEMAPHORE_TOKEN, 0x00001001);
    etna_cmd_stream_emit(stream, 0x48000000); /* command STALL (9) OP=STALL */
    etna_cmd_stream_emit(stream, 0x00001001); /* command   TOKEN FROM=FE,TO=BLT,UNK28=0x0 */
    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000000);

    etna_set_state(stream, VIVS_GL_FLUSH_CACHE, 0x00000c23);
}

void emit_blt_sync_ra(struct etna_cmd_stream *stream)
{
    etna_cmd_stream_reserve(stream, 64*2); /* Never allow BLT sequences to be broken up */
    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000001);
    etna_set_state(stream, VIVS_GL_SEMAPHORE_TOKEN, 0x00001005); /* Make RA wait for BLT */
    etna_set_state(stream, VIVS_GL_STALL_TOKEN, 0x00001005);
    etna_set_state(stream, VIVS_BLT_ENABLE, 0x00000000);

    etna_set_state(stream, VIVS_GL_FLUSH_CACHE, 0x00000002);
    etna_set_state(stream, VIVS_DUMMY_DUMMY, 0x00000000);
}


