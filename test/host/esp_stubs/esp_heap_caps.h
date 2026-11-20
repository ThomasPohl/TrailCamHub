#pragma once
#include <stddef.h>
#define MALLOC_CAP_DEFAULT 0
extern "C" size_t heap_caps_get_largest_free_block(unsigned caps);
extern "C" size_t heap_caps_get_free_size(unsigned caps);
