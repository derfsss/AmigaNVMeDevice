/*
 * compat.c — Minimal C library substitutes for -nostartfiles drivers.
 *
 * GCC may emit calls to memset/memcpy even with -fno-tree-loop-distribute-patterns
 * when these functions are called directly in source. Providing them here
 * avoids a dependency on newlib (INewlib) which is not available in driver context.
 */

#include <exec/types.h>

void *memset(void *s, int c, __SIZE_TYPE__ n)
{
    UBYTE *p = (UBYTE *)s;
    while (n--)
        *p++ = (UBYTE)c;
    return s;
}

void *memcpy(void *dst, const void *src, __SIZE_TYPE__ n)
{
    UBYTE       *d = (UBYTE *)dst;
    const UBYTE *s = (const UBYTE *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}
