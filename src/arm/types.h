/* types.h - fixed-width types for the freestanding ARM7DI driver build. */
#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>

typedef uint8_t  u8;
typedef int8_t   s8;
typedef uint16_t u16;
typedef int16_t  s16;
typedef uint32_t u32;
typedef int32_t  s32;

/* Compile-time layout checks for structures that overlay sound RAM. */
#ifdef GHIDRA_TYPES
#define STATIC_ASSERT(c, m)
#else
#define STATIC_ASSERT(c, m) typedef char static_assert_##m[(c) ? 1 : -1]
#endif
#define OFFSET_CHECK(T, f, off) STATIC_ASSERT(__builtin_offsetof(T, f) == (off), T##_##f)
#define SIZE_CHECK(T, sz) STATIC_ASSERT(sizeof(T) == (sz), T##_size)

#endif
