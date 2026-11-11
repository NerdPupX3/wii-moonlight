#ifndef HOSTS_H
#define HOSTS_H

// Hosts this Wii connected to before, so they appear even when discovery finds
// nothing (for example when the PC doesn't advertise itself over mDNS).
// Stored as text, newest first, in <keydir>/hosts.txt: "ip<TAB>name" per line.

#define HOSTS_MAX 8

typedef struct {
    char ip[16];
    char name[64];
} SAVED_HOST;

int  hosts_load(const char *dir, SAVED_HOST *out, int max);   /* returns count */
void hosts_remember(const char *dir, const char *ip, const char *name);

#endif
