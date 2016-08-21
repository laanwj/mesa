/*
 * Copyright (c) 2015 Etnaviv Project
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
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#include "etnaviv/etnaviv_screen.h"
#include "etnaviv_drm_public.h"

#include <fcntl.h>
#include <stdio.h>

static struct pipe_screen *
etna_drm_screen_create_fd(int fd)
{
   struct etna_device *dev;
   struct etna_gpu *gpu;
   uint64_t val;
   int i;

   dev = etna_device_new(fd);
   if (!dev) {
      fprintf(stderr, "Error creating device\n");
      return NULL;
   }

   for (i = 0;; i++) {
      gpu = etna_gpu_new(dev, i);
      if (!gpu) {
         fprintf(stderr, "Error creating gpu\n");
         return NULL;
      }

      /* Look for a 3D capable GPU */
      if (etna_gpu_get_param(gpu, ETNA_GPU_FEATURES_0, &val) == 0 &&
          val & (1 << 2))
         break;

      etna_gpu_del(gpu);
   }

   return etna_screen_create(dev, gpu);
}

struct pipe_screen *
etna_drm_screen_create_renderer(int fd)
{
   struct pipe_screen *screen;
   boolean cleanup_fd = FALSE;

   if (fd < 0) {
      fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
      if (fd == -1)
         return NULL;
      cleanup_fd = TRUE;
   }

   screen = etna_drm_screen_create_fd(fd);
   if (!screen) {
      if (cleanup_fd)
         close(fd);
      return NULL;
   }

   return screen;
}

struct pipe_screen *
etna_drm_screen_create(int fd)
{
   return NULL;
}
