'use strict';
// CASTV2 framing: a 4-byte big-endian length followed by a protobuf "CastMessage":
//   1 protocol_version (varint)   2 source_id   3 destination_id   4 namespace
//   5 payload_type (0 = string, 1 = binary)   6 payload_utf8   7 payload_binary

function varint(n) {
  const out = [];
  while (n >= 0x80) { out.push((n & 0x7f) | 0x80); n = Math.floor(n / 128); }
  out.push(n);
  return Buffer.from(out);
}
function field(tag, data) { return Buffer.concat([Buffer.from([tag]), varint(data.length), data]); }

// Builds one framed message (length prefix included).
function encode({ source, dest, ns, payload }) {
  const body = Buffer.concat([
    Buffer.from([0x08, 0x00]),
    field(0x12, Buffer.from(source)),
    field(0x1a, Buffer.from(dest)),
    field(0x22, Buffer.from(ns)),
    Buffer.from([0x28, 0x00]),
    field(0x32, Buffer.from(payload)),
  ]);
  const head = Buffer.alloc(4);
  head.writeUInt32BE(body.length);
  return Buffer.concat([head, body]);
}

function readVarint(buf, at) {
  let result = 0, mul = 1;
  for (let i = 0; i < 10 && at + i < buf.length; i++) {
    const b = buf[at + i];
    result += (b & 0x7f) * mul;
    if (!(b & 0x80)) return { value: result, next: at + i + 1 };
    mul *= 128;
  }
  throw new Error('bad varint');
}

function decodeBody(buf) {
  const m = { source: '', dest: '', ns: '', payload: '' };
  let at = 0;
  while (at < buf.length) {
    const key = readVarint(buf, at); at = key.next;
    const tag = key.value >> 3, wire = key.value & 7;
    if (wire === 0) { const v = readVarint(buf, at); at = v.next; continue; }
    if (wire !== 2) throw new Error('unsupported wire type ' + wire);
    const len = readVarint(buf, at); at = len.next;
    const data = buf.subarray(at, at + len.value); at += len.value;
    if (tag === 2) m.source = data.toString();
    else if (tag === 3) m.dest = data.toString();
    else if (tag === 4) m.ns = data.toString();
    else if (tag === 6) m.payload = data.toString();
  }
  return m;
}

// Collects stream chunks and calls onMessage for every complete framed message.
class Framer {
  constructor(onMessage, maxLen = 1 << 20) { this.buf = Buffer.alloc(0); this.onMessage = onMessage; this.maxLen = maxLen; }
  push(chunk) {
    this.buf = Buffer.concat([this.buf, chunk]);
    while (this.buf.length >= 4) {
      const len = this.buf.readUInt32BE(0);
      if (len === 0 || len > this.maxLen) throw new Error('bad Cast frame length ' + len);
      if (this.buf.length < 4 + len) return;
      const body = this.buf.subarray(4, 4 + len);
      this.buf = this.buf.subarray(4 + len);
      this.onMessage(decodeBody(body), len);
    }
  }
}

const NS = {
  connection: 'urn:x-cast:com.google.cast.tp.connection',
  heartbeat: 'urn:x-cast:com.google.cast.tp.heartbeat',
  receiver: 'urn:x-cast:com.google.cast.receiver',
  custom: 'urn:x-cast:com.n3ds.cast',
};

module.exports = { encode, decodeBody, Framer, NS };
