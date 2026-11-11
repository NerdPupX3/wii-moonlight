#include <stdio.h>
#include <string.h>

#include "hosts.h"
#include "applog.h"

static void hosts_path(const char *dir, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s/hosts.txt", dir);
}

int hosts_load(const char *dir, SAVED_HOST *out, int max)
{
    char path[256], line[160];
    hosts_path(dir, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (tab) *tab = 0;
        if (!line[0] || strlen(line) >= sizeof(out[n].ip))
            continue;
        snprintf(out[n].ip, sizeof(out[n].ip), "%s", line);
        snprintf(out[n].name, sizeof(out[n].name), "%s", tab ? tab + 1 : line);
        n++;
    }
    fclose(f);
    app_log("[hosts] loaded %d saved host(s)\n", n);
    return n;
}

void hosts_remember(const char *dir, const char *ip, const char *name)
{
    SAVED_HOST list[HOSTS_MAX];
    int n = hosts_load(dir, list, HOSTS_MAX);
    char path[256];
    hosts_path(dir, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        app_log("[hosts] ERROR: can't write %s\n", path);
        return;
    }
    fprintf(f, "%s\t%s\n", ip, (name && name[0]) ? name : ip);   /* newest first */
    int written = 1;
    for (int i = 0; i < n && written < HOSTS_MAX; i++) {
        if (strcmp(list[i].ip, ip) == 0)
            continue;
        fprintf(f, "%s\t%s\n", list[i].ip, list[i].name);
        written++;
    }
    fclose(f);
    app_log("[hosts] remembered %s (\"%s\"), %d saved\n", ip, name ? name : "", written);
}
