/**
 *  @file   defines.h
 * 
 *  Define common compiler / platform agnostic macros 
 */

#ifndef DEFINES_H_
#define DEFINES_H_

#include <stdint.h>


// Set up compiler defines

#if defined(_MSC_VER)
#define COMPILER_MSVC 1
#endif

#if defined(__GNUC__) && !defined(__clang__)
#define COMPILER_GCC 1
#endif

#if defined(__clang__)
#define COMPILER_CLANG 1
#endif

#if COMPILER_MSVC + COMPILER_GCC + COMPILER_CLANG != 1
#error Must define exactly one compiler
#endif


// Compiler-specific macros

#if COMPILER_CLANG
#define UNREACHABLE()   __builtin_unreachable()
#define ABORT()         __builtin_trap()
#define UNUSED_FN       __attribute__((unused))
#define SECTION(s)      __attribute__((used, section(s)))
#endif // if COMPILER_CLANG

#if COMPILER_MSVC
#define UNREACHABLE()   __assume(0)
#define ABORT()         __debugbreak()
#define UNUSED_FN
#define SECTION(s)      __pragma(section(s)); __declspec(allocate(s))
#endif // if COMPILER_MSVC

#if COMPILER_GCC
#define UNREACHABLE()   __builtin_unreachable()
#define ABORT()         __builtin_trap()
#define UNUSED_FN       __attribute__((unused))
#define SECTION(s)      __attribute__((used, section(s)))
#endif // if COMPILER_GCC


// Common macros

#define ASSERT(cond) ((cond) || (ABORT(), 1))
#define UNUSED(x) ((void)(x))

#define STRINGIFY(x) STRINGIFY_IMPL(x)
#define STRINGIFY_IMPL(x) #x


// Constants

#define invalid_index 0xFFFFFFFFU


// Min/max

static inline int32_t math_min_int32(int32_t a, int32_t b) { return a < b ? a : b; }
static inline uint32_t math_min_uint32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static inline float math_min_float(float a, float b) { return a < b ? a : b; }
static inline double math_min_double(double a, double b) { return a < b ? a : b; }

static inline int32_t math_max_int32(int32_t a, int32_t b) { return a > b ? a : b; }
static inline uint32_t math_max_uint32(uint32_t a, uint32_t b) { return a > b ? a : b; }
static inline float math_max_float(float a, float b) { return a > b ? a : b; }
static inline double math_max_double(double a, double b) { return a > b ? a : b; }


#endif // ifndef DEFINES_H_
