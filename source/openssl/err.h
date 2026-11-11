#ifndef OPENSSL_ERR_H
#define OPENSSL_ERR_H

unsigned long ERR_get_error(void);
char *ERR_error_string(unsigned long e, char *buf);
void ERR_clear_error(void);

#endif
