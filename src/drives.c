#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <syscalls.h>
#include <usbstorage.h>
#include <drives.h>

#define DEV_DIR "/dev"

// usb0 yes, usb0p1 no, a partition is the same stick again?
static int is_drive(const char *name) {
    if (strncmp(name, "usb", 3) != 0 || !name[3])
        return 0;
    for (const char *c = name + 3; *c; c++) {
        if (!isdigit((unsigned char)*c))
            return 0;
    }
    return 1;
}

static int drive_info(const char *dev, struct usb_storage_info *info) {
    char path[32];
    snprintf(path, sizeof(path), DEV_DIR "/%s", dev);

    int fd = _open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long r = _ioctl(fd, USB_STORAGE_IOC_INFO, info);
    _close(fd);
    return r == 0 ? 0 : -1;
}

int drives_read(drive_t *out, int max) {
    DIR *dir = opendir(DEV_DIR);
    if (!dir)
        return 0;

    int count = 0;
    for (struct dirent *ent; count < max && (ent = readdir(dir)); ) {
        struct usb_storage_info info;
        if (!is_drive(ent->d_name) || drive_info(ent->d_name, &info) != 0)
            continue;

        drive_t *drive = &out[count++];
        memset(drive, 0, sizeof(*drive));
        strncpy(drive->dev, ent->d_name, sizeof(drive->dev) - 1);
        strncpy(drive->mount, info.mount, sizeof(drive->mount) - 1);

        if (info.product[0])
            snprintf(drive->name, sizeof(drive->name), "%s", info.product);
        else
            snprintf(drive->name, sizeof(drive->name), "USB Drive");
    }
    closedir(dir);
    return count;
}

int drive_eject(const char *dev) {
    char path[32];
    snprintf(path, sizeof(path), DEV_DIR "/%s", dev);

    int fd = _open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long r = _ioctl(fd, USB_STORAGE_IOC_EJECT, NULL);
    _close(fd);
    return (int)r;
}
