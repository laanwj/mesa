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

#include "state_tracker/drm_driver.h"

#include "renderonly_context.h"
#include "renderonly_resource.h"
#include "renderonly_screen.h"

boolean renderonly_can_create_resource(struct pipe_screen *pscreen,
				   const struct pipe_resource *template)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);

	return screen->gpu->can_create_resource(screen->gpu, template);
}

static bool resource_import_scanout(struct renderonly_screen *screen,
		     struct renderonly_resource *resource,
		     const struct pipe_resource *template)
{
	struct winsys_handle handle;
	boolean status;
	int fd, err;

	resource->gpu = screen->gpu->resource_create(screen->gpu,
							     template);
	if (!resource->gpu)
		return false;

	memset(&handle, 0, sizeof(handle));
	handle.type = DRM_API_HANDLE_TYPE_FD;

	status = screen->gpu->resource_get_handle(screen->gpu,
							  resource->gpu,
							  &handle);
	if (!status)
		return false;

	resource->stride = handle.stride;
	fd = handle.handle;

	err = drmPrimeFDToHandle(screen->fd, fd, &resource->handle);
	if (err < 0) {
		fprintf(stderr, "drmPrimeFDToHandle() failed: %s\n",
			strerror(errno));
		close(fd);
		return false;
	}

	close(fd);

	if (screen->ops->tiling) {
		err = screen->ops->tiling(screen->fd, resource->handle);
		if (err < 0) {
			fprintf(stderr, "failed to set tiling parameters: %s\n",
				strerror(errno));
			return false;
		}
	}

	return true;
}

static bool resource_dumb(struct renderonly_screen *screen,
		     struct renderonly_resource *resource,
		     const struct pipe_resource *template)
{
	struct drm_mode_create_dumb create_dumb = { 0 };
	struct winsys_handle handle;
	int prime_fd, err;

	/* create dumb buffer at scanout GPU */
	create_dumb.width = template->width0;
	create_dumb.height = template->height0;
	create_dumb.bpp = 32;
	create_dumb.flags = 0;
	create_dumb.pitch = 0;
	create_dumb.size = 0;
	create_dumb.handle = 0;

	err = ioctl(screen->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
	if (err < 0) {
		fprintf(stderr, "DRM_IOCTL_MODE_CREATE_DUMB failed: %s\n",
			strerror(errno));
		return false;
	}

	resource->handle = create_dumb.handle;
	resource->stride = create_dumb.pitch;

	/* create resource at renderonly GPU */
	resource->gpu = screen->gpu->resource_create(screen->gpu, template);
	if (!resource->gpu)
		return false;

	/* export dumb buffer */
	err = drmPrimeHandleToFD(screen->fd, create_dumb.handle, O_CLOEXEC, &prime_fd);
	if (err < 0) {
		fprintf(stderr, "failed to export dumb buffer: %s\n",
			strerror(errno));
		return false;
	}

	/* import dumb buffer */
	handle.type = DRM_API_HANDLE_TYPE_FD;
	handle.handle = prime_fd;
	handle.stride = create_dumb.pitch;

	resource->prime = screen->gpu->resource_from_handle(screen->gpu, template, &handle);
	if (!resource->prime) {
		fprintf(stderr, "failed to create resource_from_handle: %s\n",
			strerror(errno));
		return false;
	}

	return true;
}

struct pipe_resource *
renderonly_resource_create(struct pipe_screen *pscreen,
		      const struct pipe_resource *template)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);
	struct renderonly_resource *resource;

	resource = calloc(1, sizeof(*resource));
	if (!resource)
		return NULL;

	if (template->bind & PIPE_BIND_SCANOUT) {

		bool success = false;

		if (!screen->ops->intermediate_rendering) {
			/* create scanout resource in renderonly GPU, export it
			 * and import it into the scanout hardware. If defined
			 * tiling will be setup for the crated resource. */
			success = resource_import_scanout(screen, resource, template);
		} else {
			/* create dump buffer in scanout hardware, export it
			 * and import it into renderonly GPU. */
			success = resource_dumb(screen, resource, template);
		}

		if (!success)
			goto destroy;

		resource->scanout = true;

	} else {
		resource->gpu = screen->gpu->resource_create(screen->gpu,
							     template);
		if (!resource->gpu)
			goto destroy;
	}

	memcpy(&resource->base, resource->gpu, sizeof(*resource->gpu));
	pipe_reference_init(&resource->base.reference, 1);
	resource->base.screen = &screen->base;

	return &resource->base;

destroy:
	if (resource->gpu)
		screen->gpu->resource_destroy(screen->gpu, resource->gpu);
	free(resource);
	return NULL;
}

struct pipe_resource *
renderonly_resource_from_handle(struct pipe_screen *pscreen,
			   const struct pipe_resource *template,
			   struct winsys_handle *handle,
			   unsigned usage)
{
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);
	struct renderonly_resource *resource;

	resource = calloc(1, sizeof(*resource));
	if (!resource)
		return NULL;

	if (handle->type == DRM_API_HANDLE_TYPE_SHARED &&
	    template->bind & PIPE_BIND_RENDER_TARGET) {
		/* Render targets are linear on Xorg but must be tiled
		 * here. It would be nice if dri_drawable_get_format()
		 * set scanout for these buffers too.
		 */
		resource->gpu = screen->gpu->resource_create(screen->gpu,
							     template);
		if (!resource->gpu) {
			free(resource);
			return false;
		}

		resource->prime = screen->gpu->resource_from_handle(screen->gpu,
								    template,
								    handle);
		if (!resource->prime) {
			screen->gpu->resource_destroy(screen->gpu, resource->gpu);
			free(resource);
			return false;
		}

		resource->scanout = true;
	} else {
		resource->gpu = screen->gpu->resource_from_handle(screen->gpu,
								  template,
								  handle);
	}

	if (!resource->gpu) {
		free(resource);
		return NULL;
	}

	memcpy(&resource->base, resource->gpu, sizeof(*resource->gpu));
	pipe_reference_init(&resource->base.reference, 1);
	resource->base.screen = &screen->base;

	return &resource->base;
}

boolean
renderonly_resource_get_handle(struct pipe_screen *pscreen,
			  struct pipe_resource *presource,
			  struct winsys_handle *handle,
			  unsigned usage)
{
	struct renderonly_resource *resource = to_renderonly_resource(presource);
	struct renderonly_screen *screen = to_renderonly_screen(pscreen);
	boolean ret = TRUE;

	if (presource->bind & PIPE_BIND_SCANOUT) {
		handle->handle = resource->handle;
		handle->stride = resource->stride;
	} else {
		ret = screen->gpu->resource_get_handle(screen->gpu,
						       resource->gpu,
						       handle,
						       usage);
	}

	return ret;
}

void
renderonly_resource_destroy(struct pipe_screen *pscreen,
		       struct pipe_resource *presource)
{
	struct renderonly_resource *resource = to_renderonly_resource(presource);

	pipe_resource_reference(&resource->gpu, NULL);
	pipe_resource_reference(&resource->prime, NULL);
	free(resource);
}

struct pipe_surface *
renderonly_create_surface(struct pipe_context *pcontext,
		     struct pipe_resource *presource,
		     const struct pipe_surface *template)
{
	struct renderonly_resource *resource = to_renderonly_resource(presource);
	struct renderonly_context *context = to_renderonly_context(pcontext);
	struct renderonly_surface *surface;

	surface = calloc(1, sizeof(*surface));
	if (!surface)
		return NULL;

	surface->gpu = context->gpu->create_surface(context->gpu,
						    resource->gpu,
						    template);
	if (!surface->gpu) {
		free(surface);
		return NULL;
	}

	memcpy(&surface->base, surface->gpu, sizeof(*surface->gpu));
	/* overwrite to prevent reference from being released */
	surface->base.texture = NULL;

	pipe_reference_init(&surface->base.reference, 1);
	pipe_resource_reference(&surface->base.texture, presource);
	surface->base.context = &context->base;

	return &surface->base;
}

void
renderonly_surface_destroy(struct pipe_context *pcontext,
		      struct pipe_surface *psurface)
{
	struct renderonly_surface *surface = to_renderonly_surface(psurface);

	pipe_resource_reference(&surface->base.texture, NULL);
	pipe_surface_reference(&surface->gpu, NULL);
	free(surface);
}
