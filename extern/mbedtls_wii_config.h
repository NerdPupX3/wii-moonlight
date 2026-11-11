/* Wii settings for Mbed TLS 3.6.1, applied on top of its default config
 * (mbedtls_config.h stays untouched). Used both when building the library
 * (cmake -DMBEDTLS_USER_CONFIG_FILE=...) and when building the app (Makefile),
 * so both always agree. */
#ifndef MBEDTLS_WII_CONFIG_H
#define MBEDTLS_WII_CONFIG_H

/* No /dev/urandom on the Wii: the app supplies its own entropy source
 * (mbedtls_hardware_poll in source/entropy.c). */
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* No POSIX clock: the app supplies mbedtls_ms_time() (source/main.c). */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* BSD sockets and POSIX timers aren't available the way these modules
 * expect; the app does its own networking. */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C

#endif
