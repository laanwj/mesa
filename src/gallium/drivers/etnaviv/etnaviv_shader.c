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
 */

#include "etnaviv_shader.h"

#include "etnaviv_context.h"
#include "etnaviv_compiler.h"
#include "etnaviv_debug.h"

#include "util/u_memory.h"
#include "util/u_math.h"

/* Fetch uniforms from user buffer, if bound, and mark respective uniform
 * bank as dirty. */
static void etna_fetch_uniforms(struct etna_context *ctx, uint shader)
{
    struct pipe_constant_buffer *buf = NULL;

    switch(shader)
    {
    case PIPE_SHADER_VERTEX:
        buf = &ctx->vs_cbuf_s;
        if(buf->user_buffer && ctx->vs)
        {
            memcpy(ctx->shader_state.VS_UNIFORMS, buf->user_buffer, MIN2(buf->buffer_size, ctx->vs->const_size * 4));
            ctx->dirty |= ETNA_DIRTY_VS_UNIFORMS;
        }
        break;
    case PIPE_SHADER_FRAGMENT:
        buf = &ctx->fs_cbuf_s;
        if(buf->user_buffer && ctx->fs)
        {
            memcpy(ctx->shader_state.PS_UNIFORMS, buf->user_buffer, MIN2(buf->buffer_size, ctx->fs->const_size * 4));
            ctx->dirty |= ETNA_DIRTY_PS_UNIFORMS;
        }
        break;
    default: DBG("Unhandled shader type %i", shader);
    }
}


/* Link vs and fs together: fill in shader_state from vs and fs
 * as this function is called every time a new fs or vs is bound, the goal is to do
 * little processing as possible here, and to precompute as much as possible in the
 * vs/fs shader_object.
 * XXX we could cache the link result for a certain set of VS/PS; usually a pair
 * of VS and PS will be used together anyway.
 */
static void etna_link_shaders(struct etna_context* ctx, struct compiled_shader_state* cs, const struct etna_shader_object* vs, const struct etna_shader_object* fs)
{
    assert(vs->processor == PIPE_SHADER_VERTEX);
    assert(fs->processor == PIPE_SHADER_FRAGMENT);
#ifdef DEBUG
    if(DBG_ENABLED(ETNA_DBG_DUMP_SHADERS))
    {
        etna_dump_shader_object(vs);
        etna_dump_shader_object(fs);
    }
#endif
    /* set last_varying_2x flag if the last varying has 1 or 2 components */
    bool last_varying_2x = false;
    if(fs->num_inputs>0 && fs->inputs[fs->num_inputs-1].num_components <= 2)
        last_varying_2x = true;

    cs->RA_CONTROL = VIVS_RA_CONTROL_UNK0 |
                          (last_varying_2x ? VIVS_RA_CONTROL_LAST_VARYING_2X : 0);

    cs->PA_ATTRIBUTE_ELEMENT_COUNT = VIVS_PA_ATTRIBUTE_ELEMENT_COUNT_COUNT(fs->num_inputs);
    for(int idx=0; idx<fs->num_inputs; ++idx)
        cs->PA_SHADER_ATTRIBUTES[idx] = fs->inputs[idx].pa_attributes;

    cs->VS_END_PC = vs->code_size / 4;
    cs->VS_OUTPUT_COUNT = fs->num_inputs + 1; /* position + varyings */

    /* link vs outputs to fs inputs */
    struct etna_shader_link_info link = {};
    if(etna_link_shader_objects(&link, vs, fs))
    {
        assert(0); /* linking failed: some fs inputs do not have corresponding vs outputs */
    }
    DBG_F(ETNA_DBG_LINKER_MSGS, "link result:");
    for(int idx=0; idx<fs->num_inputs; ++idx)
    {
        DBG_F(ETNA_DBG_LINKER_MSGS,"  %i -> %i", link.varyings_vs_reg[idx], idx+1);
    }

    /* vs outputs (varyings) */
    uint32_t vs_output[16] = {0};
    int varid = 0;
    vs_output[varid++] = vs->vs_pos_out_reg;
    for(int idx=0; idx<fs->num_inputs; ++idx)
        vs_output[varid++] = link.varyings_vs_reg[idx];
    if(vs->vs_pointsize_out_reg >= 0)
        vs_output[varid++] = vs->vs_pointsize_out_reg; /* pointsize is last */

    for(int idx=0; idx<4; ++idx)
    {
        cs->VS_OUTPUT[idx] =(vs_output[idx*4+0] << 0)  | (vs_output[idx*4+1] << 8) |
                                 (vs_output[idx*4+2] << 16) | (vs_output[idx*4+3] << 24);
    }

    if(vs->vs_pointsize_out_reg != -1)
    {
        /* vertex shader outputs point coordinate, provide extra output and make sure PA config is
         * not masked */
        cs->PA_CONFIG = ~0;
        cs->VS_OUTPUT_COUNT_PSIZE = cs->VS_OUTPUT_COUNT + 1;
    } else {
        /* vertex shader does not output point coordinate, make sure thate POINT_SIZE_ENABLE is masked
         * and no extra output is given */
        cs->PA_CONFIG = ~VIVS_PA_CONFIG_POINT_SIZE_ENABLE;
        cs->VS_OUTPUT_COUNT_PSIZE = cs->VS_OUTPUT_COUNT;
    }

    cs->VS_LOAD_BALANCING = vs->vs_load_balancing;
    cs->VS_START_PC = 0;

    cs->PS_END_PC = fs->code_size / 4;
    cs->PS_OUTPUT_REG = fs->ps_color_out_reg;
    cs->PS_INPUT_COUNT = VIVS_PS_INPUT_COUNT_COUNT(fs->num_inputs + 1) | /* Number of inputs plus position */
                              VIVS_PS_INPUT_COUNT_UNK8(fs->input_count_unk8);
    cs->PS_TEMP_REGISTER_CONTROL =
                              VIVS_PS_TEMP_REGISTER_CONTROL_NUM_TEMPS(MAX2(fs->num_temps, fs->num_inputs + 1));
    cs->PS_CONTROL = VIVS_PS_CONTROL_UNK1; /* XXX when can we set BYPASS? */
    cs->PS_START_PC = 0;

    /* Precompute PS_INPUT_COUNT and TEMP_REGISTER_CONTROL in the case of MSAA mode, avoids
     * some fumbling in sync_context.
     */
    cs->PS_INPUT_COUNT_MSAA = VIVS_PS_INPUT_COUNT_COUNT(fs->num_inputs + 2) | /* MSAA adds another input */
                              VIVS_PS_INPUT_COUNT_UNK8(fs->input_count_unk8);
    cs->PS_TEMP_REGISTER_CONTROL_MSAA =
                              VIVS_PS_TEMP_REGISTER_CONTROL_NUM_TEMPS(MAX2(fs->num_temps, fs->num_inputs + 2));

    uint32_t total_components = 0;
    uint32_t num_components = 0;
    uint32_t component_use[2] = {0};
    for(int idx=0; idx<fs->num_inputs; ++idx)
    {
        num_components |= fs->inputs[idx].num_components << ((idx%8)*4);
        for(int comp=0; comp<fs->inputs[idx].num_components; ++comp)
        {
            unsigned use = VARYING_COMPONENT_USE_USED;
            if(fs->inputs[idx].semantic.Name == TGSI_SEMANTIC_PCOORD)
            {
                if(comp == 0)
                    use = VARYING_COMPONENT_USE_POINTCOORD_X;
                else if(comp == 1)
                    use = VARYING_COMPONENT_USE_POINTCOORD_Y;
            }
            /* 16 components per uint32 */
            component_use[total_components/16] |= use << ((total_components%16)*2);
            total_components += 1;
        }
    }
    cs->GL_VARYING_TOTAL_COMPONENTS = VIVS_GL_VARYING_TOTAL_COMPONENTS_NUM(align(total_components, 2));
    cs->GL_VARYING_NUM_COMPONENTS = num_components;
    cs->GL_VARYING_COMPONENT_USE[0] = component_use[0];
    cs->GL_VARYING_COMPONENT_USE[1] = component_use[1];

    /* reference instruction memory */
    cs->vs_inst_mem_size = vs->code_size;
    cs->VS_INST_MEM = vs->code;
    cs->ps_inst_mem_size = fs->code_size;
    cs->PS_INST_MEM = fs->code;

    /* uniforms layout -- first constants, then immediates */
    cs->vs_uniforms_size = vs->const_size + vs->imm_size;
    memcpy(&cs->VS_UNIFORMS[vs->imm_base], vs->imm_data, vs->imm_size*4);

    cs->ps_uniforms_size = fs->const_size + fs->imm_size;
    memcpy(&cs->PS_UNIFORMS[fs->imm_base], fs->imm_data, fs->imm_size*4);

    /* fetch any previous uniforms from buffer */
    etna_fetch_uniforms(ctx, PIPE_SHADER_VERTEX);
    etna_fetch_uniforms(ctx, PIPE_SHADER_FRAGMENT);
}

bool etna_shader_link(struct etna_context *ctx)
{
    if (!ctx->vs || !ctx->fs)
        return false;

    /* re-link vs and fs if needed */
    etna_link_shaders(ctx, &ctx->shader_state, ctx->vs, ctx->fs);

    return true;
}

static bool etna_shader_update_vs_inputs(struct etna_context *ctx,
                              struct compiled_shader_state *cs,
                              const struct etna_shader_object *vs,
                              const struct compiled_vertex_elements_state *ves)
{
    unsigned num_temps, cur_temp, num_vs_inputs;

    /* Number of vertex elements determines number of VS inputs. Otherwise,
     * the GPU crashes. Allocate any unused vertex elements to VS temporary
     * registers. */
    num_vs_inputs = MAX2(ves->num_elements, vs->num_inputs);
    if (num_vs_inputs != ves->num_elements)
    {
        BUG("Number of elements %i does not match the number of VS inputs %i",
            ctx->vertex_elements->num_elements, ctx->vs->num_inputs);
        return false;
    }

    cur_temp = vs->num_temps;
    num_temps = num_vs_inputs - vs->num_inputs + cur_temp;

    cs->VS_INPUT_COUNT = VIVS_VS_INPUT_COUNT_COUNT(num_vs_inputs) |
                         VIVS_VS_INPUT_COUNT_UNK8(vs->input_count_unk8);
    cs->VS_TEMP_REGISTER_CONTROL =
                         VIVS_VS_TEMP_REGISTER_CONTROL_NUM_TEMPS(num_temps);

    /* vs inputs (attributes) */
    uint32_t vs_input[4] = {0};
    for(int idx=0; idx<num_vs_inputs; ++idx)
        if (idx < vs->num_inputs)
            vs_input[idx/4] |= vs->inputs[idx].reg << ((idx%4)*8);
        else
            vs_input[idx/4] |= cur_temp++ << ((idx%4)*8);
    for(int idx=0; idx<4; ++idx)
        cs->VS_INPUT[idx] = vs_input[idx];

    return true;
}

bool etna_shader_update_vertex(struct etna_context *ctx)
{
    return etna_shader_update_vs_inputs(ctx, &ctx->shader_state, ctx->vs, ctx->vertex_elements);
}

static void etna_set_constant_buffer(struct pipe_context *pctx,
                                uint shader, uint index,
                                struct pipe_constant_buffer *buf)
{
    struct etna_context *ctx = etna_context(pctx);

    if(buf == NULL) /* Unbinding constant buffer */
    {
        if(likely(index == 0))
        {
            switch(shader)
            {
            case PIPE_SHADER_VERTEX:
                ctx->vs_cbuf_s.user_buffer = 0; break;
            case PIPE_SHADER_FRAGMENT:
                ctx->fs_cbuf_s.user_buffer = 0; break;
            default: DBG("Unhandled shader type %i", shader);
            }
        } else {
            DBG("Unhandled buffer index %i", index);
        }
    } else {
        assert(buf->buffer == NULL && buf->user_buffer != NULL);
        /* support only user buffer for now */
        if(likely(index == 0))
        {
            /* copy only up to shader-specific constant size; never overwrite immediates */
            switch(shader)
            {
            case PIPE_SHADER_VERTEX:
                ctx->vs_cbuf_s = *buf; break;
            case PIPE_SHADER_FRAGMENT:
                ctx->fs_cbuf_s = *buf; break;
            default: DBG("Unhandled shader type %i", shader);
            }
            etna_fetch_uniforms(ctx, shader);
        } else {
            DBG("Unhandled buffer index %i", index);
        }
    }
}

static void *etna_create_shader_state(struct pipe_context *pctx, const struct pipe_shader_state *pss)
{
    struct etna_context *ctx = etna_context(pctx);
    struct etna_shader_object *out = NULL;

    if (etna_compile_shader_object(&ctx->specs, pss->tokens, &out) == false)
        return NULL;
    else
        return out;
}

static void etna_delete_shader_state(struct pipe_context *pctx, void *ss)
{
    etna_destroy_shader_object((struct etna_shader_object*)ss);
}

static void etna_bind_fs_state(struct pipe_context *pctx, void *fss_)
{
    struct etna_context *ctx = etna_context(pctx);
    struct etna_shader_object *fss = (struct etna_shader_object*)fss_;

    if(ctx->fs == fss) /* skip if already bound */
        return;
    assert(fss == NULL || fss->processor == PIPE_SHADER_FRAGMENT);
    ctx->fs = fss;
    ctx->dirty |= ETNA_DIRTY_SHADER | ETNA_DIRTY_PS_UNIFORMS;
}

static void etna_bind_vs_state(struct pipe_context *pctx, void *vss_)
{
    struct etna_context *ctx = etna_context(pctx);
    struct etna_shader_object *vss = (struct etna_shader_object*)vss_;

    if(ctx->vs == vss) /* skip if already bound */
        return;
    assert(vss == NULL || vss->processor == PIPE_SHADER_VERTEX);
    ctx->vs = vss;
    ctx->dirty |= ETNA_DIRTY_SHADER | ETNA_DIRTY_VS_UNIFORMS;
}

void etna_shader_init(struct pipe_context *pctx)
{
    pctx->create_fs_state = etna_create_shader_state;
    pctx->bind_fs_state = etna_bind_fs_state;
    pctx->delete_fs_state = etna_delete_shader_state;
    pctx->create_vs_state = etna_create_shader_state;
    pctx->bind_vs_state = etna_bind_vs_state;
    pctx->delete_vs_state = etna_delete_shader_state;
    pctx->set_constant_buffer = etna_set_constant_buffer;
}
