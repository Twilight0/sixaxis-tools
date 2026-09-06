#pragma once
/* Bluetooth-connected Sony controller enumeration (no new dependencies:
 * reads /proc/bus/input/devices, probes via sysfs + bluetoothctl).
 * Shown only when ALL hold: BT hardware present, bluetooth daemon
 * reachable, and at least one Sony controller currently connected.
 */

typedef struct {
    char name[128];    /* input device name */
    char uniq[32];     /* controller Bluetooth MAC */
    char phys[32];     /* local adapter MAC (the master it points at) */
    char js[16];       /* jsN handler, "" if none */
    char event[16];    /* eventN handler */
    int motion;        /* 1 for Motion Sensors sub-device */
} btsony_dev_t;

/* 1 hardware+daemon+tooling ok, 0 unavailable. reason always set. */
int bt_available(char *reason, unsigned long n);

/* Active BT Sony controllers now. Returns count (>=0), *out malloc'd
 * (free with bt_free); 0 with no error means simply none connected. */
int bt_list(btsony_dev_t **out);
void bt_free(btsony_dev_t *devs);
