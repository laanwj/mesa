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

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>

#include <xf86drm.h>

#include "util/u_string.h"
#include "util/u_debug.h"
#include "util/u_memory.h"

#include "renderonly_context.h"
#include "renderonly_resource.h"
#include "renderonly_screen.h"

static const char *
renderonly_get_name(struct pipe_screen *pscreen)
{
	static char buffer[256];
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);

	util_snprintf(buffer, sizeof(buffer), "%s-%s",
			drmGetDeviceNameFromFd(screen->fd),
			screen->gpu->get_name(screen->gpu));
	return buffer;
}

static const char *
renderonly_get_vendor(struct pipe_screen *pscreen)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);

	return screen->gpu->get_vendor(screen->gpu);
}

static const char *
renderonly_get_device_vendor(struct pipe_screen *pscreen)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);

	return screen->gpu->get_device_vendor(screen->gpu);
}

static void renderonly_screen_destroy(struct pipe_screen *pscreen)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);

	screen->gpu->destroy(screen->gpu);
	FREE(pscreen);
}

static int
renderonly_screen_get_param(struct pipe_screen *pscreen, enum pipe_cap param)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);

	return screen->gpu->get_param(screen->gpu, param);
}

static float
renderonly_screen_get_paramf(struct pipe_screen *pscreen, enum pipe_capf param)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);

	return screen->gpu->get_paramf(screen->gpu, param);
}

static int
renderonly_screen_get_shader_param(struct pipe_screen *pscreen,
			      unsigned shader,
			      enum pipe_shader_cap param)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);

	return screen->gpu->get_shader_param(screen->gpu, shader, param);
}

static uint64_t
renderonly_screen_get_timestamp(struct pipe_screen *pscreen)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);

	return screen->gpu->get_timestamp(screen->gpu);
}

static boolean
renderonly_screen_is_format_supported(struct pipe_screen *pscreen,
				 enum pipe_format format,
				 enum pipe_texture_target target,
				 unsigned sample_count,
				 unsigned usage)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);

	return screen->gpu->is_format_supported(screen->gpu, format, target,
						sample_count, usage);
}

static void
renderonly_fence_reference(struct pipe_screen *pscreen,
		      struct pipe_fence_handle **ptr,
		      struct pipe_fence_handle *fence)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);

	screen->gpu->fence_reference(screen->gpu, ptr, fence);
}

static boolean
renderonly_fence_finish(struct pipe_screen *pscreen,
		   struct pipe_fence_handle *fence,
		   uint64_t timeout)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);

	return screen->gpu->fence_finish(screen->gpu, fence, timeout);
}

static int
renderonly_get_driver_query_info(struct pipe_screen *pscreen,
		   unsigned index, struct pipe_driver_query_info *info)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);

	return screen->gpu->get_driver_query_info(screen->gpu, index, info);
}

struct pipe_screen *
renderonly_screen_create(int fd, const struct renderonly_ops *ops)
{
	struct renderonly_screen *screen;

	screen = CALLOC_STRUCT(renderonly_screen);
	if (!screen)
		return NULL;

	screen->fd = fd;
	screen->ops = ops;
	assert(screen->ops);

	assert(screen->ops->open);
	screen->gpu = screen->ops->open(fd);
	if (!screen->gpu) {
		fprintf(stderr, "failed to create GPU screen\n");
		FREE(screen);
		return NULL;
	}

	screen->base.get_name = renderonly_get_name;
	screen->base.get_vendor = renderonly_get_vendor;
	screen->base.get_device_vendor = renderonly_get_device_vendor;
	screen->base.destroy = renderonly_screen_destroy;
	screen->base.get_param = renderonly_screen_get_param;
	screen->base.get_paramf = renderonly_screen_get_paramf;
	screen->base.get_shader_param = renderonly_screen_get_shader_param;
	screen->base.get_timestamp = renderonly_screen_get_timestamp;
	screen->base.context_create = renderonly_context_create;
	screen->base.is_format_supported = renderonly_screen_is_format_supported;

	screen->base.fence_reference = renderonly_fence_reference;
	screen->base.fence_finish = renderonly_fence_finish;

	if (screen->gpu->get_driver_query_info)
	    screen->base.get_driver_query_info = renderonly_get_driver_query_info;

	if (ops->intermediate_rendering)
		screen->base.flush_frontbuffer = NULL; /* TODO */

	renderonly_resource_screen_init(&screen->base);

	return &screen->base;
}
