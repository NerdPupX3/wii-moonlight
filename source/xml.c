/*
 * Minimal XML parsing for Wii Moonlight (no expat dependency).
 *
 * The GameStream/Sunshine HTTP endpoints return small, well-formed XML
 * documents. This file implements just enough parsing for the fields
 * the client needs: leaf-element text search, the <root status_code>
 * attribute, and the <App> and <DisplayMode> lists.
 */

#include "xml.h"
#include "errors.h"

#include <stdlib.h>
#include <string.h>

/* Helper: copy [start, end) into a fresh NUL-terminated string. */
static char* substr_dup(const char *start, const char *end) {
    size_t len = (size_t)(end - start);
    char *out = (char*)malloc(len + 1);
    if (out == NULL)
        return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

/* Find the text content of the first <node>...</node> element. */
int xml_search(char* data, size_t len, char* node, char** result) {
    (void)len;
    char open_tag[256];
    char close_tag[256];
    snprintf(open_tag, sizeof(open_tag), "<%s>", node);
    snprintf(close_tag, sizeof(close_tag), "</%s>", node);

    *result = calloc(1, 1);
    if (*result == NULL)
        return GS_OUT_OF_MEMORY;

    char *start = strstr(data, open_tag);
    if (start == NULL)
        return GS_OK; /* not present -> empty string */

    start += strlen(open_tag);
    char *end = strstr(start, close_tag);
    if (end == NULL)
        return GS_OK;

    char *out = substr_dup(start, end);
    if (out == NULL)
        return GS_OUT_OF_MEMORY;
    free(*result);
    *result = out;
    return GS_OK;
}

/* Parse <App><ID>..</ID><AppTitle>..</AppTitle></App> entries. */
int xml_applist(char* data, size_t len, PAPP_LIST *app_list) {
    (void)len;
    PAPP_LIST head = NULL;
    char *p = data;

    while ((p = strstr(p, "<App>")) != NULL) {
        PAPP_LIST app = (PAPP_LIST)calloc(1, sizeof(APP_LIST));
        if (app == NULL)
            return GS_OUT_OF_MEMORY;

        char *idp = strstr(p, "<ID>");
        if (idp != NULL)
            app->id = atoi(idp + 4);

        char *titlep = strstr(p, "<AppTitle>");
        if (titlep != NULL) {
            char *tend = strstr(titlep + 10, "</AppTitle>");
            if (tend != NULL)
                app->name = substr_dup(titlep + 10, tend);
        }

        app->next = head;
        head = app;
        p += 5; /* advance past "<App>" */
    }

    *app_list = head;
    return GS_OK;
}

/* Parse <DisplayMode><Width/><Height/><RefreshRate/></DisplayMode> entries. */
int xml_modelist(char* data, size_t len, PDISPLAY_MODE *mode_list) {
    (void)len;
    PDISPLAY_MODE head = NULL;
    char *p = data;

    while ((p = strstr(p, "<DisplayMode>")) != NULL) {
        PDISPLAY_MODE mode = (PDISPLAY_MODE)calloc(1, sizeof(DISPLAY_MODE));
        if (mode == NULL)
            return GS_OUT_OF_MEMORY;

        char *wp = strstr(p, "<Width>");
        if (wp != NULL) mode->width = (unsigned int)atoi(wp + 7);
        char *hp = strstr(p, "<Height>");
        if (hp != NULL) mode->height = (unsigned int)atoi(hp + 8);
        char *rp = strstr(p, "<RefreshRate>");
        if (rp != NULL) mode->refresh = (unsigned int)atoi(rp + 13);

        mode->next = head;
        head = mode;
        p += 13; /* advance past "<DisplayMode>" */
    }

    *mode_list = head;
    return GS_OK;
}

/* Read the status_code attribute from the <root ...> element. */
int xml_status(char* data, size_t len) {
    (void)len;
    int status = 0;

    char *attr = strstr(data, "status_code");
    if (attr == NULL)
        return GS_ERROR;

    char *q = strchr(attr, '"');
    if (q == NULL)
        return GS_ERROR;

    status = atoi(q + 1);

    if (status != 200) {
        char *msg = strstr(data, "status_message");
        if (msg != NULL) {
            char *mq = strchr(msg, '"');
            if (mq != NULL) {
                char *me = strchr(mq + 1, '"');
                if (me != NULL) {
                    char *text = substr_dup(mq + 1, me);
                    if (text != NULL)
                        gs_error = text;
                }
            }
        }
        return GS_ERROR;
    }

    return GS_OK;
}
