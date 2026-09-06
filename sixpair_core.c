/* sixpair_core.c: wraps sixpair.c logic (2007-04-18, gcc -o sixpair sixpair.c -lusb).
 * Same VID:PID (054c:0268), same HID feature report 0xF5 control transfers:
 *   GET: bmRequest 0xA1 (IN|CLASS|IFACE), bRequest 0x01, wValue 0x03F5
 *   SET: bmRequest 0x21 (OUT|CLASS|IFACE), bRequest 0x09, wValue 0x03F5
 * Refactor: enumeration without opening (driver-safe list); open/detach
 * only for get/set, then best-effort sysfs rebind (libusb-0.1 lacks attach).
 */
#include "sixpair_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <usb.h>

#define USB_DIR_IN 0x80
#define USB_DIR_OUT 0

static char g_err[256];

const char *sixaxis_err(void) { return g_err; }

static void set_err(const char *s) {
    snprintf(g_err, sizeof g_err, "%s", s ? s : "unknown error");
}

/* Find HID interface number for a matched device, -1 if none. */
static int hid_itf(struct usb_device *dev) {
    struct usb_config_descriptor *cfg;
    for (cfg = dev->config;
         cfg < dev->config + dev->descriptor.bNumConfigurations; ++cfg) {
        int i;
        for (i = 0; i < cfg->bNumInterfaces; ++i) {
            struct usb_interface *itf = &cfg->interface[i];
            struct usb_interface_descriptor *alt;
            for (alt = itf->altsetting;
                 alt < itf->altsetting + itf->num_altsetting; ++alt) {
                if (alt->bInterfaceClass == 3)
                    return i;
            }
        }
    }
    return -1;
}

static int usb_scan(void) {
    usb_init();
    if (usb_find_busses() < 0) { set_err("usb_find_busses"); return -1; }
    if (usb_find_devices() < 0) { set_err("usb_find_devices"); return -1; }
    if (!usb_get_busses()) { set_err("usb_get_busses"); return -1; }
    return 0;
}

/* Locate idx-th matching device; returns itf via *itfp, NULL if missing. */
static struct usb_device *find_nth(int idx, int *itfp) {
    struct usb_bus *bus;
    int n = 0;
    for (bus = usb_get_busses(); bus; bus = bus->next) {
        struct usb_device *dev;
        for (dev = bus->devices; dev; dev = dev->next) {
            if (dev->descriptor.idVendor != SIXAXIS_VENDOR ||
                dev->descriptor.idProduct != SIXAXIS_PRODUCT)
                continue;
            int itf = hid_itf(dev);
            if (itf < 0)
                continue;
            if (n == idx) {
                if (itfp) *itfp = itf;
                return dev;
            }
            ++n;
        }
    }
    return NULL;
}

int sixaxis_list(sixaxis_dev_t **out, int *nout) {
    if (out) *out = NULL;
    if (nout) *nout = 0;
    if (usb_scan() < 0)
        return -1;

    int cap = 4, n = 0;
    sixaxis_dev_t *arr = malloc(sizeof *arr * (size_t)cap);
    if (!arr) { set_err("out of memory"); return -1; }

    struct usb_bus *bus;
    for (bus = usb_get_busses(); bus; bus = bus->next) {
        struct usb_device *dev;
        for (dev = bus->devices; dev; dev = dev->next) {
            if (dev->descriptor.idVendor != SIXAXIS_VENDOR ||
                dev->descriptor.idProduct != SIXAXIS_PRODUCT)
                continue;
            int itf = hid_itf(dev);
            if (itf < 0)
                continue;
            if (n == cap) {
                cap *= 2;
                sixaxis_dev_t *na = realloc(arr, sizeof *arr * (size_t)cap);
                if (!na) { free(arr); set_err("out of memory"); return -1; }
                arr = na;
            }
            sixaxis_dev_t *d = &arr[n++];
            snprintf(d->bus, sizeof d->bus, "%s", dev->bus ? dev->bus->dirname : "?");
            snprintf(d->dev, sizeof d->dev, "%s", dev->filename);
            d->itf = itf;
            /* iProduct needs an open handle; use sysfs-free fallback string. */
            snprintf(d->product, sizeof d->product, "Sony PLAYSTATION(R)3 Controller");
            d->has_master = 0;
            memset(d->master, 0, sizeof d->master);
        }
    }
    *out = arr;
    *nout = n;
    return 0;
}

void sixaxis_free(sixaxis_dev_t *devs) { free(devs); }

static int do_open(struct usb_device *dev, int itf, usb_dev_handle **h) {
    *h = usb_open(dev);
    if (!*h) { snprintf(g_err, sizeof g_err, "usb_open: %s (run as user needs 99-sixaxis.rules, else sudo)", usb_strerror()); return -1; }
    /* Same as sixpair.c: detach so we can claim. Ignore errors (may already be detached). */
    usb_detach_kernel_driver_np(*h, itf);
    if (usb_claim_interface(*h, itf) < 0) {
        snprintf(g_err, sizeof g_err, "usb_claim_interface: %s", usb_strerror());
        usb_close(*h);
        return -1;
    }
    return 0;
}

static int dev_get(usb_dev_handle *h, int itf, unsigned char mac[6]) {
    unsigned char msg[8];
    int res = usb_control_msg(h, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                              0x01, 0x03f5, itf, (char *)msg, sizeof msg, 5000);
    if (res < 0) { snprintf(g_err, sizeof g_err, "GET master: %s", usb_strerror()); return -1; }
    memcpy(mac, msg + 2, 6);
    return 0;
}

int sixaxis_get_master(int idx, unsigned char mac[6]) {
    if (usb_scan() < 0)
        return -1;
    int itf = 0;
    struct usb_device *dev = find_nth(idx, &itf);
    if (!dev) { set_err("no such device index"); return -1; }
    usb_dev_handle *h = NULL;
    if (do_open(dev, itf, &h) < 0)
        return -1;
    int rc = dev_get(h, itf, mac);
    usb_close(h);
    sixaxis_rebind();
    return rc;
}

int sixaxis_set_master(int idx, const unsigned char mac[6]) {
    if (usb_scan() < 0)
        return -1;
    int itf = 0;
    struct usb_device *dev = find_nth(idx, &itf);
    if (!dev) { set_err("no such device index"); return -1; }
    usb_dev_handle *h = NULL;
    if (do_open(dev, itf, &h) < 0)
        return -1;
    char msg[8] = { 0x01, 0x00,
        (char)mac[0], (char)mac[1], (char)mac[2],
        (char)mac[3], (char)mac[4], (char)mac[5] };
    int res = usb_control_msg(h, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                              0x09, 0x03f5, itf, msg, sizeof msg, 5000);
    usb_close(h);
    sixaxis_rebind();
    if (res < 0) { snprintf(g_err, sizeof g_err, "SET master: %s", usb_strerror()); return -1; }
    return 0;
}

int sixaxis_rebind(void) {
    /* Rebind any driver-less 054c:0268 HID interface to usbhid. */
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d)
        return -1;
    int ok = -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L < 4 || strcmp(e->d_name + L - 4, ":1.0") != 0)
            continue;
        char base[256], vend[256], prod[256], drv[512];
        snprintf(base, sizeof base, "/sys/bus/usb/devices/%s", e->d_name);
        /* strip ":1.0" for parent device dir */
        char parent[256];
        snprintf(parent, sizeof parent, "/sys/bus/usb/devices/%.*s",
                 (int)(L - 4), e->d_name);
        snprintf(vend, sizeof vend, "%s/idVendor", parent);
        snprintf(prod, sizeof prod, "%s/idProduct", parent);
        FILE *f = fopen(vend, "r");
        char vb[16] = "", pb[16] = "";
        if (f) { if (!fgets(vb, sizeof vb, f)) vb[0] = 0; fclose(f); }
        f = fopen(prod, "r");
        if (f) { if (!fgets(pb, sizeof pb, f)) pb[0] = 0; fclose(f); }
        if (strncmp(vb, "054c", 4) != 0 || strncmp(pb, "0268", 4) != 0)
            continue;
        snprintf(drv, sizeof drv, "%s/driver", base);
        DIR *dd = opendir(drv);
        if (dd) { closedir(dd); continue; } /* already bound */
        char bindpath[] = "/sys/bus/usb/drivers/usbhid/bind";
        FILE *b = fopen(bindpath, "w");
        if (!b)
            continue;
        if (fputs(e->d_name, b) >= 0)
            ok = 0;
        fclose(b);
    }
    closedir(d);
    return ok;
}

int sixaxis_local_master(unsigned char mac[6]) {
    int m[6];
    FILE *f = popen("hcitool dev 2>/dev/null", "r");
    if (f) {
        int rc = fscanf(f, "%*s\n%*s %x:%x:%x:%x:%x:%x",
                        &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
        pclose(f);
        if (rc == 6) {
            int i;
            for (i = 0; i < 6; ++i) mac[i] = (unsigned char)m[i];
            return 0;
        }
    }
    /* hcitool deprecated/missing: parse `bluetoothctl list`. */
    f = popen("bluetoothctl list 2>/dev/null", "r");
    if (!f) { set_err("no bluetooth adapter (hcitool/bluetoothctl failed)"); return -1; }
    char line[256];
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        unsigned int b[6];
        if (sscanf(line, "Controller %x:%x:%x:%x:%x:%x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6 ||
            sscanf(line, "%*s %x:%x:%x:%x:%x:%x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
            int i;
            for (i = 0; i < 6; ++i) mac[i] = (unsigned char)b[i];
            found = 0;
            break;
        }
    }
    pclose(f);
    if (found < 0) set_err("unable to get local bd_addr (enable Bluetooth or pass MAC manually)");
    return found;
}
