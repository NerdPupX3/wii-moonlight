#ifndef ENTROPY_H
#define ENTROPY_H

#include <stddef.h>

// Randomness for keys, pairing secrets, the pairing PIN and TLS.
//
// The Wii has no random-number instruction the PowerPC can use, so this module
// measures timing jitter: how long round trips to the Starlet (the Wii's second
// processor, which runs IOS on its own clock) take, in timebase ticks. The two
// clocks are not locked together, so the lowest bits of each round-trip time
// are unpredictable. Many samples are hashed with SHA-256 and fed to mbedtls's
// CTR_DRBG (a standard random generator built on AES).
//
// entropy_init() runs a self-test first and logs how much jitter it measured;
// the number of samples gathered per output block is scaled from that result.

// Seeds the shared generator. Safe to call more than once. Returns 0 on success.
int entropy_init(void);

// mbedtls-style random callback (f_rng). ctx is unused. Returns 0 on success.
int entropy_random(void *ctx, unsigned char *out, size_t len);

// Mixes extra, unpredictable-but-uncounted data into the generator (for
// example, raw Wii Remote accelerometer readings). Never lowers the quality.
void entropy_mix(const void *data, size_t len);

#endif
