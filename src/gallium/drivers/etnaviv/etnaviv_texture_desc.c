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

#include "etnaviv_texture_desc.h"

#include "hw/common.xml.h"
#include "hw/texdesc_3d.xml.h"

#include "etnaviv_clear_blit.h"
#include "etnaviv_context.h"
#include "etnaviv_emit.h"
#include "etnaviv_format.h"
#include "etnaviv_translate.h"
#include "etnaviv_texture.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include <drm_fourcc.h>

static void *
etna_create_sampler_state_desc(struct pipe_context *pipe,
                          const struct pipe_sampler_state *ss)
{
   struct etna_sampler_state_desc *cs = CALLOC_STRUCT(etna_sampler_state_desc);

   if (!cs)
      return NULL;

   cs->SAMP_CTRL0 =
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_UWRAP(translate_texture_wrapmode(ss->wrap_s)) |
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_VWRAP(translate_texture_wrapmode(ss->wrap_t)) |
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_WWRAP(translate_texture_wrapmode(ss->wrap_r)) |
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_MIN(translate_texture_filter(ss->min_img_filter)) |
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_MIP(translate_texture_mipfilter(ss->min_mip_filter)) |
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_MAG(translate_texture_filter(ss->mag_img_filter)) |
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_UNK21;
      /* no ROUND_UV bit? */
   cs->SAMP_CTRL1 = VIVS_NTE_DESCRIPTOR_SAMP_CTRL1_UNK1;
   uint32_t min_lod_fp8 = MIN2(etna_float_to_fixp88(ss->min_lod), 0xfff);
   uint32_t max_lod_fp8 = MIN2(etna_float_to_fixp88(ss->max_lod), 0xfff);
   if (ss->min_mip_filter != PIPE_TEX_MIPFILTER_NONE) {
      cs->SAMP_LOD_MINMAX =
         VIVS_NTE_DESCRIPTOR_SAMP_LOD_MINMAX_MAX(max_lod_fp8) |
         VIVS_NTE_DESCRIPTOR_SAMP_LOD_MINMAX_MIN(min_lod_fp8);
   } else {
      cs->SAMP_LOD_MINMAX =
         VIVS_NTE_DESCRIPTOR_SAMP_LOD_MINMAX_MAX(min_lod_fp8) |
         VIVS_NTE_DESCRIPTOR_SAMP_LOD_MINMAX_MIN(min_lod_fp8);
   }
   cs->SAMP_LOD_BIAS =
      VIVS_NTE_DESCRIPTOR_SAMP_LOD_BIAS_BIAS(etna_float_to_fixp88(ss->lod_bias)) |
      COND(ss->lod_bias != 0.0, VIVS_NTE_DESCRIPTOR_SAMP_LOD_BIAS_ENABLE);
   cs->TX_CTRL = 0; /* TODO: texture TS */

   return cs;
}

static void
etna_delete_sampler_state_desc(struct pipe_context *pctx, void *ss)
{
   FREE(ss);
}

static struct pipe_sampler_view *
etna_create_sampler_view_desc(struct pipe_context *pctx, struct pipe_resource *prsc,
                         const struct pipe_sampler_view *so)
{
   struct etna_sampler_view_desc *sv = CALLOC_STRUCT(etna_sampler_view_desc);
   struct etna_context *ctx = etna_context(pctx);
   const uint32_t format = translate_texture_format(so->format);
   const bool ext = !!(format & EXT_FORMAT);
   const uint32_t swiz = get_texture_swiz(so->format, so->swizzle_r,
                                          so->swizzle_g, so->swizzle_b,
                                          so->swizzle_a);

   if (!sv)
      return NULL;

   struct etna_resource *res = etna_texture_handle_incompatible(pctx, prsc);
   if (!res) {
      free(sv);
      return NULL;
   }

   sv->base = *so;
   pipe_reference_init(&sv->base.reference, 1);
   sv->base.texture = NULL;
   pipe_resource_reference(&sv->base.texture, prsc);
   sv->base.context = pctx;

   /* Determine whether target supported */
   uint32_t target_hw = translate_texture_target(sv->base.target);
   if (target_hw == ETNA_NO_MATCH) {
      BUG("Unhandled texture target");
      free(sv);
      return NULL;
   }

   /* Texture descriptor sampler bits */
   if (util_format_is_srgb(so->format)) {
      sv->SAMP_CTRL1 |= VIVS_NTE_DESCRIPTOR_SAMP_CTRL1_SRGB;
   } else {
      sv->SAMP_CTRL0 |= VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_RGB;
   }

   /* Create texture descriptor */
   sv->bo = etna_bo_new(ctx->screen->dev, 0x100, DRM_ETNA_GEM_CACHE_UNCACHED);
   if (!sv->bo)
      goto error;

   uint32_t *buf = etna_bo_map(sv->bo);
   etna_bo_cpu_prep(sv->bo, DRM_ETNA_PREP_WRITE);
   memset(buf, 0, 0x100);

   /** GC7000 needs the size of the BASELOD level */
   uint32_t base_width = u_minify(res->base.width0, sv->base.u.tex.first_level);
   uint32_t base_height = u_minify(res->base.height0, sv->base.u.tex.first_level);

#define DESC_SET(x, y) buf[(TEXDESC_##x)>>2] = (y)
   DESC_SET(CONFIG0, COND(!ext, VIVS_TE_SAMPLER_CONFIG0_FORMAT(format))
                   | VIVS_TE_SAMPLER_CONFIG0_TYPE(target_hw));
   DESC_SET(CONFIG1, COND(ext, VIVS_TE_SAMPLER_CONFIG1_FORMAT_EXT(format)) |
                            VIVS_TE_SAMPLER_CONFIG1_HALIGN(res->halign) | swiz |
                            VIVS_TE_SAMPLER_CONFIG1_UNK25);
   DESC_SET(CONFIG2, 0x00030000);
   DESC_SET(LINEAR_STRIDE, res->levels[0].stride);
   DESC_SET(SLICE, res->levels[0].layer_stride);
   DESC_SET(3D_CONFIG, 0x00000001);
   DESC_SET(BASELOD, TEXDESC_BASELOD_BASELOD(sv->base.u.tex.first_level) |
                     TEXDESC_BASELOD_MAXLOD(MIN2(sv->base.u.tex.last_level, res->base.last_level)));
   DESC_SET(LOG_SIZE_EXT, TEXDESC_LOG_SIZE_EXT_WIDTH(etna_log2_fixp88(base_width)) |
                          TEXDESC_LOG_SIZE_EXT_HEIGHT(etna_log2_fixp88(base_height)));
   DESC_SET(SIZE, VIVS_TE_SAMPLER_SIZE_WIDTH(base_width) |
                  VIVS_TE_SAMPLER_SIZE_HEIGHT(base_height));
   for (int lod = 0; lod <= res->base.last_level; ++lod)
      DESC_SET(LOD_ADDR(lod), etna_bo_gpu_address(res->bo) + res->levels[lod].offset);
#undef SET_DESC

   etna_bo_cpu_fini(sv->bo);

   sv->DESC_ADDR.bo = sv->bo;
   sv->DESC_ADDR.offset = 0;
   sv->DESC_ADDR.flags = ETNA_RELOC_READ;

   return &sv->base;
error:
   free(sv);
   return NULL;
}

void
etna_sampler_view_update_descriptor(struct etna_context *ctx,
                                    struct etna_cmd_stream *stream,
                                    struct etna_sampler_view_desc *sv)
{
   /* TODO: this should instruct the kernel to update the descriptor when the
    * bo is submitted. For now, just prevent the bo from being freed
    * while it is in use indirectly.
    */
   struct etna_resource *res = etna_resource(sv->base.texture);
   if (res->texture) {
      res = etna_resource(res->texture);
   }
   /* No need to ref LOD levels individually as they'll always come from the same bo */
   etna_cmd_stream_ref(stream, res->bo);
}

static void
etna_sampler_view_desc_destroy(struct pipe_context *pctx,
                          struct pipe_sampler_view *view_)
{
   struct etna_sampler_view_desc *view = etna_sampler_view_desc(view_);
   pipe_resource_reference(&view->base.texture, NULL);
   etna_bo_del(view->bo);
   FREE(view);
}

void
etna_texture_desc_init(struct pipe_context *pctx)
{
   DBG("etnaviv: Using descriptor-based texturing\n");
   pctx->create_sampler_state = etna_create_sampler_state_desc;
   pctx->delete_sampler_state = etna_delete_sampler_state_desc;
   pctx->create_sampler_view = etna_create_sampler_view_desc;
   pctx->sampler_view_destroy = etna_sampler_view_desc_destroy;
}

