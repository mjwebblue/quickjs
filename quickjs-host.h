#ifndef QUICKJS_HOST_H
#define QUICKJS_HOST_H

#include "quickjs.h"

typedef struct JSHostErrorEntry {
    JSAtom code_atom;
    JSAtom tag_atom;
} JSHostErrorEntry;

typedef struct JSHostResponseValidation {
    uint32_t max_units;
    const JSHostErrorEntry *errors;
    size_t error_count;
} JSHostResponseValidation;

typedef struct JSHostResponse {
    int is_error;
    uint32_t units;
    JSValue ok;
    JSAtom err_code_atom;
    JSAtom err_tag_atom;
    JSValue err_details;
} JSHostResponse;

typedef struct JSHostManifest JSHostManifest;

/* Host response envelope helpers (T-039). */
JSValue JS_ThrowHostError(JSContext *ctx, JSAtom code_atom, JSAtom tag_atom, JSValueConst details);
JSValue JS_ThrowHostTransportError(JSContext *ctx);
int JS_ParseHostResponse(JSContext *ctx,
                         const uint8_t *data,
                         size_t length,
                         const JSHostResponseValidation *validation,
                         JSHostResponse *out);
void JS_FreeHostResponse(JSContext *ctx, JSHostResponse *resp);
int JS_InitHostFromManifest(JSContext *ctx, const uint8_t *manifest_bytes, size_t manifest_size);
int JS_InitErgonomicGlobals(JSContext *ctx, const uint8_t *context_blob, size_t context_blob_size);
void JS_FreeHostManifest(JSContext *ctx);

/* Optional host-call tape (T-043). */
#define JS_HOST_TAPE_MAX_CAPACITY 1024

typedef struct JSHostTapeRecord {
    uint32_t fn_id;
    uint32_t req_len;
    uint32_t resp_len;
    uint32_t units;
    uint64_t gas_pre;
    uint64_t gas_post;
    int is_error;
    int charge_failed;
    uint8_t req_hash[32];
    uint8_t resp_hash[32];
} JSHostTapeRecord;

int JS_EnableHostTape(JSContext *ctx, size_t capacity);
int JS_ResetHostTape(JSContext *ctx);
size_t JS_GetHostTapeLength(JSContext *ctx);
int JS_ReadHostTape(JSContext *ctx, JSHostTapeRecord *out_records, size_t max_records, size_t *out_count);

#endif /* QUICKJS_HOST_H */
