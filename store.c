#include "store.h"
#include "mac.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int store_dir(char *out, unsigned long n) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char base[512];
    if (xdg && *xdg)
        snprintf(base, sizeof base, "%s/sixaxis", xdg);
    else if (home && *home)
        snprintf(base, sizeof base, "%s/.config/sixaxis", home);
    else
        return -1;
    mkdir(base, 0700); /* best effort; ignore EEXIST */
    if (out && n)
        snprintf(out, n, "%s", base);
    return 0;
}

static void paths(char *m, unsigned long mn, char *h, unsigned long hn) {
    char d[512];
    store_dir(d, sizeof d);
    if (m) snprintf(m, mn, "%s/masters", d);
    if (h) snprintf(h, hn, "%s/history", d);
}

void store_free(void *p) { free(p); }

int masters_load(master_entry_t **out, int *nout) {
    if (out) *out = NULL;
    if (nout) *nout = 0;
    char mp[512];
    paths(mp, sizeof mp, NULL, 0);
    FILE *f = fopen(mp, "r");
    if (!f) return 0; /* empty is fine */
    int cap = 8, n = 0;
    master_entry_t *arr = malloc(sizeof *arr * (size_t)cap);
    if (!arr) { fclose(f); return -1; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!*line || *line == '#') continue;
        char *sep = strchr(line, '|');
        unsigned char mac[6];
        if (mac_parse(line, mac) < 0) {
            /* allow "MAC NAME" fallback */
            char tmp[32];
            if (sscanf(line, "%31s", tmp) != 1 || mac_parse(tmp, mac) < 0)
                continue;
            sep = NULL;
        }
        if (n == cap) {
            cap *= 2;
            master_entry_t *na = realloc(arr, sizeof *arr * (size_t)cap);
            if (!na) { free(arr); fclose(f); return -1; }
            arr = na;
        }
        memcpy(arr[n].mac, mac, 6);
        const char *nm = sep ? sep + 1 : "";
        while (*nm == ' ' || *nm == '\t' || *nm == '|') ++nm;
        snprintf(arr[n].name, sizeof arr[n].name, "%s", nm);
        ++n;
    }
    fclose(f);
    *out = arr;
    *nout = n;
    return 0;
}

int masters_find(const master_entry_t *list, int n, const unsigned char mac[6]) {
    int i;
    for (i = 0; i < n; ++i)
        if (mac_equal(list[i].mac, mac)) return i;
    return -1;
}

int masters_add(const unsigned char mac[6], const char *name) {
    master_entry_t *list = NULL;
    int n = 0;
    masters_load(&list, &n);
    char mp[512];
    paths(mp, sizeof mp, NULL, 0);
    FILE *f;
    if (masters_find(list, n, mac) >= 0) {
        /* rewrite with updated name */
        f = fopen(mp, "w");
        if (!f) { free(list); return -1; }
        int i;
        for (i = 0; i < n; ++i) {
            char ms[18];
            mac_format(i == masters_find(list, n, mac) ? mac : list[i].mac, ms);
            const char *nm = i == masters_find(list, n, mac)
                ? (name && *name ? name : list[i].name) : list[i].name;
            fprintf(f, "%s|%s\n", ms, nm);
        }
        fclose(f);
        free(list);
        return 0;
    }
    free(list);
    f = fopen(mp, "a");
    if (!f) return -1;
    char ms[18];
    mac_format(mac, ms);
    fprintf(f, "%s|%s\n", ms, name ? name : "");
    fclose(f);
    return 0;
}

int masters_remove(const unsigned char mac[6]) {
    master_entry_t *list = NULL;
    int n = 0;
    if (masters_load(&list, &n) < 0) return -1;
    char mp[512];
    paths(mp, sizeof mp, NULL, 0);
    FILE *f = fopen(mp, "w");
    if (!f) { free(list); return -1; }
    int i;
    for (i = 0; i < n; ++i) {
        if (mac_equal(list[i].mac, mac)) continue;
        char ms[18];
        mac_format(list[i].mac, ms);
        fprintf(f, "%s|%s\n", ms, list[i].name);
    }
    fclose(f);
    free(list);
    return 0;
}

int masters_clear(void) {
    char mp[512];
    paths(mp, sizeof mp, NULL, 0);
    FILE *f = fopen(mp, "w");
    if (!f) return -1;
    fclose(f);
    return 0;
}

int history_load(history_entry_t **out, int *nout) {
    if (out) *out = NULL;
    if (nout) *nout = 0;
    char hp[512];
    paths(NULL, 0, hp, sizeof hp);
    FILE *f = fopen(hp, "r");
    if (!f) return 0;
    int cap = 16, n = 0;
    history_entry_t *arr = malloc(sizeof *arr * (size_t)cap);
    if (!arr) { fclose(f); return -1; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!*line) continue;
        /* EPOCH|DEV|OLD|NEW|LABEL */
        char *p1 = strchr(line, '|');
        if (!p1) continue;
        *p1 = 0;
        char *p2 = strchr(p1 + 1, '|');
        if (!p2) continue;
        *p2 = 0;
        char *p3 = strchr(p2 + 1, '|');
        if (!p3) continue;
        *p3 = 0;
        char *p4 = strchr(p3 + 1, '|');
        const char *label = "";
        if (p4) { *p4 = 0; label = p4 + 1; }
        if (n == cap) {
            cap *= 2;
            history_entry_t *na = realloc(arr, sizeof *arr * (size_t)cap);
            if (!na) { free(arr); fclose(f); return -1; }
            arr = na;
        }
        arr[n].epoch = atol(line);
        snprintf(arr[n].dev, sizeof arr[n].dev, "%s", p1 + 1);
        if (mac_parse(p2 + 1, arr[n].oldm) < 0) memset(arr[n].oldm, 0, 6);
        if (mac_parse(p3 + 1, arr[n].newm) < 0) continue;
        snprintf(arr[n].label, sizeof arr[n].label, "%s", label);
        ++n;
    }
    fclose(f);
    *out = arr;
    *nout = n;
    return 0;
}

int history_append(const char *dev, const unsigned char oldm[6],
                   const unsigned char newm[6], const char *label) {
    char hp[512];
    paths(NULL, 0, hp, sizeof hp);
    FILE *f = fopen(hp, "a");
    if (!f) return -1;
    char os[18], ns[18];
    mac_format(oldm, os);
    mac_format(newm, ns);
    fprintf(f, "%ld|%s|%s|%s|%s\n", (long)time(NULL),
            dev ? dev : "usb", os, ns, label ? label : "");
    fclose(f);
    return 0;
}

int history_clear(void) {
    char hp[512];
    paths(NULL, 0, hp, sizeof hp);
    FILE *f = fopen(hp, "w");
    if (!f) return -1;
    fclose(f);
    return 0;
}
