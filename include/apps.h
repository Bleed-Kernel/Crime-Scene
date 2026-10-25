#pragma once
#include <display.h>
#include <icon.h>
#include <crime/crime.h>

#define APPS_DIR        "/initrd/bin"
#define DATA_APPS_DIR   CS_DATA_DIR
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

// everything in APPS_DIR and the folders in DATA_APPS_DIR that wants to be in the start menu,
// by name. looked for again each time
int  apps_scan(void);
int  apps_count(void);
const app_t *apps_index(int index);

void app_start(display_t *d, const app_t *app);

int  app_run_line(display_t *d, const char *line);

// argv[0] is a path, or a name in APPS_DIR or one of the DATA_APPS_DIR folders. 0 if theres
// a program there to start
int  app_run(display_t *d, char *argv[]);

void program_run(const char *path);
