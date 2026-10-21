#include <string.h>
#include <syscalls.h>
#include <ipc.h>
#include <crime/crime.h>

#define SYS_POLL        7
#define SYS_SHM_CREATE  522
#define SYS_SHM_MAP     523
#define SYS_SHM_DESTROY 524
#define SYS_TASKPATH    526

// i mean really this should all be in libc? i didnt fancy it tho i hadnt cleaned up libc yet

static long syscall3(long number, long a, long b, long c) {
    long ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory"
    );
    return ret;
}

long poll(struct pollfd *fds, unsigned long nfds, int timeout_ms) {
    return syscall3(SYS_POLL, (long)fds, (long)nfds, timeout_ms);
}

long shm_create(unsigned long pages) {
    return syscall3(SYS_SHM_CREATE, (long)pages, 0, 0);
}

long shm_map(unsigned long id, size_t *pages_out) {
    return syscall3(SYS_SHM_MAP, (long)id, (long)pages_out, 0);
}

long shm_destroy(unsigned long id) {
    return syscall3(SYS_SHM_DESTROY, (long)id, 0, 0);
}

long task_path(uint64_t pid, char *buf, unsigned long size) {
    return syscall3(SYS_TASKPATH, (long)pid, (long)buf, (long)size);
}

// the kernel moves the page to the other task, so every message gets a fresh one
int cs_msg_send(uint64_t pid, const cs_msg_t *msg) {
    void *page = _mmap(1);
    if (!page || (long)page < 0)
        return -1;

    memcpy(page, msg, sizeof(*msg));
    if (_ipc_send(pid, (uint64_t)page, 1) < 0) {
        _munmap(page);
        return -1;
    }
    return 0;
}

int cs_msg_recv(cs_msg_t *msg, uint64_t *sender) {
    ipc_message_t in;
    if (_ipc_recv(&in) < 0)
        return 0;

    memcpy(msg, (void *)in.addr, sizeof(*msg));
    *sender = in.sender_pid;
    _munmap((void *)in.addr);
    return 1;
}
