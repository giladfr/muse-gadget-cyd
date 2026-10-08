#pragma once
#include <stdlib.h>
#define MALLOC_CAP_DMA 1
static inline void *heap_caps_malloc(size_t n, int c){(void)c;return malloc(n);}
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
static inline size_t heap_caps_get_free_size(int c){(void)c;return 100000;}
static inline size_t heap_caps_get_largest_free_block(int c){(void)c;return 60000;}
