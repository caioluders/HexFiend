#pragma once

#include <malloc.h>

static inline size_t malloc_good_size(size_t size) {
    return size;
}

static inline size_t malloc_size(const void *ptr) {
    return malloc_usable_size((void *)ptr);
}
