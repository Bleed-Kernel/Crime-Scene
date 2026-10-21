#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <syscalls.h>
#include <crime/crime.h>

#define SECTIONS_MAX    256
#define RES_MAX         (4UL * 1024 * 1024)

// just as much of elf64 as it takes to find a section by name
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_header_t;

typedef struct {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
} elf_section_t;

static int read_at(int fd, uint64_t at, void *buf, size_t len) {
    if (_seek(fd, (long)at, SEEK_SET) < 0)
        return -1;

    for (size_t got = 0; got < len; ) {
        long n = _read(fd, (uint8_t *)buf + got, len - got);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}

static uint8_t *read_alloc(int fd, uint64_t at, size_t len) {
    uint8_t *buf = malloc(len ? len : 1);
    if (buf && read_at(fd, at, buf, len) < 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

uint8_t *cs_res_read(const char *path, const char *name, size_t *size) {
    int fd = _open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    uint8_t       *out      = NULL;
    elf_section_t *sections = NULL;
    char          *names    = NULL;
    elf_header_t   h;

    if (read_at(fd, 0, &h, sizeof(h)) < 0 || memcmp(h.ident, "\x7f" "ELF", 4) != 0 || h.ident[4] != 2 ||
        h.shentsize != sizeof(elf_section_t) || !h.shnum || h.shnum > SECTIONS_MAX || h.shstrndx >= h.shnum)
        goto done;

    sections = (elf_section_t *)read_alloc(fd, h.shoff, (size_t)h.shnum * sizeof(elf_section_t));
    if (!sections)
        goto done;

    elf_section_t *strtab = &sections[h.shstrndx];
    if (strtab->size > RES_MAX)
        goto done;
    names = (char *)read_alloc(fd, strtab->offset, strtab->size);
    if (!names)
        goto done;

    size_t want = strlen(name);
    for (int i = 0; i < h.shnum; i++) {
        elf_section_t *s = &sections[i];
        if (s->name + want >= strtab->size || memcmp(names + s->name, name, want + 1) != 0)
            continue;
        if (s->size > RES_MAX)
            break;

        out = read_alloc(fd, s->offset, s->size);
        if (out && size)
            *size = s->size;
        break;
    }

done:
    free(names);
    free(sections);
    _close(fd);
    return out;
}
