#pragma once
#include <stdint.h>
#include <stddef.h>

// the whole file, the caller frees it. NULL if it isnt there, has nothing in it or is too big
uint8_t *file_read(const char *path, size_t *size_out);
