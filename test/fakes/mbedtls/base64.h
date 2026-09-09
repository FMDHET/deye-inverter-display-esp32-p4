#pragma once
/* Host stand-in for mbedtls' base64. A real (small) implementation, not a
 * pretend one: the code under test parses what comes OUT of it, so a fake that
 * decoded incorrectly would test nothing. Same return contract as mbedtls
 * (0 = ok, non-zero = refused). */
#include <stddef.h>

int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen);
int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen);
