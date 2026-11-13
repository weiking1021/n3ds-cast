#include "cast_proto.h"
#include <stdlib.h>
#include <string.h>

static size_t write_varint(uint8_t *dst, size_t cap, uint64_t val) {
    size_t i = 0;
    while (val >= 0x80) {
        if (i >= cap) {
            return 0;
        }
        dst[i++] = (uint8_t)((val & 0x7F) | 0x80);
        val >>= 7;
    }
    if (i >= cap) {
        return 0;
    }
    dst[i++] = (uint8_t)(val & 0x7F);
    return i;
}

static size_t read_varint(const uint8_t *src, size_t len, uint64_t *out_val) {
    uint64_t result = 0;
    int shift = 0;
    for (size_t i = 0; i < len && i < 10; i++) {
        uint8_t byte = src[i];
        result |= ((uint64_t)(byte & 0x7F)) << shift;
        if ((byte & 0x80) == 0) {
            *out_val = result;
            return i + 1;
        }
        shift += 7;
    }
    return 0;
}

static size_t write_length_delimited(uint8_t *dst, size_t cap, uint8_t tag, const void *data, size_t data_len) {
    size_t pos = 0;
    if (pos >= cap) {
        return 0;
    }
    dst[pos++] = tag;

    size_t vlen = write_varint(dst + pos, cap - pos, data_len);
    if (vlen == 0) {
        return 0;
    }
    pos += vlen;

    if (pos + data_len > cap) {
        return 0;
    }
    memcpy(dst + pos, data, data_len);
    pos += data_len;
    return pos;
}

size_t cast_proto_encode_utf8(uint8_t *out_buf, size_t out_capacity, const char *source_id, const char *destination_id,
                              const char *ns, const char *payload_utf8, size_t payload_len) {
    if (out_capacity < 16) {
        return 0;
    }

    // Leave 4 bytes at the front for Big-Endian message length
    uint8_t *body = out_buf + 4;
    size_t cap = out_capacity - 4;
    size_t pos = 0;

    // Field 1: protocol_version = 0 (CASTV2_1_0) -> tag 0x08, value 0x00
    if (pos + 2 > cap) {
        return 0;
    }
    body[pos++] = 0x08;
    body[pos++] = 0x00;

    // Field 2: source_id -> tag 0x12
    size_t n = write_length_delimited(body + pos, cap - pos, 0x12, source_id, strlen(source_id));
    if (n == 0) {
        return 0;
    }
    pos += n;

    // Field 3: destination_id -> tag 0x1A
    n = write_length_delimited(body + pos, cap - pos, 0x1A, destination_id, strlen(destination_id));
    if (n == 0) {
        return 0;
    }
    pos += n;

    // Field 4: namespace -> tag 0x22
    n = write_length_delimited(body + pos, cap - pos, 0x22, ns, strlen(ns));
    if (n == 0) {
        return 0;
    }
    pos += n;

    // Field 5: payload_type = 0 (STRING) -> tag 0x28, value 0x00
    if (pos + 2 > cap) {
        return 0;
    }
    body[pos++] = 0x28;
    body[pos++] = 0x00;

    // Field 6: payload_utf8 -> tag 0x32
    n = write_length_delimited(body + pos, cap - pos, 0x32, payload_utf8, payload_len);
    if (n == 0) {
        return 0;
    }
    pos += n;

    // Write 4-byte Big-Endian length header
    uint32_t msg_len = (uint32_t)pos;
    out_buf[0] = (uint8_t)((msg_len >> 24) & 0xFF);
    out_buf[1] = (uint8_t)((msg_len >> 16) & 0xFF);
    out_buf[2] = (uint8_t)((msg_len >> 8) & 0xFF);
    out_buf[3] = (uint8_t)(msg_len & 0xFF);

    return pos + 4;
}

bool cast_proto_decode(const uint8_t *buf, size_t len, CastMessage *msg) {
    memset(msg, 0, sizeof(CastMessage));
    size_t pos = 0;

    while (pos < len) {
        uint64_t tag = 0;
        size_t tn = read_varint(buf + pos, len - pos, &tag);
        if (tn == 0) {
            return false;
        }
        pos += tn;

        uint32_t field_num = (uint32_t)(tag >> 3);
        uint32_t wire_type = (uint32_t)(tag & 0x07);

        if (wire_type == 0) {
            uint64_t val = 0;
            size_t vn = read_varint(buf + pos, len - pos, &val);
            if (vn == 0) {
                return false;
            }
            pos += vn;
            if (field_num == 5) {
                msg->payload_type = (int)val;
            }
        } else if (wire_type == 2) {
            uint64_t slen = 0;
            size_t ln = read_varint(buf + pos, len - pos, &slen);
            if (ln == 0 || pos + ln + slen > len) {
                return false;
            }
            pos += ln;

            const uint8_t *sptr = buf + pos;
            if (field_num == 2) {
                size_t cpy = slen < sizeof(msg->source_id) - 1 ? (size_t)slen : sizeof(msg->source_id) - 1;
                memcpy(msg->source_id, sptr, cpy);
                msg->source_id[cpy] = '\0';
            } else if (field_num == 3) {
                size_t cpy = slen < sizeof(msg->destination_id) - 1 ? (size_t)slen : sizeof(msg->destination_id) - 1;
                memcpy(msg->destination_id, sptr, cpy);
                msg->destination_id[cpy] = '\0';
            } else if (field_num == 4) {
                size_t cpy = slen < sizeof(msg->ns) - 1 ? (size_t)slen : sizeof(msg->ns) - 1;
                memcpy(msg->ns, sptr, cpy);
                msg->ns[cpy] = '\0';
            } else if (field_num == 6) {
                if (msg->payload_utf8) {
                    free(msg->payload_utf8);
                }
                msg->payload_utf8 = (char *)malloc((size_t)slen + 1);
                if (msg->payload_utf8) {
                    memcpy(msg->payload_utf8, sptr, (size_t)slen);
                    msg->payload_utf8[slen] = '\0';
                    msg->payload_len = (size_t)slen;
                }
            }
            pos += (size_t)slen;
        } else {
            // Unsupported wire type in CastMessage
            return false;
        }
    }
    return true;
}

void cast_proto_free_message(CastMessage *msg) {
    if (msg && msg->payload_utf8) {
        free(msg->payload_utf8);
        msg->payload_utf8 = NULL;
        msg->payload_len = 0;
    }
}
