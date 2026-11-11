#ifndef APPLOG_H
#define APPLOG_H

#include <stdarg.h>

// Writes one timestamped line to moonlight.log on the SD card (set up in main.c).
// Lines are buffered and the file is synced every few lines and during teardown,
// so frequent messages (audio/video statistics) don't hammer the SD card.
// Each line is prefixed with "[seconds.millis]" since boot.
void app_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void app_logv(const char *fmt, va_list ap);
// Writes everything buffered to the SD card now (slow; for crash debugging only).
void app_log_sync(void);

#endif
