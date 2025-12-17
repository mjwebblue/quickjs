#include "quickjs.h"
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifndef JS_CLASS_OBJECT
/* Upstream class id for Object; used here to fetch the intrinsic prototype
   without relying on the global Object binding. */
#define JS_CLASS_OBJECT 1
#endif

/* Internal allocator hooks from quickjs.c */
extern void *js_malloc(JSContext *ctx, size_t size);
extern void *js_realloc(JSContext *ctx, void *ptr, size_t size);
extern void js_free(JSContext *ctx, void *ptr);

#define DV_CBOR_MAJOR_UINT 0
#define DV_CBOR_MAJOR_NINT 1
#define DV_CBOR_MAJOR_TEXT 3
#define DV_CBOR_MAJOR_ARRAY 4
#define DV_CBOR_MAJOR_MAP 5
#define DV_CBOR_MAJOR_SIMPLE 7

static const int64_t dv_max_safe_int = 9007199254740991LL;      /* 2^53 - 1 */
static const int64_t dv_min_safe_int = -9007199254740991LL;     /* -(2^53 - 1) */

static const JSDvLimits *dv_limits_or_default(const JSDvLimits *limits) {
    return limits ? limits : &JS_DV_LIMIT_DEFAULTS;
}

typedef struct {
    JSContext *ctx;
    uint8_t *data;
    size_t size;
    size_t capacity;
    size_t max_size;
} JSDvBuilder;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    JSContext *ctx;
} JSDvReader;

typedef struct {
    JSAtom atom;
    JSValue key_value;
    uint8_t *encoded_key;
    size_t encoded_key_len;
} JSDvKeyEntry;

static int dv_throw(JSContext *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    JS_ThrowTypeError(ctx, "%s", buf);
    return -1;
}

static int dv_builder_reserve(JSDvBuilder *builder, size_t additional) {
    if (additional > builder->max_size - builder->size) {
        JS_ThrowTypeError(
            builder->ctx,
            "DV encode: encoded DV exceeds maxEncodedBytes (%zu > %zu)",
            builder->size + additional,
            builder->max_size);
        return -1;
    }

    size_t required = builder->size + additional;
    if (required <= builder->capacity) {
        return 0;
    }

    size_t new_capacity = builder->capacity ? builder->capacity : 64;
    while (new_capacity < required) {
        size_t next = new_capacity * 2;
        if (next <= new_capacity) {
            new_capacity = builder->max_size;
            break;
        }
        new_capacity = next;
    }

    if (new_capacity > builder->max_size) {
        new_capacity = builder->max_size;
    }

    uint8_t *reallocated = js_realloc(builder->ctx, builder->data, new_capacity);
    if (!reallocated) {
        return -1;
    }

    builder->data = reallocated;
    builder->capacity = new_capacity;
    return 0;
}

static void dv_builder_free(JSDvBuilder *builder) {
    if (builder->data) {
        js_free(builder->ctx, builder->data);
        builder->data = NULL;
    }
    builder->size = 0;
    builder->capacity = 0;
}

static int dv_builder_push_u8(JSDvBuilder *builder, uint8_t value) {
    if (dv_builder_reserve(builder, 1) != 0) {
        return -1;
    }
    builder->data[builder->size++] = value;
    return 0;
}

static int dv_builder_push_bytes(JSDvBuilder *builder, const uint8_t *data, size_t length) {
    if (length == 0) {
        return 0;
    }
    if (dv_builder_reserve(builder, length) != 0) {
        return -1;
    }
    memcpy(builder->data + builder->size, data, length);
    builder->size += length;
    return 0;
}

static int dv_builder_push_u64_be(JSDvBuilder *builder, uint64_t value, size_t width) {
    uint8_t buf[8];
    for (size_t i = 0; i < width; i++) {
        buf[width - 1 - i] = (uint8_t)(value & 0xff);
        value >>= 8;
    }
    return dv_builder_push_bytes(builder, buf, width);
}

static int dv_validate_utf8(JSContext *ctx, const uint8_t *data, size_t length) {
    size_t i = 0;
    while (i < length) {
        uint8_t byte = data[i];
        uint32_t codepoint;
        size_t needed;

        if (byte < 0x80) {
            codepoint = byte;
            needed = 0;
        } else if ((byte & 0xe0) == 0xc0) {
            codepoint = byte & 0x1f;
            needed = 1;
            if (codepoint == 0) {
                return dv_throw(ctx, "DV string contains invalid UTF-8");
            }
        } else if ((byte & 0xf0) == 0xe0) {
            codepoint = byte & 0x0f;
            needed = 2;
        } else if ((byte & 0xf8) == 0xf0) {
            codepoint = byte & 0x07;
            needed = 3;
        } else {
            return dv_throw(ctx, "DV string contains invalid UTF-8");
        }

        if (i + needed >= length) {
            return dv_throw(ctx, "DV string contains invalid UTF-8");
        }

        for (size_t j = 0; j < needed; j++) {
            uint8_t cont = data[i + 1 + j];
            if ((cont & 0xc0) != 0x80) {
                return dv_throw(ctx, "DV string contains invalid UTF-8");
            }
            codepoint = (codepoint << 6) | (cont & 0x3f);
        }

        if ((needed == 1 && codepoint < 0x80) ||
            (needed == 2 && codepoint < 0x800) ||
            (needed == 3 && codepoint < 0x10000)) {
            return dv_throw(ctx, "DV string contains invalid UTF-8");
        }

        if (codepoint > 0x10ffff) {
            return dv_throw(ctx, "DV string contains invalid UTF-8");
        }
        if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
            return dv_throw(ctx, "DV string contains lone surrogate code points");
        }

        i += needed + 1;
    }
    return 0;
}

static int dv_encode_type_and_length(JSDvBuilder *builder, uint8_t major, uint64_t length) {
    if (length <= 23) {
        return dv_builder_push_u8(builder, (uint8_t)((major << 5) | length));
    }
    if (length <= 0xff) {
        if (dv_builder_push_u8(builder, (uint8_t)((major << 5) | 24)) != 0) {
            return -1;
        }
        return dv_builder_push_u64_be(builder, length, 1);
    }
    if (length <= 0xffff) {
        if (dv_builder_push_u8(builder, (uint8_t)((major << 5) | 25)) != 0) {
            return -1;
        }
        return dv_builder_push_u64_be(builder, length, 2);
    }
    if (length <= 0xffffffff) {
        if (dv_builder_push_u8(builder, (uint8_t)((major << 5) | 26)) != 0) {
            return -1;
        }
        return dv_builder_push_u64_be(builder, length, 4);
    }
    if (dv_builder_push_u8(builder, (uint8_t)((major << 5) | 27)) != 0) {
        return -1;
    }
    return dv_builder_push_u64_be(builder, length, 8);
}

static int dv_encode_number(JSContext *ctx, double value, JSDvBuilder *builder) {
    if (!isfinite(value)) {
        return dv_throw(ctx, "DV numbers must be finite");
    }

    if (value == 0 && signbit(value)) {
        value = 0;
    }

    double int_part;
    if (modf(value, &int_part) == 0.0) {
        if (value > (double)dv_max_safe_int || value < (double)dv_min_safe_int) {
            return dv_throw(ctx,
                            "integer is outside safe range (not in [%" PRId64 ", %" PRId64 "])",
                            dv_min_safe_int,
                            dv_max_safe_int);
        }

        uint64_t unsigned_val;
        if (value >= 0) {
            unsigned_val = (uint64_t)value;
            return dv_encode_type_and_length(builder, DV_CBOR_MAJOR_UINT, unsigned_val);
        }

        unsigned_val = (uint64_t)(-1 - (int64_t)value);
        return dv_encode_type_and_length(builder, DV_CBOR_MAJOR_NINT, unsigned_val);
    }

    if (dv_builder_push_u8(builder, 0xfb) != 0) {
        return -1;
    }
    union {
        double d;
        uint64_t u;
    } u;
    u.d = value;
    return dv_builder_push_u64_be(builder, u.u, 8);
}

static int dv_encode_string_bytes(JSContext *ctx,
                                  JSValueConst value,
                                  const JSDvLimits *limits,
                                  uint8_t **out_bytes,
                                  size_t *out_len) {
    size_t byte_length = 0;
    const char *raw = JS_ToCStringLen2(ctx, &byte_length, value, 0);
    if (!raw) {
        return -1;
    }

    if (byte_length > limits->max_string_bytes) {
        JS_FreeCString(ctx, raw);
        return dv_throw(ctx,
                        "string exceeds maxStringBytes (%zu > %u)",
                        byte_length,
                        limits->max_string_bytes);
    }

    if (dv_validate_utf8(ctx, (const uint8_t *)raw, byte_length) != 0) {
        JS_FreeCString(ctx, raw);
        return -1;
    }

    uint8_t *copy = NULL;
    if (byte_length > 0) {
        copy = js_malloc(ctx, byte_length);
        if (!copy) {
            JS_FreeCString(ctx, raw);
            return -1;
        }
        memcpy(copy, raw, byte_length);
    }
    JS_FreeCString(ctx, raw);
    *out_bytes = copy;
    *out_len = byte_length;
    return 0;
}

static int dv_encode_value(JSContext *ctx,
                           JSValueConst value,
                           const JSDvLimits *limits,
                           uint32_t depth,
                           JSDvBuilder *builder);

static int dv_encode_array(JSContext *ctx,
                           JSValueConst value,
                           const JSDvLimits *limits,
                           uint32_t depth,
                           JSDvBuilder *builder) {
    uint32_t next_depth = depth + 1;
    if (next_depth > limits->max_depth) {
        return dv_throw(ctx, "maxDepth %u exceeded", limits->max_depth);
    }

    uint64_t length64 = 0;
    JSValue length_val = JS_GetPropertyStr(ctx, value, "length");
    if (JS_IsException(length_val)) {
        return -1;
    }
    int len_rc = JS_ToIndex(ctx, &length64, length_val);
    JS_FreeValue(ctx, length_val);
    if (len_rc < 0) {
        return -1;
    }

    if (length64 > limits->max_array_length) {
        return dv_throw(ctx,
                        "array length exceeds maxArrayLength (%" PRIu64 " > %u)",
                        length64,
                        limits->max_array_length);
    }

    uint32_t length = (uint32_t)length64;
    if (dv_encode_type_and_length(builder, DV_CBOR_MAJOR_ARRAY, length) != 0) {
        return -1;
    }

    for (uint32_t i = 0; i < length; i++) {
        JSValue element = JS_GetPropertyUint32(ctx, value, i);
        if (JS_IsException(element)) {
            return -1;
        }
        int rc = dv_encode_value(ctx, element, limits, next_depth, builder);
        JS_FreeValue(ctx, element);
        if (rc != 0) {
            return -1;
        }
    }

    return 0;
}

static int dv_compare_encoded_keys(const void *a, const void *b) {
    const JSDvKeyEntry *ka = (const JSDvKeyEntry *)a;
    const JSDvKeyEntry *kb = (const JSDvKeyEntry *)b;

    if (ka->encoded_key_len != kb->encoded_key_len) {
        return ka->encoded_key_len < kb->encoded_key_len ? -1 : 1;
    }
    return memcmp(ka->encoded_key, kb->encoded_key, ka->encoded_key_len);
}

static int dv_encode_object(JSContext *ctx,
                            JSValueConst value,
                            const JSDvLimits *limits,
                            uint32_t depth,
                            JSDvBuilder *builder) {
    uint32_t next_depth = depth + 1;
    if (next_depth > limits->max_depth) {
        return dv_throw(ctx, "maxDepth %u exceeded", limits->max_depth);
    }

    JSValue proto = JS_GetPrototype(ctx, value);
    if (JS_IsException(proto)) {
        return -1;
    }

    int is_plain = 0;
    if (JS_IsNull(proto)) {
        is_plain = 1;
    } else {
        JSValue object_proto = JS_GetClassProto(ctx, JS_CLASS_OBJECT);
        if (JS_IsException(object_proto)) {
            JS_FreeValue(ctx, proto);
            return -1;
        }

        int same = JS_SameValue(ctx, proto, object_proto);
        JS_FreeValue(ctx, object_proto);
        if (same < 0) {
            JS_FreeValue(ctx, proto);
            return -1;
        }

        is_plain = same;
    }

    JS_FreeValue(ctx, proto);

    if (!is_plain) {
        return dv_throw(ctx, "unsupported DV type: object");
    }

    JSPropertyEnum *props = NULL;
    uint32_t prop_len = 0;
    if (JS_GetOwnPropertyNames(ctx,
                               &props,
                               &prop_len,
                               value,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        return -1;
    }

    if (prop_len > limits->max_map_length) {
        JS_FreePropertyEnum(ctx, props, prop_len);
        return dv_throw(ctx,
                        "map entries exceed maxMapLength (%u > %u)",
                        prop_len,
                        limits->max_map_length);
    }

    JSDvKeyEntry *entries = NULL;
    if (prop_len > 0) {
        entries = js_malloc(ctx, prop_len * sizeof(*entries));
        if (!entries) {
            JS_FreePropertyEnum(ctx, props, prop_len);
            return -1;
        }
    }

    for (uint32_t i = 0; i < prop_len; i++) {
        entries[i].atom = props[i].atom;
        entries[i].encoded_key = NULL;
        entries[i].encoded_key_len = 0;
        entries[i].key_value = JS_UNDEFINED;

        JSValue key_val = JS_AtomToString(ctx, props[i].atom);
        if (JS_IsException(key_val)) {
            goto encode_object_error;
        }

        uint8_t *key_bytes = NULL;
        size_t key_len = 0;
        if (dv_encode_string_bytes(ctx, key_val, limits, &key_bytes, &key_len) != 0) {
            JS_FreeValue(ctx, key_val);
            goto encode_object_error;
        }

        JSDvBuilder key_builder = {
            .ctx = ctx,
            .data = NULL,
            .size = 0,
            .capacity = 0,
            .max_size = limits->max_encoded_bytes,
        };

        if (dv_encode_type_and_length(&key_builder, DV_CBOR_MAJOR_TEXT, key_len) != 0 ||
            dv_builder_push_bytes(&key_builder, key_bytes, key_len) != 0) {
            dv_builder_free(&key_builder);
            js_free(ctx, key_bytes);
            JS_FreeValue(ctx, key_val);
            goto encode_object_error;
        }

        if (key_bytes) {
            js_free(ctx, key_bytes);
        }

        entries[i].encoded_key = key_builder.data;
        entries[i].encoded_key_len = key_builder.size;
        entries[i].key_value = key_val;
    }

    qsort(entries, prop_len, sizeof(entries[0]), dv_compare_encoded_keys);

    for (uint32_t i = 1; i < prop_len; i++) {
        int cmp = dv_compare_encoded_keys(&entries[i - 1], &entries[i]);
        if (cmp == 0) {
            JSValue str_val = JS_JSONStringify(ctx,
                                               entries[i].key_value,
                                               JS_UNDEFINED,
                                               JS_UNDEFINED);
            if (!JS_IsException(str_val)) {
                const char *dup = JS_ToCString(ctx, str_val);
                const char *dup_key = dup ? dup : "<duplicate>";
                dv_throw(ctx, "map contains duplicate key %s", dup_key);
                if (dup) {
                    JS_FreeCString(ctx, dup);
                }
                JS_FreeValue(ctx, str_val);
            } else {
                dv_throw(ctx, "map contains duplicate key");
            }
            goto encode_object_error;
        }
    }

    if (dv_encode_type_and_length(builder, DV_CBOR_MAJOR_MAP, prop_len) != 0) {
        goto encode_object_error;
    }

    for (uint32_t i = 0; i < prop_len; i++) {
        if (dv_builder_push_bytes(builder, entries[i].encoded_key, entries[i].encoded_key_len) != 0) {
            goto encode_object_error;
        }
        JSValue prop_val = JS_GetProperty(ctx, value, entries[i].atom);
        if (JS_IsException(prop_val)) {
            goto encode_object_error;
        }
        int rc = dv_encode_value(ctx, prop_val, limits, next_depth, builder);
        JS_FreeValue(ctx, prop_val);
        if (rc != 0) {
            goto encode_object_error;
        }
    }

    for (uint32_t i = 0; i < prop_len; i++) {
        if (entries[i].encoded_key) {
            js_free(ctx, entries[i].encoded_key);
        }
        JS_FreeValue(ctx, entries[i].key_value);
    }
    if (entries) {
        js_free(ctx, entries);
    }
    if (props) {
        JS_FreePropertyEnum(ctx, props, prop_len);
    }
    return 0;

encode_object_error:
    if (entries) {
        for (uint32_t i = 0; i < prop_len; i++) {
            if (entries[i].encoded_key) {
                js_free(ctx, entries[i].encoded_key);
            }
            if (!JS_IsUndefined(entries[i].key_value)) {
                JS_FreeValue(ctx, entries[i].key_value);
            }
        }
        js_free(ctx, entries);
    }
    if (props) {
        JS_FreePropertyEnum(ctx, props, prop_len);
    }
    return -1;
}

static int dv_encode_value(JSContext *ctx,
                           JSValueConst value,
                           const JSDvLimits *limits,
                           uint32_t depth,
                           JSDvBuilder *builder) {
    if (depth > limits->max_depth) {
        return dv_throw(ctx, "maxDepth %u exceeded", limits->max_depth);
    }

    int tag = JS_VALUE_GET_NORM_TAG(value);
    switch (tag) {
        case JS_TAG_NULL:
            return dv_builder_push_u8(builder, 0xf6);
        case JS_TAG_BOOL: {
            return dv_builder_push_u8(builder, JS_VALUE_GET_BOOL(value) ? 0xf5 : 0xf4);
        }
        case JS_TAG_INT:
            return dv_encode_number(ctx, JS_VALUE_GET_INT(value), builder);
        case JS_TAG_FLOAT64:
            return dv_encode_number(ctx, JS_VALUE_GET_FLOAT64(value), builder);
        case JS_TAG_STRING:
        case JS_TAG_STRING_ROPE: {
            uint8_t *bytes = NULL;
            size_t len = 0;
            if (dv_encode_string_bytes(ctx, value, limits, &bytes, &len) != 0) {
                return -1;
            }
            int rc = dv_encode_type_and_length(builder, DV_CBOR_MAJOR_TEXT, len);
            if (rc == 0) {
                rc = dv_builder_push_bytes(builder, bytes, len);
            }
            if (bytes) {
                js_free(ctx, bytes);
            }
            return rc;
        }
        case JS_TAG_OBJECT: {
            int is_array = JS_IsArray(ctx, value);
            if (is_array < 0) {
                return -1;
            }
            if (is_array) {
                return dv_encode_array(ctx, value, limits, depth, builder);
            }
            return dv_encode_object(ctx, value, limits, depth, builder);
        }
        default:
            return dv_throw(ctx, "unsupported DV type: %d", tag);
    }
}

int JS_EncodeDV(JSContext *ctx,
                JSValueConst value,
                const JSDvLimits *maybe_limits,
                JSDvBuffer *out_buffer) {
    const JSDvLimits *limits = dv_limits_or_default(maybe_limits);
    if (out_buffer) {
        out_buffer->data = NULL;
        out_buffer->length = 0;
    }

    JSDvBuilder builder = {
        .ctx = ctx,
        .data = NULL,
        .size = 0,
        .capacity = 0,
        .max_size = limits->max_encoded_bytes,
    };

    if (dv_encode_value(ctx, value, limits, 0, &builder) != 0) {
        dv_builder_free(&builder);
        return -1;
    }

    if (out_buffer) {
        out_buffer->data = builder.data;
        out_buffer->length = builder.size;
    } else {
        dv_builder_free(&builder);
    }
    return 0;
}

static int dv_reader_need(JSDvReader *reader, size_t amount) {
    if (reader->pos + amount > reader->size) {
        JS_ThrowTypeError(reader->ctx, "unexpected end of buffer");
        return -1;
    }
    return 0;
}

static int dv_reader_read_u8(JSDvReader *reader, uint8_t *out) {
    if (dv_reader_need(reader, 1) != 0) {
        return -1;
    }
    *out = reader->data[reader->pos++];
    return 0;
}

static int dv_reader_read_be(JSDvReader *reader, size_t width, uint64_t *out) {
    if (dv_reader_need(reader, width) != 0) {
        return -1;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < width; i++) {
        value = (value << 8) | reader->data[reader->pos + i];
    }
    reader->pos += width;
    *out = value;
    return 0;
}

static int dv_read_length(JSDvReader *reader, uint8_t additional, uint64_t *out) {
    if (additional <= 23) {
        *out = additional;
        return 0;
    }

    uint64_t value = 0;
    switch (additional) {
        case 24:
            if (dv_reader_read_be(reader, 1, &value) != 0) {
                return -1;
            }
            if (value < 24) {
                return dv_throw(reader->ctx, "length not using shortest encoding");
            }
            break;
        case 25:
            if (dv_reader_read_be(reader, 2, &value) != 0) {
                return -1;
            }
            if (value <= 0xff) {
                return dv_throw(reader->ctx, "length not using shortest encoding");
            }
            break;
        case 26:
            if (dv_reader_read_be(reader, 4, &value) != 0) {
                return -1;
            }
            if (value <= 0xffff) {
                return dv_throw(reader->ctx, "length not using shortest encoding");
            }
            break;
        case 27:
            if (dv_reader_read_be(reader, 8, &value) != 0) {
                return -1;
            }
            if (value <= 0xffffffff) {
                return dv_throw(reader->ctx, "length not using shortest encoding");
            }
            break;
        case 31:
            return dv_throw(reader->ctx, "indefinite lengths are not allowed");
        default:
            return dv_throw(reader->ctx, "unsupported additional info %u", additional);
    }

    *out = value;
    return 0;
}

static JSValue dv_decode_value(JSContext *ctx,
                               JSDvReader *reader,
                               const JSDvLimits *limits,
                               uint32_t depth);

static JSValue dv_decode_text(JSContext *ctx,
                              JSDvReader *reader,
                              const JSDvLimits *limits,
                              uint8_t additional) {
    uint64_t length64 = 0;
    if (dv_read_length(reader, additional, &length64) != 0) {
        return JS_EXCEPTION;
    }

    if (length64 > limits->max_string_bytes) {
        JS_ThrowTypeError(ctx,
                          "string exceeds maxStringBytes (%" PRIu64 " > %u)",
                          length64,
                          limits->max_string_bytes);
        return JS_EXCEPTION;
    }

    size_t length = (size_t)length64;
    if (dv_reader_need(reader, length) != 0) {
        return JS_EXCEPTION;
    }

    const uint8_t *bytes = reader->data + reader->pos;
    if (dv_validate_utf8(ctx, bytes, length) != 0) {
        return JS_EXCEPTION;
    }

    JSValue str = JS_NewStringLen(ctx, (const char *)bytes, length);
    reader->pos += length;
    return str;
}

static JSValue dv_decode_array(JSContext *ctx,
                               JSDvReader *reader,
                               const JSDvLimits *limits,
                               uint32_t depth,
                               uint8_t additional) {
    uint64_t length64 = 0;
    if (dv_read_length(reader, additional, &length64) != 0) {
        return JS_EXCEPTION;
    }

    if (length64 > limits->max_array_length) {
        JS_ThrowTypeError(ctx,
                          "array length exceeds maxArrayLength (%" PRIu64 " > %u)",
                          length64,
                          limits->max_array_length);
        return JS_EXCEPTION;
    }

    if (depth + 1 > limits->max_depth) {
        JS_ThrowTypeError(ctx, "maxDepth %u exceeded", limits->max_depth);
        return JS_EXCEPTION;
    }

    uint32_t length = (uint32_t)length64;
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        return JS_EXCEPTION;
    }

    for (uint32_t i = 0; i < length; i++) {
        JSValue element = dv_decode_value(ctx, reader, limits, depth + 1);
        if (JS_IsException(element)) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        if (JS_SetPropertyUint32(ctx, arr, i, element) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }

    return arr;
}

static JSValue dv_decode_map(JSContext *ctx,
                             JSDvReader *reader,
                             const JSDvLimits *limits,
                             uint32_t depth,
                             uint8_t additional) {
    uint64_t length64 = 0;
    if (dv_read_length(reader, additional, &length64) != 0) {
        return JS_EXCEPTION;
    }

    if (length64 > limits->max_map_length) {
        JS_ThrowTypeError(ctx,
                          "map entries exceed maxMapLength (%" PRIu64 " > %u)",
                          length64,
                          limits->max_map_length);
        return JS_EXCEPTION;
    }

    if (depth + 1 > limits->max_depth) {
        JS_ThrowTypeError(ctx, "maxDepth %u exceeded", limits->max_depth);
        return JS_EXCEPTION;
    }

    uint32_t length = (uint32_t)length64;
    JSValue obj = JS_NewObjectProto(ctx, JS_NULL);
    if (JS_IsException(obj)) {
        return JS_EXCEPTION;
    }

    uint8_t *prev_key = NULL;
    size_t prev_key_len = 0;

    for (uint32_t i = 0; i < length; i++) {
        size_t key_start = reader->pos;
        uint8_t key_initial;
        if (dv_reader_read_u8(reader, &key_initial) != 0) {
            JS_FreeValue(ctx, obj);
            js_free(ctx, prev_key);
            return JS_EXCEPTION;
        }

        uint8_t key_major = key_initial >> 5;
        if (key_major != DV_CBOR_MAJOR_TEXT) {
            JS_FreeValue(ctx, obj);
            js_free(ctx, prev_key);
            JS_ThrowTypeError(ctx, "map keys must be text strings");
            return JS_EXCEPTION;
        }

        JSValue key_val = dv_decode_text(ctx, reader, limits, key_initial & 0x1f);
        if (JS_IsException(key_val)) {
            JS_FreeValue(ctx, obj);
            js_free(ctx, prev_key);
            return JS_EXCEPTION;
        }

        size_t key_end = reader->pos;
        size_t key_len = key_end - key_start;
        const uint8_t *key_bytes = reader->data + key_start;

        if (prev_key) {
            int cmp = dv_compare_encoded_keys(&(JSDvKeyEntry){.encoded_key = prev_key, .encoded_key_len = prev_key_len},
                                              &(JSDvKeyEntry){.encoded_key = (uint8_t *)key_bytes, .encoded_key_len = key_len});
            if (cmp == 0) {
                JS_ThrowTypeError(ctx, "map contains duplicate key");
                JS_FreeValue(ctx, key_val);
                JS_FreeValue(ctx, obj);
                js_free(ctx, prev_key);
                return JS_EXCEPTION;
            }
            if (cmp > 0) {
                JS_ThrowTypeError(ctx, "map keys are not in canonical order");
                JS_FreeValue(ctx, key_val);
                JS_FreeValue(ctx, obj);
                js_free(ctx, prev_key);
                return JS_EXCEPTION;
            }
        }

        if (prev_key) {
            js_free(ctx, prev_key);
        }
        prev_key = NULL;
        if (key_len > 0) {
            prev_key = js_malloc(ctx, key_len);
            if (!prev_key) {
                JS_FreeValue(ctx, key_val);
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
            memcpy(prev_key, key_bytes, key_len);
        }
        prev_key_len = key_len;

        JSValue decoded = dv_decode_value(ctx, reader, limits, depth + 1);
        if (JS_IsException(decoded)) {
            JS_FreeValue(ctx, key_val);
            JS_FreeValue(ctx, obj);
            js_free(ctx, prev_key);
            return JS_EXCEPTION;
        }

        size_t key_str_len = 0;
        const char *key_str = JS_ToCStringLen(ctx, &key_str_len, key_val);
        if (!key_str) {
            JS_FreeValue(ctx, decoded);
            JS_FreeValue(ctx, key_val);
            JS_FreeValue(ctx, obj);
            js_free(ctx, prev_key);
            return JS_EXCEPTION;
        }

        JSAtom atom = JS_NewAtomLen(ctx, key_str, key_str_len);
        JS_FreeCString(ctx, key_str);
        JS_FreeValue(ctx, key_val);
        if (atom == JS_ATOM_NULL) {
            JS_FreeValue(ctx, decoded);
            JS_FreeValue(ctx, obj);
            js_free(ctx, prev_key);
            return JS_EXCEPTION;
        }

        if (JS_DefinePropertyValue(ctx, obj, atom, decoded, JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, obj);
            js_free(ctx, prev_key);
            return JS_EXCEPTION;
        }
        JS_FreeAtom(ctx, atom);
    }

    if (prev_key) {
        js_free(ctx, prev_key);
    }
    return obj;
}

static JSValue dv_decode_simple_or_float(JSContext *ctx,
                                         JSDvReader *reader,
                                         const JSDvLimits *limits,
                                         uint8_t additional) {
    (void)limits;
    switch (additional) {
        case 20:
            return JS_FALSE;
        case 21:
            return JS_TRUE;
        case 22:
            return JS_NULL;
        case 27: {
            uint64_t bits = 0;
            if (dv_reader_read_be(reader, 8, &bits) != 0) {
                return JS_EXCEPTION;
            }
            union {
                uint64_t u;
                double d;
            } u;
            u.u = bits;
            if (!isfinite(u.d)) {
                JS_ThrowTypeError(ctx, "DV numbers must be finite");
                return JS_EXCEPTION;
            }
            double int_part;
            if (modf(u.d, &int_part) == 0.0) {
                JS_ThrowTypeError(ctx, "integers must use CBOR integer encoding");
                return JS_EXCEPTION;
            }
            if (u.d == 0 && signbit(u.d)) {
                u.d = 0;
            }
            return JS_NewFloat64(ctx, u.d);
        }
        case 24:
        case 25:
        case 26:
            JS_ThrowTypeError(ctx, "only float64 is allowed");
            return JS_EXCEPTION;
        case 31:
            JS_ThrowTypeError(ctx, "indefinite lengths are not allowed");
            return JS_EXCEPTION;
        default:
            JS_ThrowTypeError(ctx, "unsupported simple value %u", additional);
            return JS_EXCEPTION;
    }
}

static JSValue dv_decode_value(JSContext *ctx,
                               JSDvReader *reader,
                               const JSDvLimits *limits,
                               uint32_t depth) {
    uint8_t initial = 0;
    if (dv_reader_read_u8(reader, &initial) != 0) {
        return JS_EXCEPTION;
    }

    uint8_t major = initial >> 5;
    uint8_t additional = initial & 0x1f;

    switch (major) {
        case DV_CBOR_MAJOR_UINT: {
            uint64_t value = 0;
            if (dv_read_length(reader, additional, &value) != 0) {
                return JS_EXCEPTION;
            }
            if (value > (uint64_t)dv_max_safe_int) {
                JS_ThrowTypeError(ctx, "integer is outside safe range (%" PRIu64 " > %" PRIu64 ")",
                                  value,
                                  (uint64_t)dv_max_safe_int);
                return JS_EXCEPTION;
            }
            return JS_NewInt64(ctx, (int64_t)value);
        }
        case DV_CBOR_MAJOR_NINT: {
            uint64_t value = 0;
            if (dv_read_length(reader, additional, &value) != 0) {
                return JS_EXCEPTION;
            }
            if (value >= (uint64_t)dv_max_safe_int) {
                JS_ThrowTypeError(ctx, "integer is outside safe range (-1 - %" PRIu64 " < %" PRId64 ")",
                                  value,
                                  dv_min_safe_int);
                return JS_EXCEPTION;
            }
            int64_t neg = -1 - (int64_t)value;
            return JS_NewInt64(ctx, neg);
        }
        case DV_CBOR_MAJOR_TEXT:
            return dv_decode_text(ctx, reader, limits, additional);
        case DV_CBOR_MAJOR_ARRAY:
            return dv_decode_array(ctx, reader, limits, depth, additional);
        case DV_CBOR_MAJOR_MAP:
            return dv_decode_map(ctx, reader, limits, depth, additional);
        case DV_CBOR_MAJOR_SIMPLE:
            return dv_decode_simple_or_float(ctx, reader, limits, additional);
        default:
            JS_ThrowTypeError(ctx, "unsupported CBOR major type %u", major);
            return JS_EXCEPTION;
    }
}

JSValue JS_DecodeDV(JSContext *ctx,
                    const uint8_t *data,
                    size_t length,
                    const JSDvLimits *maybe_limits) {
    const JSDvLimits *limits = dv_limits_or_default(maybe_limits);
    if (length > limits->max_encoded_bytes) {
        JS_ThrowTypeError(ctx,
                          "encoded DV exceeds maxEncodedBytes (%zu > %u)",
                          length,
                          limits->max_encoded_bytes);
        return JS_EXCEPTION;
    }

    JSDvReader reader = {
        .data = data,
        .size = length,
        .pos = 0,
        .ctx = ctx,
    };

    JSValue result = dv_decode_value(ctx, &reader, limits, 0);
    if (JS_IsException(result)) {
        return result;
    }

    if (reader.pos != reader.size) {
        JS_FreeValue(ctx, result);
        JS_ThrowTypeError(ctx, "unexpected trailing bytes after DV value");
        return JS_EXCEPTION;
    }

    return result;
}

void JS_FreeDVBuffer(JSContext *ctx, JSDvBuffer *buffer) {
    if (!buffer || !buffer->data) {
        return;
    }
    js_free(ctx, buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
}

const JSDvLimits JS_DV_LIMIT_DEFAULTS = {
    .max_depth = 64,
    .max_encoded_bytes = 1048576,
    .max_string_bytes = 262144,
    .max_array_length = 65535,
    .max_map_length = 65535,
};
