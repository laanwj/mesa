#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/u_format.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"

#include "common/native.h"

#include "etna_fbdev_public.h"

#include "etna/etna_screen.h"

#include <etnaviv/viv.h>
#include <etnaviv/etna_bo.h>
#include <etnaviv/etna_fb.h>

#include <stdio.h>
#include <linux/fb.h>
#include <sys/ioctl.h>

struct etna_fbdev_screen
{
   struct native_fbdev_screen base;
   /* anything? */
};

static inline struct etna_fbdev_screen *etna_fbdev_screen(struct native_fbdev_screen *fbdev_screen)
{
   return (struct etna_fbdev_screen*)fbdev_screen;
}

static void etna_fbdev_screen_destroy(struct native_fbdev_screen *fbdev_screen)
{
   FREE(fbdev_screen);
}

static void *etna_fbdev_create_drawable(struct native_fbdev_screen *fbdev_screen,
          int fd,
          unsigned xoffset, unsigned yoffset,
          unsigned width, unsigned height)
{
   struct etna_rs_target *drawable = CALLOC_STRUCT(etna_rs_target);
   struct fb_var_screeninfo vinfo;
   struct fb_fix_screeninfo finfo;
   struct etna_screen *screen = etna_screen(fbdev_screen->screen);

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

   drawable->width = width;
   drawable->height = height;
   drawable->bo = etna_bo_from_fbdev(screen->dev, fd,
      finfo.line_length * yoffset + vinfo.bits_per_pixel / 8 * xoffset,
      finfo.line_length * height);
   drawable->stride = finfo.line_length;

   if(width == 0 || height == 0 ||
      !etna_fb_get_format(&vinfo, &drawable->rs_format, &drawable->swap_rb))
   {
      FREE(drawable);
      return NULL;
   }
   return drawable;
}

static void etna_fbdev_destroy_drawable(struct native_fbdev_screen *fbdev_screen, void *hdrawable)
{
   struct etna_rs_target *drawable = (struct etna_rs_target*)hdrawable;
   fbdev_screen->screen->fence_reference(fbdev_screen->screen, &drawable->fence, NULL);
   FREE(drawable);
}

static struct pipe_fence_handle *etna_fbdev_get_drawable_fence(struct native_fbdev_screen *fbdev_screen, void *hdrawable)
{
   struct etna_rs_target *drawable = (struct etna_rs_target*)hdrawable;
   return (struct pipe_fence_handle*)drawable->fence;
}

static bool etna_fbdev_probe(const struct native_fbdev_driver *driver)
{
   return true;
}

static struct native_fbdev_screen *etna_fbdev_create_screen(
                  const struct native_fbdev_driver *driver,
                  int fd,
                  struct native_display *display,
                  const struct native_event_handler *event_handler)
{
   struct etna_fbdev_screen *fscreen = CALLOC_STRUCT(etna_fbdev_screen);

   fscreen->base.screen = event_handler->new_drm_screen(display, "etna", -1);
   if(!fscreen->base.screen)
   {
      FREE(fscreen);
      return NULL;
   }

   fscreen->base.destroy = etna_fbdev_screen_destroy;
   fscreen->base.create_drawable = etna_fbdev_create_drawable;
   fscreen->base.destroy_drawable = etna_fbdev_destroy_drawable;
   fscreen->base.get_drawable_fence = etna_fbdev_get_drawable_fence;
   return &fscreen->base;
}

static const struct native_fbdev_driver fbdev_driver = {
   "etna", /* name */
   etna_fbdev_probe,
   etna_fbdev_create_screen
};

const struct native_fbdev_driver *etna_fbdev_get_driver(void)
{
   return &fbdev_driver;
}
