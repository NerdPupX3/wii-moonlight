/*
 * Utility helpers for Wii Moonlight (no 3DS-specific code).
 */

#include "util.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int write_bool(char *path, bool val) {
    int fd = open(path, O_RDWR);

    if (fd >= 0) {
        int ret = write(fd, val ? "1" : "0", 1);
        if (ret < 0)
            fprintf(stderr, "Failed to write %d to %s\n", val ? 1 : 0, path);

        close(fd);
        return 0;
    } else
        return -1;
}

int read_file(char *path, char *output, int output_len) {
    int fd = open(path, O_RDONLY);

    if (fd >= 0) {
        output_len = read(fd, output, output_len);
        close(fd);
        return output_len;
    } else
        return -1;
}

bool ensure_buf_size(void **buf, size_t *buf_size, size_t required_size) {
    if (*buf_size >= required_size)
        return false;

    *buf_size = required_size;
    *buf = realloc(*buf, *buf_size);
    if (!*buf) {
        fprintf(stderr, "Failed to allocate %zu bytes\n", *buf_size);
        abort();
    }

    return true;
}

bool has_fast_aes(void) {
    /* The Wii has no AES-NI or equivalent; always use the software path. */
    return false;
}

int ensure_linear_buf_size(void **buf, size_t *buf_size, size_t required_size) {
    /* The Wii has no linear-memory requirement for the renderer; use realloc. */
    if (*buf_size >= required_size)
        return 0;

    *buf_size = required_size;
    *buf = realloc(*buf, *buf_size);
    if (!*buf)
        return 1;
    return 0;
}
