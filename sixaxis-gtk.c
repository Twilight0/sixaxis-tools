/* sixaxis-gtk: GTK3 + XApp interface for Sixaxis pairing.
 * Layout follows the canonical XApp pattern: XAppGtkWindow containing an
 * XAppStackSidebar bound to a GtkStack (Devices | Pair | Masters | History).
 * Backend wraps sixpair.c control transfers (see sixpair_core.h).
 */
#include <gtk/gtk.h>
#include <libxapp/xapp-gtk-window.h>
#include <libxapp/xapp-stack-sidebar.h>
#include "sixpair_core.h"
#include "store.h"
#include "mac.h"
#include "version.h"
#include "btsony.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static GtkListStore *g_devs;
static GtkListStore *g_masters;
static GtkListStore *g_hist;
static GtkLabel *g_status;
static GtkComboBoxText *g_combo;
static GtkEntry *g_mac_entry;
static GtkEntry *g_label_entry;
static GtkWidget *g_window;
static int g_sel_dev = 0;

static void status(const char *s) { gtk_label_set_text(g_status, s); }

static void load_devices(void) {
    gtk_list_store_clear(g_devs);
    gtk_combo_box_text_remove_all(g_combo);
    sixaxis_dev_t *devs = NULL;
    int n = 0, i;
    if (sixaxis_list(&devs, &n) < 0) { status(sixaxis_err()); return; }
    for (i = 0; i < n; ++i) {
        GtkTreeIter it;
        unsigned char mac[6];
        char ms[18] = "(unreadable)";
        if (sixaxis_get_master(i, mac) == 0) mac_format(mac, ms);
        char desc[192];
        snprintf(desc, sizeof desc, "[%d] bus %s dev %s", i, devs[i].bus, devs[i].dev);
        gtk_list_store_append(g_devs, &it);
        gtk_list_store_set(g_devs, &it, 0, desc, 1, devs[i].product, 2, ms, -1);
        gtk_combo_box_text_append_text(g_combo, desc);
    }
    if (n) gtk_combo_box_set_active(GTK_COMBO_BOX(g_combo), 0);
    sixaxis_free(devs);
    /* bluetooth rows appear only with hardware + daemon + live controllers */
    char reason[128];
    int nb = 0;
    if (bt_available(reason, sizeof reason)) {
        btsony_dev_t *bt = NULL;
        nb = bt_list(&bt);
        for (i = 0; i < nb; ++i) {
            GtkTreeIter it;
            char desc[192], via[96];
            snprintf(desc, sizeof desc, "BT %s", bt[i].uniq);
            snprintf(via, sizeof via, "via %s%s%s", bt[i].phys,
                     bt[i].js[0] ? " " : "", bt[i].js);
            gtk_list_store_append(g_devs, &it);
            gtk_list_store_set(g_devs, &it, 0, desc, 1, bt[i].name, 2, via, -1);
        }
        bt_free(bt);
    }
    if (!n && !nb) status("No controller on USB or Bluetooth.");
    else status("Devices refreshed.");
}

static void load_masters(void) {
    gtk_list_store_clear(g_masters);
    master_entry_t *l = NULL;
    int n = 0, i;
    masters_load(&l, &n);
    for (i = 0; i < n; ++i) {
        GtkTreeIter it;
        char ms[18];
        mac_format(l[i].mac, ms);
        gtk_list_store_append(g_masters, &it);
        gtk_list_store_set(g_masters, &it, 0, ms, 1, l[i].name, -1);
    }
    store_free(l);
}

static void load_history(void) {
    gtk_list_store_clear(g_hist);
    history_entry_t *l = NULL;
    int n = 0, i;
    history_load(&l, &n);
    for (i = 0; i < n; ++i) {
        GtkTreeIter it;
        char o[18], nw[18], ts[32];
        mac_format(l[i].oldm, o);
        mac_format(l[i].newm, nw);
        time_t t = (time_t)l[i].epoch;
        struct tm *tm = localtime(&t);
        if (tm) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", tm);
        else snprintf(ts, sizeof ts, "%ld", (long)l[i].epoch);
        gtk_list_store_append(g_hist, &it);
        gtk_list_store_set(g_hist, &it, 0, ts, 1, l[i].dev, 2, o, 3, nw, 4, l[i].label, -1);
    }
    store_free(l);
}

static void reload_all(void) { load_devices(); load_masters(); load_history(); }

static void pulse_while_working(void) {
    /* XApp window progress feedback around USB round-trips. */
    xapp_gtk_window_set_progress_pulse(XAPP_GTK_WINDOW(g_window), TRUE);
    while (gtk_events_pending()) gtk_main_iteration();
    xapp_gtk_window_set_progress_pulse(XAPP_GTK_WINDOW(g_window), FALSE);
}

static int current_mac(unsigned char mac[6], char *label, int ln) {
    const char *t = gtk_entry_get_text(g_mac_entry);
    if (t && *t) {
        if (mac_parse(t, mac) < 0) { status("Bad MAC (XX:XX:XX:XX:XX:XX)"); return -1; }
        const char *lb = gtk_entry_get_text(g_label_entry);
        snprintf(label, (size_t)ln, "%s", lb ? lb : "");
        return 0;
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(
        GTK_TREE_VIEW(g_object_get_data(G_OBJECT(g_masters), "view")));
    GtkTreeModel *m = NULL;
    GtkTreeIter it;
    if (sel && gtk_tree_selection_get_selected(sel, &m, &it)) {
        char *ms = NULL, *nm = NULL;
        gtk_tree_model_get(m, &it, 0, &ms, 1, &nm, -1);
        int ok = ms && mac_parse(ms, mac) == 0 ? 0 : -1;
        snprintf(label, (size_t)ln, "%s", nm ? nm : "");
        g_free(ms); g_free(nm);
        if (ok < 0) status("Select a stored master or type a MAC.");
        return ok;
    }
    status("Type a MAC or select a stored master.");
    return -1;
}

static void on_pair(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    unsigned char mac[6];
    char label[64];
    if (current_mac(mac, label, sizeof label) < 0) return;
    unsigned char oldm[6] = {0};
    sixaxis_get_master(g_sel_dev, oldm);
    if (sixaxis_set_master(g_sel_dev, mac) < 0) { status(sixaxis_err()); return; }
    char devp[32], ms[18];
    snprintf(devp, sizeof devp, "usb:%d", g_sel_dev);
    mac_format(mac, ms);
    history_append(devp, oldm, mac, label);
    masters_add(mac, label);
    pulse_while_working();
    char s[128];
    snprintf(s, sizeof s, "Paired [%d] -> %s", g_sel_dev, ms);
    status(s);
    reload_all();
}

static void on_local(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    unsigned char mac[6];
    if (sixaxis_local_master(mac) < 0) { status(sixaxis_err()); return; }
    char ms[18];
    mac_format(mac, ms);
    gtk_entry_set_text(g_mac_entry, ms);
    status("Local adapter filled in; press Pair.");
}

static void on_gen(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    unsigned char mac[6];
    if (mac_random(mac) < 0) { status("No entropy"); return; }
    char ms[18];
    mac_format(mac, ms);
    gtk_entry_set_text(g_mac_entry, ms);
    const char *lb = gtk_entry_get_text(g_label_entry);
    masters_add(mac, (lb && *lb) ? lb : "generated");
    load_masters();
    char s[64];
    snprintf(s, sizeof s, "Generated %s (stored)", ms);
    status(s);
}

static void on_add_master(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    const char *t = gtk_entry_get_text(g_mac_entry);
    unsigned char mac[6];
    if (!t || !*t || mac_parse(t, mac) < 0) { status("Type a valid MAC first."); return; }
    const char *lb = gtk_entry_get_text(g_label_entry);
    masters_add(mac, lb ? lb : "");
    load_masters();
    status("Master stored.");
}

static void on_del_master(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    GtkTreeView *v = GTK_TREE_VIEW(g_object_get_data(G_OBJECT(g_masters), "view"));
    GtkTreeSelection *sel = gtk_tree_view_get_selection(v);
    GtkTreeModel *m = NULL;
    GtkTreeIter it;
    if (!sel || !gtk_tree_selection_get_selected(sel, &m, &it)) { status("Select a master row."); return; }
    char *ms = NULL;
    gtk_tree_model_get(m, &it, 0, &ms, -1);
    unsigned char mac[6];
    if (ms && mac_parse(ms, mac) == 0) masters_remove(mac);
    g_free(ms);
    load_masters();
    status("Master deleted.");
}

static void on_clear_hist(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    history_clear();
    load_history();
    status("History cleared.");
}

static void on_refresh(GtkButton *b, gpointer u) { (void)b; (void)u; reload_all(); }

static void on_dev_changed(GtkComboBox *c, gpointer u) {
    (void)u;
    g_sel_dev = gtk_combo_box_get_active(c);
    if (g_sel_dev < 0) g_sel_dev = 0;
}

static GtkWidget *tree(const char *titles[], int n, GtkListStore **store, const GType *types) {
    *store = gtk_list_store_newv(n, (GType *)types);
    GtkWidget *v = gtk_tree_view_new_with_model(GTK_TREE_MODEL(*store));
    int i;
    for (i = 0; i < n; ++i) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(titles[i], r, "text", i, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(v), c);
    }
    return v;
}

static GtkWidget *scroll_wrap(GtkWidget *child, int h) {
    GtkWidget *s = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(s, -1, h);
    gtk_container_add(GTK_CONTAINER(s), child);
    return s;
}

int main(int argc, char **argv) {
    int ai;
    for (ai = 1; ai < argc; ++ai)
        if (strcmp(argv[ai], "--version") == 0 || strcmp(argv[ai], "-V") == 0) { printf("sixaxis-gtk %s\n", SIXAXIS_VERSION); return 0; }
    store_dir(NULL, 0);
    gtk_init(&argc, &argv);

    g_window = xapp_gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), "sixaxis-gtk");
    gtk_window_set_default_size(GTK_WINDOW(g_window), 780, 560);
    xapp_gtk_window_set_icon_name(XAPP_GTK_WINDOW(g_window), "input-gaming");
    g_signal_connect(g_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(g_window), hbox);

    /* XApp sidebar + stack: the XApp-idiomatic settings layout. */
    GtkWidget *sidebar = GTK_WIDGET(xapp_stack_sidebar_new());
    gtk_box_pack_start(GTK_BOX(hbox), sidebar, FALSE, FALSE, 0);

    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    xapp_stack_sidebar_set_stack(XAPP_STACK_SIDEBAR(sidebar), GTK_STACK(stack));
    gtk_box_pack_start(GTK_BOX(hbox), stack, TRUE, TRUE, 0);

    /* Page 1: Devices */
    {
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(page), 10);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
        g_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
        gtk_box_pack_start(GTK_BOX(row), GTK_WIDGET(g_combo), TRUE, TRUE, 0);
        g_signal_connect(g_combo, "changed", G_CALLBACK(on_dev_changed), NULL);
        GtkWidget *rb = gtk_button_new_with_label("Refresh");
        g_signal_connect(rb, "clicked", G_CALLBACK(on_refresh), NULL);
        gtk_box_pack_start(GTK_BOX(row), rb, FALSE, FALSE, 0);
        const char *dt[] = {"device", "product", "master"};
        GType dty[] = {G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING};
        GtkWidget *dv = tree(dt, 3, &g_devs, dty);
        gtk_box_pack_start(GTK_BOX(page), scroll_wrap(dv, 200), FALSE, FALSE, 0);
        gtk_stack_add_titled(GTK_STACK(stack), page, "devices", "Devices");
    }

    /* Page 2: Pair */
    {
        GtkWidget *page = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(page), 6);
        gtk_grid_set_row_spacing(GTK_GRID(page), 6);
        gtk_container_set_border_width(GTK_CONTAINER(page), 10);
        g_mac_entry = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_placeholder_text(g_mac_entry, "XX:XX:XX:XX:XX:XX (or pick stored on Masters)");
        g_label_entry = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_placeholder_text(g_label_entry, "label (e.g. laptop)");
        gtk_grid_attach(GTK_GRID(page), GTK_WIDGET(g_mac_entry), 0, 0, 2, 1);
        gtk_grid_attach(GTK_GRID(page), GTK_WIDGET(g_label_entry), 2, 0, 1, 1);
        const char *bl[] = {"Pair", "Use local BT", "Generate", "Store MAC"};
        GCallback bc[] = {G_CALLBACK(on_pair), G_CALLBACK(on_local), G_CALLBACK(on_gen), G_CALLBACK(on_add_master)};
        int i;
        for (i = 0; i < 4; ++i) {
            GtkWidget *b = gtk_button_new_with_label(bl[i]);
            g_signal_connect(b, "clicked", bc[i], NULL);
            gtk_grid_attach(GTK_GRID(page), b, i % 2, 1 + i / 2, 1, 1);
        }
        GtkWidget *hint = gtk_label_new("Pair writes the master address over USB (needs 99-sixaxis.rules, no sudo).");
        gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
        gtk_grid_attach(GTK_GRID(page), hint, 0, 3, 3, 1);
        gtk_stack_add_titled(GTK_STACK(stack), page, "pair", "Pair");
    }

    /* Page 3: Masters */
    {
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(page), 10);
        const char *mt[] = {"MAC", "name"};
        GType mty[] = {G_TYPE_STRING, G_TYPE_STRING};
        GtkWidget *mv = tree(mt, 2, &g_masters, mty);
        g_object_set_data(G_OBJECT(g_masters), "view", mv);
        gtk_box_pack_start(GTK_BOX(page), scroll_wrap(mv, 280), TRUE, TRUE, 0);
        GtkWidget *delb = gtk_button_new_with_label("Delete selected master");
        g_signal_connect(delb, "clicked", G_CALLBACK(on_del_master), NULL);
        gtk_box_pack_start(GTK_BOX(page), delb, FALSE, FALSE, 0);
        gtk_stack_add_titled(GTK_STACK(stack), page, "masters", "Masters");
    }

    /* Page 4: History */
    {
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(page), 10);
        const char *ht[] = {"time", "dev", "old", "new", "label"};
        GType hty[] = {G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING};
        GtkWidget *hv = tree(ht, 5, &g_hist, hty);
        gtk_box_pack_start(GTK_BOX(page), scroll_wrap(hv, 280), TRUE, TRUE, 0);
        GtkWidget *cb = gtk_button_new_with_label("Clear history");
        g_signal_connect(cb, "clicked", G_CALLBACK(on_clear_hist), NULL);
        gtk_box_pack_start(GTK_BOX(page), cb, FALSE, FALSE, 0);
        gtk_stack_add_titled(GTK_STACK(stack), page, "history", "History");
    }

    g_status = GTK_LABEL(gtk_label_new("ready"));
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    /* Reparent hbox into outer so the status bar sits below the sidebar+stack. */
    g_object_ref(hbox);
    gtk_container_remove(GTK_CONTAINER(g_window), hbox);
    gtk_box_pack_start(GTK_BOX(outer), hbox, TRUE, TRUE, 0);
    g_object_unref(hbox);
    gtk_box_pack_start(GTK_BOX(outer), GTK_WIDGET(g_status), FALSE, FALSE, 4);
    gtk_container_add(GTK_CONTAINER(g_window), outer);

    gtk_widget_show_all(g_window);
    reload_all();
    gtk_main();
    return 0;
}
