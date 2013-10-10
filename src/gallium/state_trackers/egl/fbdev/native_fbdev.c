/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2010 LunarG Inc.
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
 *    Chia-I Wu <olv@lunarg.com>
 */

/**
 * Considering fbdev as an in-kernel window system,
 *
 *  - opening a device opens a connection
 *  - there is only one window: the framebuffer
 *  - fb_var_screeninfo decides window position, size, and even color format
 *  - there is no pixmap
 *
 * Now EGL is built on top of this window system.  So we should have
 *
 *  - the fd as the handle of the native display
 *  - reject all but one native window: NULL
 *  - no pixmap support
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

#define ETNA  /* Use ETNA instead of swrast driver */

#include "fbdev/native_fbdev_swrast.h"
#ifdef ETNA
#include "etna_fbdev_public.h"
#endif

#define FBDEV_MAX_BUFFERS (2) /* double buffering */

struct fbdev_display {
   struct native_display base;

   int fd;
   const struct native_event_handler *event_handler;

   struct fb_fix_screeninfo finfo;
   struct fb_var_screeninfo config_vinfo;
   struct native_config config;
   struct native_fbdev_screen *fbdev_screen;

   boolean assume_fixed_vinfo;
};

struct fbdev_surface {
   struct native_surface base;

   struct fbdev_display *fbdpy;
   struct resource_surface *rsurf;
   int width, height;
   int num_buffers;
   int swap_interval;
   struct fb_var_screeninfo vinfo;

   unsigned int sequence_number;

   /* For Android-style double/triple buffering */
   volatile bool terminate; /* terminate flag for buffer swap thread */
   int buffer_head; /* next buffer to be shown */
   int buffer_tail; /* next buffer to be posted */
   int posted_buffers; /* number of posted buffers */
   pipe_thread bswap_thread;
   pipe_mutex buffer_mutex;
   pipe_condvar buffer_available; /* condition if buffer available for use */
   pipe_condvar buffer_posted; /* condition if buffer posted */
   void *drawable[FBDEV_MAX_BUFFERS]; /* drawable for buffers */
};

static INLINE struct fbdev_display *
fbdev_display(const struct native_display *ndpy)
{
   return (struct fbdev_display *) ndpy;
}

static INLINE struct fbdev_surface *
fbdev_surface(const struct native_surface *nsurf)
{
   return (struct fbdev_surface *) nsurf;
}

static boolean
fbdev_surface_validate(struct native_surface *nsurf, uint attachment_mask,
                     unsigned int *seq_num, struct pipe_resource **textures,
                     int *width, int *height)
{
   struct fbdev_surface *fbsurf = fbdev_surface(nsurf);

   if (!resource_surface_add_resources(fbsurf->rsurf, attachment_mask))
      return FALSE;
   if (textures)
      resource_surface_get_resources(fbsurf->rsurf, textures, attachment_mask);

   if (seq_num)
      *seq_num = fbsurf->sequence_number;
   if (width)
      *width = fbsurf->width;
   if (height)
      *height = fbsurf->height;

   return TRUE;
}

enum pipe_format
vinfo_to_format(const struct fb_var_screeninfo *vinfo)
{
   enum pipe_format format = PIPE_FORMAT_NONE;

   /* should also check channel offsets... */
   switch (vinfo->bits_per_pixel) {
   case 32:
      if (vinfo->red.length == 8 &&
          vinfo->green.length == 8 &&
          vinfo->blue.length == 8) {
         format = (vinfo->transp.length == 8) ?
            PIPE_FORMAT_B8G8R8A8_UNORM : PIPE_FORMAT_B8G8R8X8_UNORM;
      }
      break;
   case 16:
      if (vinfo->red.length == 5 &&
          vinfo->green.length == 6 &&
          vinfo->blue.length == 5 &&
          vinfo->transp.length == 0)
         format = PIPE_FORMAT_B5G6R5_UNORM;
      if (vinfo->red.length == 5 &&
          vinfo->green.length == 5 &&
          vinfo->blue.length == 5) {
         format = (vinfo->transp.length == 1) ?
            PIPE_FORMAT_B5G5R5A1_UNORM : PIPE_FORMAT_B5G5R5X1_UNORM;
      }
      break;
   default:
      break;
   }

   return format;
}

/* Set currently visible buffer id */
static int fbdev_set_buffer(struct fbdev_surface *fbsurf, int buffer)
{
    assert(buffer < fbsurf->num_buffers);
    /* Is this supposed to wait for vblank or just postpone the operation asynchronously?
     * This assumes the former.
     */
    fbsurf->vinfo.activate = fbsurf->swap_interval ? FB_ACTIVATE_VBL : FB_ACTIVATE_NOW;
    fbsurf->vinfo.yoffset = buffer * fbsurf->height;
    /* Pan framebuffer in y direction.
     * Android uses FBIOPUT_VSCREENINFO for this; however on some hardware this does a
     * reconfiguration of the DC every time it is called which causes flicker and slowness.
     * On the other hand, FBIOPAN_DISPLAY causes a smooth scroll on some hardware,
     * according to the Android rationale. Choose the least of both evils.
     */
    if (ioctl(fbsurf->fbdpy->fd, FBIOPAN_DISPLAY, &fbsurf->vinfo))
    {
        printf("Error: failed to run ioctl to pan display: %s\n", strerror(errno));
        return errno;
    }
    return 0;
}

static PIPE_THREAD_ROUTINE(fbdev_bswap_thread, param)
{
    struct fbdev_surface *fbsurf = (struct fbdev_surface*)param;
    struct pipe_screen *screen = fbsurf->fbdpy->base.screen;
    struct native_fbdev_screen *fbdev_screen = fbsurf->fbdpy->fbdev_screen;
    while(!fbsurf->terminate)
    {
        int cur;
        struct pipe_fence_handle *fence;
        /* unqueue buffer */
        pipe_mutex_lock(fbsurf->buffer_mutex);
        while(fbsurf->posted_buffers == 0 && !fbsurf->terminate)
        {
           pipe_condvar_wait(fbsurf->buffer_posted, fbsurf->buffer_mutex);
        }
        cur = fbsurf->buffer_head;
        pipe_mutex_unlock(fbsurf->buffer_mutex);

        if(fbsurf->terminate)
            break;
        /* wait for buffer fence */
        fence = fbdev_screen->get_drawable_fence(fbdev_screen, fbsurf->drawable[cur]);
        if(fence)
            screen->fence_finish(screen, fence, PIPE_TIMEOUT_INFINITE);
        /* switch to buffer */
        fbdev_set_buffer(fbsurf, cur);
        /* notify that previously visible buffer is available again */
        pipe_mutex_lock(fbsurf->buffer_mutex);
        fbsurf->posted_buffers -= 1;
        fbsurf->buffer_head = (fbsurf->buffer_head + 1) % fbsurf->num_buffers;
        pipe_condvar_signal(fbsurf->buffer_available);
        pipe_mutex_unlock(fbsurf->buffer_mutex);
    }
    return NULL;
}

static void fbdev_destroy_buffers(struct fbdev_surface *fbsurf)
{
   struct native_fbdev_screen *fbdev_screen = fbsurf->fbdpy->fbdev_screen;
   int buf;
   /* Terminate buffer swap thread, wait for it to exit */
   pipe_mutex_lock(fbsurf->buffer_mutex);
   fbsurf->terminate = 1;
   pipe_condvar_signal(fbsurf->buffer_posted);
   pipe_mutex_unlock(fbsurf->buffer_mutex);
   pipe_thread_wait(fbsurf->bswap_thread);
   pipe_thread_destroy(fbsurf->bswap_thread);

   /* Clean up synchronization primitives */
   pipe_condvar_destroy(fbsurf->buffer_posted);
   pipe_condvar_destroy(fbsurf->buffer_available);
   pipe_mutex_destroy(fbsurf->buffer_mutex);
   for(buf=0; buf<fbsurf->num_buffers; ++buf)
       fbdev_screen->destroy_drawable(fbdev_screen, fbsurf->drawable[buf]);
   fbsurf->num_buffers = 0;
}

static bool fbdev_create_buffers(struct fbdev_surface *fbsurf, const struct fb_var_screeninfo *vinfo)
{
   int buf;
   struct fbdev_display *fbdpy = fbsurf->fbdpy;
   struct native_fbdev_screen *fbdev_screen = fbsurf->fbdpy->fbdev_screen;
   bool fail = false;
   if(fbsurf->num_buffers) /* if buffers already exist, destroy and recreate */
      fbdev_destroy_buffers(fbsurf);
   /* By default, use maximum number of buffers possible given screen mode */
   fbsurf->num_buffers = MIN2(vinfo->yres_virtual / vinfo->yres, FBDEV_MAX_BUFFERS);
   /* Allow overriding number of buffers to a lower amount from command line */
   if(getenv("EGL_FBDEV_BUFFERS"))
   {
      int requested_buffers = atoi(getenv("EGL_FBDEV_BUFFERS"));
      if(requested_buffers > 0)
          fbsurf->num_buffers = MIN2(fbsurf->num_buffers, requested_buffers);
   }
   printf("native_fbdev: %i buffers of %ix%i\n",
           fbsurf->num_buffers, fbsurf->width, fbsurf->height);
   if(fbsurf->num_buffers > 1) /* double or more buffered */
   {
       for(buf=0; buf<fbsurf->num_buffers; ++buf)
       {
           fbsurf->drawable[buf] = fbdev_screen->create_drawable(fbdev_screen,
                   fbdpy->fd,
                   0, vinfo->yres*buf, vinfo->xres, vinfo->yres);
           if(fbsurf->drawable[buf] == NULL)
              fail = true;
        }
    } else /* single buffer, at current virtual x/y offset */
    {
       fbsurf->drawable[0] = fbdev_screen->create_drawable(fbdev_screen,
               fbdpy->fd,
               vinfo->xoffset, vinfo->yoffset, vinfo->xres, vinfo->yres);
       if(fbsurf->drawable[0] == NULL)
          fail = true;
    }
    if(fail)
    {
       for(buf=0; buf<fbsurf->num_buffers; ++buf)
          fbdev_screen->destroy_drawable(fbdev_screen, fbsurf->drawable[buf]);
       return false;
    }
    fbsurf->terminate = 0;
    fbsurf->buffer_head = 0;
    fbsurf->buffer_tail = 0;
    fbsurf->posted_buffers = 0;
    pipe_mutex_init(fbsurf->buffer_mutex);
    pipe_condvar_init(fbsurf->buffer_available);
    pipe_condvar_init(fbsurf->buffer_posted);
    fbsurf->bswap_thread = pipe_thread_create(fbdev_bswap_thread, fbsurf);
    return true;
}

static boolean
fbdev_surface_present(struct native_surface *nsurf,
                      const struct native_present_control *ctrl)
{
   struct fbdev_surface *fbsurf = fbdev_surface(nsurf);
   struct fbdev_display *fbdpy = fbsurf->fbdpy;
   boolean ret = FALSE;

   if (ctrl->natt != NATIVE_ATTACHMENT_BACK_LEFT)
      return FALSE;

   if (!fbdpy->assume_fixed_vinfo) {
      struct fb_var_screeninfo vinfo;

      memset(&vinfo, 0, sizeof(vinfo));
      if (ioctl(fbdpy->fd, FBIOGET_VSCREENINFO, &vinfo))
         return FALSE;

      if (fbsurf->width != vinfo.xres || fbsurf->height != vinfo.yres)
      {
         fbsurf->width = vinfo.xres;
         fbsurf->height = vinfo.yres;

         if(!fbdev_create_buffers(fbsurf, &vinfo))
            return FALSE;

         if (resource_surface_set_size(fbsurf->rsurf,
                  fbsurf->width, fbsurf->height)) {
            /* surface resized */
            fbsurf->sequence_number++;
            fbdpy->event_handler->invalid_surface(&fbdpy->base,
                  &fbsurf->base, fbsurf->sequence_number);
         }
      }
   }

   int cur = 0;
   fbsurf->swap_interval = ctrl->swap_interval;
   if(fbsurf->num_buffers > 1)
   {
      /* wait for buffer to be available */
      pipe_mutex_lock(fbsurf->buffer_mutex);
      while(fbsurf->posted_buffers >= fbsurf->num_buffers-1)
      {
         pipe_condvar_wait(fbsurf->buffer_available, fbsurf->buffer_mutex);
      }
      cur = fbsurf->buffer_tail;
      pipe_mutex_unlock(fbsurf->buffer_mutex);
   }

   /* present */
   ret = resource_surface_present(fbsurf->rsurf,
         ctrl->natt, fbsurf->drawable[cur]);

   if(fbsurf->num_buffers > 1)
   {
       /* post the buffer */
       pipe_mutex_lock(fbsurf->buffer_mutex);
       fbsurf->posted_buffers += 1;
       fbsurf->buffer_tail = (fbsurf->buffer_tail + 1) % fbsurf->num_buffers;
       pipe_condvar_signal(fbsurf->buffer_posted);
       pipe_mutex_unlock(fbsurf->buffer_mutex);
   }

   return ret;
}

static void
fbdev_surface_wait(struct native_surface *nsurf)
{
   /* no-op */
}

static void
fbdev_surface_destroy(struct native_surface *nsurf)
{
   struct fbdev_surface *fbsurf = fbdev_surface(nsurf);

   resource_surface_destroy(fbsurf->rsurf);
   fbdev_destroy_buffers(fbsurf);
   FREE(fbsurf);
}

static struct native_surface *
fbdev_display_create_window_surface(struct native_display *ndpy,
                                    EGLNativeWindowType win,
                                    const struct native_config *nconf)
{
   struct fbdev_display *fbdpy = fbdev_display(ndpy);
   struct fbdev_surface *fbsurf;
   struct fb_var_screeninfo vinfo;

   /* there is only one native window: NULL */
   if (win)
      return NULL;

   fbsurf = CALLOC_STRUCT(fbdev_surface);
   if (!fbsurf)
      return NULL;

   fbsurf->fbdpy = fbdpy;
   fbsurf->swap_interval = 1;

   /* get current vinfo */
   if (fbdpy->assume_fixed_vinfo) {
      vinfo = fbdpy->config_vinfo;
   }
   else {
      memset(&fbsurf->vinfo, 0, sizeof(vinfo));
      if (ioctl(fbdpy->fd, FBIOGET_VSCREENINFO, &vinfo)) {
         FREE(fbsurf);
         return NULL;
      }
   }

   /* determine number of buffers */
   fbsurf->width = vinfo.xres;
   fbsurf->height = vinfo.yres;

   if(!fbdev_create_buffers(fbsurf, &vinfo))
   {
      FREE(fbsurf);
      return NULL;
   }

   fbsurf->rsurf = resource_surface_create(fbdpy->base.screen,
         nconf->color_format,
         PIPE_BIND_RENDER_TARGET |
         PIPE_BIND_DISPLAY_TARGET);
   if (!fbsurf->rsurf) {
      FREE(fbsurf);
      return NULL;
   }

   resource_surface_set_size(fbsurf->rsurf, fbsurf->width, fbsurf->height);

   fbsurf->base.destroy = fbdev_surface_destroy;
   fbsurf->base.present = fbdev_surface_present;
   fbsurf->base.validate = fbdev_surface_validate;
   fbsurf->base.wait = fbdev_surface_wait;
   fbsurf->vinfo = vinfo;

   return &fbsurf->base;
}

static struct native_surface *
fbdev_display_create_scanout_surface(struct native_display *ndpy,
                                     const struct native_config *nconf,
                                     uint width, uint height)
{
   return fbdev_display_create_window_surface(ndpy,
         (EGLNativeWindowType) NULL, nconf);
}

static boolean
fbdev_display_program(struct native_display *ndpy, int crtc_idx,
                      struct native_surface *nsurf, uint x, uint y,
                      const struct native_connector **nconns, int num_nconns,
                      const struct native_mode *nmode)
{
   return TRUE;
}

static const struct native_mode **
fbdev_display_get_modes(struct native_display *ndpy,
                        const struct native_connector *nconn,
                        int *num_modes)
{
   static struct native_mode mode;
   const struct native_mode **modes;

   if (!mode.desc) {
      struct fbdev_display *fbdpy = fbdev_display(ndpy);
      mode.desc = "Current Mode";
      mode.width = fbdpy->config_vinfo.xres;
      mode.height = fbdpy->config_vinfo.yres;
      mode.refresh_rate = 60 * 1000; /* dummy */
   }

   modes = MALLOC(sizeof(*modes));
   if (modes) {
      modes[0] = &mode;
      if (num_modes)
         *num_modes = 1;
   }

   return modes;
}

static const struct native_connector **
fbdev_display_get_connectors(struct native_display *ndpy, int *num_connectors,
                           int *num_crtc)
{
   static struct native_connector connector;
   const struct native_connector **connectors;

   connectors = MALLOC(sizeof(*connectors));
   if (connectors) {
      connectors[0] = &connector;
      if (num_connectors)
         *num_connectors = 1;
   }

   return connectors;
}

/* remove modeset support one day! */
static const struct native_display_modeset fbdev_display_modeset = {
   .get_connectors = fbdev_display_get_connectors,
   .get_modes = fbdev_display_get_modes,
   .create_scanout_surface = fbdev_display_create_scanout_surface,
   .program = fbdev_display_program
};

static const struct native_config **
fbdev_display_get_configs(struct native_display *ndpy, int *num_configs)
{
   struct fbdev_display *fbdpy = fbdev_display(ndpy);
   const struct native_config **configs;

   configs = MALLOC(sizeof(*configs));
   if (configs) {
      configs[0] = &fbdpy->config;
      if (num_configs)
         *num_configs = 1;
   }

   return configs;
}

static int
fbdev_display_get_param(struct native_display *ndpy,
                      enum native_param_type param)
{
   int val;

   switch (param) {
   case NATIVE_PARAM_PRESERVE_BUFFER:
   case NATIVE_PARAM_MAX_SWAP_INTERVAL:
      val = 1;
      break;
   case NATIVE_PARAM_USE_NATIVE_BUFFER:
   default:
      val = 0;
      break;
   }

   return val;
}

static void
fbdev_display_destroy(struct native_display *ndpy)
{
   struct fbdev_display *fbdpy = fbdev_display(ndpy);

   ndpy_uninit(&fbdpy->base);

   fbdpy->fbdev_screen->destroy(fbdpy->fbdev_screen);

   close(fbdpy->fd);
   FREE(fbdpy);
}

static boolean
fbdev_display_init_screen(struct native_display *ndpy)
{
   struct fbdev_display *fbdpy = fbdev_display(ndpy);
   const struct native_fbdev_driver *driver = NULL;
   const char *driver_name;

   driver_name = os_get_option("EGL_FBDEV_DRIVER");
   if(!driver_name)
      driver_name = "etna"; /* XXX probe drivers */

   if(!strcmp(driver_name, "etna"))
      driver = etna_fbdev_get_driver();
   else if(!strcmp(driver_name, "swrast"))
      driver = swrast_fbdev_get_driver();
   if(!driver)
      return FALSE;

   fbdpy->fbdev_screen = driver->create_screen(driver, fbdpy->fd, &fbdpy->base, fbdpy->event_handler);
   if(!fbdpy->fbdev_screen)
      return FALSE;
   fbdpy->base.screen = fbdpy->fbdev_screen->screen;

   if (!fbdpy->base.screen->is_format_supported(fbdpy->base.screen,
            fbdpy->config.color_format, PIPE_TEXTURE_2D, 0,
            PIPE_BIND_RENDER_TARGET)) {
      fbdpy->fbdev_screen->destroy(fbdpy->fbdev_screen);
      fbdpy->base.screen->destroy(fbdpy->base.screen);
      fbdpy->base.screen = NULL;
      printf("native_fbdev: color format for screen (%i) not supported by driver\n", fbdpy->config.color_format);
      /* XXX try next driver */
      return FALSE;
   }

   return TRUE;
}

static boolean
fbdev_display_init_config(struct native_display *ndpy)
{
   struct fbdev_display *fbdpy = fbdev_display(ndpy);
   struct native_config *nconf = &fbdpy->config;

   if (ioctl(fbdpy->fd, FBIOGET_VSCREENINFO, &fbdpy->config_vinfo))
      return FALSE;

   nconf->color_format = vinfo_to_format(&fbdpy->config_vinfo);
   if (nconf->color_format == PIPE_FORMAT_NONE)
      return FALSE;

   nconf->buffer_mask = (1 << NATIVE_ATTACHMENT_BACK_LEFT);

   nconf->window_bit = TRUE;

   printf("fbdev_display succesful\n");

   return TRUE;
}

static struct native_display *
fbdev_display_create(int fd, const struct native_event_handler *event_handler)
{
   struct fbdev_display *fbdpy;

   fbdpy = CALLOC_STRUCT(fbdev_display);
   if (!fbdpy)
      return NULL;

   fbdpy->fd = fd;
   fbdpy->event_handler = event_handler;

   if (ioctl(fbdpy->fd, FBIOGET_FSCREENINFO, &fbdpy->finfo))
      goto fail;

   if (fbdpy->finfo.visual != FB_VISUAL_TRUECOLOR ||
       fbdpy->finfo.type != FB_TYPE_PACKED_PIXELS)
      goto fail;

   if (!fbdev_display_init_config(&fbdpy->base))
      goto fail;

   fbdpy->assume_fixed_vinfo = TRUE;

   fbdpy->base.init_screen = fbdev_display_init_screen;
   fbdpy->base.destroy = fbdev_display_destroy;
   fbdpy->base.get_param = fbdev_display_get_param;
   fbdpy->base.get_configs = fbdev_display_get_configs;

   fbdpy->base.create_window_surface = fbdev_display_create_window_surface;

   /* we'd like to remove modeset support one day */
   fbdpy->config.scanout_bit = TRUE;
   fbdpy->base.modeset = &fbdev_display_modeset;

   return &fbdpy->base;

fail:
   FREE(fbdpy);
   return NULL;
}

static const struct native_event_handler *fbdev_event_handler;

static struct native_display *
native_create_display(void *dpy, boolean use_sw)
{
   struct native_display *ndpy;
   int fd;

   /* well, this makes fd 0 being ignored */
   if (!dpy) {
      const char *device_name="/dev/fb0";
#ifdef O_CLOEXEC
      fd = open(device_name, O_RDWR | O_CLOEXEC);
      if (fd == -1 && errno == EINVAL)
#endif
      {
         fd = open(device_name, O_RDWR);
         if (fd != -1)
            fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
      }
   }
   else {
      fd = dup((int) pointer_to_intptr(dpy));
   }
   if (fd < 0)
      return NULL;

   ndpy = fbdev_display_create(fd, fbdev_event_handler);
   if (!ndpy)
      close(fd);

   return ndpy;
}

static const struct native_platform fbdev_platform = {
   "FBDEV", /* name */
   native_create_display
};

const struct native_platform *
native_get_fbdev_platform(const struct native_event_handler *event_handler)
{
   fbdev_event_handler = event_handler;
   return &fbdev_platform;
}
