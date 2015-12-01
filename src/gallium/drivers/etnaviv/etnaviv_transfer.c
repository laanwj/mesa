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

#include "etnaviv_clear_blit.h"
#include "etnaviv_transfer.h"
#include "etnaviv_context.h"
#include "etnaviv_debug.h"

#include "pipe/p_defines.h"
#include "pipe/p_format.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_format.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_surface.h"
#include "util/u_transfer.h"

/* Compute offset into a 1D/2D/3D buffer of a certain box.
 * This box must be aligned to the block width and height of the underlying format.
 */
static inline size_t etna_compute_offset(enum pipe_format format, const struct pipe_box *box,
        size_t stride, size_t layer_stride)
{
    return box->z * layer_stride +
           box->y / util_format_get_blockheight(format) * stride +
           box->x / util_format_get_blockwidth(format) * util_format_get_blocksize(format);
}

static void *etna_transfer_map(struct pipe_context *pctx,
                         struct pipe_resource *resource,
                         unsigned level,
                         unsigned usage,  /* a combination of PIPE_TRANSFER_x */
                         const struct pipe_box *box,
                         struct pipe_transfer **out_transfer)
{
    struct etna_context *ctx = etna_context(pctx);
    struct etna_transfer *ptrans = util_slab_alloc(&ctx->transfer_pool);
    struct etna_resource *resource_priv = etna_resource(resource);
    enum pipe_format format = resource->format;

    if (!ptrans)
        return NULL;

    ptrans->base.resource = resource;
    ptrans->base.level = level;
    ptrans->base.usage = usage;
    ptrans->base.box = *box;
    ptrans->rsc = NULL;

    assert(level <= resource->last_level);

    if (resource_priv->texture && resource_priv->seqno - etna_resource(resource_priv->texture)->seqno >= 0)
    {
        /* We have a texture resource which is the same age or newer than the
         * render resource. Use the texture resource, which avoids bouncing
         * pixels between the two resources, and we can de-tile it in s/w. */
        resource_priv = etna_resource(resource_priv->texture);
    }
    else if (resource_priv->ts_bo ||
             (resource_priv->layout != ETNA_LAYOUT_LINEAR &&
              util_format_get_blocksize(format) > 1))
    {
        /* If the surface has tile status, we need to resolve it first.
         * The strategy we implement here is to use the RS to copy the
         * depth buffer, filling in the "holes" where the tile status
         * indicates that it's clear. We also do this for tiled
         * resources, but only if the RS can blit them. */
        if (usage & PIPE_TRANSFER_MAP_DIRECTLY)
        {
            util_slab_free(&ctx->transfer_pool, ptrans);
            BUG("unsupported transfer flags %#x with tile status/tiled layout", usage);
            return NULL;
        }

        if (resource->depth0 > 1)
        {
            util_slab_free(&ctx->transfer_pool, ptrans);
            BUG("resource has depth >1 with tile status");
            return NULL;
        }

        struct pipe_resource templ = *resource;
        templ.bind = PIPE_BIND_RENDER_TARGET;

        ptrans->rsc = etna_resource_alloc(pctx->screen, ETNA_LAYOUT_LINEAR, &templ);
        if (!ptrans->rsc) {
            util_slab_free(&ctx->transfer_pool, ptrans);
            return NULL;
        }

        etna_copy_resource(pctx, ptrans->rsc, resource, level, ptrans->rsc->last_level);

        /* Switch to using the temporary resource instead */
        resource_priv = etna_resource(ptrans->rsc);
    }

    struct etna_resource_level *res_level = &resource_priv->levels[level];

    /* Always sync if we have the temporary resource.  The PIPE_TRANSFER_READ
     * case could be optimised if we knew whether the resource has outstanding
     * rendering. */
    if(usage & PIPE_TRANSFER_READ || ptrans->rsc)
    {
        struct pipe_fence_handle *fence;
        struct pipe_screen *pscreen = pctx->screen;

        pctx->flush(pctx, &fence, 0);

        if (!pscreen->fence_finish(pscreen, fence, 5000))
            BUG("fence timed out (hung GPU?)");
        pscreen->fence_reference(pscreen, &fence, NULL);
    }
    /* XXX we don't handle PIPE_TRANSFER_FLUSH_EXPLICIT; this flag can be ignored when mapping in-place,
     * but when not in place we need to fire off the copy operation in transfer_flush_region (currently
     * a no-op) instead of unmap. Need to handle this to support ARB_map_buffer_range extension at least.
     */
    /* XXX we don't take care of current operations on the resource; which can be, at some point in the pipeline
       which is not yet executed:

       - bound as surface
       - bound through vertex buffer
       - bound through index buffer
       - bound in sampler view
       - used in clear_render_target / clear_depth_stencil operation
       - used in blit
       - used in resource_copy_region

       How do other drivers record this information over course of the rendering pipeline?
       Is it necessary at all? Only in case we want to provide a fast path and map the resource directly
       (and for PIPE_TRANSFER_MAP_DIRECTLY) and we don't want to force a sync.
       We also need to know whether the resource is in use to determine if a sync is needed (or just do it
       always, but that comes at the expense of performance).

       A conservative approximation without too much overhead would be to mark all resources that have
       been bound at some point as busy. A drawback would be that accessing resources that have
       been bound but are no longer in use for a while still carry a performance penalty. On the other hand,
       the program could be using PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE or PIPE_TRANSFER_UNSYNCHRONIZED to
       avoid this in the first place...

       A) We use an in-pipe copy engine, and queue the copy operation after unmap so that the copy
          will be performed when all current commands have been executed.
          Using the RS is possible, not sure if always efficient. This can also do any kind of tiling for us.
          Only possible when PIPE_TRANSFER_DISCARD_RANGE is set.
       B) We discard the entire resource (or at least, the mipmap level) and allocate new memory for it.
          Only possible when mapping the entire resource or PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE is set.
     */

    /* No need to allocate a buffer for copying if the resource is not in use,
     * and no tiling is needed, can just return a direct pointer.
     */
    ptrans->in_place = resource_priv->layout == ETNA_LAYOUT_LINEAR ||
                       (resource_priv->layout == ETNA_LAYOUT_TILED && util_format_is_compressed(resource->format));

    /* map buffer object */
    void *mapped = etna_bo_map(resource_priv->bo);
    if (!mapped) {
        pipe_resource_reference(&ptrans->rsc, NULL);
        util_slab_free(&ctx->transfer_pool, ptrans);
        return NULL;
    }

    if(likely(ptrans->in_place))
    {
        ptrans->base.stride = res_level->stride;
        ptrans->base.layer_stride = res_level->layer_stride;
        ptrans->buffer = mapped + res_level->offset + etna_compute_offset(resource->format, box, res_level->stride, res_level->layer_stride);
    } else {
        unsigned divSizeX = util_format_get_blockwidth(format);
        unsigned divSizeY = util_format_get_blockheight(format);
        if(usage & PIPE_TRANSFER_MAP_DIRECTLY)
        {
            /* No in-place transfer possible */
            pipe_resource_reference(&ptrans->rsc, NULL);
            util_slab_free(&ctx->transfer_pool, ptrans);
            return NULL;
        }

        mapped += res_level->offset;
        ptrans->base.stride = align(box->width, divSizeX) * util_format_get_blocksize(format); /* row stride in bytes */
        ptrans->base.layer_stride = align(box->height, divSizeY) * ptrans->base.stride;
        size_t size = ptrans->base.layer_stride * box->depth;
        ptrans->buffer = MALLOC(size);

        if(usage & PIPE_TRANSFER_READ)
        {
            /* untile or copy resource for reading */
            if(resource_priv->layout == ETNA_LAYOUT_LINEAR || resource_priv->layout == ETNA_LAYOUT_TILED)
            {
                if(resource_priv->layout == ETNA_LAYOUT_TILED && !util_format_is_compressed(resource_priv->base.format))
                {
                    etna_texture_untile(ptrans->buffer, mapped + ptrans->base.box.z * res_level->layer_stride,
                            ptrans->base.box.x, ptrans->base.box.y, res_level->stride,
                            ptrans->base.box.width, ptrans->base.box.height, ptrans->base.stride,
                            util_format_get_blocksize(resource_priv->base.format));
                } else { /* non-tiled or compressed format */
                    util_copy_box(ptrans->buffer,
                      resource_priv->base.format,
                      ptrans->base.stride, ptrans->base.layer_stride,
                      0, 0, 0, /* dst x,y,z */
                      ptrans->base.box.width, ptrans->base.box.height, ptrans->base.box.depth,
                      mapped,
                      res_level->stride, res_level->layer_stride,
                      ptrans->base.box.x, ptrans->base.box.y, ptrans->base.box.z);
                }
            } else /* TODO supertiling */
            {
                BUG("unsupported tiling %i for reading", resource_priv->layout);
            }
        }
    }

    *out_transfer = &ptrans->base;
    return ptrans->buffer;
}

static void etna_transfer_flush_region(struct pipe_context *pipe,
				  struct pipe_transfer *transfer_,
				  const struct pipe_box *box)
{
    /* NOOP for now */
}

static void etna_transfer_unmap(struct pipe_context *pctx,
                      struct pipe_transfer *transfer_)
{
    struct etna_context *ctx = etna_context(pctx);
    struct etna_transfer *ptrans = etna_transfer(transfer_);

    /* XXX
     * When writing to a resource that is already in use, replace the resource with a completely new buffer
     * and free the old one using a fenced free.
     * The most tricky case to implement will be: tiled or supertiled surface, partial write, target not aligned to 4/64
     */
    struct etna_resource *resource = etna_resource(ptrans->base.resource);
    assert(ptrans->base.level <= resource->base.last_level);

    if (resource->texture && resource->seqno - etna_resource(resource->texture)->seqno >= 0)
    {
        /* switch to using the texture resource */
        resource = etna_resource(resource->texture);
    }

    if(ptrans->base.usage & PIPE_TRANSFER_WRITE)
    {
        if(ptrans->rsc)
        {
            /* We have a temporary resource due to either tile status or
             * tiling format. Write back the updated buffer contents.
             * FIXME: we need to invalidate the tile status. */
            etna_copy_resource(pctx, ptrans->base.resource, ptrans->rsc,
                               ptrans->base.level, ptrans->rsc->last_level);
        }
        else if(unlikely(!ptrans->in_place))
        {
            /* map buffer object */
            struct etna_resource_level *res_level = &resource->levels[ptrans->base.level];
            void *mapped = etna_bo_map(resource->bo) + res_level->offset;
            if(resource->layout == ETNA_LAYOUT_LINEAR || resource->layout == ETNA_LAYOUT_TILED)
            {
                if(resource->layout == ETNA_LAYOUT_TILED && !util_format_is_compressed(resource->base.format))
                {
                    etna_texture_tile(mapped + ptrans->base.box.z * res_level->layer_stride, ptrans->buffer,
                            ptrans->base.box.x, ptrans->base.box.y, res_level->stride,
                            ptrans->base.box.width, ptrans->base.box.height, ptrans->base.stride,
                            util_format_get_blocksize(resource->base.format));
                } else { /* non-tiled or compressed format */
                    util_copy_box(mapped,
                      resource->base.format,
                      res_level->stride, res_level->layer_stride,
                      ptrans->base.box.x, ptrans->base.box.y, ptrans->base.box.z,
                      ptrans->base.box.width, ptrans->base.box.height, ptrans->base.box.depth,
                      ptrans->buffer,
                      ptrans->base.stride, ptrans->base.layer_stride,
                      0, 0, 0 /* src x,y,z */);
                }
            } else
            {
                BUG("unsupported tiling %i", resource->layout);
            }
            FREE(ptrans->buffer);
        }
        resource->seqno++;
        if(resource->base.bind & PIPE_BIND_SAMPLER_VIEW)
        {
            /* XXX do we need to flush the CPU cache too or start a write barrier
             * to make sure the GPU sees it? */
            ctx->dirty |= ETNA_DIRTY_TEXTURE_CACHES;
        }
    }

    pipe_resource_reference(&ptrans->rsc, NULL);
    util_slab_free(&ctx->transfer_pool, ptrans);
}

void etna_transfer_init(struct pipe_context *pctx)
{
    pctx->transfer_map = etna_transfer_map;
    pctx->transfer_flush_region = etna_transfer_flush_region;
    pctx->transfer_unmap = etna_transfer_unmap;
    pctx->transfer_inline_write = u_default_transfer_inline_write;
}
