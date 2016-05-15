/*
 * Copyright (c) 2016 Etnaviv Project
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
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#include "etnaviv_uniforms.h"

#include "etnaviv_compiler.h"
#include "pipe/p_defines.h"
#include "util/u_math.h"

void etna_uniforms_write(const struct etna_shader_object *sobj,
        struct pipe_constant_buffer *cb, uint32_t *uniforms, unsigned *size)
{
    const struct etna_shader_uniform_info *uinfo = &sobj->uniforms;

    if (cb->user_buffer) {
        unsigned size = MIN2(cb->buffer_size, uinfo->const_count * 4);

        memcpy(uniforms, cb->user_buffer, size);
    }

    for (uint32_t i = 0; i < uinfo->imm_count; i++) {
        switch (uinfo->imm_contents[i]) {
        case ETNA_IMMEDIATE_CONSTANT:
            uniforms[i + uinfo->const_count] = uinfo->imm_data[i];
            break;

        case ETNA_IMMEDIATE_UNUSED:
            /* nothing to do */
            break;
        }
    }

    *size = uinfo->const_count + uinfo->imm_count;
}

void etna_set_shader_uniforms_dirty_flags(struct etna_shader_object *sobj)
{
    uint32_t dirty = 0;

    for (uint32_t i = 0; i < sobj->uniforms.imm_count; i++) {
        switch (sobj->uniforms.imm_contents[i]) {
        case ETNA_IMMEDIATE_UNUSED:
        case ETNA_IMMEDIATE_CONSTANT:
            break;
        }
    }

    sobj->uniforms_dirty_bits = dirty;
}
