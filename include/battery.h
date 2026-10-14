#pragma once
#include <stdint.h>

// what the kernel fills in for POWER_DEVICE_BATTERY on /dev/power
typedef struct {
    uint8_t  present;
    uint8_t  charging;
    uint8_t  ac;
    uint8_t  percent;
    uint32_t voltage;       // mV
    uint32_t rate;          // mA
    uint32_t capacity;      // mAh left
    uint32_t full;          // mAh
} battery_t;

// 1 if theres a battery and out has it, 0 on a machine without one
int battery_read(battery_t *out);
