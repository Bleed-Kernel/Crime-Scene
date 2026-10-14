#include <stdlib.h>
#include <fcntl.h>
#include <syscalls.h>
#include <file.h>

#define FILE_MAX (16 * 1024 * 1024)

uint8_t *file_read(const char *path, size_t *size_out) {
    int fd = _open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    size_t   cap  = 256 * 1024;
    size_t   size = 0;
    uint8_t *data = malloc(cap);

    while (data) {
        long n = _read(fd, data + size, cap - size);
        if (n <= 0)
            break;
        size += (size_t)n;

        if (size == cap) {
            if (cap >= FILE_MAX) {
                free(data);
                data = NULL;
                break;
            }
            uint8_t *bigger = realloc(data, cap * 2);
            if (!bigger)
                free(data);
            data = bigger;
            cap *= 2;
        }
    }
    _close(fd);

    if (data && !size) {
        free(data);
        data = NULL;
    }
    *size_out = size;
    return data;
}
