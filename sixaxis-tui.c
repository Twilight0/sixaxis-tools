/* sixaxis-tui: ncurses interface, laid out like pulsemixer (1.5.1):
 * top menu line with F-key modes (active BOLD, idle DIM), single-line rows
 * with BOLD focus, bottom info line with scroll arrow, `?` help popup,
 * j/k + arrows + PgUp/PgDn navigation, Tab mode cycling, q/Esc quit.
 * Modes: F1 Devices | F2 Pair | F3 Masters | F4 History.
 * USB rows pair over USB; Bluetooth rows appear only when a controller
 * is currently connected via Bluetooth (hardware + daemon required).
 */
#include "sixpair_core.h"
#include "store.h"
#include "mac.h"
#include "version.h"
#include "btsony.h"
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { M_DEV, M_PAIR, M_MAST, M_HIST, N_MODES };

static const char *mode_names[N_MODES] = {"Devices", "Pair", "Masters", "History"};

static sixaxis_dev_t *g_devs = NULL;
static int g_nd = 0;
static char g_macs[16][18];
static btsony_dev_t *g_bt = NULL;
static int g_nb = 0;
static int g_usb_dev = 0; /* USB device used by Pair page */

static master_entry_t *g_m = NULL;
static int g_nm = 0;

static history_entry_t *g_h = NULL;
static int g_nh = 0;

static unsigned char g_pending[6];
static int g_have_pending = 0;
static char g_pending_label[64] = "";
static char g_local[18] = "?";

static int g_mode = M_DEV;
static int g_focus[N_MODES] = {0, 0, 0, 0};
static int g_top[N_MODES] = {0, 0, 0, 0};

static char g_info[256] = "";
static char g_menu[256] = "";
static int g_color = 2;
static int g_green = 0, g_red = 0, g_dim = 0;

static void reload_devices(void) {
    sixaxis_free(g_devs);
    g_devs = NULL;
    g_nd = 0;
    sixaxis_list(&g_devs, &g_nd);
    int i;
    for (i = 0; i < g_nd && i < 16; ++i) {
        unsigned char mac[6];
        if (sixaxis_get_master(i, mac) == 0)
            mac_format(mac, g_macs[i]);
        else
            snprintf(g_macs[i], sizeof g_macs[i], "(unreadable)");
    }
    /* bluetooth rows appear only with hardware + daemon + live controllers */
    bt_free(g_bt);
    g_bt = NULL;
    g_nb = 0;
    char reason[128];
    if (bt_available(reason, sizeof reason))
        g_nb = bt_list(&g_bt);
    if (g_usb_dev >= g_nd) g_usb_dev = g_nd - 1;
    if (g_usb_dev < 0) g_usb_dev = 0;
    unsigned char lm[6];
    if (sixaxis_local_master(lm) == 0)
        mac_format(lm, g_local);
    else
        snprintf(g_local, sizeof g_local, "?");
}

static void reload_store(void) {
    store_free(g_m);
    g_m = NULL;
    g_nm = 0;
    masters_load(&g_m, &g_nm);
    store_free(g_h);
    g_h = NULL;
    g_nh = 0;
    history_load(&g_h, &g_nh);
}

static void reload_all(void) {
    reload_devices();
    reload_store();
}

/* row counts per mode */
static int nrows(void) {
    switch (g_mode) {
    case M_DEV: return g_nd + g_nb ? g_nd + g_nb : 1;
    case M_PAIR: return 5;
    case M_MAST: return g_nm ? g_nm : 1;
    case M_HIST: return g_nh ? g_nh : 1;
    }
    return 1;
}

static void clamp_focus(void) {
    int n = nrows();
    if (g_focus[g_mode] >= n) g_focus[g_mode] = n - 1;
    if (g_focus[g_mode] < 0) g_focus[g_mode] = 0;
    if (g_top[g_mode] > g_focus[g_mode]) g_top[g_mode] = g_focus[g_mode];
}

static void set_mode(int m) {
    g_mode = (m + N_MODES) % N_MODES;
    clamp_focus();
}

static void update_menu(void) {
    char *p = g_menu;
    int i;
    for (i = 0; i < N_MODES; ++i)
        p += snprintf(p, sizeof(g_menu) - (size_t)(p - g_menu),
                      "%sF%d %s  ", i == g_mode ? "*" : " ", i + 1, mode_names[i]);
    snprintf(p, sizeof(g_menu) - (size_t)(p - g_menu), " sixaxis %s", SIXAXIS_VERSION);
}

/* scroll arrow like pulsemixer: ↕ ↑ ↓ or blank */
static char scroll_arrow(int lines) {
    int bottom = g_top[g_mode] + lines;
    int n = nrows();
    if (bottom < n && g_top[g_mode] > 0) return 'X'; /* ↕ fallback below */
    if (g_top[g_mode] > 0) return '^';
    if (bottom < n) return 'v';
    return ' ';
}

static void update_info(int lines) {
    char arrow = scroll_arrow(lines);
    const char *a = arrow == 'X' ? "<>" : arrow == ' ' ? "" : (char[]){arrow, 0};
    switch (g_mode) {
    case M_DEV: {
        char pm[18] = "none";
        if (g_have_pending) mac_format(g_pending, pm);
        int f = g_focus[M_DEV];
        if (f < g_nd && g_nd)
            snprintf(g_info, sizeof g_info, "usb [%d/%d] master %s | pending %s %s %s",
                     f, g_nd, f < 16 ? g_macs[f] : "?", pm, g_pending_label, a);
        else if (f >= g_nd && f < g_nd + g_nb)
            snprintf(g_info, sizeof g_info, "bt %s via %s | replug USB to re-pair %s",
                     g_bt[f - g_nd].uniq, g_bt[f - g_nd].phys, a);
        else if (g_nb)
            snprintf(g_info, sizeof g_info, "bluetooth only | pending %s %s", pm, a);
        else
            snprintf(g_info, sizeof g_info, "no controller on USB or Bluetooth %s", a);
        break;
    }
    case M_PAIR: {
        char pm[18] = "-";
        if (g_have_pending) mac_format(g_pending, pm);
        snprintf(g_info, sizeof g_info, "pending %s (%s) -> dev [%d] | local %s %s",
                 pm, g_pending_label, g_focus[M_DEV], g_local, a);
        break;
    }
    case M_MAST:
        if (g_nm)
            snprintf(g_info, sizeof g_info, "master [%d/%d] | Enter: use for pairing %s",
                     g_focus[M_MAST], g_nm, a);
        else
            snprintf(g_info, sizeof g_info, "no stored masters (a: add, g: generate) %s", a);
        break;
    case M_HIST:
        if (g_nh)
            snprintf(g_info, sizeof g_info, "entry [%d/%d] | Enter: reuse MAC %s",
                     g_focus[M_HIST], g_nh, a);
        else
            snprintf(g_info, sizeof g_info, "empty history %s", a);
        break;
    }
}

/* UTF-8 arrows when the locale allows; set UTF8=1 via environment. */
static const char *arrow_up(void) { const char *u = getenv("UTF8"); return u && *u ? "↑" : "^"; }
static const char *arrow_dn(void) { const char *u = getenv("UTF8"); return u && *u ? "↓" : "v"; }
static const char *arrow_both(void) { const char *u = getenv("UTF8"); return u && *u ? "↕" : "<>"; }

static void draw(int rows, int cols) {
    erase();
    /* menu line: active mode BOLD, rest DIM (pulsemixer update_menu style) */
    int x = 0, i;
    for (i = 0; i < N_MODES; ++i) {
        char t[32];
        snprintf(t, sizeof t, "%sF%d %s  ", i == g_mode ? "*" : " ", i + 1, mode_names[i]);
        attron(i == g_mode ? A_BOLD : A_DIM);
        mvaddnstr(0, x, t, cols - x - 1);
        attroff(i == g_mode ? A_BOLD : A_DIM);
        x += (int)strlen(t);
        if (x >= cols - 1) break;
    }
    attron(A_DIM);
    mvaddnstr(0, x, "?-help", cols - x - 1);
    attroff(A_DIM);

    int lines = rows - 2; /* data rows between menu and info */
    int n = nrows();
    int r;
    for (r = 0; r < lines; ++r) {
        int idx = g_top[g_mode] + r;
        if (idx >= n) break;
        int focused = idx == g_focus[g_mode];
        if (focused) attron(A_BOLD);
        switch (g_mode) {
        case M_DEV:
            if (!g_nd && !g_nb) {
                attron(A_DIM);
                mvaddnstr(1 + r, 0, "no data", cols - 1);
                attroff(A_DIM);
            } else if (idx < g_nd) {
                char line[192];
                snprintf(line, sizeof line, "[%d] bus %s dev %s  %s  %s", idx,
                         g_devs[idx].bus, g_devs[idx].dev,
                         g_devs[idx].product, idx < 16 ? g_macs[idx] : "?");
                mvaddnstr(1 + r, 0, line, cols - 1);
                if (g_color == 2 && idx < 16 && g_macs[idx][0] != '(') {
                    /* green master suffix */
                    int off = (int)strlen(line) - 17;
                    if (off > 0 && off < cols - 17) {
                        attron(g_green);
                        mvaddnstr(1 + r, off, g_macs[idx], 17);
                        attroff(g_green);
                        if (focused) attron(A_BOLD);
                    }
                }
            } else {
                char line[224];
                btsony_dev_t *b = &g_bt[idx - g_nd];
                snprintf(line, sizeof line, "BT %s  %s%s  via %s%s%s", b->uniq,
                         b->name, b->motion ? " [motion]" : "",
                         b->phys, b->js[0] ? " " : "", b->js);
                if (g_color == 2) attron(g_green);
                mvaddnstr(1 + r, 0, line, cols - 1);
                if (g_color == 2) attroff(g_green);
                if (focused) attron(A_BOLD);
            }
            break;
        case M_PAIR: {
            char pm[18] = "(none: pick from Masters/History, or e/g)", lb[80], dv[96], lc[64], hint[64];
            if (g_have_pending) mac_format(g_pending, pm);
            snprintf(lb, sizeof lb, "Label:   %s", g_pending_label);
            snprintf(dv, sizeof dv, "Device:  USB [%d]%s", g_usb_dev,
                     g_nd ? "" : " (none connected)");
            snprintf(lc, sizeof lc, "Local:   %s", g_local);
            const char *rows5[] = {NULL, lb, dv, lc, "Enter: pair   e: edit   g: generate   L: use local"};
            char macrow[96];
            snprintf(macrow, sizeof macrow, "MAC:     %s", pm);
            rows5[0] = macrow;
            mvaddnstr(1 + r, 0, rows5[idx], cols - 1);
            (void)hint;
            break;
        }
        case M_MAST:
            if (!g_nm) {
                attron(A_DIM);
                mvaddnstr(1 + r, 0, "no data (a: add, g: generate)", cols - 1);
                attroff(A_DIM);
            } else {
                char ms[18], line[128];
                int pend = g_have_pending && mac_equal(g_m[idx].mac, g_pending);
                mac_format(g_m[idx].mac, ms);
                snprintf(line, sizeof line, "%s %s  %s", pend ? "*" : " ", ms, g_m[idx].name);
                mvaddnstr(1 + r, 0, line, cols - 1);
            }
            break;
        case M_HIST:
            if (!g_nh) {
                attron(A_DIM);
                mvaddnstr(1 + r, 0, "no data", cols - 1);
                attroff(A_DIM);
            } else {
                char o[18], nw[18], ts[32], line[224];
                mac_format(g_h[idx].oldm, o);
                mac_format(g_h[idx].newm, nw);
                time_t t = (time_t)g_h[idx].epoch;
                struct tm *tm = localtime(&t);
                if (tm) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", tm);
                else snprintf(ts, sizeof ts, "%ld", (long)g_h[idx].epoch);
                snprintf(line, sizeof line, "%s  %s  %s -> %s  %s",
                         ts, g_h[idx].dev, o, nw, g_h[idx].label);
                mvaddnstr(1 + r, 0, line, cols - 1);
            }
            break;
        }
        if (focused) attroff(A_BOLD);
    }
    /* info line with scroll arrow at right edge (pulsemixer update_info style) */
    update_info(lines);
    attron(A_DIM);
    mvaddnstr(rows - 1, 0, g_info, cols - 8);
    attroff(A_DIM);
    int bottom = g_top[g_mode] + lines, nn = nrows();
    const char *a = "";
    if (bottom < nn && g_top[g_mode] > 0) a = arrow_both();
    else if (g_top[g_mode] > 0) a = arrow_up();
    else if (bottom < nn) a = arrow_dn();
    if (*a) mvaddstr(rows - 1, cols - 4, a);
    refresh();
}

static void popup(const char *title, char *buf, size_t n) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int w = cols - 10;
    if (w > 60) w = 60;
    if (w < 30) w = cols - 2;
    WINDOW *win = newwin(3, w, rows / 2 - 1, (cols - w) / 2);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " %s ", title);
    echo();
    curs_set(1);
    mvwgetnstr(win, 1, 1, buf, (int)n - 1);
    noecho();
    curs_set(0);
    delwin(win);
}

static void help_popup(void) {
    static const char *doc[][2] = {
        {"j k  Up Down", "navigation"},
        {"PgUp PgDn Home End", "scroll"},
        {"F1 F2 F3 F4 / 1-4", "Devices Pair Masters History"},
        {"Tab Shift-Tab", "next/previous mode"},
        {"Enter", "pair (Devices/Pair) / use MAC (Masters/History)"},
        {"e", "edit pending MAC + label"},
        {"g", "generate random MAC"},
        {"L", "use local bluetooth adapter"},
        {"a / d", "add / delete stored master"},
        {"c", "clear history"},
        {"r", "refresh USB + store"},
        {"?", "this help"},
        {"q Esc", "quit"},
        {NULL, NULL},
    };
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int w = 56, h = 16;
    if (w > cols - 2) w = cols - 2;
    WINDOW *win = newwin(h, w, (rows - h) / 2, (cols - w) / 2);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " help ");
    int i;
    for (i = 0; doc[i][0]; ++i)
        mvwprintw(win, 1 + i, 2, "%-22s %s", doc[i][0], doc[i][1]);
    wrefresh(win);
    wgetch(win);
    delwin(win);
}


static int do_pair(int dev, const unsigned char mac[6], const char *label) {
    if (dev < 0 || dev >= g_nd) {
        snprintf(g_info, sizeof g_info, "no device");
        return -1;
    }
    unsigned char oldm[6] = {0};
    sixaxis_get_master(dev, oldm);
    if (sixaxis_set_master(dev, mac) < 0) {
        snprintf(g_info, sizeof g_info, "%s", sixaxis_err());
        return -1;
    }
    char devp[32];
    snprintf(devp, sizeof devp, "usb:%d", dev);
    history_append(devp, oldm, mac, label);
    masters_add(mac, label);
    char ms[18];
    mac_format(mac, ms);
    snprintf(g_info, sizeof g_info, "paired [%d] -> %s", dev, ms);
    reload_all();
    return 0;
}

static void edit_pending(void) {
    char macb[64] = "", lb[64] = "";
    if (g_have_pending) mac_format(g_pending, macb);
    popup("pending MAC", macb, sizeof macb);
    if (!*macb) return;
    unsigned char mac[6];
    if (mac_parse(macb, mac) < 0) {
        snprintf(g_info, sizeof g_info, "bad MAC");
        return;
    }
    snprintf(lb, sizeof lb, "%s", g_pending_label);
    popup("label", lb, sizeof lb);
    memcpy(g_pending, mac, 6);
    g_have_pending = 1;
    snprintf(g_pending_label, sizeof g_pending_label, "%s", lb);
}

static void use_row_as_pending(void) {
    if (g_mode == M_MAST && g_nm) {
        memcpy(g_pending, g_m[g_focus[M_MAST]].mac, 6);
        snprintf(g_pending_label, sizeof g_pending_label, "%s", g_m[g_focus[M_MAST]].name);
        g_have_pending = 1;
        set_mode(M_PAIR);
    } else if (g_mode == M_HIST && g_nh) {
        memcpy(g_pending, g_h[g_focus[M_HIST]].newm, 6);
        snprintf(g_pending_label, sizeof g_pending_label, "%s", g_h[g_focus[M_HIST]].label);
        g_have_pending = 1;
        set_mode(M_PAIR);
    }
}

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--color") == 0 && i + 1 < argc)
            g_color = atoi(argv[++i]);
        else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("sixaxis-tui %s\n", SIXAXIS_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("usage: sixaxis-tui [--color 0|1|2] [--version]\n");
            return 0;
        }
    }
    store_dir(NULL, 0);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    set_escdelay(25);
    if (g_color && has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_GREEN, -1);
        init_pair(2, COLOR_YELLOW, -1);
        init_pair(3, COLOR_RED, -1);
        g_green = COLOR_PAIR(1);
        g_red = COLOR_PAIR(3);
        int n = COLORS < 256 ? 7 : 67;
        init_pair(n, (short)(n - 1), -1);
        g_dim = COLOR_PAIR(n);
        (void)g_red;
    }

    reload_all();
    update_menu();

    int rows, cols, ch;
    while (1) {
        getmaxyx(stdscr, rows, cols);
        clamp_focus();
        update_menu();
        draw(rows, cols);
        ch = getch();
        int n = nrows();
        int lines = rows - 2;
        if (ch == 'q' || ch == 27) break;
        else if (ch == KEY_F(1) || ch == '1') set_mode(M_DEV);
        else if (ch == KEY_F(2) || ch == '2') set_mode(M_PAIR);
        else if (ch == KEY_F(3) || ch == '3') set_mode(M_MAST);
        else if (ch == KEY_F(4) || ch == '4') set_mode(M_HIST);
        else if (ch == '\t') set_mode(g_mode + 1);
        else if (ch == KEY_BTAB) set_mode(g_mode - 1);
        else if (ch == 'j' || ch == KEY_DOWN) {
            if (g_focus[g_mode] + 1 < n) ++g_focus[g_mode];
            if (g_focus[g_mode] >= g_top[g_mode] + lines) g_top[g_mode] = g_focus[g_mode] - lines + 1;
        } else if (ch == 'k' || ch == KEY_UP) {
            if (g_focus[g_mode] > 0) --g_focus[g_mode];
            if (g_focus[g_mode] < g_top[g_mode]) g_top[g_mode] = g_focus[g_mode];
        } else if (ch == KEY_NPAGE) {
            g_focus[g_mode] += lines;
            if (g_focus[g_mode] >= n) g_focus[g_mode] = n - 1;
            g_top[g_mode] = g_focus[g_mode] - lines + 1;
            if (g_top[g_mode] < 0) g_top[g_mode] = 0;
        } else if (ch == KEY_PPAGE) {
            g_focus[g_mode] -= lines;
            if (g_focus[g_mode] < 0) g_focus[g_mode] = 0;
            g_top[g_mode] = g_focus[g_mode];
        } else if (ch == KEY_HOME) { g_focus[g_mode] = 0; g_top[g_mode] = 0; }
        else if (ch == KEY_END) {
            g_focus[g_mode] = n - 1;
            g_top[g_mode] = n - lines;
            if (g_top[g_mode] < 0) g_top[g_mode] = 0;
        } else if (ch == 'h' || ch == KEY_LEFT) set_mode(g_mode - 1);
        else if (ch == 'l' || ch == KEY_RIGHT) set_mode(g_mode + 1);
        else if (ch == '?') help_popup();
        else if (ch == 'r') { reload_all(); snprintf(g_info, sizeof g_info, "refreshed"); }
        else if (ch == '\n' || ch == KEY_ENTER) {
            if (g_mode == M_PAIR) {
                if (!g_have_pending)
                    snprintf(g_info, sizeof g_info, "no pending MAC (F3/F4 Enter, e, g, or L)");
                else
                    do_pair(g_usb_dev, g_pending, g_pending_label);
            } else if (g_mode == M_DEV) {
                int f = g_focus[M_DEV];
                if (f >= g_nd) {
                    snprintf(g_info, sizeof g_info, "bluetooth row: already connected; replug USB to re-pair");
                } else if (!g_have_pending) {
                    g_usb_dev = f;
                    snprintf(g_info, sizeof g_info, "device [%d] selected (set pending MAC first: F3/F4 Enter, e, g, L)", f);
                } else {
                    g_usb_dev = f;
                    do_pair(g_usb_dev, g_pending, g_pending_label);
                }
            } else {
                use_row_as_pending();
            }
        } else if (ch == 'e') edit_pending();
        else if (ch == 'a') {
            char macb[64] = "";
            popup("new master MAC", macb, sizeof macb);
            unsigned char mac[6];
            if (mac_parse(macb, mac) < 0)
                snprintf(g_info, sizeof g_info, "bad MAC");
            else {
                char lb[64] = "";
                popup("label", lb, sizeof lb);
                masters_add(mac, lb);
                reload_store();
            }
        } else if (ch == 'd') {
            if (g_mode == M_MAST && g_nm) {
                masters_remove(g_m[g_focus[M_MAST]].mac);
                reload_store();
                clamp_focus();
            }
        } else if (ch == 'L') {
            unsigned char mac[6];
            if (sixaxis_local_master(mac) < 0)
                snprintf(g_info, sizeof g_info, "%s", sixaxis_err());
            else {
                memcpy(g_pending, mac, 6);
                g_have_pending = 1;
                snprintf(g_pending_label, sizeof g_pending_label, "local");
                if (g_mode == M_DEV && g_focus[M_DEV] >= g_nd) {
                    snprintf(g_info, sizeof g_info, "bluetooth row: pending set to local; replug USB to pair");
                } else if (!g_nd)
                    snprintf(g_info, sizeof g_info, "no USB device (pending set to local)");
                else {
                    if (g_mode == M_DEV) g_usb_dev = g_focus[M_DEV];
                    do_pair(g_usb_dev, mac, "local");
                }
            }
        } else if (ch == 'c') {
            if (g_mode == M_HIST) {
                history_clear();
                reload_store();
                clamp_focus();
            }
        }
    }
    endwin();
    sixaxis_free(g_devs);
    bt_free(g_bt);
    store_free(g_m);
    store_free(g_h);
    return 0;
}
