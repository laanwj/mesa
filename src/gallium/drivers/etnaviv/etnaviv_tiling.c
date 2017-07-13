/*
 * Copyright (c) 2012-2017 Etnaviv Project
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

#include "etnaviv_tiling.h"

#include <stdint.h>
#include <stdio.h>

/*** Fallback implementation */
#define TEX_TILE_WIDTH (4)
#define TEX_TILE_HEIGHT (4)
#define TEX_TILE_WORDS (TEX_TILE_WIDTH * TEX_TILE_HEIGHT)

#define DO_TILE(type)                                                   \
   src_stride /= sizeof(type);                                          \
   dst_stride = (dst_stride * TEX_TILE_HEIGHT) / sizeof(type);          \
   for (unsigned srcy = 0; srcy < height; ++srcy) {                     \
      unsigned dsty = basey + srcy;                                     \
      unsigned ty = (dsty / TEX_TILE_HEIGHT) * dst_stride +             \
                    (dsty % TEX_TILE_HEIGHT) * TEX_TILE_WIDTH;          \
      for (unsigned srcx = 0; srcx < width; ++srcx) {                   \
         unsigned dstx = basex + srcx;                                  \
         ((type *)dest)[ty + (dstx / TEX_TILE_WIDTH) * TEX_TILE_WORDS + \
                        (dstx % TEX_TILE_WIDTH)] =                      \
            ((type *)src)[srcy * src_stride + srcx];                    \
      }                                                                 \
   }

#define DO_UNTILE(type)                                                   \
   src_stride = (src_stride * TEX_TILE_HEIGHT) / sizeof(type);            \
   dst_stride /= sizeof(type);                                            \
   for (unsigned dsty = 0; dsty < height; ++dsty) {                       \
      unsigned srcy = basey + dsty;                                       \
      unsigned sy = (srcy / TEX_TILE_HEIGHT) * src_stride +               \
                    (srcy % TEX_TILE_HEIGHT) * TEX_TILE_WIDTH;            \
      for (unsigned dstx = 0; dstx < width; ++dstx) {                     \
         unsigned srcx = basex + dstx;                                    \
         ((type *)dest)[dsty * dst_stride + dstx] =                       \
            ((type *)src)[sy + (srcx / TEX_TILE_WIDTH) * TEX_TILE_WORDS + \
                          (srcx % TEX_TILE_WIDTH)];                       \
      }                                                                   \
   }

static void
etna_texture_tile_fb(void *dest, void *src, unsigned basex, unsigned basey,
                  unsigned dst_stride, unsigned width, unsigned height,
                  unsigned src_stride, unsigned elmtsize)
{
   if (width == 0 || height == 0)
      return;
   if (elmtsize == 4) {
      DO_TILE(uint32_t)
   } else if (elmtsize == 2) {
      DO_TILE(uint16_t)
   } else if (elmtsize == 1) {
      DO_TILE(uint8_t)
   } else {
      printf("etna_texture_tile_fb: unhandled element size %i\n", elmtsize);
   }
}

static void
etna_texture_untile_fb(void *dest, void *src, unsigned basex, unsigned basey,
                    unsigned src_stride, unsigned width, unsigned height,
                    unsigned dst_stride, unsigned elmtsize)
{
   if (width == 0 || height == 0)
      return;
   if (elmtsize == 4) {
      DO_UNTILE(uint32_t);
   } else if (elmtsize == 2) {
      DO_UNTILE(uint16_t);
   } else if (elmtsize == 1) {
      DO_UNTILE(uint8_t);
   } else {
      printf("etna_texture_untile_fb: unhandled element size %i\n", elmtsize);
   }
}

/*** NEON specializations */

inline void tile32_1x_impl(void *gpu, const void *cpu, uint32_t cpu_stride)
{
    __asm__ volatile (
    "vld1.8    {d0,d1}, [%0], %2;\n"
    "vld1.8    {d2,d3}, [%0], %2;\n"
    "vld1.8    {d4,d5}, [%0], %2;\n"
    "vld1.8    {d6,d7}, [%0], %2;\n"
    "vstm %1,  {q0, q1, q2, q3};\n"
    : "=r"(cpu) /* changed */
    : "r"(gpu), "r"(cpu_stride), "0"(cpu)
    : "q0", "q1", "q2", "q3");
}

inline void tile32_2x_impl(void *gpu, const void *cpu, uint32_t cpu_stride)
{
    const void *cpunext = cpu + 16;
    __asm__ volatile (
    "vld1.8    {d0,d1}, [%0], %3;\n"
    "vld1.8    {d8,d9}, [%1], %3;\n"
    "vld1.8    {d2,d3}, [%0], %3;\n"
    "vld1.8    {d10,d11}, [%1], %3;\n"
    "vld1.8    {d4,d5}, [%0], %3;\n"
    "vld1.8    {d12,d13}, [%1], %3;\n"
    "vld1.8    {d6,d7}, [%0], %3;\n"
    "vld1.8    {d14,d15}, [%1], %3;\n"
    "vstm %2,  {q0, q1, q2, q3, q4, q5, q6, q7};\n"
    : "=r"(cpu), "=r"(cpunext)
    : "r"(gpu), "r"(cpu_stride), "0"(cpu), "1"(cpunext)
    : "q0", "q1", "q2", "q3", "q4", "q5", "q6", "q7");
}

inline void tile16_1x_impl(void *gpu, const void *cpu, uint32_t cpu_stride)
{
    __asm__ volatile (
    "vld1.8    {d0}, [%0], %2;\n"
    "vld1.8    {d1}, [%0], %2;\n"
    "vld1.8    {d2}, [%0], %2;\n"
    "vld1.8    {d3}, [%0], %2;\n"
    "vstm %1,  {q0, q1};\n"
    : "=r"(cpu) /* changed */
    : "r"(gpu), "r"(cpu_stride), "0"(cpu)
    : "q0", "q1");
}

inline void tile16_2x_impl(void *gpu, const void *cpu, uint32_t cpu_stride)
{
    const void *cpunext = cpu + 8;
    __asm__ volatile (
    /* load two adjacent tiles from untiled */
    "vld1.8    {d0}, [%0], %3;\n"
    "vld1.8    {d4}, [%1], %3;\n"
    "vld1.8    {d1}, [%0], %3;\n"
    "vld1.8    {d5}, [%1], %3;\n"
    "vld1.8    {d2}, [%0], %3;\n"
    "vld1.8    {d6}, [%1], %3;\n"
    "vld1.8    {d3}, [%0], %3;\n"
    "vld1.8    {d7}, [%1], %3;\n"
    /* store two adjacent tiles, tiled */
    "vstm %2,  {q0, q1, q2, q3};\n"
    : "=r"(cpu), "=r"(cpunext)
    : "r"(gpu), "r"(cpu_stride), "0"(cpu), "1"(cpunext)
    : "q0", "q1", "q2", "q3");
}

inline void tile8_1x_impl(void *gpu, const void *cpu, uint32_t cpu_stride)
{
    __asm__ volatile (
    "vld1.32    {d0[0]}, [%0], %2;\n"
    "vld1.32    {d0[1]}, [%0], %2;\n"
    "vld1.32    {d1[0]}, [%0], %2;\n"
    "vld1.32    {d1[1]}, [%0], %2;\n"
    "vstm %1,   {d0-d1};\n"
    : "=r"(cpu)
    : "r"(gpu), "r"(cpu_stride), "0"(cpu)
    : "q0", "q1");
}

inline void tile8_2x_impl(void *gpu, const void *cpu, uint32_t cpu_stride)
{
    __asm__ volatile (
    /* load two adjacent tiles, from untiled */
    "vld1.8    {d0}, [%0], %2;\n"
    "vld1.8    {d1}, [%0], %2;\n"
    "vld1.8    {d2}, [%0], %2;\n"
    "vld1.8    {d3}, [%0], %2;\n"
    /* Transpose:
     *   Start
     *   [d0]  x1  x0
     *   [d1]  x3  x2
     *   [d2]  x5  x4
     *   [d3]  x7  x6
     */
    "vtrn.32 d0, d1;\n"
    "vtrn.32 d2, d3;\n"
     /*  [d0]  x2  x0
     *   [d1]  x3  x1
     *   [d2]  x6  x4
     *   [d3]  x7  x5
     */
    "vswp d1, d2;\n"
     /*  [d0]  x2  x0
     *   [d1]  x6  x4
     *   [d2]  x3  x1
     *   [d3]  x7  x5
     */
    /* store two adjacent tiles, to tiled */
    "vstm %1,   {d0-d3};\n"
    : "=r"(cpu)
    : "r"(gpu), "r"(cpu_stride), "0"(cpu)
    : "q0", "q1");
}

inline void tile8_4x_impl(void *gpu, const void *cpu, uint32_t cpu_stride)
{
    __asm__ volatile (
    /* load four adjacent tiles, from untiled */
    "vld1.8    {d0,d1}, [%0], %2;\n"
    "vld1.8    {d2,d3}, [%0], %2;\n"
    "vld1.8    {d4,d5}, [%0], %2;\n"
    "vld1.8    {d6,d7}, [%0], %2;\n"
    /* Transpose:
     *   Start
     *   [q0]  x3  x2  x1  x0
     *   [q1]  x7  x6  x5  x4
     *   [q2] x11 x10  x9  x8
     *   [q3] x15 x14 x13 x12
     */
    "vtrn.32 q0, q1;\n"
    "vtrn.32 q2, q3;\n"
     /*  [q0]  x6  x2  x4  x0
     *   [q1]  x7  x3  x5  x1
     *   [q2] x14 x10 x12  x8
     *   [q3] x15 x11 x13  x9
     */
    "vswp d1, d4;\n"
    "vswp d3, d6;\n"
     /*  [q0] x12  x8  x4  x0
     *   [q1] x13  x9  x5  x1
     *   [q2] x14 x10  x6  x2
     *   [q3] x15 x11  x7  x3
     */
    /* store four adjacent tiles, to tiled */
    "vstm %1,   {q0, q1, q2, q3};\n"
    : "=r"(cpu)
    : "r"(gpu), "r"(cpu_stride), "0"(cpu)
    : "q0", "q1", "q2", "q3");
}

inline void untile32_1x_impl(const void *gpu, void *cpu, uint32_t cpu_stride)
{
    __asm__ volatile (
    "vldm %1,  {q0, q1, q2, q3};\n"
    "vst1.8    {d0,d1}, [%0], %2;\n"
    "vst1.8    {d2,d3}, [%0], %2;\n"
    "vst1.8    {d4,d5}, [%0], %2;\n"
    "vst1.8    {d6,d7}, [%0], %2;\n"
    : "=r"(cpu)
    : "r"(gpu), "r"(cpu_stride), "0"(cpu)
    : "q0", "q1", "q2", "q3");
}

inline void untile32_2x_impl(const void *gpu, void *cpu, uint32_t cpu_stride)
{
    void *cpunext = cpu + 16;
    __asm__ volatile (
    "vldm %2,  {q0, q1, q2, q3, q4, q5, q6, q7};\n"
    "vst1.8    {d0,d1}, [%0], %3;\n"
    "vst1.8    {d8,d9}, [%1], %3;\n"
    "vst1.8    {d2,d3}, [%0], %3;\n"
    "vst1.8    {d10,d11}, [%1], %3;\n"
    "vst1.8    {d4,d5}, [%0], %3;\n"
    "vst1.8    {d12,d13}, [%1], %3;\n"
    "vst1.8    {d6,d7}, [%0], %3;\n"
    "vst1.8    {d14,d15}, [%1], %3;\n"
    : "=r"(cpu), "=r"(cpunext)
    : "r"(gpu), "r"(cpu_stride), "0"(cpu), "1"(cpunext)
    : "q0", "q1", "q2", "q3", "q4", "q5", "q6", "q7");
}

inline void untile16_1x_impl(const void *gpu, void *cpu, uint32_t cpu_stride)
{
    __asm__ volatile (
    "vldm %1,  {q0, q1};\n"
    "vst1.8    {d0}, [%0], %2;\n"
    "vst1.8    {d1}, [%0], %2;\n"
    "vst1.8    {d2}, [%0], %2;\n"
    "vst1.8    {d3}, [%0], %2;\n"
    : "=r"(cpu)
    : "r"(gpu), "r"(cpu_stride), "0"(cpu)
    : "q0", "q1");
}

inline void untile16_2x_impl(const void *gpu, void *cpu, uint32_t cpu_stride)
{
    void *cpunext = cpu + 8;
    __asm__ volatile (
    /* load two adjacent tiles, tiled */
    "vldm %2,  {q0, q1, q2, q3};\n"
    /* store two adjacent tiles, untiled */
    "vst1.8    {d0}, [%0], %3;\n"
    "vst1.8    {d4}, [%1], %3;\n"
    "vst1.8    {d1}, [%0], %3;\n"
    "vst1.8    {d5}, [%1], %3;\n"
    "vst1.8    {d2}, [%0], %3;\n"
    "vst1.8    {d6}, [%1], %3;\n"
    "vst1.8    {d3}, [%0], %3;\n"
    "vst1.8    {d7}, [%1], %3;\n"
    : "=r"(cpu), "=r"(cpunext)
    : "r"(gpu), "r"(cpu_stride), "0"(cpu), "1"(cpunext)
    : "q0", "q1", "q2", "q3");
}

inline void untile8_1x_impl(const void *gpu, void *cpu, uint32_t cpu_stride)
{
    __asm__ volatile (
    "vldm %1,   {d0-d1};\n"
    "vst1.32    {d0[0]}, [%0], %2;\n"
    "vst1.32    {d0[1]}, [%0], %2;\n"
    "vst1.32    {d1[0]}, [%0], %2;\n"
    "vst1.32    {d1[1]}, [%0], %2;\n"
    : "=r"(cpu)
    : "r"(gpu), "r"(cpu_stride), "0"(cpu)
    : "q0", "q1");
}


inline void untile8_2x_impl(const void *gpu, void *cpu, uint32_t cpu_stride)
{
    __asm__ volatile (
    /* load two adjacent tiles, from tiled */
    "vldm %1,   {d0-d3};\n"
    /* Transpose:
     *   Start
     *   [d0]  x2  x0
     *   [d1]  x6  x4
     *   [d2]  x3  x1
     *   [d3]  x7  x5
     */
    "vswp d1, d2;\n"
     /*  [d0]  x2  x0
     *   [d1]  x3  x1
     *   [d2]  x6  x4
     *   [d3]  x7  x5
     */
    "vtrn.32 d0, d1;\n"
    "vtrn.32 d2, d3;\n"
     /*  [d0]  x1  x0
     *   [d1]  x3  x2
     *   [d2]  x5  x4
     *   [d3]  x7  x6
     */
    /* store two adjacent tiles, to untiled */
    "vst1.8    {d0}, [%0], %2;\n"
    "vst1.8    {d1}, [%0], %2;\n"
    "vst1.8    {d2}, [%0], %2;\n"
    "vst1.8    {d3}, [%0], %2;\n"
    : "=r"(cpu)
    : "r"(gpu), "r"(cpu_stride), "0"(cpu)
    : "q0", "q1");
}

inline void untile8_4x_impl(const void *gpu, void *cpu, uint32_t cpu_stride)
{
    __asm__ volatile (
    /* load four adjacent tiles, from tiled */
    "vldm %1,   {q0, q1, q2, q3};\n"
    /* Transpose:
     *   Start
     *   [q0] x12  x8  x4  x0
     *   [q1] x13  x9  x5  x1
     *   [q2] x14 x10  x6  x2
     *   [q3] x15 x11  x7  x3
     */
    "vswp d1, d4;\n"
    "vswp d3, d6;\n"
     /*  [q0]  x6  x2  x4  x0
     *   [q1]  x7  x3  x5  x1
     *   [q2] x14 x10 x12  x8
     *   [q3] x15 x11 x13  x9
     */
    "vtrn.32 q0, q1;\n"
    "vtrn.32 q2, q3;\n"
     /*  [q0]  x3  x2  x1  x0
     *   [q1]  x7  x6  x5  x4
     *   [q2] x11 x10  x9  x8
     *   [q3] x15 x14 x13 x12
     */
    /* store four adjacent tiles, to untiled */
    "vst1.8    {d0,d1}, [%0], %2;\n"
    "vst1.8    {d2,d3}, [%0], %2;\n"
    "vst1.8    {d4,d5}, [%0], %2;\n"
    "vst1.8    {d6,d7}, [%0], %2;\n"
    : "=r"(cpu)
    : "r"(gpu), "r"(cpu_stride), "0"(cpu)
    : "q0", "q1", "q2", "q3");
}

/*** Tile visitor functions */
#define TILE_FUNC(elmtsize,htiles,func) \
    static void func(void *gpu, const void *cpu, uint32_t gpu_stride, uint32_t cpu_stride, uint32_t width, uint32_t height) \
    { \
        for (uint32_t y=0; y<height; y+=4) { \
            void *gpu_tile = gpu; \
            const void *cpu_tile = cpu; \
            for (uint32_t x=0; x<width; x+=htiles*4) { \
                func##_impl(gpu_tile, cpu_tile, cpu_stride);\
                gpu_tile += htiles*elmtsize/8*16; \
                cpu_tile += htiles*elmtsize/8*4; \
            } \
            gpu += gpu_stride; \
            cpu += cpu_stride*4; \
        } \
    }

#define UNTILE_FUNC(elmtsize,htiles,func) \
    static void func(const void *gpu, void *cpu, uint32_t gpu_stride, uint32_t cpu_stride, uint32_t width, uint32_t height) \
    { \
        for (uint32_t y=0; y<height; y+=4) { \
            const void *gpu_tile = gpu; \
            void *cpu_tile = cpu; \
            for (uint32_t x=0; x<width; x+=htiles*4) { \
                func##_impl(gpu_tile, cpu_tile, cpu_stride);\
                gpu_tile += htiles*elmtsize/8*16; \
                cpu_tile += htiles*elmtsize/8*4; \
            } \
            gpu += gpu_stride; \
            cpu += cpu_stride*4; \
        } \
    }

TILE_FUNC(32, 1, tile32_1x);
TILE_FUNC(32, 2, tile32_2x);
TILE_FUNC(16, 1, tile16_1x);
TILE_FUNC(16, 2, tile16_2x);
TILE_FUNC(8, 1, tile8_1x);
TILE_FUNC(8, 2, tile8_2x);
TILE_FUNC(8, 4, tile8_4x);
UNTILE_FUNC(32, 1, untile32_1x);
UNTILE_FUNC(32, 2, untile32_2x);
UNTILE_FUNC(16, 1, untile16_1x);
UNTILE_FUNC(16, 2, untile16_2x);
UNTILE_FUNC(8, 1, untile8_1x);
UNTILE_FUNC(8, 2, untile8_2x);
UNTILE_FUNC(8, 4, untile8_4x);

/* Accelerated NEON implementation
 * srcx, srcy, width, height must be 4-aligned
 */
static void
etna_texture_tile_neon(void *dest, void *src, unsigned basex, unsigned basey,
                  unsigned dst_stride, unsigned width, unsigned height,
                  unsigned src_stride, unsigned elmtsize)
{
   if (width == 0 || height == 0)
      return;
   dest += basey*dst_stride + (basex/4)*4*4*elmtsize;
   if (elmtsize == 4) {
      if ((width & 7)==0) {
         tile32_2x(dest, src, dst_stride*4, src_stride, width, height);
      } else {
         tile32_1x(dest, src, dst_stride*4, src_stride, width, height);
      }
   } else if (elmtsize == 2) {
      if ((width & 7)==0) {
         tile16_2x(dest, src, dst_stride*4, src_stride, width, height);
      } else {
         tile16_1x(dest, src, dst_stride*4, src_stride, width, height);
      }
   } else if (elmtsize == 1) {
      if ((width & 15)==0) {
         tile8_4x(dest, src, dst_stride*4, src_stride, width, height);
      } else if ((width & 7)==0) {
         tile8_2x(dest, src, dst_stride*4, src_stride, width, height);
      } else {
         tile8_1x(dest, src, dst_stride*4, src_stride, width, height);
      }
   } else {
       printf("etna_texture_tile_neon: unhandled element size %i\n", elmtsize);
   }
}

/* accelerated NEON implementation
 * srcx, srcy, width, height must be 4-aligned
 */
static void
etna_texture_untile_neon(void *dest, void *src, unsigned basex, unsigned basey,
                    unsigned src_stride, unsigned width, unsigned height,
                    unsigned dst_stride, unsigned elmtsize)
{
   if (width == 0 || height == 0)
      return;
   src += basey*src_stride + (basex/4)*4*4*elmtsize;
   if (elmtsize == 4) {
      if ((width & 7)==0) {
         untile32_2x(src, dest, src_stride*4, dst_stride, width, height);
      } else {
         untile32_1x(src, dest, src_stride*4, dst_stride, width, height);
      }
   } else if (elmtsize == 2) {
      if ((width & 7)==0) {
         untile16_2x(src, dest, src_stride*4, dst_stride, width, height);
      } else {
         untile16_1x(src, dest, src_stride*4, dst_stride, width, height);
      }
   } else if (elmtsize == 1) {
      if ((width & 15)==0) {
         untile8_4x(src, dest, src_stride*4, dst_stride, width, height);
      } else if ((width & 7)==0) {
         untile8_2x(src, dest, src_stride*4, dst_stride, width, height);
      } else {
         untile8_1x(src, dest, src_stride*4, dst_stride, width, height);
      }
   } else {
       printf("etna_texture_untile_neon: unhandled element size %i\n", elmtsize);
   }
}

void
etna_texture_tile(void *dest, void *src, unsigned basex, unsigned basey,
                  unsigned dst_stride, unsigned width, unsigned height,
                  unsigned src_stride, unsigned elmtsize)
{
   /**
    * Use optimized NEON implementation to transfer whole tiles (region e),
    * transfer other parts by means of fallback implementation.
    *
    * basex,basey                        basex+width,basey
    * +-----------------------------------+
    * |                   a               |
    * +-----------------------------------+
    * |b |                e            | c|
    * |  |                             |  |
    * |  |                             |  |
    * |-----------------------------------+
    * |                   d               |
    * |-----------------------------------+
    * basex,basey+height                  basex+width,basey+height
    *
    * regions - a and d will be less than a tile high
    *         - b and c will be less than a tile wide
    */
   unsigned ex0 = (basex+3)&~3, ey0 = (basey+3)&~3, ex1 = (basex+width)&~3, ey1 = (basey+height)&~3;
   unsigned ax0 = basex, ay0 = basey, ax1 = basex+width, ay1 = ey0;
   unsigned bx0 = basex, by0 = ey0, bx1 = ex0, by1 = ey1;
   unsigned cx0 = ex1, cy0 = ey0, cx1 = basex+width, cy1 = ey1;
   unsigned dx0 = basex, dy0 = ey1, dx1 = basex+width, dy1 = basey+height;

#define SRCOFS(x,y) (src + ((y)-basey)*src_stride + ((x)-basex)*elmtsize)
   etna_texture_tile_fb(dest, SRCOFS(ax0,ay0),
      ax0, ay0, dst_stride, ax1-ax0, ay1-ay0, src_stride, elmtsize);
   etna_texture_tile_fb(dest, SRCOFS(bx0,by0),
      bx0, by0, dst_stride, bx1-bx0, by1-by0, src_stride, elmtsize);
   etna_texture_tile_fb(dest, SRCOFS(cx0,cy0),
      cx0, cy0, dst_stride, cx1-cx0, cy1-cy0, src_stride, elmtsize);
   etna_texture_tile_fb(dest, SRCOFS(dx0,dy0),
      dx0, dy0, dst_stride, dx1-dx0, dy1-dy0, src_stride, elmtsize);
   etna_texture_tile_neon(dest, SRCOFS(ex0,ey0),
      ex0, ey0, dst_stride, ex1-ex0, ey1-ey0, src_stride, elmtsize);
#undef SRCOFS
}

void
etna_texture_untile(void *dest, void *src, unsigned basex, unsigned basey,
                    unsigned src_stride, unsigned width, unsigned height,
                    unsigned dst_stride, unsigned elmtsize)
{
   unsigned ex0 = (basex+3)&~3, ey0 = (basey+3)&~3, ex1 = (basex+width)&~3, ey1 = (basey+height)&~3;
   unsigned ax0 = basex, ay0 = basey, ax1 = basex+width, ay1 = ey0;
   unsigned bx0 = basex, by0 = ey0, bx1 = ex0, by1 = ey1;
   unsigned cx0 = ex1, cy0 = ey0, cx1 = basex+width, cy1 = ey1;
   unsigned dx0 = basex, dy0 = ey1, dx1 = basex+width, dy1 = basey+height;

#define DSTOFS(x,y) (dest + ((y)-basey)*dst_stride + ((x)-basex)*elmtsize)
   etna_texture_untile_fb(DSTOFS(ax0,ay0), src,
      ax0, ay0, src_stride, ax1-ax0, ay1-ay0, dst_stride, elmtsize);
   etna_texture_untile_fb(DSTOFS(bx0,by0), src,
      bx0, by0, src_stride, bx1-bx0, by1-by0, dst_stride, elmtsize);
   etna_texture_untile_fb(DSTOFS(cx0,cy0), src,
      cx0, cy0, src_stride, cx1-cx0, cy1-cy0, dst_stride, elmtsize);
   etna_texture_untile_fb(DSTOFS(dx0,dy0), src,
      dx0, dy0, src_stride, dx1-dx0, dy1-dy0, dst_stride, elmtsize);
   etna_texture_untile_neon(DSTOFS(ex0,ey0), src,
      ex0, ey0, src_stride, ex1-ex0, ey1-ey0, dst_stride, elmtsize);
#undef DSTOFS
}
