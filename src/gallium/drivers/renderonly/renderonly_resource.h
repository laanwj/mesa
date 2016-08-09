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
to_renderonly_resource(struct pipe_resource *prsc)
{
	return (struct renderonly_resource *)prsc;
}

static inline struct pipe_resource *
renderonly_resource_unwrap(struct pipe_resource *prsc)
{
	if (!prsc)
		return NULL;

	return to_renderonly_resource(prsc)->gpu;
}

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

void
renderonly_resource_screen_init(struct pipe_screen *pscreen);

void
renderonly_resource_context_init(struct pipe_context *pctx);


#endif /* RENDERONLY_RESOURCE_H */
