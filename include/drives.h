#pragma once

#define DRIVES_MAX      4

typedef struct {
    char dev[16];       // not whole parititon
    char name[32];
    char mount[64];
} drive_t;

// every usb stick plugged in right now, how many went into out
int drives_read(drive_t *out, int max);
int drive_eject(const char *dev);
