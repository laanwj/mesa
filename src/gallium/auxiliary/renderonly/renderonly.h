/*
 * Copyright (C) 2016 Christian Gmeiner <christian.gmeiner@gmail.com>
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#ifndef RENDERONLY_H
#define RENDERONLY_H

#include <stdint.h>
#include "state_tracker/drm_driver.h"
#include "pipe/p_state.h"

struct renderonly;

struct renderonly_ops {
   struct pipe_screen *(*create)(struct renderonly *ctx);
   int (*tiling)(int fd, uint32_t handle);
   bool intermediate_renderong;
};

struct renderonly {
   int kms_fd;
   const struct renderonly_ops *ops;
   struct pipe_screen *screen;
   void *priv;
};

struct pipe_screen *
renderonly_screen_create(int fd, const struct renderonly_ops *ops, void *priv);

struct renderonly_scanout {
   uint32_t handle;
   uint32_t stride;

   struct pipe_resource *prime;
};

struct renderonly_scanout *
renderonly_scanout_for_resource(struct pipe_resource *rsc, struct renderonly *ro);

struct renderonly_scanout *
renderonly_scanout_for_prime(struct pipe_resource *rsc, struct renderonly *ro);

void
renderonly_scanout_destroy(struct renderonly_scanout *scanout);

static inline boolean
renderonly_get_handle(struct renderonly_scanout *scanout,
      struct winsys_handle *handle)
{
   if (!scanout)
      return FALSE;

   handle->handle = scanout->handle;
   handle->stride = scanout->stride;

   return TRUE;
}

#endif /* RENDERONLY_H_ */
