#pragma once
/* Shared Sixaxis backend wrapping sixpair.c (libusb-0.1 control transfers).
 * GET/SET feature report 0xF5 on the HID interface, same as sixpair.c. */
#define SIXAXIS_VENDOR 0x054c
#define SIXAXIS_PRODUCT 0x0268

typedef struct {
    char bus[16];      /* usbfs bus dirname, e.g. "003" */
    char dev[16];      /* usbfs filename, e.g. "006" */
    int itf;           /* HID interface number */
    char product[128]; /* iProduct string or fallback */
    unsigned char master[6];
    int has_master;    /* 1 if master was read ok */
} sixaxis_dev_t;

/* Enumerate connected Sixaxis controllers (no device open, driver-safe).
 * Returns 0 on success, -1 on USB error. Caller frees *out with sixaxis_free. */
int sixaxis_list(sixaxis_dev_t **out, int *nout);
void sixaxis_free(sixaxis_dev_t *devs);

/* Read current Bluetooth master of device idx (enumeration order).
 * Returns 0 on success, -1 on error. Detaches kernel driver like sixpair,
 * then best-effort rebinds it (see sixaxis_rebind). */
int sixaxis_get_master(int idx, unsigned char mac[6]);

/* Set Bluetooth master of device idx. Same detach/reattach behaviour. */
int sixaxis_set_master(int idx, const unsigned char mac[6]);

/* Best-effort rebind of 054c:0268 HID interfaces back to usbhid after
 * sixpair-style detach (libusb-0.1 has no attach API). Returns 0 if at
 * least one bind attempted ok, -1 otherwise. Never fatal. */
int sixaxis_rebind(void);

/* Last error string (usb strerror or internal). */
const char *sixaxis_err(void);

/* Local adapter BD_ADDR: tries `hcitool dev`, falls back to
 * `bluetoothctl list`. Returns 0 on success. */
int sixaxis_local_master(unsigned char mac[6]);
