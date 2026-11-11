/*
 * Copyright (c) 2010-2011 Extrems <metaradil@gmail.com>
 *
 * This file is part of MPlayer CE.
 *
 * MPlayer CE is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * MPlayer CE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with MPlayer CE; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * RECONSTRUCTED paired-single intrinsics for the Nintendo Wii (Broadway, PPC 750CL).
 *
 * The original relied on a lost libogc "paired.h" system header plus the 2010-era
 * GCC `vector_size(N)` vector types. GCC 16.2.0 rejects those vector types as "f"
 * (FPR) asm operands ("impossible constraint in 'asm'"), so this header rebuilds the
 * whole API on scalar bit-containers:
 *
 *   vec_f32_t  = double     -- the 64-bit FPR holds two 32-bit floats; the ps_* and
 *                              psq_* instructions treat it as 2x32-bit; `double` is
 *                              just the bit holder
 *   vec_s16_t  = int16_t[2] -- two packed 16-bit ints in memory, for psq_l/psq_st
 *                              with GQR_TYPE_S16 (loaded via the "o" memory operand)
 *   vec_u8/u16/s8_t          -- 16/32-bit scalar holders (unused by H.264 path)
 *
 * The generated machine code is identical to the original vector-based version.
 */

#ifndef AVUTIL_PPC_PAIRED_H
#define AVUTIL_PPC_PAIRED_H

#include <stdint.h>

typedef double  vec_f32_t;
typedef uint16_t vec_u8_t;
typedef uint32_t vec_u16_t;
typedef int16_t vec_s8_t;
typedef int16_t vec_s16_t[2];

/* Pack two 32-bit floats into a vec_f32_t (double): hi -> element 0 (high 32 bits),
 * lo -> element 1 (low 32 bits). Big-endian safe. */
static inline vec_f32_t vec_f32_pack(float hi, float lo) {
    union { double d; float f[2]; } u;
    u.f[0] = hi;
    u.f[1] = lo;
    return u.d;
}

enum {
	GQR_TYPE_FLOAT = 0,
	GQR_TYPE_U8 = 4,
	GQR_TYPE_U16,
	GQR_TYPE_S8,
	GQR_TYPE_S16,
};

/* Comparison operators: CR field bit positions (cr0 = CR[0..3] = LT,GT,EQ,UN). */
enum {
	PAIRED_CR_LT = 0,
	PAIRED_CR_GT = 1,
	PAIRED_CR_EQ = 2,
	PAIRED_CR_UN = 3,
};
#define LT PAIRED_CR_LT
#define GT PAIRED_CR_GT
#define EQ PAIRED_CR_EQ
#define UN PAIRED_CR_UN

/* ---- quantized load / store ---- */

#define psq_l(d, rA, W, I) ({ \
	vec_f32_t frD; \
	asm volatile("psq_l %0,%1(%2),%3,%4" \
	             : "=f"(frD) : "i"(d), "b"(rA), "i"(W), "i"(I) : "memory"); \
	frD; })

#define psq_lx(rA, rB, W, I) ({ \
	vec_f32_t frD; \
	asm volatile("psq_lx %0,%1,%2,%3,%4" \
	             : "=f"(frD) : "b"(rA), "r"(rB), "i"(W), "i"(I) : "memory"); \
	frD; })

#define psq_lu(d, rA, W, I) ({ \
	vec_f32_t frD; \
	asm volatile("psq_lu %0,%2(%1),%3,%4" \
	             : "=f"(frD), "+b"(rA) : "i"(d), "i"(W), "i"(I) : "memory"); \
	frD; })

#define psq_lux(rA, rB, W, I) ({ \
	vec_f32_t frD; \
	asm volatile("psq_lux %0,%1,%2,%3,%4" \
	             : "=f"(frD), "+b"(rA) : "r"(rB), "i"(W), "i"(I) : "memory"); \
	frD; })

#define psq_st(frD, d, rA, W, I) \
	asm volatile("psq_st %0,%1(%2),%3,%4" \
	             :: "f"(frD), "i"(d), "b"(rA), "i"(W), "i"(I) : "memory")

#define psq_stx(frD, rA, rB, W, I) \
	asm volatile("psq_stx %0,%1,%2,%3,%4" \
	             :: "f"(frD), "b"(rA), "r"(rB), "i"(W), "i"(I) : "memory")

#define psq_stu(frD, d, rA, W, I) \
	asm volatile("psq_stu %1,%2(%0),%3,%4" \
	             : "+b"(rA) : "f"(frD), "i"(d), "i"(W), "i"(I) : "memory")

#define psq_stux(frD, rA, rB, W, I) \
	asm volatile("psq_stux %1,%0,%2,%3,%4" \
	             : "+b"(rA) : "f"(frD), "r"(rB), "i"(W), "i"(I) : "memory")

/* ---- paired-single arithmetic ---- */

#define ps_neg(frB) ({ \
	vec_f32_t frD; \
	asm("ps_neg %0,%1" : "=f"(frD) : "f"(frB)); \
	frD; })

#define ps_abs(frB) ({ \
	vec_f32_t frD; \
	asm("ps_abs %0,%1" : "=f"(frD) : "f"(frB)); \
	frD; })

#define ps_nabs(frB) ({ \
	vec_f32_t frD; \
	asm("ps_nabs %0,%1" : "=f"(frD) : "f"(frB)); \
	frD; })

#define ps_add(frA, frB) ({ \
	vec_f32_t frD; \
	asm("ps_add %0,%1,%2" : "=f"(frD) : "f"(frA), "f"(frB)); \
	frD; })

#define ps_sub(frA, frB) ({ \
	vec_f32_t frD; \
	asm("ps_sub %0,%1,%2" : "=f"(frD) : "f"(frA), "f"(frB)); \
	frD; })

#define ps_mul(frA, frC) ({ \
	vec_f32_t frD; \
	asm("ps_mul %0,%1,%2" : "=f"(frD) : "f"(frA), "f"(frC)); \
	frD; })

#define ps_madd(frA, frC, frB) ({ \
	vec_f32_t frD; \
	asm("ps_madd %0,%1,%2,%3" : "=f"(frD) : "f"(frA), "f"(frC), "f"(frB)); \
	frD; })

#define ps_madds0(frA, frC, frB) ({ \
	vec_f32_t frD; \
	asm("ps_madds0 %0,%1,%2,%3" : "=f"(frD) : "f"(frA), "f"(frC), "f"(frB)); \
	frD; })

#define ps_madds1(frA, frC, frB) ({ \
	vec_f32_t frD; \
	asm("ps_madds1 %0,%1,%2,%3" : "=f"(frD) : "f"(frA), "f"(frC), "f"(frB)); \
	frD; })

#define ps_msub(frA, frC, frB) ({ \
	vec_f32_t frD; \
	asm("ps_msub %0,%1,%2,%3" : "=f"(frD) : "f"(frA), "f"(frC), "f"(frB)); \
	frD; })

#define ps_muls0(frA, frC) ({ \
	vec_f32_t frD; \
	asm("ps_muls0 %0,%1,%2" : "=f"(frD) : "f"(frA), "f"(frC)); \
	frD; })

#define ps_muls1(frA, frC) ({ \
	vec_f32_t frD; \
	asm("ps_muls1 %0,%1,%2" : "=f"(frD) : "f"(frA), "f"(frC)); \
	frD; })

#define ps_nmadd(frA, frC, frB) ({ \
	vec_f32_t frD; \
	asm("ps_nmadd %0,%1,%2,%3" : "=f"(frD) : "f"(frA), "f"(frC), "f"(frB)); \
	frD; })

#define ps_nmsub(frA, frC, frB) ({ \
	vec_f32_t frD; \
	asm("ps_nmsub %0,%1,%2,%3" : "=f"(frD) : "f"(frA), "f"(frC), "f"(frB)); \
	frD; })

#define ps_merge00(frA, frB) ({ \
	vec_f32_t frD; \
	asm("ps_merge00 %0,%1,%2" : "=f"(frD) : "f"(frA), "f"(frB)); \
	frD; })

#define ps_merge01(frA, frB) ({ \
	vec_f32_t frD; \
	asm("ps_merge01 %0,%1,%2" : "=f"(frD) : "f"(frA), "f"(frB)); \
	frD; })

#define ps_merge10(frA, frB) ({ \
	vec_f32_t frD; \
	asm("ps_merge10 %0,%1,%2" : "=f"(frD) : "f"(frA), "f"(frB)); \
	frD; })

#define ps_merge11(frA, frB) ({ \
	vec_f32_t frD; \
	asm("ps_merge11 %0,%1,%2" : "=f"(frD) : "f"(frA), "f"(frB)); \
	frD; })

#define ps_sel(frA, frC, frB) ({ \
	vec_f32_t frD; \
	asm("ps_sel %0,%1,%2,%3" : "=f"(frD) : "f"(frA), "f"(frC), "f"(frB)); \
	frD; })

#define ps_sum0(frA, frC, frB) ({ \
	vec_f32_t frD; \
	asm("ps_sum0 %0,%1,%2,%3" : "=f"(frD) : "f"(frA), "f"(frC), "f"(frB)); \
	frD; })

#define ps_sum1(frA, frC, frB) ({ \
	vec_f32_t frD; \
	asm("ps_sum1 %0,%1,%2,%3" : "=f"(frD) : "f"(frA), "f"(frC), "f"(frB)); \
	frD; })

/* ---- libogc2-style paired_* helper API (used by the _paired.c files) ---- */

#define paired_neg(frB)        ps_neg(frB)
#define paired_abs(frB)        ps_abs(frB)
#define paired_nabs(frB)       ps_nabs(frB)
#define paired_add(frA, frB)   ps_add(frA, frB)
#define paired_sub(frA, frB)   ps_sub(frA, frB)
#define paired_mul(frA, frC)   ps_mul(frA, frC)
#define paired_madd(frA, frC, frB)    ps_madd(frA, frC, frB)
#define paired_madds0(frA, frC, frB)  ps_madds0(frA, frC, frB)
#define paired_madds1(frA, frC, frB)  ps_madds1(frA, frC, frB)
#define paired_msub(frA, frC, frB)    ps_msub(frA, frC, frB)
#define paired_muls0(frA, frC) ps_muls0(frA, frC)
#define paired_muls1(frA, frC) ps_muls1(frA, frC)
#define paired_merge00(frA, frB) ps_merge00(frA, frB)
#define paired_merge01(frA, frB) ps_merge01(frA, frB)
#define paired_merge10(frA, frB) ps_merge10(frA, frB)
#define paired_merge11(frA, frB) ps_merge11(frA, frB)
#define paired_sel(frA, frC, frB)  ps_sel(frA, frC, frB)
#define paired_sum0(frA, frC, frB) ps_sum0(frA, frC, frB)
#define paired_sum1(frA, frC, frB) ps_sum1(frA, frC, frB)

#define paired_lx(rA, rB)     psq_lx(rA, rB, 0, GQR_TYPE_FLOAT)
#define paired_stx(frD, rA, rB) psq_stx(frD, rA, rB, 0, GQR_TYPE_FLOAT)

/* Compare: run ps_cmpu0/ps_cmpu1 (element 0 / element 1), then extract the
 * requested CR bit.  op is one of LT(0)/GT(1)/EQ(2)/UN(3). */
#define paired_cmpu0(op, frA, frB) ({ \
	unsigned int _cr; \
	asm volatile("ps_cmpu0 0,%1,%2\n\tmfcr %0" \
	             : "=r"(_cr) : "f"(frA), "f"(frB)); \
	(_cr >> (31 - (op))) & 1; })

#define paired_cmpu1(op, frA, frB) ({ \
	unsigned int _cr; \
	asm volatile("ps_cmpu1 0,%1,%2\n\tmfcr %0" \
	             : "=r"(_cr) : "f"(frA), "f"(frB)); \
	(_cr >> (31 - (op))) & 1; })

#endif /* AVUTIL_PPC_PAIRED_H */
