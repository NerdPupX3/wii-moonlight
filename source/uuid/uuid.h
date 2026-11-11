#ifndef UUID_H
#define UUID_H

typedef unsigned char uuid_t[16];

void uuid_generate(uuid_t out);
void uuid_unparse(const uuid_t uu, char *out);

#define uuid_generate_random uuid_generate
#endif
