/*
 * Copyright (c) 2012-2015 Etnaviv Project
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
 *    Wladimir J. van der Laan <laanwj@gmail.com>
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#include "etnaviv_screen.h"

#include "hw/common.xml.h"

#include "etnaviv_context.h"
#include "etnaviv_compiler.h"
#include "etnaviv_translate.h"
#include "etnaviv_debug.h"
#include "etnaviv_fence.h"
#include "etnaviv_resource.h"

#include "util/u_string.h"
#include "util/u_memory.h"
#include "os/os_time.h"

#include "state_tracker/drm_driver.h"

static const struct debug_named_value debug_options[] = {
    {"dbg_msgs",       ETNA_DBG_MSGS, "Print debug messages"},
    {"frame_msgs",     ETNA_DBG_FRAME_MSGS, "Print frame messages"},
    {"resource_msgs",  ETNA_DBG_RESOURCE_MSGS, "Print resource messages"},
    {"compiler_msgs",  ETNA_DBG_COMPILER_MSGS, "Print compiler messages"},
    {"linker_msgs",    ETNA_DBG_LINKER_MSGS, "Print linker messages"},
    {"dump_shaders",   ETNA_DBG_DUMP_SHADERS, "Dump shaders"},
    {"no_ts",          ETNA_DBG_NO_TS, "Disable TS"},
    {"no_autodisable", ETNA_DBG_NO_AUTODISABLE, "Disable autodisable"},
    {"no_supertile",   ETNA_DBG_NO_SUPERTILE,  "Disable supertiles"},
    {"no_early_z",     ETNA_DBG_NO_EARLY_Z, "Disable early z"},
    {"cflush_all",     ETNA_DBG_CFLUSH_ALL, "Flush every cash before state update"},
    {"msaa2x",         ETNA_DBG_MSAA_2X, "Force 2x msaa"},
    {"msaa4x",         ETNA_DBG_MSAA_4X, "Force 4x msaa"},
    //{"finish_all",     ETNA_DBG_FINISH_ALL,"Temporary flag to force GLSL 1.20 (rather than 1.30) on a3xx+"},
    {"flush_all",      ETNA_DBG_FLUSH_ALL, "Flush after every rendered primitive"},
    {"zero",           ETNA_DBG_ZERO, "Zero all resources after allocation"},
    {"draw_stall",     ETNA_DBG_DRAW_STALL, "Stall FE/PE after each rendered primitive"},
    DEBUG_NAMED_VALUE_END
};

DEBUG_GET_ONCE_FLAGS_OPTION(etna_mesa_debug, "ETNA_MESA_DEBUG", debug_options, 0)

int etna_mesa_debug = 0;

static void etna_screen_destroy(struct pipe_screen *pscreen)
{
    struct etna_screen *screen = etna_screen(pscreen);

    if (screen->pipe)
      etna_pipe_del(screen->pipe);

    if (screen->gpu)
      etna_gpu_del(screen->gpu);

    if (screen->dev)
      etna_device_del(screen->dev);

    FREE(screen);
}

static const char *etna_screen_get_name(struct pipe_screen *pscreen)
{
    struct etna_screen *priv = etna_screen(pscreen);
    static char buffer[128];

    util_snprintf(buffer, sizeof(buffer), "Vivante GC%x rev %04x",
            priv->model, priv->revision);

    return buffer;}

static const char *etna_screen_get_vendor(struct pipe_screen *pscreen)
{
    return "etnaviv";
}

static const char *etna_screen_get_device_vendor(struct pipe_screen *pscreen)
{
    return "Vivante";
}

static int etna_screen_get_param( struct pipe_screen *pscreen, enum pipe_cap param )
{
    struct etna_screen *screen = etna_screen(pscreen);

    switch (param) {
    /* Supported features (boolean caps). */
    case PIPE_CAP_TWO_SIDED_STENCIL:
    case PIPE_CAP_ANISOTROPIC_FILTER:
    case PIPE_CAP_POINT_SPRITE:
    case PIPE_CAP_TEXTURE_SHADOW_MAP:
    case PIPE_CAP_BLEND_EQUATION_SEPARATE:
    case PIPE_CAP_TGSI_FS_COORD_ORIGIN_UPPER_LEFT: /* FS coordinates start in upper left */
    case PIPE_CAP_TGSI_FS_COORD_PIXEL_CENTER_HALF_INTEGER: /* Pixel center on 0.5 */
    case PIPE_CAP_SM3:
    case PIPE_CAP_SEAMLESS_CUBE_MAP: /* ??? */
    case PIPE_CAP_TEXTURE_BARRIER:
    case PIPE_CAP_QUADS_FOLLOW_PROVOKING_VERTEX_CONVENTION:
    case PIPE_CAP_VERTEX_BUFFER_OFFSET_4BYTE_ALIGNED_ONLY:
    case PIPE_CAP_VERTEX_BUFFER_STRIDE_4BYTE_ALIGNED_ONLY:
    case PIPE_CAP_VERTEX_ELEMENT_SRC_OFFSET_4BYTE_ALIGNED_ONLY:
    case PIPE_CAP_USER_CONSTANT_BUFFERS: /* constant buffers can be user buffers; they end up in command stream anyway */
    case PIPE_CAP_TGSI_TEXCOORD: /* explicit TEXCOORD and POINTCOORD semantics */
            return 1;

    /* Memory */
    case PIPE_CAP_CONSTANT_BUFFER_OFFSET_ALIGNMENT:
            return 256;
    case PIPE_CAP_MIN_MAP_BUFFER_ALIGNMENT:
            return 4; /* XXX could easily be supported */
    case PIPE_CAP_GLSL_FEATURE_LEVEL:
            return 120;

    case PIPE_CAP_NPOT_TEXTURES: /* MUST be supported with GLES 2.0: what the capability specifies is filtering support */
            return true; /* VIV_FEATURE(priv->dev, chipMinorFeatures1, NON_POWER_OF_TWO); */

    case PIPE_CAP_MAX_VERTEX_BUFFERS:
            return screen->specs.stream_count;
    case PIPE_CAP_ENDIANNESS:
            return PIPE_ENDIAN_LITTLE; /* on most Viv hw this is configurable (feature ENDIANNESS_CONFIG) */

    /* Unsupported features. */
    case PIPE_CAP_TEXTURE_SWIZZLE: /* XXX supported on gc2000 */
    case PIPE_CAP_COMPUTE: /* XXX supported on gc2000 */
    case PIPE_CAP_MIXED_COLORBUFFER_FORMATS: /* only one colorbuffer supported, so mixing makes no sense */
    case PIPE_CAP_PRIMITIVE_RESTART: /* primitive restart index AFAIK not supported */
    case PIPE_CAP_VERTEX_COLOR_UNCLAMPED: /* no floating point buffer support */
    case PIPE_CAP_CONDITIONAL_RENDER: /* no occlusion queries */
    case PIPE_CAP_TGSI_INSTANCEID: /* no idea, really */
    case PIPE_CAP_START_INSTANCE: /* instancing not supported AFAIK */
    case PIPE_CAP_VERTEX_ELEMENT_INSTANCE_DIVISOR:  /* instancing not supported AFAIK */
    case PIPE_CAP_SHADER_STENCIL_EXPORT: /* Fragment shader cannot export stencil value */
    case PIPE_CAP_MAX_DUAL_SOURCE_RENDER_TARGETS: /* no dual-source supported */
    case PIPE_CAP_TEXTURE_MULTISAMPLE: /* no texture multisample */
    case PIPE_CAP_TEXTURE_MIRROR_CLAMP: /* only mirrored repeat */
    case PIPE_CAP_INDEP_BLEND_ENABLE:
    case PIPE_CAP_INDEP_BLEND_FUNC:
    case PIPE_CAP_DEPTH_CLIP_DISABLE:
    case PIPE_CAP_SEAMLESS_CUBE_MAP_PER_TEXTURE:
    case PIPE_CAP_TGSI_FS_COORD_ORIGIN_LOWER_LEFT:
    case PIPE_CAP_TGSI_FS_COORD_PIXEL_CENTER_INTEGER:
    case PIPE_CAP_TGSI_CAN_COMPACT_CONSTANTS: /* Don't skip strict max uniform limit check */
    case PIPE_CAP_FRAGMENT_COLOR_CLAMPED:
    case PIPE_CAP_VERTEX_COLOR_CLAMPED:
    case PIPE_CAP_USER_VERTEX_BUFFERS:
    case PIPE_CAP_USER_INDEX_BUFFERS:
    case PIPE_CAP_TEXTURE_BUFFER_OBJECTS:
            return 0;

    /* Stream output. */
    case PIPE_CAP_MAX_STREAM_OUTPUT_BUFFERS:
    case PIPE_CAP_STREAM_OUTPUT_PAUSE_RESUME:
    case PIPE_CAP_MAX_STREAM_OUTPUT_SEPARATE_COMPONENTS:
    case PIPE_CAP_MAX_STREAM_OUTPUT_INTERLEAVED_COMPONENTS:
            return 0;

    /* Texturing. */
    case PIPE_CAP_MAX_TEXTURE_2D_LEVELS:
    case PIPE_CAP_MAX_TEXTURE_CUBE_LEVELS:
            return 14;
    case PIPE_CAP_MAX_TEXTURE_3D_LEVELS: /* 3D textures not supported */
            return 0;
    case PIPE_CAP_MAX_TEXTURE_ARRAY_LAYERS:
            return 0;
    case PIPE_CAP_CUBE_MAP_ARRAY:
            return 0;
    case PIPE_CAP_MIN_TEXEL_OFFSET:
            return -8;
    case PIPE_CAP_MAX_TEXEL_OFFSET:
            return 7;
    case PIPE_CAP_TEXTURE_BORDER_COLOR_QUIRK:
            return 0;
    case PIPE_CAP_MAX_TEXTURE_BUFFER_SIZE:
            return 65536;

    /* Render targets. */
    case PIPE_CAP_MAX_RENDER_TARGETS:
            return 1;

    /* Viewports and scissors. */
    case PIPE_CAP_MAX_VIEWPORTS:
            return 1;

    /* Timer queries. */
    case PIPE_CAP_QUERY_TIME_ELAPSED:
    case PIPE_CAP_OCCLUSION_QUERY:
            return 0;
    case PIPE_CAP_QUERY_TIMESTAMP:
            return 1;
    case PIPE_CAP_QUERY_PIPELINE_STATISTICS:
            return 0;

    /* Preferences */
    case PIPE_CAP_PREFER_BLIT_BASED_TEXTURE_TRANSFER:
            return 0;

    default:
            DBG("unknown param %d", param);
            return 0;
    }
}

static float etna_screen_get_paramf(struct pipe_screen *pscreen, enum pipe_capf param)
{
    switch (param) {
    case PIPE_CAPF_MAX_LINE_WIDTH:
    case PIPE_CAPF_MAX_LINE_WIDTH_AA:
    case PIPE_CAPF_MAX_POINT_WIDTH:
    case PIPE_CAPF_MAX_POINT_WIDTH_AA:
            return 8192.0f;
    case PIPE_CAPF_MAX_TEXTURE_ANISOTROPY:
            return 16.0f;
    case PIPE_CAPF_MAX_TEXTURE_LOD_BIAS:
            return 16.0f;
    case PIPE_CAPF_GUARD_BAND_LEFT:
    case PIPE_CAPF_GUARD_BAND_TOP:
    case PIPE_CAPF_GUARD_BAND_RIGHT:
    case PIPE_CAPF_GUARD_BAND_BOTTOM:
            return 0.0f;
    default:
            DBG("unknown paramf %d", param);
            return 0;
    }
}

static int etna_screen_get_shader_param(struct pipe_screen *pscreen, unsigned shader, enum pipe_shader_cap param)
{
    struct etna_screen *screen = etna_screen(pscreen);

    switch(shader) {
    case PIPE_SHADER_FRAGMENT:
    case PIPE_SHADER_VERTEX:
            break;
    case PIPE_SHADER_COMPUTE:
    case PIPE_SHADER_GEOMETRY:
    case PIPE_SHADER_TESS_CTRL:
    case PIPE_SHADER_TESS_EVAL:
            /* maybe we could emulate.. */
            return 0;
    default:
            DBG("unknown shader type %d", shader);
            return 0;
    }

    switch (param) {
    case PIPE_SHADER_CAP_MAX_INSTRUCTIONS:
    case PIPE_SHADER_CAP_MAX_ALU_INSTRUCTIONS:
    case PIPE_SHADER_CAP_MAX_TEX_INSTRUCTIONS:
    case PIPE_SHADER_CAP_MAX_TEX_INDIRECTIONS:
            return ETNA_MAX_TOKENS;
    case PIPE_SHADER_CAP_MAX_CONTROL_FLOW_DEPTH:
            return ETNA_MAX_DEPTH; /* XXX */
    case PIPE_SHADER_CAP_MAX_INPUTS:
            /* Maximum number of inputs for the vertex shader is the number
             * of vertex elements - each element defines one vertex shader
             * input register.  For the fragment shader, this is the number
             * of varyings. */
            return shader==PIPE_SHADER_FRAGMENT ? screen->specs.max_varyings :
                                                  screen->specs.vertex_max_elements;
    case PIPE_SHADER_CAP_MAX_OUTPUTS:
            return 16; /* see VIVS_VS_OUTPUT */
    case PIPE_SHADER_CAP_MAX_TEMPS:
            return 64; /* Max native temporaries. */
    case PIPE_SHADER_CAP_MAX_CONST_BUFFERS:
            return 1;
    case PIPE_SHADER_CAP_MAX_PREDS:
            return 0; /* nothing uses this */
    case PIPE_SHADER_CAP_TGSI_CONT_SUPPORTED:
            return 1;
    case PIPE_SHADER_CAP_INDIRECT_INPUT_ADDR:
    case PIPE_SHADER_CAP_INDIRECT_OUTPUT_ADDR:
    case PIPE_SHADER_CAP_INDIRECT_TEMP_ADDR:
    case PIPE_SHADER_CAP_INDIRECT_CONST_ADDR:
            return 1;
    case PIPE_SHADER_CAP_SUBROUTINES:
            return 0;
    case PIPE_SHADER_CAP_TGSI_SQRT_SUPPORTED:
            return VIV_FEATURE(screen, chipMinorFeatures0, HAS_SQRT_TRIG);
    case PIPE_SHADER_CAP_INTEGERS: /* XXX supported on gc2000 but not yet implemented */
            return 0;
    case PIPE_SHADER_CAP_MAX_TEXTURE_SAMPLERS:
            return shader==PIPE_SHADER_FRAGMENT ? screen->specs.fragment_sampler_count :
                                                  screen->specs.vertex_sampler_count;
    case PIPE_SHADER_CAP_PREFERRED_IR:
            return PIPE_SHADER_IR_TGSI;
    case PIPE_SHADER_CAP_MAX_CONST_BUFFER_SIZE:
            return 4096;
    case PIPE_SHADER_CAP_DOUBLES:
    case PIPE_SHADER_CAP_TGSI_DROUND_SUPPORTED:
    case PIPE_SHADER_CAP_TGSI_DFRACEXP_DLDEXP_SUPPORTED:
    case PIPE_SHADER_CAP_TGSI_FMA_SUPPORTED:
    case PIPE_SHADER_CAP_TGSI_ANY_INOUT_DECL_RANGE:
            return false;
    default:
            DBG("unknown shader param %d", param);
            return 0;
    }
    return 0;
}

static uint64_t etna_screen_get_timestamp(struct pipe_screen *pscreen)
{
    return os_time_get_nano();
}

static boolean etna_screen_is_format_supported( struct pipe_screen *pscreen,
                               enum pipe_format format,
                               enum pipe_texture_target target,
                               unsigned sample_count,
                               unsigned usage)
{
    struct etna_screen *screen = etna_screen(pscreen);
    unsigned allowed = 0;

    if (target >= PIPE_MAX_TEXTURE_TYPES)
    {
        return FALSE;
    }

    if (usage & PIPE_BIND_RENDER_TARGET)
    {
        /* if render target, must be RS-supported format */
        if(translate_rt_format(format, true) != ETNA_NO_MATCH)
        {
            /* Validate MSAA; number of samples must be allowed, and render target must have
             * MSAA'able format.
             */
            if(sample_count > 1)
            {
                if(translate_samples_to_xyscale(sample_count, NULL, NULL, NULL) &&
                   translate_msaa_format(format, true) != ETNA_NO_MATCH)
                {
                    allowed |= PIPE_BIND_RENDER_TARGET;
                }
            } else {
                allowed |= PIPE_BIND_RENDER_TARGET;
            }
        }
    }
    if (usage & PIPE_BIND_DEPTH_STENCIL)
    {
        /* must be supported depth format */
        if(translate_depth_format(format, true) != ETNA_NO_MATCH)
        {
            allowed |= PIPE_BIND_DEPTH_STENCIL;
        }
    }
    if (usage & PIPE_BIND_SAMPLER_VIEW)
    {
        /* must be supported texture format */
        if(sample_count < 2 && translate_texture_format(format, true) != ETNA_NO_MATCH)
        {
            allowed |= PIPE_BIND_SAMPLER_VIEW;
        }
    }
    if (usage & PIPE_BIND_VERTEX_BUFFER)
    {
        /* must be supported vertex format */
        if(translate_vertex_format_type(format, true) != ETNA_NO_MATCH)
        {
            allowed |= PIPE_BIND_VERTEX_BUFFER;
        }
    }
    if (usage & PIPE_BIND_INDEX_BUFFER)
    {
        /* must be supported index format */
        if(format == PIPE_FORMAT_I8_UINT ||
           format == PIPE_FORMAT_I16_UINT ||
           (format == PIPE_FORMAT_I32_UINT && VIV_FEATURE(screen, chipFeatures, 32_BIT_INDICES)))
        {
            allowed |= PIPE_BIND_INDEX_BUFFER;
        }
    }
    /* Always allowed */
    allowed |= usage & (PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT |
            PIPE_BIND_SHARED | PIPE_BIND_TRANSFER_READ | PIPE_BIND_TRANSFER_WRITE);

    return usage == allowed;
}

static void etna_screen_flush_frontbuffer(struct pipe_screen *pscreen,
                          struct pipe_resource *presource,
                          unsigned level, unsigned layer,
                          void *winsys_drawable_handle,
                          struct pipe_box *sub_box)
{
    struct etna_resource *drawable = etna_resource(winsys_drawable_handle);
    struct etna_resource *resource = etna_resource(presource);
    struct pipe_blit_info blit;

#if 0 /* TODO */
    assert(level <= resource->last_level && layer < resource->array_size);

    /* release previous fence, make reference to fence if we need one */
    pscreen->fence_reference(pscreen, &drawable->fence, NULL);
#endif

    memset(&blit, 0, sizeof(blit));
    blit.mask = PIPE_MASK_RGBA;
    blit.filter = PIPE_TEX_FILTER_LINEAR;
    blit.src.resource = &resource->base;
    blit.src.format = resource->base.format;
    blit.src.level = level;
    blit.src.box.width = resource->levels[level].width;
    blit.src.box.height = resource->levels[level].height;
    blit.dst.resource = &drawable->base;
    blit.dst.format = drawable->base.format;
    blit.dst.level = 0;
    blit.dst.box.width = drawable->levels[0].width;
    blit.dst.box.height = drawable->levels[0].height;

#if 0 /* TODO */
    ectx->base.blit(&ectx->base, &blit);
    ectx->base.flush(&ectx->base, &drawable->fence, 0);
#endif
}

static boolean etna_get_specs(struct etna_screen *screen)
{
    uint64_t val;
    uint32_t instruction_count;

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_INSTRUCTION_COUNT, &val)) {
        DBG("could not get ETNA_GPU_INSTRUCTION_COUNT");
        goto fail;
    }
    instruction_count = val;

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_VERTEX_OUTPUT_BUFFER_SIZE, &val)) {
        DBG("could not get ETNA_GPU_VERTEX_OUTPUT_BUFFER_SIZE");
        goto fail;
    }
    screen->specs.vertex_output_buffer_size = val;

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_VERTEX_CACHE_SIZE, &val)) {
        DBG("could not get ETNA_GPU_VERTEX_CACHE_SIZE");
        goto fail;
    }
    screen->specs.vertex_cache_size = val;

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_SHADER_CORE_COUNT, &val)) {
        DBG("could not get ETNA_GPU_SHADER_CORE_COUNT");
        goto fail;
    }
    screen->specs.shader_core_count = val;

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_STREAM_COUNT, &val)) {
        DBG("could not get ETNA_GPU_STREAM_COUNT");
        goto fail;
    }
    screen->specs.stream_count = val;

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_REGISTER_MAX, &val)) {
        DBG("could not get ETNA_GPU_REGISTER_MAX");
        goto fail;
    }
    screen->specs.max_registers = val;

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_PIXEL_PIPES, &val)) {
        DBG("could not get ETNA_GPU_PIXEL_PIPES");
        goto fail;
    }
    if (val < 1 && val > ETNA_MAX_PIXELPIPES) {
        if (val == 0) {
            fprintf(stderr, "Warning: zero pixel pipes (update kernel?)\n");
            val = 1;
        } else {
            fprintf(stderr, "Error: bad pixel pipes value %u\n",
                    (unsigned int)val);
            goto fail;
        }
    }
    screen->specs.pixel_pipes = val;

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_NUM_CONSTANTS, &val)) {
        DBG("could not get %s", "ETNA_GPU_NUM_CONSTANTS");
        goto fail;
    }
    if (val == 0) {
        fprintf(stderr, "Warning: zero num constants (update kernel?)\n");
        val = 168;
    }
    screen->specs.num_constants = val;

    screen->specs.can_supertile = VIV_FEATURE(screen, chipMinorFeatures0, SUPER_TILED);
    screen->specs.bits_per_tile = VIV_FEATURE(screen, chipMinorFeatures0, 2BITPERTILE)?2:4;
    screen->specs.ts_clear_value = VIV_FEATURE(screen, chipMinorFeatures0, 2BITPERTILE)?0x55555555:0x11111111;
    screen->specs.vertex_sampler_offset = 8; /* vertex and fragment samplers live in one address space, with vertex shaders at this offset */
    screen->specs.fragment_sampler_count = 8;
    screen->specs.vertex_sampler_count = 4;
    screen->specs.vs_need_z_div = screen->model < 0x1000 && screen->model != 0x880;
    screen->specs.has_sin_cos_sqrt = VIV_FEATURE(screen, chipMinorFeatures0, HAS_SQRT_TRIG);
    screen->specs.has_sign_floor_ceil = VIV_FEATURE(screen, chipMinorFeatures0, HAS_SIGN_FLOOR_CEIL);
    screen->specs.has_shader_range_registers = screen->model >= 0x1000 || screen->model == 0x880;
    screen->specs.npot_tex_any_wrap = VIV_FEATURE(screen, chipMinorFeatures1, NON_POWER_OF_TWO);

    if (instruction_count > 256) /* unified instruction memory? */
    {
        screen->specs.vs_offset = 0xC000;
        screen->specs.ps_offset = 0xD000; //like vivante driver
        screen->specs.max_instructions = 256;
    } else {
        screen->specs.vs_offset = 0x4000;
        screen->specs.ps_offset = 0x6000;
        screen->specs.max_instructions = instruction_count/2;
    }

    if (VIV_FEATURE(screen, chipMinorFeatures1, HALTI0))
    {
        screen->specs.max_varyings = 12;
        screen->specs.vertex_max_elements = 16;
    } else {
        screen->specs.max_varyings = 8;
        /* Etna_viv documentation seems confused over the correct value
         * here so choose the lower to be safe: HALTI0 says 16 i.s.o.
         * 10, but VERTEX_ELEMENT_CONFIG register says 16 i.s.o. 12.
         */
        screen->specs.vertex_max_elements = 10;
    }

    if (screen->model < chipModel_GC4000) /* from QueryShaderCaps in kernel driver */
    {
        screen->specs.max_vs_uniforms = 168;
        screen->specs.max_ps_uniforms = 64;
    } else {
        screen->specs.max_vs_uniforms = 256;
        screen->specs.max_ps_uniforms = 256;
    }

    screen->specs.max_texture_size = VIV_FEATURE(screen, chipMinorFeatures0, TEXTURE_8K)?8192:4096;
    screen->specs.max_rendertarget_size = VIV_FEATURE(screen, chipMinorFeatures0, RENDERTARGET_8K)?8192:4096;

    return true;

fail:
    return false;
}

struct etna_bo *etna_screen_bo_from_handle(struct pipe_screen *pscreen,
                struct winsys_handle *whandle,
                unsigned *out_stride)
{
    struct etna_screen *screen = etna_screen(pscreen);
    struct etna_bo *bo;

    if (whandle->type == DRM_API_HANDLE_TYPE_SHARED) {
        bo = etna_bo_from_name(screen->dev, whandle->handle);
    } else if (whandle->type == DRM_API_HANDLE_TYPE_FD) {
        bo = etna_bo_from_dmabuf(screen->dev, whandle->handle);
    } else {
        DBG("Attempt to import unsupported handle type %d", whandle->type);
        return NULL;
    }

    if (!bo) {
        DBG("ref name 0x%08x failed", whandle->handle);
        return NULL;
    }

    *out_stride = whandle->stride;

    return bo;
}

struct pipe_screen *
etna_screen_create(struct etna_device *dev, struct etna_gpu *gpu)
{
    struct etna_screen *screen = CALLOC_STRUCT(etna_screen);
    struct pipe_screen *pscreen;
    uint64_t val;

    if (!screen)
        return NULL;

    pscreen = &screen->base;
    screen->dev = dev;
    screen->gpu = gpu;

    etna_mesa_debug = debug_get_option_etna_mesa_debug();
    /* FIXME: Disable tile status for stability at the moment */
    etna_mesa_debug |= ETNA_DBG_NO_TS;

    screen->pipe = etna_pipe_new(gpu, ETNA_PIPE_3D);
    if (!screen->pipe) {
        DBG("could not create 3d pipe");
        goto fail;
    }

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_MODEL, &val)) {
        DBG("could not get ETNA_GPU_MODEL");
        goto fail;
    }
    screen->model = val;

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_REVISION, &val)) {
        DBG("could not get ETNA_GPU_REVISION");
        goto fail;
    }
    screen->revision = val;

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_FEATURES_0, &val)) {
        DBG("could not get ETNA_GPU_FEATURES_0");
        goto fail;
    }
    screen->features[0] = val;

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_FEATURES_1, &val)) {
        DBG("could not get ETNA_GPU_FEATURES_1");
        goto fail;
    }
    screen->features[1] = val;

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_FEATURES_2, &val)) {
        DBG("could not get ETNA_GPU_FEATURES_2");
        goto fail;
    }
    screen->features[2] = val;

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_FEATURES_3, &val)) {
        DBG("could not get ETNA_GPU_FEATURES_3");
        goto fail;
    }
    screen->features[3] = val;

    if (etna_gpu_get_param(screen->gpu, ETNA_GPU_FEATURES_4, &val)) {
        DBG("could not get ETNA_GPU_FEATURES_4");
        goto fail;
    }
    screen->features[4] = val;

    if (!etna_get_specs(screen))
        goto fail;

    /* Initialize vtable */
    pscreen->destroy = etna_screen_destroy;
    pscreen->get_param = etna_screen_get_param;
    pscreen->get_paramf = etna_screen_get_paramf;
    pscreen->get_shader_param = etna_screen_get_shader_param;

    pscreen->get_name = etna_screen_get_name;
    pscreen->get_vendor = etna_screen_get_vendor;
    pscreen->get_device_vendor = etna_screen_get_device_vendor;

    pscreen->get_timestamp = etna_screen_get_timestamp;
    pscreen->context_create = etna_context_create;
    pscreen->is_format_supported = etna_screen_is_format_supported;
    pscreen->flush_frontbuffer = etna_screen_flush_frontbuffer;

    etna_fence_screen_init(pscreen);
    etna_resource_screen_init(pscreen);

    return pscreen;
fail:
    etna_screen_destroy(pscreen);
    return NULL;
}
