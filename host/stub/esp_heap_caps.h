// Host build: every capability is plain heap
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_32BIT    (1 << 1)
#define MALLOC_CAP_DMA      (1 << 3)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_SPIRAM   (1 << 10)
inline void * heap_caps_malloc(size_t size, uint32_t)              { return malloc(size); }
inline void * heap_caps_calloc(size_t n, size_t size, uint32_t)    { return calloc(n, size); }
inline void   heap_caps_free(void * p)                             { free(p); }
inline size_t heap_caps_get_free_size(uint32_t)                    { return 160 * 1024; }
inline size_t heap_caps_get_largest_free_block(uint32_t)           { return 110 * 1024; }
inline size_t heap_caps_get_minimum_free_size(uint32_t)            { return 100 * 1024; }
