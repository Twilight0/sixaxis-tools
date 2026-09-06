#pragma once
/* Persistent store: known masters + pairing history.
 * Location: $XDG_CONFIG_HOME/sixaxis/ (fallback ~/.config/sixaxis/).
 * masters file lines:  MAC|NAME
 * history file lines:  EPOCH|DEV|OLD|NEW|LABEL
 */

typedef struct {
    unsigned char mac[6];
    char name[64];
} master_entry_t;

typedef struct {
    long epoch;
    char dev[64];
    unsigned char oldm[6];
    unsigned char newm[6];
    char label[64];
} history_entry_t;

int store_dir(char *out, unsigned long n); /* ensure dir exists, return 0 */
int masters_load(master_entry_t **out, int *nout);
int masters_add(const unsigned char mac[6], const char *name);
int masters_remove(const unsigned char mac[6]);
int masters_clear(void);
int masters_find(const master_entry_t *list, int n, const unsigned char mac[6]);

int history_load(history_entry_t **out, int *nout);
int history_append(const char *dev, const unsigned char oldm[6],
                   const unsigned char newm[6], const char *label);
int history_clear(void);
void store_free(void *p);
