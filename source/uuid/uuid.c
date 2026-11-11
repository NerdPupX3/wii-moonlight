#include <stdio.h>
#include <ogc/lwp_watchdog.h>
#include "uuid.h"
#include "../entropy.h"

// Random version-4 UUID (used as a per-request nonce in the host's URLs).
// Before, this copied the timer value into the bytes, so it was predictable.
void uuid_generate(uuid_t out) {
    if (entropy_random(NULL, out, 16) != 0) {
        u64 t = gettime();               /* fallback: never leave it uninitialized */
        for (int i = 0; i < 16; i++)
            out[i] = (t >> ((i % 8) * 8)) & 0xFF;
    }
    out[6] = (out[6] & 0x0F) | 0x40;     /* version 4 (random) */
    out[8] = (out[8] & 0x3F) | 0x80;     /* RFC 4122 variant */
}

// Convert the binary UUID to a string format
void uuid_unparse(const uuid_t uu, char *out) {
    sprintf(out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uu[0], uu[1], uu[2], uu[3], uu[4], uu[5], uu[6], uu[7],
        uu[8], uu[9], uu[10], uu[11], uu[12], uu[13], uu[14], uu[15]);
}
