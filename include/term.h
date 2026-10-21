#pragma once
#include <display.h>
#include <window.h>
#include <crime/crime.h>

// a new terminal with a shell in it, started in cwd if thats not NULL or empty. 0 if it worked
int  term_open(display_t *d, const char *cwd);

// argv instead of the shell, the window goes when it exits
int  term_run(display_t *d, char *const argv[], const char *title);

// something the window manager would have sent the client, had there been one
void term_event(display_t *d, window_t *win, const cs_msg_t *msg);

// one pollfd for each terminal, then term_service with the same ones once poll has been at them so they can poll indipendently
int  term_count(void);
void term_pollfds(struct pollfd *fds, int count);
void term_service(display_t *d, const struct pollfd *fds, int count);

// close it and kill everything running in it
void term_kill(display_t *d, window_t *win);

// the program it was started with
const char *term_path(const window_t *win);

// close the ones whose shell has exited
void term_sweep(display_t *d);

void term_shutdown(display_t *d);
