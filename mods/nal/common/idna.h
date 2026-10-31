#ifndef MLB_IDNA_H
#define MLB_IDNA_H
#include <stddef.h>
/* Strict UTF-8 domain-to-ASCII conversion. Unicode labels are encoded with RFC 3492
   Punycode; invalid UTF-8, control characters and malformed DNS labels are rejected. */
int nidna_to_ascii(const char *input, char *output, size_t output_cap);
#endif
