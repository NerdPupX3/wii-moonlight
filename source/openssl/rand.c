#include "rand.h"
#include "../entropy.h"

// OpenSSL-style RAND_bytes for client.c's pairing code (salt, challenge,
// client secret). Backed by the shared generator in entropy.c. Before, every
// call built a new generator seeded from a clock-based pseudo-random sequence,
// so the "random" bytes were predictable from the time of day.
int RAND_bytes(unsigned char *buf, int num) {
    if (num <= 0)
        return 1;
    return entropy_random(NULL, buf, (size_t)num) == 0 ? 1 : 0; // 1 = success in OpenSSL
}
