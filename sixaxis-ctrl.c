/* sixaxis-ctrl: fully CLI-driven Sixaxis pairing tool (wraps sixpair.c core).
 * Runnable as user once 99-sixaxis.rules is installed.
 */
#include "sixpair_core.h"
#include "store.h"
#include "mac.h"
#include "version.h"
#include "btsony.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>

static void usage(const char *p) {
    printf("  list [-v]                     show USB Sixaxis controllers (-v also reads masters)\n");
    printf("                                + Bluetooth-connected ones, when present\n");
    printf("  bt                          show Bluetooth-connected Sony controllers\n");
    printf("  show <idx>                  show current master of device\n");
    printf("  pair <MAC> [<idx>] [-n name]  set master, store history+master\n");
    printf("  local [<idx>] [-n name]     pair to local bluetooth adapter\n");
    printf("  gen [-n name] [--set [<idx>]]  create random LAA MAC (+store, opt. pair)\n");
    printf("  masters list | add <MAC> [name] | rm <MAC> | clear\n");
    printf("  history [list | clear]\n");
    printf("MAC format XX:XX:XX:XX:XX:XX. idx defaults to 0.\n");
}

static void print_dev(int i, const sixaxis_dev_t *d) {
    printf("[%d] bus %s dev %s if=%d  %s\n", i, d->bus, d->dev, d->itf, d->product);
}

static const char *flag_val(int argc, char **argv, const char *flag) {
    int i;
    for (i = 0; i + 1 < argc; ++i)
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    return NULL;
}

static int has_flag(int argc, char **argv, const char *flag) {
    int i;
    for (i = 0; i < argc; ++i)
        if (strcmp(argv[i], flag) == 0) return 1;
    return 0;
}

static int cmd_bt(int verbose_only) {
    char reason[128];
    if (!bt_available(reason, sizeof reason)) {
        if (!verbose_only) printf("Bluetooth unavailable: %s.\n", reason);
        return verbose_only ? 0 : 1;
    }
    btsony_dev_t *devs = NULL;
    int n = bt_list(&devs);
    if (!n) {
        if (!verbose_only) printf("No Sony controller connected via Bluetooth.\n");
        bt_free(devs);
        return 0;
    }
    printf("Bluetooth (%d):\n", n);
    int i;
    for (i = 0; i < n; ++i)
        printf("  %s  %s%s  via %s%s%s\n", devs[i].uniq,
               devs[i].name, devs[i].motion ? " [motion]" : "",
               devs[i].phys,
               devs[i].js[0] ? " " : "", devs[i].js);
    bt_free(devs);
    return 0;
}

static int cmd_list(int verbose) {
    sixaxis_dev_t *devs = NULL;
    int n = 0;
    if (sixaxis_list(&devs, &n) < 0) { fprintf(stderr, "error: %s\n", sixaxis_err()); return 1; }
    if (!n) { printf("No controller found on USB busses.\n"); }
    int i;
    for (i = 0; i < n; ++i) {
        print_dev(i, &devs[i]);
        if (!verbose) continue;
        unsigned char mac[6];
        if (sixaxis_get_master(i, mac) == 0) {
            char ms[18];
            mac_format(mac, ms);
            printf("     master: %s\n", ms);
        } else {
            printf("     master: (unreadable: %s)\n", sixaxis_err());
        }
    }
    if (!verbose && n)
        printf("(use 'show <idx>' to read the paired master; re-plug after read/pair as user)\n");
    sixaxis_free(devs);
    cmd_bt(1); /* bluetooth section appears only when usable + connected */
    return 0;
}

static int cmd_show(int idx) {
    unsigned char mac[6];
    if (sixaxis_get_master(idx, mac) < 0) { fprintf(stderr, "error: %s\n", sixaxis_err()); return 1; }
    char ms[18];
    mac_format(mac, ms);
    printf("Current Bluetooth master: %s\n", ms);
    return 0;
}

/* shared pair path: read old, set new, record history + masters */
static int do_pair(int idx, const unsigned char mac[6], const char *label) {
    unsigned char oldm[6] = {0};
    int have_old = sixaxis_get_master(idx, oldm) == 0;
    char devpath[64];
    snprintf(devpath, sizeof devpath, "usb:%d", idx);
    if (sixaxis_set_master(idx, mac) < 0) { fprintf(stderr, "error: %s\n", sixaxis_err()); return 1; }
    char ms[18];
    mac_format(mac, ms);
    printf("Setting master bd_addr to %s\n", ms);
    if (!have_old) memset(oldm, 0, 6);
    history_append(devpath, oldm, mac, label ? label : "");
    masters_add(mac, label ? label : "");
    return 0;
}

static int cmd_pair(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "pair needs <MAC>\n"); return 1; }
    unsigned char mac[6];
    if (mac_parse(argv[0], mac) < 0) { fprintf(stderr, "bad MAC '%s'\n", argv[0]); return 1; }
    int idx = 0;
    /* optional positional idx: pair MAC IDX */
    if (argc >= 2 && argv[1][0] != '-') {
        idx = atoi(argv[1]);
        argc--; argv++;
    }
    const char *name = flag_val(argc + 1, argv - 1, "-n");
    return do_pair(idx, mac, name ? name : "");
}

static int cmd_local(int argc, char **argv) {
    int idx = 0;
    if (argc >= 1 && argv[0][0] != '-') idx = atoi(argv[0]);
    const char *name = flag_val(argc, argv, "-n");
    unsigned char mac[6];
    if (sixaxis_local_master(mac) < 0) { fprintf(stderr, "error: %s\n", sixaxis_err()); return 1; }
    return do_pair(idx, mac, name ? name : "local");
}

static int cmd_gen(int argc, char **argv) {
    unsigned char mac[6];
    if (mac_random(mac) < 0) { fprintf(stderr, "error: no entropy\n"); return 1; }
    char ms[18];
    mac_format(mac, ms);
    const char *name = flag_val(argc, argv, "-n");
    printf("Generated: %s\n", ms);
    masters_add(mac, name ? name : "generated");
    if (has_flag(argc, argv, "--set")) {
        int idx = 0;
        /* --set [idx]: find value after --set if numeric */
        int i;
        for (i = 0; i < argc; ++i)
            if (strcmp(argv[i], "--set") == 0 && i + 1 < argc && argv[i+1][0] != '-')
                idx = atoi(argv[i + 1]);
        return do_pair(idx, mac, name ? name : "generated");
    }
    return 0;
}

static int cmd_masters(int argc, char **argv) {
    const char *sub = argc >= 1 ? argv[0] : "list";
    if (strcasecmp(sub, "list") == 0) {
        master_entry_t *l = NULL;
        int n = 0;
        masters_load(&l, &n);
        int i;
        for (i = 0; i < n; ++i) {
            char ms[18];
            mac_format(l[i].mac, ms);
            printf("%s  %s\n", ms, l[i].name);
        }
        if (!n) printf("(no stored masters)\n");
        store_free(l);
        return 0;
    }
    if (strcasecmp(sub, "add") == 0) {
        if (argc < 2) { fprintf(stderr, "masters add needs <MAC> [name]\n"); return 1; }
        unsigned char mac[6];
        if (mac_parse(argv[1], mac) < 0) { fprintf(stderr, "bad MAC\n"); return 1; }
        return masters_add(mac, argc >= 3 ? argv[2] : "") < 0 ? 1 : 0;
    }
    if (strcasecmp(sub, "rm") == 0 || strcasecmp(sub, "remove") == 0 || strcasecmp(sub, "del") == 0) {
        if (argc < 2) { fprintf(stderr, "masters rm needs <MAC>\n"); return 1; }
        unsigned char mac[6];
        if (mac_parse(argv[1], mac) < 0) { fprintf(stderr, "bad MAC\n"); return 1; }
        return masters_remove(mac) < 0 ? 1 : 0;
    }
    if (strcasecmp(sub, "clear") == 0)
        return masters_clear() < 0 ? 1 : 0;
    fprintf(stderr, "unknown masters subcommand\n");
    return 1;
}

static int cmd_history(int argc, char **argv) {
    const char *sub = argc >= 1 ? argv[0] : "list";
        return history_clear() < 0 ? 1 : 0;
    history_entry_t *l = NULL;
    int n = 0;
    history_load(&l, &n);
    int i;
    for (i = 0; i < n; ++i) {
        char os[18], ns[18], ts[32];
        mac_format(l[i].oldm, os);
        mac_format(l[i].newm, ns);
        time_t t = (time_t)l[i].epoch;
        struct tm *tm = localtime(&t);
        if (tm) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", tm);
        else snprintf(ts, sizeof ts, "%ld", (long)l[i].epoch);
        printf("%s  %s  %s -> %s  %s\n", ts, l[i].dev, os, ns, l[i].label);
    }
    if (!n) printf("(empty history)\n");
    store_free(l);
    return 0;
}

int main(int argc, char **argv) {
    store_dir(NULL, 0);
    if (argc < 2) { usage(argv[0]); return 1; }
    const char *cmd = argv[1];
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) { printf("sixaxis-ctrl %s\n", SIXAXIS_VERSION); return 0; }
    if (strcmp(cmd, "list") == 0) return cmd_list(argc >= 3 && (strcmp(argv[2], "-v") == 0 || strcmp(argv[2], "--verbose") == 0));
    if (strcmp(cmd, "show") == 0) return cmd_show(argc >= 3 ? atoi(argv[2]) : 0);
    if (strcmp(cmd, "pair") == 0) return cmd_pair(argc - 2, argv + 2);
    if (strcmp(cmd, "local") == 0 || strcmp(cmd, "pair-local") == 0) return cmd_local(argc - 2, argv + 2);
    if (strcmp(cmd, "gen") == 0) return cmd_gen(argc - 2, argv + 2);
    if (strcmp(cmd, "masters") == 0 || strcmp(cmd, "master") == 0) return cmd_masters(argc - 2, argv + 2);
    if (strcmp(cmd, "history") == 0 || strcmp(cmd, "hist") == 0) return cmd_history(argc - 2, argv + 2);
    if (strcmp(cmd, "bt") == 0 || strcmp(cmd, "bluetooth") == 0) return cmd_bt(0);
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) { usage(argv[0]); return 0; }
    fprintf(stderr, "unknown command '%s'\n", cmd);
    usage(argv[0]);
    return 1;
}
