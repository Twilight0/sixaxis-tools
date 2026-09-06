#include "btsony.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

int bt_available(char *reason, unsigned long n) {
    if (reason && n) reason[0] = 0;
    /* hardware: any hciN adapter */
    DIR *d = opendir("/sys/class/bluetooth");
    int have_hci = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (strncmp(e->d_name, "hci", 3) == 0) { have_hci = 1; break; }
        closedir(d);
    }
    if (!have_hci) {
        if (reason && n) snprintf(reason, n, "no bluetooth hardware");
        return 0;
    }
    /* tooling + daemon: bluetoothctl must talk to bluez */
    FILE *f = popen("bluetoothctl show 2>/dev/null", "r");
    if (!f) {
        if (reason && n) snprintf(reason, n, "bluetoothctl missing (install bluez)");
        return 0;
    }
    char line[256];
    int ok = 0;
    while (fgets(line, sizeof line, f))
        if (strstr(line, "Controller")) { ok = 1; break; }
    pclose(f);
    if (!ok) {
        if (reason && n) snprintf(reason, n, "bluetooth daemon not running");
        return 0;
    }
    return 1;
}
static int is_sony(const char *name) {
    return strstr(name, "PLAYSTATION") != NULL ||
        (strstr(name, "Sony") != NULL && strstr(name, "Controller") != NULL) ||
        strstr(name, "SIXAXIS") != NULL || strstr(name, "Sixaxis") != NULL;
}

typedef struct { char name[128], phys[32], handlers[256], uniq[32]; } btblock_t;

static void commit(btsony_dev_t **arr, int *n, int *cap, const btblock_t *b) {
    if (!is_sony(b->name) || !*b->phys) return;
    if (strncmp(b->phys, "usb-", 4) == 0) return; /* USB, not bluetooth */
    if (*n == *cap) {
        *cap *= 2;
        btsony_dev_t *na = realloc(*arr, sizeof *na * (size_t)*cap);
        if (!na) return;
        *arr = na;
    }
    btsony_dev_t *e = &(*arr)[(*n)++];
    snprintf(e->name, sizeof e->name, "%s", b->name);
    snprintf(e->uniq, sizeof e->uniq, "%s", b->uniq);
    snprintf(e->phys, sizeof e->phys, "%s", b->phys);
    e->js[0] = 0;
    e->event[0] = 0;
    /* handlers like "js0 event21" */
    char tmp[256], *t;
    snprintf(tmp, sizeof tmp, "%s", b->handlers);
    t = strtok(tmp, " ");
    while (t) {
        if (strncmp(t, "js", 2) == 0) snprintf(e->js, sizeof e->js, "%s", t);
        else if (strncmp(t, "event", 5) == 0) snprintf(e->event, sizeof e->event, "%s", t);
        t = strtok(NULL, " ");
    }
    e->motion = strstr(b->name, "Motion") != NULL;
}
int bt_list(btsony_dev_t **out) {
    if (out) *out = NULL;
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) return 0;
    int cap = 4, n = 0;
    btsony_dev_t *arr = malloc(sizeof *arr * (size_t)cap);
    if (!arr) { fclose(f); return 0; }

    char line[512];
    btblock_t b;
    memset(&b, 0, sizeof b);
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '\n' || line[0] == '\r') {
            commit(&arr, &n, &cap, &b);
            memset(&b, 0, sizeof b);
            continue;
        }
        if (strncmp(line, "N: Name=", 8) == 0) {
            char *q = strchr(line + 9, '"');
            size_t L = q ? (size_t)(q - (line + 9)) : 0;
            if (L >= sizeof b.name) L = sizeof b.name - 1;
            memcpy(b.name, line + 9, L);
            b.name[L] = 0;
        } else if (strncmp(line, "P: Phys=", 8) == 0) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(b.phys, sizeof b.phys, "%s", line + 8);
        } else if (strncmp(line, "H: Handlers=", 12) == 0) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(b.handlers, sizeof b.handlers, "%s", line + 12);
        } else if (strncmp(line, "U: Uniq=", 8) == 0) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(b.uniq, sizeof b.uniq, "%s", line + 8);
        }
    }
    commit(&arr, &n, &cap, &b);
    fclose(f);
    *out = arr;
    return n;
}

void bt_free(btsony_dev_t *devs) { free(devs); }
