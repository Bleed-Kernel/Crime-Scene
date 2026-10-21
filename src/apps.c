#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <syscalls.h>
#include <apps.h>
#include <term.h>

#define SHELL_PATH      "/initrd/bin/verdict"
#define FD_SCAN         256
#define RUN_ARGS_MAX    16

static app_t *apps;
static int    apps_len;
static int    apps_cap;

static int is_yes(const char *value) {
    return strcmp(value, "yes") == 0 || strcmp(value, "1") == 0 || strcmp(value, "true") == 0;
}

// key=value a line, anything it doesnt know is left alone
static void info_parse(app_t *app, char *text) {
    for (char *line = text; line && *line; ) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = '\0';

        char *eq  = strchr(line, '=');
        char *end = line + strlen(line);
        while (end > line && (end[-1] == '\r' || end[-1] == ' '))
            *--end = '\0';

        if (eq) {
            *eq = '\0';
            const char *value = eq + 1;
            if (strcmp(line, "name") == 0 && value[0]) {
                strncpy(app->name, value, CS_TITLE_MAX - 1);
                app->name[CS_TITLE_MAX - 1] = '\0';
            }
            if (strcmp(line, "terminal") == 0) app->terminal = is_yes(value);
            if (strcmp(line, "menu") == 0)     app->menu     = is_yes(value);
        }
        line = next;
    }
}

int app_info(const char *path, app_t *out) {
    if (!path || strlen(path) >= APP_PATH_MAX)
        return -1;
    int fd = _open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    _close(fd);

    memset(out, 0, sizeof(*out));
    strcpy(out->path, path);
    out->icon = icon_for_path(path);

    const char *base = strrchr(path, '/');
    strncpy(out->name, base ? base + 1 : path, CS_TITLE_MAX - 1);

    size_t size;
    char  *text = (char *)cs_res_read(path, CS_RES_APP, &size);
    if (!text) {
        out->terminal = !out->icon;
        return 0;
    }

    char *info = malloc(size + 1);
    if (info) {
        memcpy(info, text, size);
        info[size] = '\0';
        out->menu  = 1;
        info_parse(out, info);
        free(info);
    }
    free(text);
    return 0;
}

static int app_add(const app_t *app) {
    if (apps_len == apps_cap) {
        int    cap    = apps_cap ? apps_cap * 2 : 16;
        app_t *bigger = realloc(apps, (size_t)cap * sizeof(app_t));
        if (!bigger)
            return -1;
        apps     = bigger;
        apps_cap = cap;
    }

    int at = apps_len;
    while (at > 0 && strcasecmp(apps[at - 1].name, app->name) > 0) {
        apps[at] = apps[at - 1];
        at--;
    }
    apps[at] = *app;
    apps_len++;
    return 0;
}

int apps_scan(void) {
    apps_len = 0;

    int dir = _open(APPS_DIR, O_RDONLY);
    if (dir < 0)
        return 0;

    dirent_t entry;
    for (size_t i = 0; _readdir(dir, i, &entry) == 0; i++) {
        entry.name[sizeof(entry.name) - 1] = '\0';
        if (entry.name[0] == '.' || sizeof(APPS_DIR) + strlen(entry.name) + 1 > APP_PATH_MAX)
            continue;

        char path[APP_PATH_MAX];
        strcpy(path, APPS_DIR "/");
        strcat(path, entry.name);

        app_t app;
        if (app_info(path, &app) == 0 && app.menu)
            app_add(&app);
    }
    _close(dir);
    return apps_len;
}

int apps_count(void) {
    return apps_len;
}

const app_t *apps_index(int index) {
    return (index >= 0 && index < apps_len) ? &apps[index] : NULL;
}

static void program_exec(char *const argv[]) {
    long pid = fork();
    if (pid < 0)
        return;
    if (pid > 0) {
        _waitpid(pid);
        return;
    }

    if (fork() == 0) {
        for (int fd = 3; fd < FD_SCAN; fd++)
            _close(fd);

        execv(argv[0], argv);
        _exit(127);
    }
    _exit(0);
}

void program_run(const char *path) {
    char *const argv[] = { (char *)path, NULL };
    program_exec(argv);
}

static void app_exec(display_t *d, const app_t *app, char *const argv[]) {
    if (strcmp(app->path, SHELL_PATH) == 0 && !argv[1])
        term_open(d, NULL);
    else if (app->terminal)
        term_run(d, argv, app->name);
    else
        program_exec(argv);
}

void app_start(display_t *d, const app_t *app) {
    char *const argv[] = { (char *)app->path, NULL };
    app_exec(d, app, argv);
}

int app_run_line(display_t *d, const char *line) {
    char  buf[APP_PATH_MAX];
    char *argv[RUN_ARGS_MAX + 1];
    int   argc = 0;

    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    for (char *word = strtok(buf, " "); word && argc < RUN_ARGS_MAX; word = strtok(NULL, " "))
        argv[argc++] = word;
    argv[argc] = NULL;
    if (!argc)
        return -1;

    char path[APP_PATH_MAX];
    if (strchr(argv[0], '/')) {
        strncpy(path, argv[0], sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        if (sizeof(APPS_DIR) + strlen(argv[0]) + 1 > sizeof(path))
            return -1;
        strcpy(path, APPS_DIR "/");
        strcat(path, argv[0]);
    }

    app_t app;
    if (app_info(path, &app) != 0)
        return -1;

    argv[0] = app.path;
    app_exec(d, &app, argv);
    return 0;
}
