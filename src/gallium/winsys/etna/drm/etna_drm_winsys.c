#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/u_format.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"

#include "etna_drm_public.h"

#include "etnaviv/viv.h"
#include "etna/etna_screen.h"

#include <stdio.h>

struct pipe_screen *
etna_drm_screen_create(int fd)
{
    struct viv_conn *dev = 0;
    /* XXX this handle will leak */
    int rv = viv_open(VIV_HW_3D, &dev);
    if(rv != 0)
    {
        fprintf(stderr, "Error opening device\n");
        return NULL;
    }
    fprintf(stderr, "Succesfully opened device\n");

    return etna_screen_create(dev);
}
