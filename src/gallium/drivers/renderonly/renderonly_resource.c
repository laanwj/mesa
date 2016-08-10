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
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include <xf86drm.h>

#include "pipe/p_state.h"
#include "util/u_debug.h"
#include "util/u_format.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "state_tracker/drm_driver.h"

#include "renderonly_context.h"
#include "renderonly_resource.h"
#include "renderonly_screen.h"

static boolean
renderonly_can_create_resource(struct pipe_screen *pscreen,
				   const struct pipe_resource *template)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);

	return screen->gpu->can_create_resource(screen->gpu, template);
}

static bool resource_import_scanout(struct renderonly_screen *screen,
		     struct renderonly_resource *rsc,
		     const struct pipe_resource *template)
{
	boolean status;
	int fd, err;
	struct winsys_handle handle = {
	      .handle = DRM_API_HANDLE_TYPE_FD
	};

	rsc->gpu = screen->gpu->resource_create(screen->gpu,
							     template);
	if (!rsc->gpu)
		return false;

	status = screen->gpu->resource_get_handle(screen->gpu,
							  rsc->gpu,
							  &handle,
							  PIPE_HANDLE_USAGE_READ_WRITE);
	if (!status)
		goto out_free_gpu_resource;

	rsc->stride = handle.stride;
	fd = handle.handle;

	err = drmPrimeFDToHandle(screen->fd, fd, &rsc->handle);
	close(fd);

	if (err < 0) {
		fprintf(stderr, "drmPrimeFDToHandle() failed: %s\n",
			strerror(errno));
		goto out_free_gpu_resource;
	}

	if (screen->ops->tiling) {
		err = screen->ops->tiling(screen->fd, rsc->handle);
		if (err < 0) {
			fprintf(stderr, "failed to set tiling parameters: %s\n",
				strerror(errno));
			goto out_free_gpu_resource;
		}
	}

	return true;

out_free_gpu_resource:
	screen->gpu->resource_destroy(screen->gpu, rsc->gpu);

	return false;
}

static bool resource_dumb(struct renderonly_screen *screen,
		     struct renderonly_resource *rsc,
		     const struct pipe_resource *template)
{
	struct winsys_handle handle;
	int prime_fd, err;
	struct drm_mode_create_dumb create_dumb = {
	      .width = template->width0,
	      .height = template->height0,
	      .bpp = 32,
	};
	struct drm_mode_destroy_dumb destroy_dumb = { };

	/* create dumb buffer at scanout GPU */
	err = ioctl(screen->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
	if (err < 0) {
		fprintf(stderr, "DRM_IOCTL_MODE_CREATE_DUMB failed: %s\n",
			strerror(errno));
		return false;
	}

	rsc->handle = create_dumb.handle;
	rsc->stride = create_dumb.pitch;

	/* create resource at renderonly GPU */
	rsc->gpu = screen->gpu->resource_create(screen->gpu, template);
	if (!rsc->gpu)
		goto out_free_dump;

	/* export dumb buffer */
	err = drmPrimeHandleToFD(screen->fd, create_dumb.handle, O_CLOEXEC, &prime_fd);
	if (err < 0) {
		fprintf(stderr, "failed to export dumb buffer: %s\n",
			strerror(errno));
		goto out_free_gpu_resource;
	}

	/* import dumb buffer */
	handle.type = DRM_API_HANDLE_TYPE_FD;
	handle.handle = prime_fd;
	handle.stride = create_dumb.pitch;

	rsc->prime = screen->gpu->resource_from_handle(screen->gpu,
							  template,
							  &handle,
							  PIPE_HANDLE_USAGE_READ_WRITE);

	if (!rsc->prime) {
		fprintf(stderr, "failed to create resource_from_handle: %s\n",
			strerror(errno));
		goto out_free_gpu_resource;
	}

	return true;

out_free_gpu_resource:
	screen->gpu->resource_destroy(screen->gpu, rsc->gpu);

out_free_dump:
	destroy_dumb.handle = rsc->handle;
	ioctl(screen->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);

	return false;
}

static struct pipe_resource *
renderonly_resource_create(struct pipe_screen *pscreen,
		      const struct pipe_resource *template)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);
	struct renderonly_resource *rsc;

	rsc = CALLOC_STRUCT(renderonly_resource);
	if (!rsc)
		return NULL;

	if (template->bind & PIPE_BIND_SCANOUT) {

		bool success = false;

		if (!screen->ops->intermediate_rendering) {
			/* create scanout resource in renderonly GPU, export it
			 * and import it into the scanout hardware. If defined
			 * tiling will be setup for the crated resource. */
			success = resource_import_scanout(screen, rsc, template);
		} else {
			/* create dump buffer in scanout hardware, export it
			 * and import it into renderonly GPU. */
			success = resource_dumb(screen, rsc, template);
		}

		if (!success)
			goto destroy;

		rsc->scanout = true;

	} else {
		rsc->gpu = screen->gpu->resource_create(screen->gpu,
							     template);
		if (!rsc->gpu)
			goto destroy;
	}

	memcpy(&rsc->base, rsc->gpu, sizeof(*rsc->gpu));
	pipe_reference_init(&rsc->base.reference, 1);
	rsc->base.screen = &screen->base;

	return &rsc->base;

destroy:
	if (rsc->gpu)
		screen->gpu->resource_destroy(screen->gpu, rsc->gpu);
	FREE(rsc);
	return NULL;
}

static struct pipe_resource *
renderonly_resource_from_handle(struct pipe_screen *pscreen,
			   const struct pipe_resource *template,
			   struct winsys_handle *handle,
			   unsigned usage)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);
	struct renderonly_resource *rsc;

	rsc = CALLOC_STRUCT(renderonly_resource);
	if (!rsc)
		return NULL;

	if (handle->type == DRM_API_HANDLE_TYPE_SHARED &&
	    template->bind & PIPE_BIND_RENDER_TARGET) {
		/* Render targets are linear on Xorg but must be tiled
		 * here. It would be nice if dri_drawable_get_format()
		 * set scanout for these buffers too.
		 */
		rsc->gpu = screen->gpu->resource_create(screen->gpu,
							     template);
		if (!rsc->gpu) {
			FREE(rsc);
			return false;
		}

		rsc->prime = screen->gpu->resource_from_handle(screen->gpu,
								    template,
								    handle,
								    PIPE_HANDLE_USAGE_READ_WRITE);
		if (!rsc->prime) {
			screen->gpu->resource_destroy(screen->gpu, rsc->gpu);
			FREE(rsc);
			return false;
		}

		rsc->scanout = true;
	} else {
		rsc->gpu = screen->gpu->resource_from_handle(screen->gpu,
								  template,
								  handle,
								  PIPE_HANDLE_USAGE_READ_WRITE);
	}

	if (!rsc->gpu) {
		FREE(rsc);
		return NULL;
	}

	memcpy(&rsc->base, rsc->gpu, sizeof(*rsc->gpu));
	pipe_reference_init(&rsc->base.reference, 1);
	rsc->base.screen = &screen->base;

	return &rsc->base;
}

static boolean
renderonly_resource_get_handle(struct pipe_screen *pscreen,
			  struct pipe_resource *prsc,
			  struct winsys_handle *handle,
			  unsigned usage)
{
	struct renderonly_resource *rsc = to_renderonly_resource(prsc);
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);
	boolean ret = TRUE;

	if (prsc->bind & PIPE_BIND_SCANOUT) {
		handle->handle = rsc->handle;
		handle->stride = rsc->stride;
	} else {
		ret = screen->gpu->resource_get_handle(screen->gpu,
						       rsc->gpu,
						       handle,
						       usage);
	}

	return ret;
}

static void
renderonly_resource_destroy(struct pipe_screen *pscreen,
		       struct pipe_resource *prsc)
{
	struct renderonly_resource *rsc = to_renderonly_resource(prsc);

	pipe_resource_reference(&rsc->gpu, NULL);
	pipe_resource_reference(&rsc->prime, NULL);
	FREE(rsc);
}

static struct pipe_surface *
renderonly_create_surface(struct pipe_context *pctx,
		     struct pipe_resource *prsc,
		     const struct pipe_surface *template)
{
	struct renderonly_resource *rsc = to_renderonly_resource(prsc);
	struct renderonly_context *ctx = to_renderonly_context(pctx);
	struct renderonly_surface *surface;

	surface = CALLOC_STRUCT(renderonly_surface);
	if (!surface)
		return NULL;

	surface->gpu = ctx->gpu->create_surface(ctx->gpu,
						    rsc->gpu,
						    template);
	if (!surface->gpu) {
		FREE(surface);
		return NULL;
	}

	memcpy(&surface->base, surface->gpu, sizeof(*surface->gpu));
	/* overwrite to prevent reference from being released */
	surface->base.texture = NULL;

	pipe_reference_init(&surface->base.reference, 1);
	pipe_resource_reference(&surface->base.texture, prsc);
	surface->base.context = &ctx->base;

	return &surface->base;
}

static void
renderonly_surface_destroy(struct pipe_context *pctx,
		      struct pipe_surface *psurf)
{
	struct renderonly_surface *surface = to_renderonly_surface(psurf);

	pipe_resource_reference(&surface->base.texture, NULL);
	pipe_surface_reference(&surface->gpu, NULL);
	FREE(surface);
}

void
renderonly_resource_screen_init(struct pipe_screen *pscreen)
{
   pscreen->can_create_resource = renderonly_can_create_resource;
   pscreen->resource_create = renderonly_resource_create;
   pscreen->resource_from_handle = renderonly_resource_from_handle;
   pscreen->resource_get_handle = renderonly_resource_get_handle;
   pscreen->resource_destroy = renderonly_resource_destroy;
}

void
renderonly_resource_context_init(struct pipe_context *pctx)
{
   pctx->create_surface = renderonly_create_surface;
   pctx->surface_destroy = renderonly_surface_destroy;
}
