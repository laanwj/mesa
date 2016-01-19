#include "etnaviv_fence.h"
#include "etnaviv_context.h"
#include "etnaviv_screen.h"

#include "util/u_memory.h"
#include "util/u_inlines.h"

struct pipe_fence_handle {
    struct pipe_reference reference;
    struct etna_context *ctx;
    struct etna_screen *screen;
    uint32_t timestamp;
};

static void etna_screen_fence_reference(struct pipe_screen *pscreen,
                        struct pipe_fence_handle **ptr,
                        struct pipe_fence_handle *fence)
{
    if (pipe_reference(&(*ptr)->reference, &fence->reference))
        FREE(*ptr);

    *ptr = fence;
}

static boolean etna_screen_fence_signalled(struct pipe_screen *pscreen,
		struct pipe_fence_handle *fence)
{
    uint32_t timestamp = etna_cmd_stream_timestamp(fence->ctx->stream);

    /* TODO util helper for compare w/ rollover? */
    return timestamp >= fence->timestamp;
}

static boolean etna_screen_fence_finish(struct pipe_screen *pscreen,
                        struct pipe_fence_handle *fence,
                        uint64_t timeout)
{
    uint32_t ms = timeout / 1000000ULL;

    if (!timeout)
        return etna_screen_fence_signalled(pscreen, fence);

    if (etna_pipe_wait(fence->screen->pipe, fence->timestamp, ms))
        return false;

    return true;
}

struct pipe_fence_handle * etna_fence_create(struct pipe_context *pctx)
{
    struct pipe_fence_handle *fence;
    struct etna_context *ctx = etna_context(pctx);

    fence = CALLOC_STRUCT(pipe_fence_handle);
    if (!fence)
        return NULL;

    pipe_reference_init(&fence->reference, 1);

    fence->ctx = ctx;
    fence->screen = ctx->screen;
    fence->timestamp = etna_cmd_stream_timestamp(ctx->stream);

    return fence;
}

void etna_fence_screen_init(struct pipe_screen *pscreen)
{
    pscreen->fence_reference = etna_screen_fence_reference;
    pscreen->fence_finish = etna_screen_fence_finish;
}
