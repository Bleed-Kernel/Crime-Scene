#include <fcntl.h>
#include <string.h>
#include <syscalls.h>
#include <battery.h>

#define POWER_PATH      "/dev/power"
#define POWER_BATTERY   0xBA77BA77UL

static int power_fd = -1;

int battery_read(battery_t *out) {
    memset(out, 0, sizeof(*out));

    if (power_fd < 0)
        power_fd = _open(POWER_PATH, O_RDONLY);
    if (power_fd < 0 || _ioctl(power_fd, POWER_BATTERY, out) != 0)
        return 0;
    return out->present;
}
