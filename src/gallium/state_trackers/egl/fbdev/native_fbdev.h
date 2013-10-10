#ifndef __NATIVE_FBDEV_H__
#define __NATIVE_FBDEV_H__

struct native_fbdev_screen;
struct native_event_handler;
struct fb_var_screeninfo;

/* Framebuffer rendering driver. This top-level structure is used
 * for probing the device and creating a screen.
 */
struct native_fbdev_driver
{
   const char *driver_name;
   /* Probe for the device.
    * @returns true if this device can be used, false otherwise.
    */
   bool (*probe)(const struct native_fbdev_driver *driver);
   /* Create screen, given framebuffer handle.
    * (XXX passing a framebuffer handle is necessary here because sw_winsys needs
    * a framebuffer handle on creation, maybe get rid of this)
    */
   struct native_fbdev_screen *(*create_screen)(
         const struct native_fbdev_driver *driver,
         int fd,
         struct native_display *display,
         const struct native_event_handler *event_handler);
};

/* Structure that represents a screen for a fbdev_driver.
 */
struct native_fbdev_screen
{
   /* Get pipe screen associated with this fbdev screen. */
   struct pipe_screen *screen;

   /* Destroy this fbdev_screen.
    * This does not destroy the pipe_screen, this is the responsibility
    * of the client code.
    */
   void (*destroy)(struct native_fbdev_screen *fbdev_screen);

   /* Create window drawable for subset of (the virtual resolution of) a framebuffer.
    * This returns a drawable that can be passed to screen->flush_frontbuffer
    * or resource_surface_present.
    *
    * @input fd  fb file descriptor. It is possible to provide another
    *    fd handle than the one this screen was created with, to be able to render to multiple
    *    framebuffers (or subsets thereof) at once.
    */
   void *(*create_drawable)(struct native_fbdev_screen *fbdev_screen,
             int fd,
             unsigned xoffset, unsigned yoffset,
             unsigned width, unsigned height);

   /* Destroy drawable handle. */
   void (*destroy_drawable)(struct native_fbdev_screen *fbdev_screen, void *drawable);

   /* Get fence handle to track finished rendering and copying of the last
    * frame (after calling flush_frontbuffer with the handle of a drawable).
    */
   struct pipe_fence_handle *(*get_drawable_fence)(struct native_fbdev_screen *fbdev_screen, void *drawable);
};

enum pipe_format
vinfo_to_format(const struct fb_var_screeninfo *vinfo);

#endif
