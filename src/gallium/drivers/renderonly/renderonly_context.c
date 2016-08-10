/*
 * Copyright © 2014 NVIDIA Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdlib.h>

#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "renderonly_context.h"
#include "renderonly_resource.h"
#include "renderonly_screen.h"

static void
renderonly_set_active_query_state(struct pipe_context *pctx, boolean enable)
{
    struct renderonly_context *ctx = to_renderonly_context(pctx);

    return ctx->gpu->set_active_query_state(ctx->gpu, enable);
}


static void
renderonly_destroy(struct pipe_context *pctx)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->destroy(ctx->gpu);
	FREE(ctx);
}

static void
renderonly_draw_vbo(struct pipe_context *pctx,
	       const struct pipe_draw_info *pinfo)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);
	struct pipe_draw_info info;

	if (pinfo->indirect) {
		memcpy(&info, pinfo, sizeof(info));
		info.indirect = renderonly_resource_unwrap(info.indirect);
		pinfo = &info;
	}

	ctx->gpu->draw_vbo(ctx->gpu, pinfo);
}

static void *
renderonly_create_blend_state(struct pipe_context *pctx,
			 const struct pipe_blend_state *cso)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	return ctx->gpu->create_blend_state(ctx->gpu, cso);
}

static void
renderonly_bind_blend_state(struct pipe_context *pctx,
		       void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->bind_blend_state(ctx->gpu, so);
}

static void
renderonly_delete_blend_state(struct pipe_context *pctx,
			 void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->delete_blend_state(ctx->gpu, so);
}

static void *
renderonly_create_sampler_state(struct pipe_context *pctx,
			   const struct pipe_sampler_state *cso)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	return ctx->gpu->create_sampler_state(ctx->gpu, cso);
}

static void
renderonly_bind_sampler_states(struct pipe_context *pctx,
			  unsigned shader,
			  unsigned start_slot,
			  unsigned num_samplers,
			  void **samplers)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->bind_sampler_states(ctx->gpu, shader, start_slot,
					  num_samplers, samplers);
}

static void
renderonly_delete_sampler_state(struct pipe_context *pctx,
			   void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->delete_sampler_state(ctx->gpu, so);
}

static void *
renderonly_create_rasterizer_state(struct pipe_context *pctx,
			      const struct pipe_rasterizer_state *cso)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	return ctx->gpu->create_rasterizer_state(ctx->gpu, cso);
}

static void
renderonly_bind_rasterizer_state(struct pipe_context *pctx,
			    void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->bind_rasterizer_state(ctx->gpu, so);
}

static void
renderonly_delete_rasterizer_state(struct pipe_context *pctx,
			      void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->delete_rasterizer_state(ctx->gpu, so);
}

static void *
renderonly_create_depth_stencil_alpha_state(struct pipe_context *pctx,
				       const struct pipe_depth_stencil_alpha_state *cso)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	return ctx->gpu->create_depth_stencil_alpha_state(ctx->gpu,
							      cso);
}

static void
renderonly_bind_depth_stencil_alpha_state(struct pipe_context *pctx,
				     void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->bind_depth_stencil_alpha_state(ctx->gpu, so);
}

static void
renderonly_delete_depth_stencil_alpha_state(struct pipe_context *pctx,
				       void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->delete_depth_stencil_alpha_state(ctx->gpu, so);
}

static void *
renderonly_create_fs_state(struct pipe_context *pctx,
		      const struct pipe_shader_state *cso)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	return ctx->gpu->create_fs_state(ctx->gpu, cso);
}

static void
renderonly_bind_fs_state(struct pipe_context *pctx,
		    void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->bind_fs_state(ctx->gpu, so);
}

static void
renderonly_delete_fs_state(struct pipe_context *pctx,
		      void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->delete_fs_state(ctx->gpu, so);
}

static void *
renderonly_create_vs_state(struct pipe_context *pctx,
		      const struct pipe_shader_state *cso)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	return ctx->gpu->create_vs_state(ctx->gpu, cso);
}

static void
renderonly_bind_vs_state(struct pipe_context *pctx,
		    void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->bind_vs_state(ctx->gpu, so);
}

static void
renderonly_delete_vs_state(struct pipe_context *pctx,
		      void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->delete_vs_state(ctx->gpu, so);
}

static void *
renderonly_create_gs_state(struct pipe_context *pctx,
		      const struct pipe_shader_state *cso)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	return ctx->gpu->create_gs_state(ctx->gpu, cso);
}

static void
renderonly_bind_gs_state(struct pipe_context *pctx,
		    void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->bind_gs_state(ctx->gpu, so);
}

static void
renderonly_delete_gs_state(struct pipe_context *pctx,
		      void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->delete_gs_state(ctx->gpu, so);
}

static void *
renderonly_create_vertex_elements_state(struct pipe_context *pctx,
				   unsigned num_elements,
				   const struct pipe_vertex_element *elements)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	return ctx->gpu->create_vertex_elements_state(ctx->gpu,
							  num_elements,
							  elements);
}

static void
renderonly_bind_vertex_elements_state(struct pipe_context *pctx,
				 void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->bind_vertex_elements_state(ctx->gpu, so);
}

static void
renderonly_delete_vertex_elements_state(struct pipe_context *pctx,
				   void *so)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->delete_vertex_elements_state(ctx->gpu, so);
}

static void
renderonly_set_blend_color(struct pipe_context *pctx,
                          const struct pipe_blend_color *bc)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->set_blend_color(ctx->gpu, bc);
}

static void
renderonly_set_stencil_ref(struct pipe_context *pctx,
                          const struct pipe_stencil_ref *ref)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->set_stencil_ref(ctx->gpu, ref);
}

static void
renderonly_set_clip_state(struct pipe_context *pctx,
                          const struct pipe_clip_state *pcs)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->set_clip_state(ctx->gpu, pcs);
}

static void
renderonly_set_constant_buffer(struct pipe_context *pctx,
			  uint shader,
			  uint index,
			  struct pipe_constant_buffer *buf)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);
	struct pipe_constant_buffer buffer;

	if (buf && buf->buffer) {
		memcpy(&buffer, buf, sizeof(buffer));
		buffer.buffer = renderonly_resource_unwrap(buffer.buffer);
		buf = &buffer;
	}

	ctx->gpu->set_constant_buffer(ctx->gpu, shader, index, buf);
}

static void
renderonly_set_framebuffer_state(struct pipe_context *pctx,
			    const struct pipe_framebuffer_state *fb)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);
	struct pipe_framebuffer_state state;
	unsigned i;

	memcpy(&state, fb, sizeof(state));

	for (i = 0; i < fb->nr_cbufs; i++)
		state.cbufs[i] = renderonly_surface_unwrap(fb->cbufs[i]);

	while (i < PIPE_MAX_COLOR_BUFS)
		state.cbufs[i++] = NULL;

	state.zsbuf = renderonly_surface_unwrap(fb->zsbuf);

	fb = &state;
	ctx->gpu->set_framebuffer_state(ctx->gpu, fb);
}

static void
renderonly_set_polygon_stipple(struct pipe_context *pctx,
			  const struct pipe_poly_stipple *stipple)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->set_polygon_stipple(ctx->gpu, stipple);
}

static void
renderonly_set_scissor_states(struct pipe_context *pctx,
			 unsigned start_slot,
			 unsigned num_scissors,
			 const struct pipe_scissor_state *scissors)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->set_scissor_states(ctx->gpu, start_slot,
					 num_scissors, scissors);
}

static void
renderonly_set_viewport_states(struct pipe_context *pctx,
			  unsigned start_slot,
			  unsigned num_viewports,
			  const struct pipe_viewport_state *viewports)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->set_viewport_states(ctx->gpu, start_slot,
					  num_viewports, viewports);
}

static void
renderonly_set_sampler_views(struct pipe_context *pctx,
			unsigned shader,
			unsigned start_slot,
			unsigned num_views,
			struct pipe_sampler_view **pviews)
{
	struct pipe_sampler_view *views[PIPE_MAX_SHADER_SAMPLER_VIEWS];
	struct renderonly_context *ctx = to_renderonly_context(pctx);
	unsigned i;

	for (i = 0; i < num_views; i++)
		views[i] = renderonly_sampler_view_unwrap(pviews[i]);

	ctx->gpu->set_sampler_views(ctx->gpu, shader, start_slot,
					num_views, views);
}

static void
renderonly_set_shader_images(struct pipe_context *pctx,
			   unsigned shader,
			   unsigned start_slot, unsigned count,
			   struct pipe_image_view *images)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->set_shader_images(ctx->gpu, shader, start_slot,
					count, images);
}

static void
renderonly_set_vertex_buffers(struct pipe_context *pctx,
			 unsigned start_slot,
			 unsigned num_buffers,
			 const struct pipe_vertex_buffer *buffers)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);
	struct pipe_vertex_buffer buf[PIPE_MAX_SHADER_INPUTS];
	unsigned i;

	if (num_buffers && buffers) {
		memcpy(buf, buffers, num_buffers * sizeof(struct pipe_vertex_buffer));

		for (i = 0; i < num_buffers; i++)
			buf[i].buffer = renderonly_resource_unwrap(buf[i].buffer);

		buffers = buf;
	}

	ctx->gpu->set_vertex_buffers(ctx->gpu, start_slot,
					 num_buffers, buffers);
}

static void
renderonly_set_index_buffer(struct pipe_context *pctx,
		       const struct pipe_index_buffer *buffer)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);
	struct pipe_index_buffer buf;

	if (buffer) {
		memcpy(&buf, buffer, sizeof(buf));
		buf.buffer = renderonly_resource_unwrap(buf.buffer);
		buffer = &buf;
	}

	ctx->gpu->set_index_buffer(ctx->gpu, buffer);
}

static struct pipe_stream_output_target *
renderonly_create_stream_output_target(struct pipe_context *pctx,
				  struct pipe_resource *prsc,
				  unsigned buffer_offset,
				  unsigned buffer_size)
{
	struct renderonly_resource *rsc = to_renderonly_resource(prsc);
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	return ctx->gpu->create_stream_output_target(ctx->gpu,
							 rsc->gpu,
							 buffer_offset,
							 buffer_size);
}

static void
renderonly_stream_output_target_destroy(struct pipe_context *pctx,
				   struct pipe_stream_output_target *target)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->stream_output_target_destroy(ctx->gpu, target);
}

static void
renderonly_set_stream_output_targets(struct pipe_context *pctx,
				unsigned num_targets,
				struct pipe_stream_output_target **targets,
				const unsigned *offsets)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->set_stream_output_targets(ctx->gpu, num_targets,
						targets, offsets);
}

static void
renderonly_resource_copy_region(struct pipe_context *pctx,
           struct pipe_resource *dst, unsigned dst_level,
           unsigned dstx, unsigned dsty, unsigned dstz,
           struct pipe_resource *src, unsigned src_level,
           const struct pipe_box *src_box)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->resource_copy_region(ctx->gpu,
					   renderonly_resource_unwrap(dst),
					   dst_level, dstx, dsty, dstz,
					   renderonly_resource_unwrap(src),
					   src_level, src_box);
}

static void
renderonly_blit(struct pipe_context *pctx,
	   const struct pipe_blit_info *pinfo)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);
	struct pipe_blit_info info;

	memcpy(&info, pinfo, sizeof(info));
	info.dst.resource = renderonly_resource_unwrap(info.dst.resource);
	info.src.resource = renderonly_resource_unwrap(info.src.resource);
	pinfo = &info;

	ctx->gpu->blit(ctx->gpu, pinfo);
}

static void
renderonly_clear(struct pipe_context *pctx,
	    unsigned buffers,
	    const union pipe_color_union *color,
	    double depth,
	    unsigned stencil)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->clear(ctx->gpu, buffers, color, depth, stencil);
}

static void
renderonly_flush(struct pipe_context *pctx,
	    struct pipe_fence_handle **fence,
	    unsigned flags)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->flush(ctx->gpu, fence, flags);
}

static struct pipe_sampler_view *
renderonly_create_sampler_view(struct pipe_context *pctx,
			  struct pipe_resource *ptexture,
			  const struct pipe_sampler_view *template)
{
	struct renderonly_resource *texture = to_renderonly_resource(ptexture);
	struct renderonly_context *ctx = to_renderonly_context(pctx);
	struct renderonly_sampler_view *view;

	view = CALLOC_STRUCT(renderonly_sampler_view);
	if (!view)
		return NULL;

	view->gpu = ctx->gpu->create_sampler_view(ctx->gpu,
						      texture->gpu,
						      template);
        if (view->gpu == NULL) {
		FREE(view);
		return NULL;
	}

	memcpy(&view->base, view->gpu, sizeof(*view->gpu));
	/* overwrite to prevent reference from being released */
	view->base.texture = NULL;

	pipe_reference_init(&view->base.reference, 1);
	pipe_resource_reference(&view->base.texture, ptexture);
	view->base.context = pctx;

	return &view->base;
}

static void
renderonly_sampler_view_destroy(struct pipe_context *pctx,
			   struct pipe_sampler_view *pview)
{
	struct renderonly_sampler_view *view = to_renderonly_sampler_view(pview);

	pipe_resource_reference(&view->base.texture, NULL);
	pipe_sampler_view_reference(&view->gpu, NULL);
	FREE(view);
}

static void
renderonly_texture_barrier(struct pipe_context *pctx)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->texture_barrier(ctx->gpu);
}

static void
renderonly_flush_resource(struct pipe_context *pctx,
		     struct pipe_resource *prsc)
{
	struct renderonly_resource *rsc = to_renderonly_resource(prsc);
	struct renderonly_context *ctx = to_renderonly_context(pctx);
	struct renderonly_screen *screen = to_renderonly_screen(prsc->screen);

	/* we need to blit our gpu render result to dumb buffer */
	struct pipe_blit_info blit = {
	      .mask = PIPE_MASK_RGBA,
	      .filter = PIPE_TEX_FILTER_LINEAR,
	      .src.resource = rsc->gpu,
	      .src.format = rsc->gpu->format,
	      .src.level = 0,
	      .src.box.width = rsc->gpu->width0,
	      .src.box.height = rsc->gpu->height0,
	      .src.box.depth = 1,
	      .dst.resource = rsc->prime,
	      .dst.format = rsc->prime->format,
	      .dst.level = 0,
	      .dst.box.width = rsc->prime->width0,
	      .dst.box.height = rsc->prime->height0,
	      .dst.box.depth = 1
	};

	ctx->gpu->flush_resource(ctx->gpu, rsc->gpu);

	if (!rsc->scanout || !screen->ops->intermediate_rendering)
		return;

	ctx->gpu->blit(ctx->gpu, &blit);
}

static void *
renderonly_transfer_map(struct pipe_context *pctx,
		   struct pipe_resource *prsc,
		   unsigned level,
		   unsigned usage,
		   const struct pipe_box *box,
		   struct pipe_transfer **ptransfer)
{
	struct renderonly_resource *rsc = to_renderonly_resource(prsc);
	struct renderonly_context *ctx = to_renderonly_context(pctx);
	struct renderonly_transfer *transfer;

	transfer = CALLOC_STRUCT(renderonly_transfer);
	if (!transfer)
		return NULL;

	transfer->map = ctx->gpu->transfer_map(ctx->gpu,
						   rsc->gpu,
						   level,
						   usage,
						   box,
						   &transfer->gpu);
	if (!transfer->map) {
		FREE(transfer);
		return NULL;
	}

	memcpy(&transfer->base, transfer->gpu, sizeof(*transfer->gpu));
	transfer->base.resource = NULL;
	pipe_resource_reference(&transfer->base.resource, prsc);

	*ptransfer = &transfer->base;

	return transfer->map;
}

static void
renderonly_transfer_unmap(struct pipe_context *pctx,
		     struct pipe_transfer *ptransfer)
{
	struct renderonly_transfer *transfer = to_renderonly_transfer(ptransfer);
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->transfer_unmap(ctx->gpu, transfer->gpu);
	pipe_resource_reference(&transfer->base.resource, NULL);
	FREE(transfer);
}

static void
renderonly_transfer_inline_write(struct pipe_context *pctx,
			    struct pipe_resource *prsc,
			    unsigned level,
			    unsigned usage,
			    const struct pipe_box *box,
			    const void *data,
			    unsigned stride,
			    unsigned layer_stride)
{
	struct renderonly_resource *rsc = to_renderonly_resource(prsc);
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->transfer_inline_write(ctx->gpu, rsc->gpu,
					    level, usage, box, data, stride,
					    layer_stride);
}

static void
renderonly_transfer_flush_region(struct pipe_context *pctx,
				  struct pipe_transfer *ptransfer,
				  const struct pipe_box *box)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);
	struct renderonly_transfer *transfer = to_renderonly_transfer(ptransfer);

	ctx->gpu->transfer_flush_region(ctx->gpu, transfer->gpu, box);
}

static struct pipe_query *
renderonly_create_query(struct pipe_context *pctx,
		     unsigned query_type, unsigned index)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	return ctx->gpu->create_query(ctx->gpu, query_type, index);
}

static struct pipe_query *
renderonly_create_batch_query(struct pipe_context *pctx,
		     unsigned num_queries, unsigned *query_types)
{
    struct renderonly_context *ctx = to_renderonly_context(pctx);

    return ctx->gpu->create_batch_query(ctx->gpu, num_queries, query_types);
}

static void
renderonly_destroy_query(struct pipe_context *pctx,
		     struct pipe_query *q)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	ctx->gpu->destroy_query(ctx->gpu, q);
}

static boolean
renderonly_begin_query(struct pipe_context *pctx, struct pipe_query *q)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	return ctx->gpu->begin_query(ctx->gpu, q);
}

static bool
renderonly_end_query(struct pipe_context *pctx, struct pipe_query *q)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	return ctx->gpu->end_query(ctx->gpu, q);
}

static boolean
renderonly_get_query_result(struct pipe_context *pctx,
		     struct pipe_query *q,
		     boolean wait,
		     union pipe_query_result *result)
{
	struct renderonly_context *ctx = to_renderonly_context(pctx);

	return ctx->gpu->get_query_result(ctx->gpu, q, wait, result);
}

struct pipe_context *
renderonly_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);
	struct renderonly_context *ctx;

	ctx = CALLOC_STRUCT(renderonly_context);
	if (!ctx)
		return NULL;

	ctx->gpu = screen->gpu->context_create(screen->gpu, priv, flags);
	if (!ctx->gpu) {
		debug_error("failed to create GPU context\n");
		FREE(ctx);
		return NULL;
	}

	ctx->base.screen = &screen->base;
	ctx->base.priv = priv;

	ctx->base.destroy = renderonly_destroy;

	ctx->base.draw_vbo = renderonly_draw_vbo;

	ctx->base.create_blend_state = renderonly_create_blend_state;
	ctx->base.bind_blend_state = renderonly_bind_blend_state;
	ctx->base.delete_blend_state = renderonly_delete_blend_state;

	ctx->base.create_sampler_state = renderonly_create_sampler_state;
	ctx->base.bind_sampler_states = renderonly_bind_sampler_states;
	ctx->base.delete_sampler_state = renderonly_delete_sampler_state;

	ctx->base.create_rasterizer_state = renderonly_create_rasterizer_state;
	ctx->base.bind_rasterizer_state = renderonly_bind_rasterizer_state;
	ctx->base.delete_rasterizer_state = renderonly_delete_rasterizer_state;

	ctx->base.create_depth_stencil_alpha_state = renderonly_create_depth_stencil_alpha_state;
	ctx->base.bind_depth_stencil_alpha_state = renderonly_bind_depth_stencil_alpha_state;
	ctx->base.delete_depth_stencil_alpha_state = renderonly_delete_depth_stencil_alpha_state;

	ctx->base.create_fs_state = renderonly_create_fs_state;
	ctx->base.bind_fs_state = renderonly_bind_fs_state;
	ctx->base.delete_fs_state = renderonly_delete_fs_state;

	ctx->base.create_vs_state = renderonly_create_vs_state;
	ctx->base.bind_vs_state = renderonly_bind_vs_state;
	ctx->base.delete_vs_state = renderonly_delete_vs_state;

	ctx->base.create_gs_state = renderonly_create_gs_state;
	ctx->base.bind_gs_state = renderonly_bind_gs_state;
	ctx->base.delete_gs_state = renderonly_delete_gs_state;

	ctx->base.create_vertex_elements_state = renderonly_create_vertex_elements_state;
	ctx->base.bind_vertex_elements_state = renderonly_bind_vertex_elements_state;
	ctx->base.delete_vertex_elements_state = renderonly_delete_vertex_elements_state;

	ctx->base.set_blend_color = renderonly_set_blend_color;
	ctx->base.set_stencil_ref = renderonly_set_stencil_ref;
	ctx->base.set_clip_state = renderonly_set_clip_state;
	ctx->base.set_constant_buffer = renderonly_set_constant_buffer;
	ctx->base.set_framebuffer_state = renderonly_set_framebuffer_state;
	ctx->base.set_polygon_stipple = renderonly_set_polygon_stipple;
	ctx->base.set_scissor_states = renderonly_set_scissor_states;
	ctx->base.set_viewport_states = renderonly_set_viewport_states;
	ctx->base.set_sampler_views = renderonly_set_sampler_views;

	if (ctx->gpu->set_shader_images)
		ctx->base.set_shader_images = renderonly_set_shader_images;

	ctx->base.set_vertex_buffers = renderonly_set_vertex_buffers;
	ctx->base.set_index_buffer = renderonly_set_index_buffer;

	ctx->base.create_stream_output_target = renderonly_create_stream_output_target;
	ctx->base.stream_output_target_destroy = renderonly_stream_output_target_destroy;
	ctx->base.set_stream_output_targets = renderonly_set_stream_output_targets;

	ctx->base.resource_copy_region = renderonly_resource_copy_region;
	ctx->base.blit = renderonly_blit;
	ctx->base.clear = renderonly_clear;
	ctx->base.flush = renderonly_flush;

	ctx->base.create_sampler_view = renderonly_create_sampler_view;
	ctx->base.sampler_view_destroy = renderonly_sampler_view_destroy;
	ctx->base.texture_barrier = renderonly_texture_barrier;

	ctx->base.flush_resource = renderonly_flush_resource;

	ctx->base.transfer_map = renderonly_transfer_map;
	ctx->base.transfer_unmap = renderonly_transfer_unmap;
	ctx->base.transfer_inline_write = renderonly_transfer_inline_write;
	ctx->base.transfer_flush_region = renderonly_transfer_flush_region;

	ctx->base.create_query = renderonly_create_query;
	ctx->base.create_batch_query = renderonly_create_batch_query;
	ctx->base.destroy_query = renderonly_destroy_query;
	ctx->base.begin_query = renderonly_begin_query;
	ctx->base.end_query = renderonly_end_query;
	ctx->base.get_query_result = renderonly_get_query_result;
	ctx->base.set_active_query_state = renderonly_set_active_query_state;

	renderonly_resource_context_init(&ctx->base);

	return &ctx->base;
}
