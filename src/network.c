#include <fcntl.h>
#include <string.h>
#include <syscalls.h>
#include <network.h>

#define NET_PATH "/dev/net"

static int net_fd = -1;

int network_read(struct net_device_info *out) {
    memset(out, 0, sizeof(*out));

    if (net_fd < 0)
        net_fd = _open(NET_PATH, O_RDONLY);
    if (net_fd < 0 || _ioctl(net_fd, NET_DEVICE_INFO, out) != 0)
        return NET_STATE_NONE;
    return out->state;
}
