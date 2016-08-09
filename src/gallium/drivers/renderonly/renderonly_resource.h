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

#ifndef RENDERONLY_RESOURCE_H
#define RENDERONLY_RESOURCE_H

#include "pipe/p_state.h"

struct winsys_handle;

struct renderonly_resource {
	struct pipe_resource base;
	struct pipe_resource *gpu;

	bool scanout;
	struct pipe_resource *prime;

	uint32_t stride;
	uint32_t handle;
	size_t size;
};

static inline struct renderonly_resource *
to_renderonly_resource(struct pipe_resource *resource)
{
	return (struct renderonly_resource *)resource;
}

static inline struct pipe_resource *
renderonly_resource_unwrap(struct pipe_resource *resource)
{
	if (!resource)
		return NULL;

	return to_renderonly_resource(resource)->gpu;
}

boolean renderonly_can_create_resource(struct pipe_screen *pscreen,
				   const struct pipe_resource *template);
struct pipe_resource *
renderonly_resource_create(struct pipe_screen *pscreen,
		      const struct pipe_resource *template);
struct pipe_resource *
renderonly_resource_from_handle(struct pipe_screen *pscreen,
			   const struct pipe_resource *template,
			   struct winsys_handle *handle,
			   unsigned usage);
boolean
renderonly_resource_get_handle(struct pipe_screen *pscreen,
			  struct pipe_resource *resource,
			  struct winsys_handle *handle,
			  unsigned usage);
void
renderonly_resource_destroy(struct pipe_screen *pscreen,
		       struct pipe_resource *resource);

struct renderonly_surface {
	struct pipe_surface base;
	struct pipe_surface *gpu;
};

static inline struct renderonly_surface *
to_renderonly_surface(struct pipe_surface *psurf)
{
	return (struct renderonly_surface *)psurf;
}

static inline struct pipe_surface *
renderonly_surface_unwrap(struct pipe_surface *psurf)
{
	if (!psurf)
		return NULL;

	return to_renderonly_surface(psurf)->gpu;
}

struct pipe_surface *
renderonly_create_surface(struct pipe_context *pctx,
		     struct pipe_resource *prsc,
		     const struct pipe_surface *template);
void
renderonly_surface_destroy(struct pipe_context *pctx,
		      struct pipe_surface *psurf);

#endif /* RENDERONLY_RESOURCE_H */
