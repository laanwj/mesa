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

#include "etnaviv_clear_blit.h"

#include "hw/common.xml.h"

#include "etnaviv_blt.h"
#include "etnaviv_context.h"
#include "etnaviv_emit.h"
#include "etnaviv_format.h"
#include "etnaviv_resource.h"
#include "etnaviv_surface.h"
#include "etnaviv_translate.h"

#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_blitter.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_surface.h"

#define translate_blt_format translate_rs_format

static void
etna_blit_clear_color_blt(struct pipe_context *pctx, struct pipe_surface *dst,
                      const union pipe_color_union *color)
{
   struct etna_context *ctx = etna_context(pctx);
   struct etna_surface *surf = etna_surface(dst);
   uint32_t new_clear_value = etna_clear_blit_pack_rgba(surf->base.format, color->f);

   struct etna_resource *res = etna_resource(surf->base.texture);
   struct blt_clear_op clr = {};
   clr.dest.addr.bo = res->bo;
   clr.dest.addr.offset = surf->surf.offset;
   clr.dest.addr.flags = ETNA_RELOC_WRITE;
   clr.dest.bpp = util_format_get_blocksize(surf->base.format);
   clr.dest.stride = surf->surf.stride;
   /* TODO: color compression
   clr.dest.compressed = 1;
   clr.dest.compress_fmt = 3;
   */
   clr.dest.tiling = res->layout;
   clr.dest.cache_mode = TS_CACHE_MODE_128; /* TODO: cache modes */

   if (surf->surf.ts_size) {
      clr.dest.use_ts = 1;
      clr.dest.ts_addr.bo = res->ts_bo;
      clr.dest.ts_addr.offset = 0;
      clr.dest.ts_addr.flags = ETNA_RELOC_WRITE;
      clr.dest.ts_clear_value[0] = new_clear_value;
      clr.dest.ts_clear_value[1] = new_clear_value;
   }

   clr.clear_value[0] = new_clear_value;
   clr.clear_value[1] = new_clear_value;
   clr.clear_bits[0] = 0xffffffff; /* TODO: Might want to clear only specific channels? */
   clr.clear_bits[1] = 0xffffffff;
   clr.rect_x = 0; /* What about scissors? */
   clr.rect_y = 0;
   clr.rect_w = surf->surf.width;
   clr.rect_h = surf->surf.height;

   emit_blt_clearimage(ctx->stream, &clr);

   /* This made the TS valid */
   if (surf->surf.ts_size) {
      ctx->framebuffer.TS_COLOR_CLEAR_VALUE = new_clear_value;
      surf->level->ts_valid = true;
   }

   surf->level->clear_value = new_clear_value;
   resource_written(ctx, surf->base.texture);
   etna_resource(surf->base.texture)->seqno++;
}

static void
etna_blit_clear_zs_blt(struct pipe_context *pctx, struct pipe_surface *dst,
                   unsigned buffers, double depth, unsigned stencil)
{
   struct etna_context *ctx = etna_context(pctx);
   struct etna_surface *surf = etna_surface(dst);
   uint32_t new_clear_value = translate_clear_depth_stencil(surf->base.format, depth, stencil);
   uint32_t new_clear_bits = 0, clear_bits_depth, clear_bits_stencil;

   /* Get the channels to clear */
   switch (surf->base.format) {
   case PIPE_FORMAT_Z16_UNORM:
      clear_bits_depth = 0xffffffff;
      clear_bits_stencil = 0x00000000;
      break;
   case PIPE_FORMAT_X8Z24_UNORM:
   case PIPE_FORMAT_S8_UINT_Z24_UNORM:
      clear_bits_depth = 0xffffff00;
      clear_bits_stencil = 0x000000ff;
      break;
   default:
      clear_bits_depth = clear_bits_stencil = 0xffffffff;
      break;
   }

   if (buffers & PIPE_CLEAR_DEPTH)
      new_clear_bits |= clear_bits_depth;
   if (buffers & PIPE_CLEAR_STENCIL)
      new_clear_bits |= clear_bits_stencil;

   /* TODO unduplicate this */
   struct etna_resource *res = etna_resource(surf->base.texture);
   struct blt_clear_op clr = {};
   clr.dest.addr.bo = res->bo;
   clr.dest.addr.offset = surf->surf.offset;
   clr.dest.addr.flags = ETNA_RELOC_WRITE;
   clr.dest.bpp = util_format_get_blocksize(surf->base.format);
   clr.dest.stride = surf->surf.stride;
#if 0 /* TODO depth compression */
   clr.dest.compressed = 1;
   clr.dest.compress_fmt = COLOR_COMPRESSION_FORMAT_D24S8;
#endif
   clr.dest.tiling = res->layout;
   clr.dest.cache_mode = TS_CACHE_MODE_128; /* TODO: cache modes */

   if (surf->surf.ts_size) {
      clr.dest.use_ts = 1;
      clr.dest.ts_addr.bo = res->ts_bo;
      clr.dest.ts_addr.offset = 0;
      clr.dest.ts_addr.flags = ETNA_RELOC_WRITE;
      clr.dest.ts_clear_value[0] = new_clear_value;
      clr.dest.ts_clear_value[1] = new_clear_value;
   }

   clr.clear_value[0] = new_clear_value;
   clr.clear_value[1] = new_clear_value;
   clr.clear_bits[0] = new_clear_bits;
   clr.clear_bits[1] = new_clear_bits;
   clr.rect_x = 0; /* What about scissors? */
   clr.rect_y = 0;
   clr.rect_w = surf->surf.width;
   clr.rect_h = surf->surf.height;

   emit_blt_clearimage(ctx->stream, &clr);

   /* This made the TS valid */
   if (surf->surf.ts_size) {
      ctx->framebuffer.TS_DEPTH_CLEAR_VALUE = new_clear_value;
      surf->level->ts_valid = true;
   }

   surf->level->clear_value = new_clear_value;
   resource_written(ctx, surf->base.texture);
   etna_resource(surf->base.texture)->seqno++;
}

static void
etna_clear_blt(struct pipe_context *pctx, unsigned buffers,
           const union pipe_color_union *color, double depth, unsigned stencil)
{
   struct etna_context *ctx = etna_context(pctx);

   etna_set_state(ctx->stream, VIVS_GL_FLUSH_CACHE, 0x00000c23);
   etna_set_state(ctx->stream, VIVS_TS_FLUSH_CACHE, VIVS_TS_FLUSH_CACHE_FLUSH);

   if (buffers & PIPE_CLEAR_COLOR) {
      for (int idx = 0; idx < ctx->framebuffer_s.nr_cbufs; ++idx) {
         etna_blit_clear_color_blt(pctx, ctx->framebuffer_s.cbufs[idx],
                               &color[idx]);
      }
   }

   if ((buffers & PIPE_CLEAR_DEPTHSTENCIL) && ctx->framebuffer_s.zsbuf != NULL)
      etna_blit_clear_zs_blt(pctx, ctx->framebuffer_s.zsbuf, buffers, depth, stencil);

   etna_stall(ctx->stream, SYNC_RECIPIENT_RA, SYNC_RECIPIENT_BLT);

   if ((buffers & PIPE_CLEAR_COLOR) && (buffers & PIPE_CLEAR_DEPTH))
      etna_set_state(ctx->stream, VIVS_GL_FLUSH_CACHE, 0x00000c23);
   else
      etna_set_state(ctx->stream, VIVS_GL_FLUSH_CACHE, 0x00000002);
}


static bool
etna_try_blt_blit(struct pipe_context *pctx,
                 const struct pipe_blit_info *blit_info)
{
   struct etna_context *ctx = etna_context(pctx);
   struct etna_resource *src = etna_resource(blit_info->src.resource);
   struct etna_resource *dst = etna_resource(blit_info->dst.resource);
   int msaa_xscale = 1, msaa_yscale = 1;

   /* Ensure that the level is valid */
   assert(blit_info->src.level <= src->base.last_level);
   assert(blit_info->dst.level <= dst->base.last_level);

   if (!translate_samples_to_xyscale(src->base.nr_samples, &msaa_xscale, &msaa_yscale, NULL))
      return FALSE;

   /* The width/height are in pixels; they do not change as a result of
    * multi-sampling. So, when blitting from a 4x multisampled surface
    * to a non-multisampled surface, the width and height will be
    * identical. As we do not support scaling, reject different sizes.
    * TODO: could handle 2x downsample here with emit_blt_genmipmaps */
   if (blit_info->dst.box.width != blit_info->src.box.width ||
       blit_info->dst.box.height != abs(blit_info->src.box.height)) { /* allow y flip for glTexImage2D */
      DBG("scaling requested: source %dx%d destination %dx%d",
          blit_info->src.box.width, blit_info->src.box.height,
          blit_info->dst.box.width, blit_info->dst.box.height);
      return FALSE;
   }

   /* No masks - not sure if BLT can copy individual channels */
   unsigned mask = util_format_get_mask(blit_info->dst.format);
   if ((blit_info->mask & mask) != mask) {
      DBG("sub-mask requested: 0x%02x vs format mask 0x%02x", blit_info->mask, mask);
      return FALSE;
   }

   /* TODO: 1 byte per pixel formats aren't handled by etna_compatible_rs_format nor
    * translate_rs_format.
    * Also this should be smarter about format conversions; etna_compatible_rs_format
    * assumes all 2-byte pixel format are laid out as 4444, all 4-byte pixel formats
    * are 8888.
    */
   unsigned src_format = etna_compatible_rs_format(blit_info->src.format);
   unsigned dst_format = etna_compatible_rs_format(blit_info->dst.format);
   if (translate_blt_format(src_format) == ETNA_NO_MATCH ||
       translate_blt_format(dst_format) == ETNA_NO_MATCH ||
       blit_info->scissor_enable ||
       blit_info->dst.box.depth != blit_info->src.box.depth ||
       blit_info->dst.box.depth != 1) {
      return FALSE;
   }

   /* Ensure that the Z coordinate is sane */
   assert(dst->base.target == PIPE_TEXTURE_CUBE || blit_info->dst.box.z == 0);
   assert(src->base.target == PIPE_TEXTURE_CUBE || blit_info->src.box.z == 0);
   assert(blit_info->src.box.z < src->base.array_size);
   assert(blit_info->dst.box.z < dst->base.array_size);

   struct etna_resource_level *src_lev = &src->levels[blit_info->src.level];
   struct etna_resource_level *dst_lev = &dst->levels[blit_info->dst.level];

   /* Kick off BLT here */
   if (src == dst) {
      /* Resolve-in-place */
      assert(!memcmp(&blit_info->src, &blit_info->dst, sizeof(blit_info->src)));
      if (!src_lev->ts_size || !src_lev->ts_valid) /* No TS, no worries */
         return TRUE;
      struct blt_inplace_op op = {};

      op.addr.bo = src->bo;
      op.addr.offset = src_lev->offset + blit_info->src.box.z * src_lev->layer_stride;
      op.addr.flags = ETNA_RELOC_READ | ETNA_RELOC_WRITE;
      op.ts_addr.bo = src->ts_bo;
      op.ts_addr.offset = src_lev->ts_offset + blit_info->src.box.z * src_lev->ts_layer_stride;
      op.ts_addr.flags = ETNA_RELOC_READ;
      op.ts_clear_value[0] = src_lev->clear_value;
      op.ts_clear_value[1] = src_lev->clear_value;
      op.cache_mode = TS_CACHE_MODE_128; /* TODO: cache modes */
      op.num_tiles = src_lev->size / 128; /* TODO: cache modes */
      op.bpp = util_format_get_blocksize(src->base.format);

      etna_set_state(ctx->stream, VIVS_GL_FLUSH_CACHE, 0x00000c23);
      etna_set_state(ctx->stream, VIVS_TS_FLUSH_CACHE, 0x00000001);
      emit_blt_inplace(ctx->stream, &op);
   } else {
      /* Copy op */
      struct blt_imgcopy_op op = {};

      op.src.addr.bo = src->bo;
      op.src.addr.offset = src_lev->offset + blit_info->src.box.z * src_lev->layer_stride;
      op.src.addr.flags = ETNA_RELOC_READ;
      op.src.format = translate_blt_format(src_format);
      op.src.stride = src_lev->stride;
      op.src.tiling = src->layout;
      op.src.cache_mode = TS_CACHE_MODE_128; /* TODO: cache modes */
      const struct util_format_description *src_format_desc =
         util_format_description(blit_info->src.format);
      for (unsigned x=0; x<4; ++x)
         op.src.swizzle[x] = src_format_desc->swizzle[x];

      if (src_lev->ts_size && src_lev->ts_valid) {
         op.src.use_ts = 1;
         op.src.ts_addr.bo = src->ts_bo;
         op.src.ts_addr.offset = src_lev->ts_offset + blit_info->src.box.z * src_lev->ts_layer_stride;
         op.src.ts_addr.flags = ETNA_RELOC_READ;
         op.src.ts_clear_value[0] = src_lev->clear_value;
         op.src.ts_clear_value[1] = src_lev->clear_value;
      }

      op.dest.addr.bo = dst->bo;
      op.dest.addr.offset = dst_lev->offset + blit_info->dst.box.z * dst_lev->layer_stride;
      op.dest.addr.flags = ETNA_RELOC_WRITE;
      op.dest.format = translate_blt_format(dst_format);
      op.dest.stride = dst_lev->stride;
      /* TODO color compression
      op.dest.compressed = 1;
      op.dest.compress_fmt = 3;
      */
      op.dest.tiling = dst->layout;
      op.dest.cache_mode = TS_CACHE_MODE_128; /* TODO cache modes */
      const struct util_format_description *dst_format_desc =
         util_format_description(blit_info->dst.format);
      for (unsigned x=0; x<4; ++x)
         op.dest.swizzle[x] = dst_format_desc->swizzle[x];

      op.dest_x = blit_info->dst.box.x;
      op.dest_y = blit_info->dst.box.y;
      op.src_x = blit_info->src.box.x;
      op.src_y = blit_info->src.box.y;
      op.rect_w = blit_info->dst.box.width;
      op.rect_h = blit_info->dst.box.height;

      if (blit_info->src.box.height < 0) { /* flipped? fix up base y */
         op.flip_y = 1;
         op.src_y += blit_info->src.box.height;
      }

      assert(op.src_x < src_lev->padded_width);
      assert(op.src_y < src_lev->padded_height);
      assert((op.src_x + op.rect_w) <= src_lev->padded_width);
      assert((op.src_y + op.rect_h) <= src_lev->padded_height);
      assert(op.dest_x < dst_lev->padded_width);
      assert(op.dest_y < dst_lev->padded_height);
      assert((op.dest_x + op.rect_w) <= dst_lev->padded_width);
      assert((op.dest_y + op.rect_h) <= dst_lev->padded_height);

      etna_set_state(ctx->stream, VIVS_GL_FLUSH_CACHE, 0x00000c23);
      etna_set_state(ctx->stream, VIVS_TS_FLUSH_CACHE, 0x00000001);
      emit_blt_copyimage(ctx->stream, &op);
   }

   /* Make FE wait for BLT, in case we want to do something with the image next.
    * This probably shouldn't be here, and depend on what is done with the resource.
    */
   etna_stall(ctx->stream, SYNC_RECIPIENT_FE, SYNC_RECIPIENT_BLT);
   etna_set_state(ctx->stream, VIVS_GL_FLUSH_CACHE, 0x00000c23);

   resource_written(ctx, &dst->base);
   dst->seqno++;
   dst_lev->ts_valid = false;

   return TRUE;
}

static void
etna_blit_blt(struct pipe_context *pctx, const struct pipe_blit_info *blit_info)
{
   struct etna_context *ctx = etna_context(pctx);
   struct pipe_blit_info info = *blit_info;

   if (info.src.resource->nr_samples > 1 &&
       info.dst.resource->nr_samples <= 1 &&
       !util_format_is_depth_or_stencil(info.src.resource->format) &&
       !util_format_is_pure_integer(info.src.resource->format)) {
      DBG("color resolve unimplemented");
      return;
   }

   if (etna_try_blt_blit(pctx, blit_info))
      return;

   if (util_try_blit_via_copy_region(pctx, blit_info))
      return;

   if (info.mask & PIPE_MASK_S) {
      DBG("cannot blit stencil, skipping");
      info.mask &= ~PIPE_MASK_S;
   }

   if (!util_blitter_is_blit_supported(ctx->blitter, &info)) {
      DBG("blit unsupported %s -> %s",
          util_format_short_name(info.src.resource->format),
          util_format_short_name(info.dst.resource->format));
      return;
   }

   etna_blit_save_state(ctx);
   util_blitter_blit(ctx->blitter, &info);
}

void
etna_clear_blit_blt_init(struct pipe_context *pctx)
{
   pctx->clear = etna_clear_blt;
   pctx->blit = etna_blit_blt;
}
