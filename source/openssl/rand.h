#ifndef OPENSSL_RAND_H
#define OPENSSL_RAND_H

#include <stddef.h>

int RAND_bytes(unsigned char *buf, int num);

#endif
