/*
 * Copyright (c) 2012-2015 Etnaviv Project
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
 */

#include "etnaviv_clear_blit.h"

#include "hw/common.xml.h"

#include "etnaviv_emit.h"
#include "etnaviv_context.h"
#include "etnaviv_emit.h"
#include "etnaviv_resource.h"
#include "etnaviv_surface.h"
#include "etnaviv_translate.h"

#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_blitter.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_surface.h"

/* Save current state for blitter operation */
static void etna_blit_save_state(struct etna_context *ctx)
{
    util_blitter_save_vertex_buffer_slot(ctx->blitter, &ctx->vertex_buffer_s[0]);
    util_blitter_save_vertex_elements(ctx->blitter, ctx->vertex_elements);
    util_blitter_save_vertex_shader(ctx->blitter, ctx->vs);
    util_blitter_save_rasterizer(ctx->blitter, ctx->rasterizer);
    util_blitter_save_viewport(ctx->blitter, &ctx->viewport_s);
    util_blitter_save_scissor(ctx->blitter, &ctx->scissor_s);
    util_blitter_save_fragment_shader(ctx->blitter, ctx->fs);
    util_blitter_save_blend(ctx->blitter, ctx->blend);
    util_blitter_save_depth_stencil_alpha(ctx->blitter, ctx->zsa);
    util_blitter_save_stencil_ref(ctx->blitter, &ctx->stencil_ref_s);
    util_blitter_save_sample_mask(ctx->blitter, ctx->sample_mask);
    util_blitter_save_framebuffer(ctx->blitter, &ctx->framebuffer_s);
    util_blitter_save_fragment_sampler_states(ctx->blitter,
                    ctx->num_fragment_samplers,
                    (void **)ctx->sampler);
    util_blitter_save_fragment_sampler_views(ctx->blitter,
                    ctx->num_fragment_sampler_views, ctx->sampler_view);
}

/* Generate clear command for a surface (non-fast clear case) */
void etna_rs_gen_clear_surface(struct etna_context *ctx, struct compiled_rs_state *rs_state, struct etna_surface *surf, uint32_t clear_value)
{
    uint bs = util_format_get_blocksize(surf->base.format);
    uint format = 0;
    switch(bs)
    {
    case 2: format = RS_FORMAT_A1R5G5B5; break;
    case 4: format = RS_FORMAT_A8R8G8B8; break;
    default: BUG("etna_rs_gen_clear_surface: Unhandled clear blocksize: %i (fmt %i)", bs, surf->base.format);
             format = RS_FORMAT_A8R8G8B8;
             assert(0);
    }
    /* use tiled clear if width is multiple of 16 */
    bool tiled_clear = (surf->surf.padded_width & ETNA_RS_WIDTH_MASK) == 0 &&
                       (surf->surf.padded_height & ETNA_RS_HEIGHT_MASK) == 0;
    struct etna_bo *dest_bo = etna_resource(surf->base.texture)->bo;
    etna_compile_rs_state(ctx, rs_state, &(struct rs_state){
            .source_format = format,
            .dest_format = format,
            .dest[0] = dest_bo,
            .dest_offset[0] = surf->surf.offset,
            .dest_stride = surf->surf.stride,
            .dest_tiling = tiled_clear ? surf->layout : ETNA_LAYOUT_LINEAR,
            .dither = {0xffffffff, 0xffffffff},
            .width = surf->surf.padded_width, /* These must be padded to 16x4 if !LINEAR, otherwise RS will hang */
            .height = surf->surf.padded_height,
            .clear_value = {clear_value},
            .clear_mode = VIVS_RS_CLEAR_CONTROL_MODE_ENABLED1,
            .clear_bits = 0xffff
        });
}

static void etna_clear(struct pipe_context *pctx,
             unsigned buffers,
             const union pipe_color_union *color,
             double depth,
             unsigned stencil)
{
    struct etna_context *ctx = etna_context(pctx);
    /* Flush color and depth cache before clearing anything.
     * This is especially important when coming from another surface, as otherwise it may clear
     * part of the old surface instead.
     */
    etna_set_state(ctx->stream, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_COLOR | VIVS_GL_FLUSH_CACHE_DEPTH);
    etna_stall(ctx->stream, SYNC_RECIPIENT_RA, SYNC_RECIPIENT_PE);
    /* Preparation: Flush the TS if needed. This must be done after flushing
     * color and depth, otherwise it can result in crashes */
    bool need_ts_flush = false;
    if ((buffers & PIPE_CLEAR_COLOR) && ctx->framebuffer_s.nr_cbufs)
    {
        struct etna_surface *surf = etna_surface(ctx->framebuffer_s.cbufs[0]);
        if (surf->surf.ts_size)
            need_ts_flush = true;
    }
    if ((buffers & PIPE_CLEAR_DEPTHSTENCIL) && ctx->framebuffer_s.zsbuf != NULL)
    {
        struct etna_surface *surf = etna_surface(ctx->framebuffer_s.zsbuf);
        if (surf->surf.ts_size)
            need_ts_flush = true;
    }
    if (need_ts_flush)
    {
        etna_set_state(ctx->stream, VIVS_TS_FLUSH_CACHE, VIVS_TS_FLUSH_CACHE_FLUSH);
    }
    /* No need to set up the TS here as RS clear operations (in contrast to
     * resolve and copy) do not require the TS state.
     */
    if (buffers & PIPE_CLEAR_COLOR)
    {
        for (int idx = 0; idx < ctx->framebuffer_s.nr_cbufs; ++idx)
        {
            struct etna_surface *surf = etna_surface(ctx->framebuffer_s.cbufs[idx]);
            uint32_t new_clear_value = translate_clear_color(surf->base.format, &color[idx]);
            if (surf->surf.ts_size) /* TS: use precompiled clear command */
            {
                /* Set new clear color */
                ctx->framebuffer.TS_COLOR_CLEAR_VALUE = new_clear_value;
                if (!DBG_ENABLED(ETNA_DBG_NO_AUTODISABLE))
                {
                    /* Set number of color tiles to be filled */
                    etna_set_state(ctx->stream, VIVS_TS_COLOR_AUTO_DISABLE_COUNT, surf->surf.padded_width*surf->surf.padded_height/16);
                    ctx->framebuffer.TS_MEM_CONFIG |= VIVS_TS_MEM_CONFIG_COLOR_AUTO_DISABLE;
                }
                ctx->dirty |= ETNA_DIRTY_TS;
            }
            else if (unlikely(new_clear_value != surf->level->clear_value)) /* Queue normal RS clear for non-TS surfaces */
            {
                /* If clear color changed, re-generate stored command */
                etna_rs_gen_clear_surface(ctx, &surf->clear_command, surf, new_clear_value);
            }
            etna_submit_rs_state(ctx, &surf->clear_command);
            surf->level->clear_value = new_clear_value;
        }
    }
    if ((buffers & PIPE_CLEAR_DEPTHSTENCIL) && ctx->framebuffer_s.zsbuf != NULL)
    {
        struct etna_surface *surf = etna_surface(ctx->framebuffer_s.zsbuf);
        uint32_t new_clear_value = translate_clear_depth_stencil(surf->base.format, depth, stencil);
        if (surf->surf.ts_size) /* TS: use precompiled clear command */
        {
            /* Set new clear depth value */
            ctx->framebuffer.TS_DEPTH_CLEAR_VALUE = new_clear_value;
            if (!DBG_ENABLED(ETNA_DBG_NO_AUTODISABLE))
            {
                /* Set number of depth tiles to be filled */
                etna_set_state(ctx->stream, VIVS_TS_DEPTH_AUTO_DISABLE_COUNT, surf->surf.padded_width*surf->surf.padded_height/16);
                ctx->framebuffer.TS_MEM_CONFIG |= VIVS_TS_MEM_CONFIG_DEPTH_AUTO_DISABLE;
            }
            ctx->dirty |= ETNA_DIRTY_TS;
        } else if (unlikely(new_clear_value != surf->level->clear_value)) /* Queue normal RS clear for non-TS surfaces */
        {
            /* If clear depth value changed, re-generate stored command */
            etna_rs_gen_clear_surface(ctx, &surf->clear_command, surf, new_clear_value);
        }
        etna_submit_rs_state(ctx, &surf->clear_command);
        surf->level->clear_value = new_clear_value;
    }
    etna_stall(ctx->stream, SYNC_RECIPIENT_RA, SYNC_RECIPIENT_PE);
}

static void etna_clear_render_target(struct pipe_context *pctx,
                           struct pipe_surface *dst,
                           const union pipe_color_union *color,
                           unsigned dstx, unsigned dsty,
                           unsigned width, unsigned height)
{
    struct etna_context *ctx = etna_context(pctx);
    /* XXX could fall back to RS when target area is full screen / resolveable and no TS. */
    etna_blit_save_state(ctx);
    util_blitter_clear_render_target(ctx->blitter, dst, color, dstx, dsty, width, height);
}

static void etna_clear_depth_stencil(struct pipe_context *pctx,
                           struct pipe_surface *dst,
                           unsigned clear_flags,
                           double depth,
                           unsigned stencil,
                           unsigned dstx, unsigned dsty,
                           unsigned width, unsigned height)
{
    struct etna_context *ctx = etna_context(pctx);
    /* XXX could fall back to RS when target area is full screen / resolveable and no TS. */
    etna_blit_save_state(ctx);
    util_blitter_clear_depth_stencil(ctx->blitter, dst, clear_flags, depth, stencil, dstx, dsty, width, height);
}

static void etna_resource_copy_region(struct pipe_context *pctx,
                            struct pipe_resource *dst,
                            unsigned dst_level,
                            unsigned dstx, unsigned dsty, unsigned dstz,
                            struct pipe_resource *src,
                            unsigned src_level,
                            const struct pipe_box *src_box)
{
    struct etna_context *ctx = etna_context(pctx);
    /* The resource must be of the same format. */
    assert(src->format == dst->format);
    /* Resources with nr_samples > 1 are not allowed. */
    assert(src->nr_samples == 1 && dst->nr_samples == 1);
    /* XXX we can use the RS as a literal copy engine here
     * the only complexity is tiling; the size of the boxes needs to be aligned to the tile size
     * how to handle the case where a resource is copied from/to a non-aligned position?
     * from non-aligned: can fall back to rendering-based copy?
     * to non-aligned: can fall back to rendering-based copy?
     * XXX this goes wrong when source surface is supertiled.
     */
    etna_blit_save_state(ctx);
    util_blitter_copy_texture(ctx->blitter, dst, dst_level, dstx, dsty, dstz, src, src_level, src_box);
}

static bool etna_try_rs_blit(struct pipe_context *pctx,
                             const struct pipe_blit_info *blit_info)
{
   struct etna_context *ctx = etna_context(pctx);
   struct etna_resource *src = etna_resource(blit_info->src.resource);
   struct etna_resource *dst = etna_resource(blit_info->dst.resource);
   struct compiled_rs_state copy_to_screen;
   uint32_t ts_mem_config = 0;
   int msaa_xscale = 1, msaa_yscale = 1;

   if (!translate_samples_to_xyscale(src->base.nr_samples, &msaa_xscale,
                                    &msaa_yscale, NULL))
      return FALSE;

   if (translate_rt_format(blit_info->src.format, true) == ETNA_NO_MATCH ||
       translate_rt_format(blit_info->dst.format, true) == ETNA_NO_MATCH ||
       blit_info->mask != PIPE_MASK_RGBA || blit_info->scissor_enable ||
       blit_info->src.box.x != 0 || blit_info->src.box.y != 0 ||
       blit_info->src.box.z != 0 || blit_info->dst.box.x != 0 ||
       blit_info->dst.box.y != 0 || blit_info->dst.box.z != 0 ||
       blit_info->dst.box.width != (blit_info->src.box.width / msaa_xscale) ||
       blit_info->dst.box.height != (blit_info->src.box.height / msaa_yscale))
   {
      printf("rs blit bail out\n");
      return FALSE;
   }

   etna_set_state(ctx->stream, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_COLOR);
   etna_stall(ctx->stream, SYNC_RECIPIENT_RA, SYNC_RECIPIENT_PE);

   /* Set up color TS to source surface before blit, if needed */
   if (src->base.nr_samples > 1)
      ts_mem_config |= VIVS_TS_MEM_CONFIG_MSAA |
                       translate_msaa_format(src->base.format, false);
   if (src->levels[blit_info->src.level].ts_size) {
      struct etna_reloc reloc;

      ctx->gpu3d.TS_MEM_CONFIG = VIVS_TS_MEM_CONFIG_COLOR_FAST_CLEAR | ts_mem_config;
      etna_set_state(ctx->stream, VIVS_TS_MEM_CONFIG, ctx->gpu3d.TS_MEM_CONFIG);

      memset(&reloc, 0, sizeof(struct etna_reloc));
      reloc.bo = src->ts_bo;
      reloc.offset = src->levels[blit_info->src.level].ts_offset;
      reloc.flags = ETNA_RELOC_READ;
      etna_set_state_reloc(ctx->stream, VIVS_TS_COLOR_STATUS_BASE, &reloc);

      memset(&reloc, 0, sizeof(struct etna_reloc));
      reloc.bo = src->bo;
      reloc.offset = src->levels[blit_info->src.level].offset;
      reloc.flags = ETNA_RELOC_READ;
      etna_set_state_reloc(ctx->stream, VIVS_TS_COLOR_SURFACE_BASE, &reloc);

      ctx->gpu3d.TS_COLOR_CLEAR_VALUE = src->levels[blit_info->src.level].clear_value;
      etna_set_state(ctx->stream, VIVS_TS_COLOR_CLEAR_VALUE, ctx->gpu3d.TS_COLOR_CLEAR_VALUE);
   } else {
      ctx->gpu3d.TS_MEM_CONFIG = ts_mem_config;
      etna_set_state(ctx->stream, VIVS_TS_MEM_CONFIG, ctx->gpu3d.TS_MEM_CONFIG);
   }
   ctx->dirty |= ETNA_DIRTY_TS;

   /* Kick off RS here */
   etna_compile_rs_state(ctx, &copy_to_screen, &(struct rs_state){
      .source_format = translate_rt_format(blit_info->src.format, false),
      .source_tiling = src->layout,
      .source[0] = src->bo,
      .source_offset[0] = src->levels[blit_info->src.level].offset,
      .source[1] = src->bo,
      .source_offset[1] = src->levels[blit_info->src.level].offset + src->levels[blit_info->src.level].size / 2,
      .source_stride = src->levels[blit_info->src.level].stride,
      .dest_format = translate_rt_format(blit_info->dst.format, false),
      .dest_tiling = dst->layout,
      .dest[0] = dst->bo,
      .dest_offset[0] = 0,
      .dest_stride = dst->levels[blit_info->dst.level].stride,
      .downsample_x = msaa_xscale > 1,
      .downsample_y = msaa_yscale > 1,
      .swap_rb = translate_rb_src_dst_swap(src->base.format, dst->base.format),
      .dither = {0xffffffff, 0xffffffff}, // XXX dither when going from 24 to 16 bit?
      .clear_mode = VIVS_RS_CLEAR_CONTROL_MODE_DISABLED,
      .width = dst->levels[blit_info->dst.level].width * msaa_xscale,
      .height = dst->levels[blit_info->dst.level].height * msaa_yscale
   });

   etna_submit_rs_state(ctx, &copy_to_screen);

   return TRUE;
}

static void etna_blit(struct pipe_context *pctx, const struct pipe_blit_info *blit_info)
{
    /* This is a more extended version of resource_copy_region */
    /* TODO Some cases can be handled by RS; if not, fall back to rendering or even CPU */
    /* copy block of pixels from info->src to info->dst (resource, level, box, format);
     * function is used for scaling, flipping in x and y direction (negative width/height), format conversion, mask and filter
     * and even a scissor rectangle
     *
     * What can the RS do for us:
     *   convert between tiling formats (layouts)
     *   downsample 2x in x and y
     *   convert between a limited number of pixel formats
     *
     * For the rest, fall back to util_blitter
     * XXX this goes wrong when source surface is supertiled.
     */
    struct etna_context *ctx = etna_context(pctx);
    struct pipe_blit_info info = *blit_info;

    if (info.src.resource->nr_samples > 1 &&
                info.dst.resource->nr_samples <= 1 &&
                !util_format_is_depth_or_stencil(info.src.resource->format) &&
                !util_format_is_pure_integer(info.src.resource->format)) {
        DBG("color resolve unimplemented");
        return;
    }

    if (etna_try_rs_blit(pctx, blit_info))
       return;

    if (util_try_blit_via_copy_region(pctx, blit_info)) {
        return; /* done */
    }

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

static void etna_flush_resource(struct pipe_context *pctx, struct pipe_resource *prsc)
{
    /* TODO */
}

void etna_clear_blit_init(struct pipe_context *pctx)
{
    pctx->clear = etna_clear;
    pctx->clear_render_target = etna_clear_render_target;
    pctx->clear_depth_stencil = etna_clear_depth_stencil;
    pctx->resource_copy_region = etna_resource_copy_region;
    pctx->blit = etna_blit;
    pctx->flush_resource = etna_flush_resource;
}
