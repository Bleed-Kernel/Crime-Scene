#pragma once
#include <stdint.h>
#include <stddef.h>
#include <crime/protocol.h>

#define CS_PAGE_SIZE 4096UL

#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020

#define POLL_FD_IPC (-2)

struct pollfd {
    int   fd;
    short events;
    short revents;
};

// blibc doesnt have these yet
long poll(struct pollfd *fds, unsigned long nfds, int timeout_ms);
long shm_create(unsigned long pages);
long shm_map(unsigned long id, size_t *pages_out);
long shm_destroy(unsigned long id);

int cs_msg_send(uint64_t pid, const cs_msg_t *msg);
int cs_msg_recv(cs_msg_t *msg, uint64_t *sender);
