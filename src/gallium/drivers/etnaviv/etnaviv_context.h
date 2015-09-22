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
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#ifndef H_ETNAVIV_CONTEXT
#define H_ETNAVIV_CONTEXT

#include <stdint.h>

#include "etnaviv_tiling.h"
#include "etnaviv_resource.h"
#include "pipe/p_defines.h"
#include "pipe/p_format.h"
#include "pipe/p_shader_tokens.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "util/u_slab.h"

struct pipe_screen;
struct etna_shader_object;

struct etna_shader_input
{
    int vs_reg; /* VS input register */
};

enum etna_varying_special {
    ETNA_VARYING_VSOUT = 0, /* from VS */
    ETNA_VARYING_POINTCOORD, /* point texture coord */
};

struct etna_shader_varying
{
    int num_components;
    enum etna_varying_special special;
    int pa_attributes;
    int vs_reg; /* VS output register */
};

struct etna_transfer
{
    struct pipe_transfer base;

    /* Pointer to buffer (same pointer as returned by transfer_map) */
    void *buffer;
    /* If true, transfer happens in-place. buffer is not allocated separately but
     * points into the actual resource, and thus does not need to be copied or freed.
     */
    bool in_place;
};

/* private opaque context structure */
struct etna_context
{
    struct pipe_context base;

    struct etna_specs specs;
    struct etna_screen *screen;
    struct etna_cmd_stream *stream;

    /* which state objects need to be re-emit'd: */
    enum {
        ETNA_DIRTY_BLEND           = (1 <<  0),
        ETNA_DIRTY_SAMPLERS        = (1 <<  1),
        ETNA_DIRTY_RASTERIZER      = (1 <<  2),
        ETNA_DIRTY_ZSA             = (1 <<  3),
        ETNA_DIRTY_VERTEX_ELEMENTS = (1 <<  4),
        ETNA_DIRTY_BLEND_COLOR     = (1 <<  6),
        ETNA_DIRTY_STENCIL_REF     = (1 <<  7),
        ETNA_DIRTY_SAMPLE_MASK     = (1 <<  8),
        ETNA_DIRTY_VIEWPORT        = (1 <<  9),
        ETNA_DIRTY_FRAMEBUFFER     = (1 << 10),
        ETNA_DIRTY_SCISSOR         = (1 << 11),
        ETNA_DIRTY_SAMPLER_VIEWS   = (1 << 12),
        ETNA_DIRTY_VERTEX_BUFFERS  = (1 << 13),
        ETNA_DIRTY_INDEX_BUFFER    = (1 << 14),
        ETNA_DIRTY_SHADER          = (1 << 15),
        ETNA_DIRTY_VS_UNIFORMS     = (1 << 16),
        ETNA_DIRTY_PS_UNIFORMS     = (1 << 17),
        ETNA_DIRTY_TS              = (1 << 18), /* set after clear and when RS blit operations from other surface affect TS */
        ETNA_DIRTY_TEXTURE_CACHES  = (1 << 19), /* set when texture has been modified/uploaded */
    } dirty;

    struct util_slab_mempool transfer_pool;
    struct blitter_context *blitter;

    /* compiled bindable state */
    unsigned sample_mask;
    struct pipe_blend_state *blend;
    unsigned num_vertex_samplers;
    unsigned num_fragment_samplers;
    struct pipe_sampler_state *sampler[PIPE_MAX_SAMPLERS];
    struct pipe_rasterizer_state *rasterizer;
    struct pipe_depth_stencil_alpha_state *zsa;
    struct compiled_vertex_elements_state *vertex_elements;
    struct compiled_shader_state shader_state;

    /* to simplify the emit process we store pre compiled state objects,
     * which got 'compiled' during state change. */
    struct compiled_blend_color blend_color;
    struct compiled_stencil_ref stencil_ref;
    struct compiled_framebuffer_state framebuffer;
    struct compiled_scissor_state scissor;
    struct compiled_viewport_state viewport;
    unsigned num_fragment_sampler_views;
    unsigned num_vertex_sampler_views;
    struct pipe_sampler_view *sampler_view[PIPE_MAX_SAMPLERS];
    struct compiled_set_vertex_buffer vertex_buffer[PIPE_MAX_ATTRIBS];
    struct pipe_index_buffer index_buffer;

    /* pointers to the bound state. these are mainly kept around for the blitter. */
    struct etna_shader_object *vs;
    struct etna_shader_object *fs;

    /* saved parameter-like state. these are mainly kept around for the blitter. */
    struct pipe_framebuffer_state framebuffer_s;
    struct pipe_stencil_ref stencil_ref_s;
    struct pipe_viewport_state viewport_s;
    struct pipe_scissor_state scissor_s;
    struct pipe_vertex_buffer vertex_buffer_s[PIPE_MAX_ATTRIBS];
    struct pipe_constant_buffer vs_cbuf_s;
    struct pipe_constant_buffer fs_cbuf_s;

    /* cached state of entire GPU */
    struct etna_3d_state gpu3d;
};

static inline struct etna_context *
etna_context(struct pipe_context *pctx)
{
    return (struct etna_context *)pctx;
}

static inline struct etna_transfer *
etna_transfer(struct pipe_transfer *p)
{
    return (struct etna_transfer *)p;
}

struct pipe_context *etna_context_create(struct pipe_screen* pscreen, void* priv, unsigned flags);

#endif
