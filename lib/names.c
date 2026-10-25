#include <string.h>
#include <strings.h>
#include <crime/crime.h>

// nobody made these on purpose
static const char *const hidden[] = {
    "lost+found",
    "PaxHeader",
    "__MACOSX",
    "Thumbs.db",
    "desktop.ini",
    "$RECYCLE.BIN",
    "System Volume Information",
};

int cs_name_hidden(const char *name) {
    if (!name || name[0] == '.')
        return 1;

    if (!strncasecmp(name, "PaxHeaders.", 11))
        return 1;

    for (size_t i = 0; i < sizeof(hidden) / sizeof(hidden[0]); i++) {
        if (!strcasecmp(name, hidden[i]))
            return 1;
    }
    return 0;
}
