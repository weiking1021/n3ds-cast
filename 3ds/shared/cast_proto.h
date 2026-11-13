#ifndef N3DS_CAST_PROTO_H
#define N3DS_CAST_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CAST_NS_CONNECTION "urn:x-cast:com.google.cast.tp.connection"
#define CAST_NS_HEARTBEAT "urn:x-cast:com.google.cast.tp.heartbeat"
#define CAST_NS_RECEIVER "urn:x-cast:com.google.cast.receiver"
#define CAST_NS_CUSTOM "urn:x-cast:com.n3ds.cast"

#define CAST_DEFAULT_SOURCE "sender-0"
#define CAST_DEFAULT_DEST "receiver-0"

typedef struct {
    char source_id[64];
    char destination_id[64];
    char ns[128];
    int payload_type; // 0 = UTF8 string, 1 = binary
    char *payload_utf8;
    size_t payload_len;
} CastMessage;

/**
 * Encode a UTF-8 CastMessage into out_buf, including the 4-byte Big-Endian length header.
 * Returns total bytes written, or 0 on buffer overflow.
 */
size_t cast_proto_encode_utf8(uint8_t *out_buf, size_t out_capacity, const char *source_id, const char *destination_id,
                              const char *ns, const char *payload_utf8, size_t payload_len);

/**
 * Decode a raw protobuf body (excluding the 4-byte length prefix) into CastMessage.
 * Note: msg->payload_utf8 points into a newly allocated buffer if payload_len > 0;
 * caller must call cast_proto_free_message(msg).
 */
bool cast_proto_decode(const uint8_t *buf, size_t len, CastMessage *msg);

void cast_proto_free_message(CastMessage *msg);

#endif // N3DS_CAST_PROTO_H
