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
 *
 * Authors:
 *    Wladimir J. van der Laan <laanwj@gmail.com>
 */

#include "etnaviv_texture.h"

#include "hw/common.xml.h"
#include "hw/texdesc_3d.xml.h"

#include "etnaviv_clear_blit.h"
#include "etnaviv_context.h"
#include "etnaviv_emit.h"
#include "etnaviv_format.h"
#include "etnaviv_translate.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include <drm_fourcc.h>

static void *
etna_create_sampler_state(struct pipe_context *pipe,
                          const struct pipe_sampler_state *ss)
{
   struct etna_sampler_state *cs = CALLOC_STRUCT(etna_sampler_state);

   if (!cs)
      return NULL;
#if 0
   cs->TE_SAMPLER_CONFIG0 =
      VIVS_TE_SAMPLER_CONFIG0_UWRAP(translate_texture_wrapmode(ss->wrap_s)) |
      VIVS_TE_SAMPLER_CONFIG0_VWRAP(translate_texture_wrapmode(ss->wrap_t)) |
      VIVS_TE_SAMPLER_CONFIG0_MIN(translate_texture_filter(ss->min_img_filter)) |
      VIVS_TE_SAMPLER_CONFIG0_MIP(translate_texture_mipfilter(ss->min_mip_filter)) |
      VIVS_TE_SAMPLER_CONFIG0_MAG(translate_texture_filter(ss->mag_img_filter)) |
      COND(ss->normalized_coords, VIVS_TE_SAMPLER_CONFIG0_ROUND_UV);
   cs->TE_SAMPLER_CONFIG1 = 0; /* VIVS_TE_SAMPLER_CONFIG1 (swizzle, extended
                                  format) fully determined by sampler view */
   cs->TE_SAMPLER_LOD_CONFIG =
      COND(ss->lod_bias != 0.0, VIVS_TE_SAMPLER_LOD_CONFIG_BIAS_ENABLE) |
      VIVS_TE_SAMPLER_LOD_CONFIG_BIAS(etna_float_to_fixp55(ss->lod_bias));

   if (ss->min_mip_filter != PIPE_TEX_MIPFILTER_NONE) {
      cs->min_lod = etna_float_to_fixp55(ss->min_lod);
      cs->max_lod = etna_float_to_fixp55(ss->max_lod);
   } else {
      /* when not mipmapping, we need to set max/min lod so that always
       * lowest LOD is selected */
      cs->min_lod = cs->max_lod = etna_float_to_fixp55(ss->min_lod);
   }
#endif
   cs->SAMP_CTRL0 =
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_UWRAP(translate_texture_wrapmode(ss->wrap_s)) |
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_VWRAP(translate_texture_wrapmode(ss->wrap_t)) |
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_WWRAP(translate_texture_wrapmode(ss->wrap_r)) |
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_MIN(translate_texture_filter(ss->min_img_filter)) |
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_MIP(translate_texture_mipfilter(ss->min_mip_filter)) |
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_MAG(translate_texture_filter(ss->mag_img_filter)) |
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_UNK21 |
      VIVS_NTE_DESCRIPTOR_SAMP_CTRL0_UNK23;
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
etna_bind_sampler_states(struct pipe_context *pctx, enum pipe_shader_type shader,
                         unsigned start_slot, unsigned num_samplers,
                         void **samplers)
{
   /* bind fragment sampler */
   struct etna_context *ctx = etna_context(pctx);
   int offset;

   switch (shader) {
   case PIPE_SHADER_FRAGMENT:
      offset = 0;
      ctx->num_fragment_samplers = num_samplers;
      break;
   case PIPE_SHADER_VERTEX:
      offset = ctx->specs.vertex_sampler_offset;
      break;
   default:
      assert(!"Invalid shader");
      return;
   }

   uint32_t mask = 1 << offset;
   for (int idx = 0; idx < num_samplers; ++idx, mask <<= 1) {
      ctx->sampler[offset + idx] = samplers[idx];
      if (samplers[idx])
         ctx->active_samplers |= mask;
      else
         ctx->active_samplers &= ~mask;
   }

   ctx->dirty |= ETNA_DIRTY_SAMPLERS;
}

static void
etna_delete_sampler_state(struct pipe_context *pctx, void *ss)
{
   FREE(ss);
}

static void
etna_update_sampler_source(struct pipe_sampler_view *view)
{
   struct etna_resource *base = etna_resource(view->texture);
   struct etna_resource *to = base, *from = base;

   if (base->external && etna_resource_newer(etna_resource(base->external), base))
      from = etna_resource(base->external);

   if (base->texture)
      to = etna_resource(base->texture);

   if ((to != from) && etna_resource_older(to, from)) {
      etna_copy_resource(view->context, &to->base, &from->base, 0,
                         view->texture->last_level);
      to->seqno = from->seqno;
   } else if ((to == from) && etna_resource_needs_flush(to)) {
      /* Resolve TS if needed, remove when adding sampler TS */
      etna_copy_resource(view->context, &to->base, &from->base, 0,
                         view->texture->last_level);
      to->flush_seqno = from->seqno;
   }
}

static bool
etna_resource_sampler_compatible(struct etna_resource *res)
{
   if (util_format_is_compressed(res->base.format))
      return true;

   struct etna_screen *screen = etna_screen(res->base.screen);
   /* This GPU supports texturing from supertiled textures? */
   if (res->layout == ETNA_LAYOUT_SUPER_TILED && VIV_FEATURE(screen, chipMinorFeatures2, SUPERTILED_TEXTURE))
      return true;

   /* TODO: LINEAR_TEXTURE_SUPPORT */

   /* Otherwise, only support tiled layouts */
   if (res->layout != ETNA_LAYOUT_TILED)
      return false;

   /* If we have HALIGN support, we can allow for the RS padding */
   if (VIV_FEATURE(screen, chipMinorFeatures1, TEXTURE_HALIGN))
      return true;

   /* Non-HALIGN GPUs only accept 4x4 tile-aligned textures */
   if (res->halign != TEXTURE_HALIGN_FOUR)
      return false;

   return true;
}

static struct pipe_sampler_view *
etna_create_sampler_view(struct pipe_context *pctx, struct pipe_resource *prsc,
                         const struct pipe_sampler_view *so)
{
   struct etna_sampler_view *sv = CALLOC_STRUCT(etna_sampler_view);
   struct etna_resource *res = etna_resource(prsc);
   struct etna_context *ctx = etna_context(pctx);
   const uint32_t format = translate_texture_format(so->format);
   const bool ext = !!(format & EXT_FORMAT);
   const uint32_t swiz = get_texture_swiz(so->format, so->swizzle_r,
                                          so->swizzle_g, so->swizzle_b,
                                          so->swizzle_a);

   if (!sv)
      return NULL;

   if (!etna_resource_sampler_compatible(res)) {
      /* The original resource is not compatible with the sampler.
       * Allocate an appropriately tiled texture. */
      if (!res->texture) {
         struct pipe_resource templat = *prsc;

         templat.bind &= ~(PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_RENDER_TARGET |
                           PIPE_BIND_BLENDABLE);
         res->texture =
            etna_resource_alloc(pctx->screen, ETNA_LAYOUT_TILED,
                                DRM_FORMAT_MOD_LINEAR, &templat);
      }

      if (!res->texture) {
         free(sv);
         return NULL;
      }
      res = etna_resource(res->texture);
   }

   sv->base = *so;
   pipe_reference_init(&sv->base.reference, 1);
   sv->base.texture = NULL;
   pipe_resource_reference(&sv->base.texture, prsc);
   sv->base.context = pctx;

   /* merged with sampler state */
   sv->TE_SAMPLER_CONFIG0 = COND(!ext, VIVS_TE_SAMPLER_CONFIG0_FORMAT(format));
   sv->TE_SAMPLER_CONFIG0_MASK = 0xffffffff;

   switch (sv->base.target) {
   case PIPE_TEXTURE_1D:
      /* For 1D textures, we will have a height of 1, so we can use 2D
       * but set T wrap to repeat */
      sv->TE_SAMPLER_CONFIG0_MASK = ~VIVS_TE_SAMPLER_CONFIG0_VWRAP__MASK;
      sv->TE_SAMPLER_CONFIG0 |= VIVS_TE_SAMPLER_CONFIG0_VWRAP(TEXTURE_WRAPMODE_REPEAT);
      /* fallthrough */
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_RECT:
      sv->TE_SAMPLER_CONFIG0 |= VIVS_TE_SAMPLER_CONFIG0_TYPE(TEXTURE_TYPE_2D);
      break;
   case PIPE_TEXTURE_CUBE:
      sv->TE_SAMPLER_CONFIG0 |= VIVS_TE_SAMPLER_CONFIG0_TYPE(TEXTURE_TYPE_CUBE_MAP);
      break;
   default:
      BUG("Unhandled texture target");
      free(sv);
      return NULL;
   }

   sv->TE_SAMPLER_CONFIG1 = COND(ext, VIVS_TE_SAMPLER_CONFIG1_FORMAT_EXT(format)) |
                            VIVS_TE_SAMPLER_CONFIG1_HALIGN(res->halign) | swiz |
                            VIVS_TE_SAMPLER_CONFIG1_UNK25;
   sv->TE_SAMPLER_SIZE = VIVS_TE_SAMPLER_SIZE_WIDTH(res->base.width0 >> sv->base.u.tex.first_level) |
                         VIVS_TE_SAMPLER_SIZE_HEIGHT(res->base.height0 >> sv->base.u.tex.first_level);
   sv->TE_SAMPLER_LOG_SIZE =
      VIVS_TE_SAMPLER_LOG_SIZE_WIDTH(etna_log2_fixp55(res->base.width0)) |
      VIVS_TE_SAMPLER_LOG_SIZE_HEIGHT(etna_log2_fixp55(res->base.height0));

   /* Set up levels-of-detail */
   for (int lod = 0; lod <= res->base.last_level; ++lod) {
      sv->TE_SAMPLER_LOD_ADDR[lod].bo = res->bo;
      sv->TE_SAMPLER_LOD_ADDR[lod].offset = res->levels[lod].offset;
      sv->TE_SAMPLER_LOD_ADDR[lod].flags = ETNA_RELOC_READ;
   }
   sv->min_lod = sv->base.u.tex.first_level << 5;
   sv->max_lod = MIN2(sv->base.u.tex.last_level, res->base.last_level) << 5;

   /* Workaround for npot textures -- it appears that only CLAMP_TO_EDGE is
    * supported when the appropriate capability is not set. */
   if (!ctx->specs.npot_tex_any_wrap &&
       (!util_is_power_of_two(res->base.width0) || !util_is_power_of_two(res->base.height0))) {
      sv->TE_SAMPLER_CONFIG0_MASK = ~(VIVS_TE_SAMPLER_CONFIG0_UWRAP__MASK |
                                      VIVS_TE_SAMPLER_CONFIG0_VWRAP__MASK);
      sv->TE_SAMPLER_CONFIG0 |=
         VIVS_TE_SAMPLER_CONFIG0_UWRAP(TEXTURE_WRAPMODE_CLAMP_TO_EDGE) |
         VIVS_TE_SAMPLER_CONFIG0_VWRAP(TEXTURE_WRAPMODE_CLAMP_TO_EDGE);
   }

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
   DESC_SET(CONFIG0, sv->TE_SAMPLER_CONFIG0);
   DESC_SET(CONFIG1, sv->TE_SAMPLER_CONFIG1);
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
   DESC_SET(LOG_SIZE, 0); /* appears unused */
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
                                    struct etna_sampler_view *sv)
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
etna_sampler_view_destroy(struct pipe_context *pctx,
                          struct pipe_sampler_view *view_)
{
   struct etna_sampler_view *view = etna_sampler_view(view_);
   pipe_resource_reference(&view->base.texture, NULL);
   etna_bo_del(view->bo);
   FREE(view);
}

static void
set_sampler_views(struct etna_context *ctx, unsigned start, unsigned end,
                  unsigned nr, struct pipe_sampler_view **views)
{
   unsigned i, j;
   uint32_t mask = 1 << start;

   for (i = start, j = 0; j < nr; i++, j++, mask <<= 1) {
      pipe_sampler_view_reference(&ctx->sampler_view[i], views[j]);
      if (views[j])
         ctx->active_sampler_views |= mask;
      else
         ctx->active_sampler_views &= ~mask;
   }

   for (; i < end; i++, mask <<= 1) {
      pipe_sampler_view_reference(&ctx->sampler_view[i], NULL);
      ctx->active_sampler_views &= ~mask;
   }
}

static inline void
etna_fragtex_set_sampler_views(struct etna_context *ctx, unsigned nr,
                               struct pipe_sampler_view **views)
{
   unsigned start = 0;
   unsigned end = start + ctx->specs.fragment_sampler_count;

   set_sampler_views(ctx, start, end, nr, views);
   ctx->num_fragment_sampler_views = nr;
}


static inline void
etna_vertex_set_sampler_views(struct etna_context *ctx, unsigned nr,
                              struct pipe_sampler_view **views)
{
   unsigned start = ctx->specs.vertex_sampler_offset;
   unsigned end = start + ctx->specs.vertex_sampler_count;

   set_sampler_views(ctx, start, end, nr, views);
}

static void
etna_set_sampler_views(struct pipe_context *pctx, enum pipe_shader_type shader,
                       unsigned start_slot, unsigned num_views,
                       struct pipe_sampler_view **views)
{
   struct etna_context *ctx = etna_context(pctx);
   assert(start_slot == 0);

   ctx->dirty |= ETNA_DIRTY_SAMPLER_VIEWS | ETNA_DIRTY_TEXTURE_CACHES;

   for (unsigned idx = 0; idx < num_views; ++idx) {
      if (views[idx])
         etna_update_sampler_source(views[idx]);
   }

   switch (shader) {
   case PIPE_SHADER_FRAGMENT:
      etna_fragtex_set_sampler_views(ctx, num_views, views);
      break;
   case PIPE_SHADER_VERTEX:
      etna_vertex_set_sampler_views(ctx, num_views, views);
      break;
   default:;
   }
}

static void
etna_texture_barrier(struct pipe_context *pctx, unsigned flags)
{
   struct etna_context *ctx = etna_context(pctx);
   /* clear color and texture cache to make sure that texture unit reads
    * what has been written */
   etna_set_state(ctx->stream, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_COLOR | VIVS_GL_FLUSH_CACHE_TEXTURE);
}

void
etna_texture_init(struct pipe_context *pctx)
{
   pctx->create_sampler_state = etna_create_sampler_state;
   pctx->bind_sampler_states = etna_bind_sampler_states;
   pctx->delete_sampler_state = etna_delete_sampler_state;
   pctx->set_sampler_views = etna_set_sampler_views;
   pctx->create_sampler_view = etna_create_sampler_view;
   pctx->sampler_view_destroy = etna_sampler_view_destroy;
   pctx->texture_barrier = etna_texture_barrier;
}
