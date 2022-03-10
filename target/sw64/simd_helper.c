#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"

#undef DEBUG_SIMD
static inline uint32_t *get_element_w(CPUSW64State *env, uint64_t ra,
    int index)
{
    return (uint32_t*)&env->fr[ra + (index / 2) * 32] + (index % 2);
}

static inline uint64_t *get_element_l(CPUSW64State *env, uint64_t ra,
    int index)
{
    return &env->fr[ra + index * 32];
}

void helper_srlow(CPUSW64State *env, uint64_t ra, uint64_t rc, uint64_t shift)
{
    int i;
    int adden;
    int dest, src;
    adden = shift >> 6;
    shift &= 0x3f;
#ifdef DEBUG_SIMD
    printf("right shift = %ld adden = %d\n", shift, adden);
    printf("in_fr[%ld]:", ra);
    for (i = 3 ; i >= 0; i--) {
	    printf("%016lx ", env->fr[ra + 32 * i]);
    }
    printf("\n");
#endif

    for (i = 0; (i + adden) < 4; i++) {
        dest = i * 32 + rc;
        src = (i + adden) * 32 + ra;
        env->fr[dest] = env->fr[src] >> shift;
        if (((i + adden) < 3) && (shift != 0))
            env->fr[dest] |= (env->fr[src + 32] << (64 - shift));
    }

    for (; i < 4; i++) {
        env->fr[rc + i * 32] = 0;
    }
#ifdef DEBUG_SIMD
    printf("out_fr[%ld]:", rc);
    for (i = 3 ; i >= 0; i--) {
	    printf("%016lx ", env->fr[rc + 32 * i]);
    }
    printf("\n");
#endif
}

void helper_sllow(CPUSW64State *env, uint64_t ra, uint64_t rc, uint64_t shift)
{
    int i;
    int adden;
    int dest, src;
    adden = shift >> 6;
    shift &= 0x3f;
#ifdef DEBUG_SIMD
    printf("left shift = %ld adden = %d\n", shift, adden);
    printf("in_fr[%ld]:", ra);
    for (i = 3 ; i >= 0; i--) {
	    printf("%016lx ", env->fr[ra + 32 * i]);
    }
    printf("\n");
#endif

    for (i = 3; (i - adden) >= 0; i--) {
        dest = i * 32 + rc;
        src = (i - adden) * 32 + ra;
        env->fr[dest] = env->fr[src] << shift;
        if (((i - adden) > 0) && (shift != 0))
            env->fr[dest] |= (env->fr[src - 32] >> (64 - shift));
    }
    for (; i >= 0; i--) {
        env->fr[rc + i * 32] = 0;
    }
#ifdef DEBUG_SIMD
    printf("out_fr[%ld]:", rc);
    for (i = 3 ; i >= 0; i--) {
	    printf("%016lx ", env->fr[rc + 32 * i]);
    }
    printf("\n");
#endif
}

static uint64_t do_logzz(uint64_t va, uint64_t vb, uint64_t vc, uint64_t zz)
{
    int i;
    uint64_t ret = 0;
    int index;

    for (i = 0; i < 64; i++) {
        index = (((va >> i) & 1) << 2) | (((vb >> i) & 1) << 1) | ((vc >> i) & 1);
        ret |= ((zz >> index) & 1) << i;
    }

    return ret;
}

void helper_vlogzz(CPUSW64State *env, uint64_t args, uint64_t rd, uint64_t zz)
{
    int i;
    int ra, rb, rc;
    ra = args >> 16;
    rb = (args >> 8) & 0xff;
    rc = args & 0xff;
#ifdef DEBUG_SIMD
    printf("zz = %lx\n", zz);
    printf("in_fr[%d]:", ra);
    for (i = 3 ; i >= 0; i--) {
	    printf("%016lx ", env->fr[ra + 32 * i]);
    }
    printf("\n");
    printf("in_fr[%d]:", rb);
    for (i = 3 ; i >= 0; i--) {
	    printf("%016lx ", env->fr[rb + 32 * i]);
    }
    printf("\n");
    printf("in_fr[%d]:", rc);
    for (i = 3 ; i >= 0; i--) {
	    printf("%016lx ", env->fr[rc + 32 * i]);
    }
    printf("\n");
#endif
    for (i = 0; i < 4; i++) {
        env->fr[rd + i * 32] = do_logzz(env->fr[ra + i * 32], env->fr[rb + i * 32],
            env->fr[rc + i * 32], zz);
    }
#ifdef DEBUG_SIMD
    printf("out_fr[%ld]:", rd);
    for (i = 3 ; i >= 0; i--) {
	    printf("%016lx ", env->fr[rd + 32 * i]);
    }
    printf("\n");
#endif
}

void helper_v_print(CPUSW64State *env, uint64_t v)
{
    printf("PC[%lx]: fr[%lx]:\n", GETPC(), v);
}

void helper_vconw(CPUSW64State *env, uint64_t args, uint64_t rd,
    uint64_t byte4_len)
{
    int ra, rb;
    int count;
    int i;
    uint32_t *ptr_dst, *ptr_src;
    uint32_t tmp[8];

    ra = (args >> 8) & 0xff;
    rb = args & 0xff;
    count = 8 - byte4_len;

    for (i = 0; i < 8; i++) {
        ptr_dst = get_element_w(env, rd, i);
        if (i < count) {
            ptr_src = get_element_w(env, ra, i + byte4_len);
        } else {
            ptr_src = get_element_w(env, rb, i - count);
        }
        tmp[i] = *ptr_src;
    }
    for (i = 0; i < 8; i++) {
        ptr_dst = get_element_w(env, rd, i);
        *ptr_dst = tmp[i];
    }
}

void helper_vcond(CPUSW64State *env, uint64_t args, uint64_t rd,
    uint64_t byte8_len)
{
    int ra, rb;
    int count;
    int i;
    uint64_t *ptr_dst, *ptr_src;
    uint64_t tmp[8];

    ra = (args >> 8) & 0xff;
    rb = args & 0xff;
    count = 4 - byte8_len;

    for (i = 0; i < 4; i++) {
        if (i < count) {
            ptr_src = get_element_l(env, ra, i + byte8_len);
        } else {
            ptr_src = get_element_l(env, rb, i - count);
        }
        tmp[i] = *ptr_src;
    }
    for (i = 0; i < 4; i++) {
        ptr_dst = get_element_l(env, rd, i + byte8_len);
        *ptr_dst = tmp[i];
    }
}

void helper_vshfw(CPUSW64State *env, uint64_t args, uint64_t rd, uint64_t vc)
{
    int ra, rb;
    int i;
    uint32_t *ptr_dst, *ptr_src;
    uint32_t tmp[8];
    int flag, idx;

    ra = (args >> 8) & 0xff;
    rb = args & 0xff;

    for (i = 0; i < 8; i++) {
        flag = (vc >> (i * 4)) & 0x8;
        idx = (vc >> (i * 4)) & 0x7;
        if (flag == 0) {
            ptr_src = get_element_w(env, ra, idx);
        } else {
            ptr_src = get_element_w(env, rb, idx);
        }
        tmp[i] = *ptr_src;
    }
    for (i = 0; i < 8; i++) {
        ptr_dst = get_element_w(env, rd, i);
        *ptr_dst = tmp[i];
    }
}

uint64_t helper_ctlzow(CPUSW64State *env, uint64_t ra)
{
    int i, j;
    uint64_t val;
    uint64_t ctlz = 0;

    for (j = 3; j >= 0; j--) {
        val = env->fr[ra + 32 * j];
        for (i = 63; i >= 0; i--) {
            if ((val >> i) & 1)
                return ctlz << 29;
            else
                ctlz++;
        }
    }
    return ctlz << 29;
}

void helper_vucaddw(CPUSW64State *env, uint64_t ra, uint64_t rb, uint64_t rc)
{
    int a, b, c;
    int ret;
    int i;

    for (i = 0; i < 4; i++) {
        a = (int)(env->fr[ra + i * 32] & 0xffffffff);
        b = (int)(env->fr[rb + i * 32] & 0xffffffff);
        c = a + b;
        if ((c ^ a) < 0 && (c ^ b) < 0) {
            if (a < 0)
                c = 0x80000000;
            else
                c = 0x7fffffff;
        }
        ret = c;

        a = (int)(env->fr[ra + i * 32] >> 32);
        b = (int)(env->fr[rb + i * 32] >> 32);
        c = a + b;
        if ((c ^ a) < 0 && (c ^ b) < 0) {
            if (a < 0)
                c = 0x80000000;
            else
                c = 0x7fffffff;
        }
        env->fr[rc + i * 32] = ((uint64_t)(uint32_t)c << 32) |
            (uint64_t)(uint32_t)ret;
    }
}

void helper_vucaddwi(CPUSW64State *env, uint64_t ra, uint64_t vb, uint64_t rc)
{
    int a, b, c;
    int ret;
    int i;

    b = (int)vb;
    for (i = 0; i < 4; i++) {
        a = (int)(env->fr[ra + i * 32] & 0xffffffff);
        c = a + b;
        if ((c ^ a) < 0 && (c ^ b) < 0) {
            if (a < 0)
                c = 0x80000000;
            else
                c = 0x7fffffff;
        }
        ret = c;

        a = (int)(env->fr[ra + i * 32] >> 32);
        c = a + b;
        if ((c ^ a) < 0 && (c ^ b) < 0) {
            if (a < 0)
                c = 0x80000000;
            else
                c = 0x7fffffff;
        }
        env->fr[rc + i * 32] = ((uint64_t)(uint32_t)c << 32) |
            (uint64_t)(uint32_t)ret;
    }
}

void helper_vucsubw(CPUSW64State *env, uint64_t ra, uint64_t rb, uint64_t rc)
{
    int a, b, c;
    int ret;
    int i;

    for (i = 0; i < 4; i++) {
        a = (int)(env->fr[ra + i * 32] & 0xffffffff);
        b = (int)(env->fr[rb + i * 32] & 0xffffffff);
        c = a - b;
        if ((b ^ a) < 0 && (c ^ a) < 0) {
            if (a < 0)
                c = 0x80000000;
            else
                c = 0x7fffffff;
        }
        ret = c;

        a = (int)(env->fr[ra + i * 32] >> 32);
        b = (int)(env->fr[rb + i * 32] >> 32);
        c = a - b;
        if ((b ^ a) < 0 && (c ^ a) < 0) {
            if (a < 0)
                c = 0x80000000;
            else
                c = 0x7fffffff;
        }
        env->fr[rc + i * 32] = ((uint64_t)(uint32_t)c << 32) |
            (uint64_t)(uint32_t)ret;
    }
}

void helper_vucsubwi(CPUSW64State *env, uint64_t ra, uint64_t vb, uint64_t rc)
{
    int a, b, c;
    int ret;
    int i;

    b = (int)vb;
    for (i = 0; i < 4; i++) {
        a = (int)(env->fr[ra + i * 32] & 0xffffffff);
        c = a - b;
        if ((b ^ a) < 0 && (c ^ a) < 0) {
            if (a < 0)
                c = 0x80000000;
            else
                c = 0x7fffffff;
        }
        ret = c;

        a = (int)(env->fr[ra + i * 32] >> 32);
        c = a - b;
        if ((b ^ a) < 0 && (c ^ a) < 0) {
            if (a < 0)
                c = 0x80000000;
            else
                c = 0x7fffffff;
        }
        env->fr[rc + i * 32] = ((uint64_t)(uint32_t)c << 32) |
            (uint64_t)(uint32_t)ret;
    }
}

void helper_vucaddh(CPUSW64State *env, uint64_t ra, uint64_t rb, uint64_t rc)
{
    short a, b, c;
    uint64_t ret;
    int i, j;

    for (i = 0; i < 4; i++) {
        ret = 0;
        for (j = 0; j < 4; j++) {
            a = (short)((env->fr[ra + i * 32] >> (j * 16)) & 0xffff);
            b = (short)((env->fr[rb + i * 32] >> (j * 16)) & 0xffff);
            c = a + b;
            if ((c ^ a) < 0 && (c ^ b) < 0) {
                if (a < 0)
                    c = 0x8000;
                else
                    c = 0x7fff;
            }
            ret |= ((uint64_t)(uint16_t)c) << (j * 16);
        }
        env->fr[rc + i * 32] = ret;
    }
}

void helper_vucaddhi(CPUSW64State *env, uint64_t ra, uint64_t vb, uint64_t rc)
{
    short a, b, c;
    uint64_t ret;
    int i, j;

    b = (short)vb;
    for (i = 0; i < 4; i++) {
        ret = 0;
        for (j = 0; j < 4; j++) {
            a = (short)((env->fr[ra + i * 32] >> (j * 16)) & 0xffff);
            c = a + b;
            if ((c ^ a) < 0 && (c ^ b) < 0) {
                if (a < 0)
                    c = 0x8000;
                else
                    c = 0x7fff;
            }
            ret |= ((uint64_t)(uint16_t)c) << (j * 16);
        }
        env->fr[rc + i * 32] = ret;
    }
}

void helper_vucsubh(CPUSW64State *env, uint64_t ra, uint64_t rb, uint64_t rc)
{
    short a, b, c;
    uint64_t ret;
    int i, j;

    for (i = 0; i < 4; i++) {
        ret = 0;
        for (j = 0; j < 4; j++) {
            a = (short)((env->fr[ra + i * 32] >> (j * 16)) & 0xffff);
            b = (short)((env->fr[rb + i * 32] >> (j * 16)) & 0xffff);
            c = a - b;
            if ((b ^ a) < 0 && (c ^ a) < 0) {
                if (a < 0)
                    c = 0x8000;
                else
                    c = 0x7fff;
            }
            ret |= ((uint64_t)(uint16_t)c) << (j * 16);
        }
        env->fr[rc + i * 32] = ret;
    }
}

void helper_vucsubhi(CPUSW64State *env, uint64_t ra, uint64_t vb, uint64_t rc)
{
    short a, b, c;
    uint64_t ret;
    int i, j;

    b = (short)vb;
    for (i = 0; i < 4; i++) {
        ret = 0;
        for (j = 0; j < 4; j++) {
            a = (short)((env->fr[ra + i * 32] >> (j * 16)) & 0xffff);
            c = a - b;
            if ((b ^ a) < 0 && (c ^ a) < 0) {
                if (a < 0)
                    c = 0x8000;
                else
                    c = 0x7fff;
            }
            ret |= ((uint64_t)(uint16_t)c) << (j * 16);
        }
        env->fr[rc + i * 32] = ret;
    }
}

void helper_vucaddb(CPUSW64State *env, uint64_t ra, uint64_t rb, uint64_t rc)
{
    int8_t a, b, c;
    uint64_t ret;
    int i, j;

    for (i = 0; i < 4; i++) {
        ret = 0;
        for (j = 0; j < 8; j++) {
            a = (int8_t)((env->fr[ra + i * 32] >> (j * 8)) & 0xff);
            b = (int8_t)((env->fr[rb + i * 32] >> (j * 8)) & 0xff);
            c = a + b;
            if ((c ^ a) < 0 && (c ^ b) < 0) {
                if (a < 0)
                    c = 0x80;
                else
                    c = 0x7f;
            }
            ret |= ((uint64_t)(uint8_t)c) << (j * 8);
        }
        env->fr[rc + i * 32] = ret;
    }
}

void helper_vucaddbi(CPUSW64State *env, uint64_t ra, uint64_t vb, uint64_t rc)
{
    int8_t a, b, c;
    uint64_t ret;
    int i, j;

    b = (int8_t)(vb & 0xff);
    for (i = 0; i < 4; i++) {
        ret = 0;
        for (j = 0; j < 8; j++) {
            a = (int8_t)((env->fr[ra + i * 32] >> (j * 8)) & 0xff);
            c = a + b;
            if ((c ^ a) < 0 && (c ^ b) < 0) {
                if (a < 0)
                    c = 0x80;
                else
                    c = 0x7f;
            }
            ret |= ((uint64_t)(uint8_t)c) << (j * 8);
        }
        env->fr[rc + i * 32] = ret;
    }
}

void helper_vucsubb(CPUSW64State *env, uint64_t ra, uint64_t rb, uint64_t rc)
{
    int8_t a, b, c;
    uint64_t ret;
    int i, j;

    for (i = 0; i < 4; i++) {
        ret = 0;
        for (j = 0; j < 8; j++) {
            a = (int8_t)((env->fr[ra + i * 32] >> (j * 8)) & 0xff);
            b = (int8_t)((env->fr[rb + i * 32] >> (j * 8)) & 0xff);
            c = a - b;
            if ((b ^ a) < 0 && (c ^ a) < 0) {
                if (a < 0)
                    c = 0x80;
                else
                    c = 0x7f;
            }
            ret |= ((uint64_t)(uint8_t)c) << (j * 8);
        }
        env->fr[rc + i * 32] = ret;
    }
}

void helper_vucsubbi(CPUSW64State *env, uint64_t ra, uint64_t vb, uint64_t rc)
{
    int8_t a, b, c;
    uint64_t ret;
    int i, j;

    b = (int8_t)(vb & 0xff);
    for (i = 0; i < 4; i++) {
        ret = 0;
        for (j = 0; j < 8; j++) {
            a = (int8_t)((env->fr[ra + i * 32] >> (j * 8)) & 0xffff);
            c = a - b;
            if ((b ^ a) < 0 && (c ^ a) < 0) {
                if (a < 0)
                    c = 0x80;
                else
                    c = 0x7f;
            }
            ret |= ((uint64_t)(uint8_t)c) << (j * 8);
        }
        env->fr[rc + i * 32] = ret;
    }
}
