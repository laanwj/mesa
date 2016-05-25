#include "renderonly/renderonly_screen.h"
#include "etnaviv_drm_public.h"
#include "etnaviv/etnaviv_screen.h"

#include <stdio.h>
#include <fcntl.h>

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

    for (i = 0; ; i++) {
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

static const struct renderonly_ops etna_native_ro_ops = {
    .open = etna_drm_screen_create_renderer,
    .intermediate_rendering = true,
};

struct pipe_screen *etna_drm_screen_create(int fd)
{
    return renderonly_screen_create(fd, &etna_native_ro_ops);
}
