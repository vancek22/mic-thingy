/* SHA-256 (FIPS 180-4), used to check a downloaded update against the checksum published with the release. */
#ifndef VF_SHA256_H
#define VF_SHA256_H

#include <stddef.h>
#include <stdint.h>

void vf_sha256(const void *data, size_t n, uint8_t out[32]);
int  vf_sha256_file(const char *path, char hex[65]);      /* lower-case hex + NUL; 0 = ok, -1 = unreadable */

#endif
