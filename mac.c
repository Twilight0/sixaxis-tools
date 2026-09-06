#include "mac.h"
#include <stdio.h>
#include <string.h>

int mac_parse(const char *s, unsigned char mac[6]) {
    unsigned int b[6];
    if (!s) return -1;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return -1;
    int i;
    for (i = 0; i < 6; ++i) {
        if (b[i] > 0xff) return -1;
        mac[i] = (unsigned char)b[i];
    }
    return 0;
}

void mac_format(const unsigned char mac[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int mac_random(unsigned char mac[6]) {
    FILE *f = fopen("/dev/urandom", "r");
    if (!f) return -1;
    int ok = fread(mac, 1, 6, f) == 6 ? 0 : -1;
    fclose(f);
    if (ok < 0) return -1;
    mac[0] = (unsigned char)((mac[0] | 0x02) & 0xFE); /* LAA unicast */
    return 0;
}

int mac_equal(const unsigned char a[6], const unsigned char b[6]) {
    return memcmp(a, b, 6) == 0;
}
