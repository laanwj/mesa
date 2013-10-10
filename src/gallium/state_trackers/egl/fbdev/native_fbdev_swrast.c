/*
 * Mesa 3-D graphics library
 * Version:  7.9
 *
 * Copyright (C) 2013 Mesa developers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Wladimir van der Laan <laanwj@gmail.com>
 */

#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>

#include "pipe/p_screen.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_pointer.h"
#include "os/os_thread.h"

#include "common/native.h"
#include "common/native_helper.h"

#include "fbdev/native_fbdev.h"
#include "fbdev/native_fbdev_swrast.h"
#include "fbdev/fbdev_sw_winsys.h"

struct swrast_fbdev_screen
{
   struct native_fbdev_screen base;
   int fd; /* fbdev fd used to create fbdev_sw_winsys */
};

static inline struct swrast_fbdev_screen *swrast_fbdev_screen(struct native_fbdev_screen *fbdev_screen)
{
   return (struct swrast_fbdev_screen*)fbdev_screen;
}

static void swrast_fbdev_screen_destroy(struct native_fbdev_screen *fbdev_screen)
{
   FREE(fbdev_screen);
}

static void *swrast_fbdev_create_drawable(struct native_fbdev_screen *hfbdev_screen,
          int fd,
          unsigned xoffset, unsigned yoffset,
          unsigned width, unsigned height)
{
   struct swrast_fbdev_screen *fbdev_screen = swrast_fbdev_screen(hfbdev_screen);
   struct fbdev_sw_drawable *drawable = CALLOC_STRUCT(fbdev_sw_drawable);
   struct fb_var_screeninfo vinfo;
   struct fb_fix_screeninfo finfo;

   if (fd != fbdev_screen->fd)
      return NULL;
   if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo))
      return NULL;
   if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo))
      return NULL;

   /* sanitize the values */
   if (xoffset + width > vinfo.xres_virtual) {
      if (xoffset > vinfo.xres_virtual)
         width = 0;
      else
         width = vinfo.xres_virtual - xoffset;
   }
   if (yoffset + height > vinfo.yres_virtual) {
      if (yoffset > vinfo.yres_virtual)
         height = 0;
      else
         height = vinfo.yres_virtual - yoffset;
   }

   drawable->format = vinfo_to_format(&vinfo);
   drawable->x = xoffset;
   drawable->y = yoffset;
   drawable->width = width;
   drawable->height = height;

   if(drawable->format == PIPE_FORMAT_NONE ||
      !drawable->width || !drawable->height)
   {
      FREE(drawable);
      return NULL;
   }

   return drawable;
}

static void swrast_fbdev_destroy_drawable(struct native_fbdev_screen *fbdev_screen, void *hdrawable)
{
   FREE(hdrawable);
}

static struct pipe_fence_handle *swrast_fbdev_get_drawable_fence(struct native_fbdev_screen *fbdev_screen, void *hdrawable)
{
   return NULL;
}

static bool swrast_fbdev_probe(const struct native_fbdev_driver *driver)
{
   return true;
}

static struct native_fbdev_screen *swrast_fbdev_create_screen(
                  const struct native_fbdev_driver *driver,
                  int fd,
                  struct native_display *display,
                  const struct native_event_handler *event_handler)
{
   struct swrast_fbdev_screen *fscreen = CALLOC_STRUCT(swrast_fbdev_screen);
   struct sw_winsys *ws;

   ws = fbdev_create_sw_winsys(fd);
   if (!ws)
      return FALSE;

   fscreen->fd = fd;
   fscreen->base.screen = event_handler->new_sw_screen(display, ws);
   if(!fscreen->base.screen)
   {
      FREE(fscreen);
      return NULL;
   }

   fscreen->base.destroy = swrast_fbdev_screen_destroy;
   fscreen->base.create_drawable = swrast_fbdev_create_drawable;
   fscreen->base.destroy_drawable = swrast_fbdev_destroy_drawable;
   fscreen->base.get_drawable_fence = swrast_fbdev_get_drawable_fence;
   return &fscreen->base;
}

static const struct native_fbdev_driver fbdev_driver = {
   "swrast", /* name */
   swrast_fbdev_probe,
   swrast_fbdev_create_screen
};

const struct native_fbdev_driver *swrast_fbdev_get_driver(void)
{
   return &fbdev_driver;
}
