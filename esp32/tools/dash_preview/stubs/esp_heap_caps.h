#pragma once
#include <stdlib.h>
#define MALLOC_CAP_DMA 1
static inline void *heap_caps_malloc(size_t n, int c){(void)c;return malloc(n);}
#define MALLOC_CAP_INTERNAL 2
static inline size_t heap_caps_get_free_size(int c){(void)c;return 100000;}
