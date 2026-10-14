#pragma once
#include <stdint.h>

#define FS_PER_MS 1000000000000ULL

// femtoseconds since boot. it wraps after a few hours so only ever subtract two of these
// this isnt the best way to do it at all but ill figure it out when it becomes a problem
uint64_t clock_now(void);
