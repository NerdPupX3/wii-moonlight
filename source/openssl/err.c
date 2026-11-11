#include "err.h"

unsigned long ERR_get_error(void) {
    return 1; // Return a dummy error code
}

char *ERR_error_string(unsigned long e, char *buf) {
    return "mbedTLS cryptographic error"; 
}

void ERR_clear_error(void) {
    // No-op, we don't have an error queue to clear
}
