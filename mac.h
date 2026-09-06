#pragma once
/* MAC address helpers. */

int mac_parse(const char *s, unsigned char mac[6]);
void mac_format(const unsigned char mac[6], char out[18]);
int mac_random(unsigned char mac[6]); /* locally-administered unicast */
int mac_equal(const unsigned char a[6], const unsigned char b[6]);
