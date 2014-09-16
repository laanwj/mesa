
#ifndef __ETNA_DRM_PUBLIC_H__
#define __ETNA_DRM_PUBLIC_H__

struct pipe_screen;

struct pipe_screen *etna_drm_screen_create(int drmFD);

#endif
