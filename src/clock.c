#include <fcntl.h>
#include <syscalls.h>
#include <hpet.h>
#include <clock.h>

static int hpet_fd = -1;

uint64_t clock_now(void) {
    uint64_t now = 0;

    if (hpet_fd < 0)
        hpet_fd = _open("/dev/hpet", O_RDONLY);
    _ioctl(hpet_fd, HPET_IOCTL_GET_FEMTOSECONDS, &now);
    return now;
}
