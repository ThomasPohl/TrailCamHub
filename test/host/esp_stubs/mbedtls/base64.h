#pragma once
#include <stddef.h>
// Host: a small encoder, enough for the thumbnail preview line.
extern "C" int mbedtls_base64_encode(unsigned char *dst, size_t dlen,
                                     size_t *olen, const unsigned char *src,
                                     size_t slen);
