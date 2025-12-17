#ifndef QUICKJS_INTERNAL_H
#define QUICKJS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* Internal helpers not exposed in the public QuickJS API surface. */
void js_sha256(const uint8_t *data, size_t len, uint8_t out_hash[32]);
void js_sha256_to_hex(const uint8_t hash[32], char out_hex[65]);

#endif /* QUICKJS_INTERNAL_H */
