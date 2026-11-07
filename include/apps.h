#pragma once
#include <display.h>
#include <icon.h>
#include <crime/crime.h>

#define DEFAULT_PATH    "/usr/local/bin:/bin"
#define APP_PATH_MAX    256
#define RUN_ARGS_MAX    16

typedef struct {
    char          path[APP_PATH_MAX];
    char          name[CS_TITLE_MAX];
    int           terminal;
    int           menu;
    const icon_t *icon;
} app_t;

// what the program at path says about itself, its file name if it says nothing. 0 if its there
int  app_info(const char *path, app_t *out);

int  apps_scan(void);
int  apps_count(void);

uint32_t apps_signature(void);
const app_t *apps_index(int index);

void app_start(display_t *d, const app_t *app);

int  app_run_line(display_t *d, const char *line);

// argv[0] is a path, or a name found along $PATH. 0 if theres
// a program there to start
int  app_run(display_t *d, char *argv[]);

void program_run(const char *path);
